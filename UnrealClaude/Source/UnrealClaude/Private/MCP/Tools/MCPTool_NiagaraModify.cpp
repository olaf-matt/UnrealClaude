// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_NiagaraModify.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"

// Niagara runtime
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "NiagaraCommon.h"
#include "NiagaraParameterStore.h"   // FNiagaraParameterStore::SetParameterValue<T>

// Niagara editor (graph editing)
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeAssignment.h"         // UNiagaraNodeAssignment (public) — auto-builds PMGet/PMSet inner graph
#include "NiagaraDataInterface2DArrayTexture.h" // UNiagaraDataInterface2DArrayTexture — handles RT2DArray
#include "NiagaraEditorUtilities.h"

// Niagara runtime (renderers, collections, simulation stages)
#include "NiagaraRendererProperties.h"
#include "NiagaraDataInterface.h"        // UNiagaraDataInterface — needed for DI type support in add_system_user_parameter
#include "NiagaraSimulationStageBase.h"  // UNiagaraSimulationStageBase — TODO-38: GPU simulation stage support

// Graph / package
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"        // UEdGraphSchema_Niagara::TypeDefinitionToPinType — for bind_module_input
#include "NiagaraScriptVariable.h"        // UNiagaraScriptVariable::Metadata.GetVariableGuid()
#include "Misc/PackageName.h"
#include "Algo/Reverse.h"

// ============================================================
//  Shared helpers
// ============================================================

UNiagaraSystem* FMCPTool_NiagaraModify::LoadNiagaraSystem(const FString& SystemPath)
{
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		// Try with explicit object-name suffix (e.g. "/Game/Foo/Bar.Bar")
		const FString FullPath = SystemPath + TEXT(".") + FPackageName::GetShortName(SystemPath);
		System = LoadObject<UNiagaraSystem>(nullptr, *FullPath);
	}
	return System;
}

FVersionedNiagaraEmitterData* FMCPTool_NiagaraModify::GetEmitterDataByName(
	UNiagaraSystem* System, const FString& EmitterName, int32& OutHandleIndex)
{
	OutHandleIndex = INDEX_NONE;
	if (!System) return nullptr;

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		if (!Handle.IsValid()) continue;

		// In UE 5.7, GetEmitterData() returns FVersionedNiagaraEmitterData* directly.
		// Match only by the handle display name (what list_emitters reports as "handle_name").
		if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
		{
			FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data)
			{
				OutHandleIndex = i;
				return Data;
			}
		}
	}
	return nullptr;
}

TArray<FString> FMCPTool_NiagaraModify::GetStageNames(FVersionedNiagaraEmitterData* Data)
{
	TArray<FString> Names;
	if (!Data) return Names;

	if (Data->EmitterSpawnScriptProps.Script)  Names.Add(TEXT("EmitterSpawn"));
	if (Data->EmitterUpdateScriptProps.Script) Names.Add(TEXT("EmitterUpdate"));
	if (Data->SpawnScriptProps.Script)         Names.Add(TEXT("ParticleSpawn"));
	if (Data->UpdateScriptProps.Script)        Names.Add(TEXT("ParticleUpdate"));

	const TArray<FNiagaraEventScriptProperties>& EventHandlers = Data->GetEventHandlers();
	for (int32 i = 0; i < EventHandlers.Num(); ++i)
	{
		if (EventHandlers[i].Script)
			Names.Add(FString::Printf(TEXT("EventHandler_%d"), i));
	}

	// TODO-38: Add GPU simulation stage names
	for (UNiagaraSimulationStageBase* SimStage : Data->GetSimulationStages())
	{
		if (SimStage && SimStage->Script)
			Names.Add(SimStage->SimulationStageName.ToString());
	}

	return Names;
}

UNiagaraScript* FMCPTool_NiagaraModify::GetStageScript(
	FVersionedNiagaraEmitterData* Data, const FString& StageName)
{
	if (!Data) return nullptr;
	const FString Lower = StageName.ToLower();

	if (Lower == TEXT("emitterspawn"))  return Data->EmitterSpawnScriptProps.Script;
	if (Lower == TEXT("emitterupdate")) return Data->EmitterUpdateScriptProps.Script;
	if (Lower == TEXT("particlespawn") || Lower == TEXT("spawn")) return Data->SpawnScriptProps.Script;
	if (Lower == TEXT("particleupdate") || Lower == TEXT("update")) return Data->UpdateScriptProps.Script;

	// EventHandler_N
	if (Lower.StartsWith(TEXT("eventhandler_")))
	{
		const int32 Index = FCString::Atoi(*StageName.Mid(13));
		const TArray<FNiagaraEventScriptProperties>& Handlers = Data->GetEventHandlers();
		if (Handlers.IsValidIndex(Index)) return Handlers[Index].Script;
	}

	// TODO-38: GPU simulation stages — search by SimulationStageName (case-insensitive)
	for (UNiagaraSimulationStageBase* SimStage : Data->GetSimulationStages())
	{
		if (SimStage && SimStage->SimulationStageName.ToString().Equals(StageName, ESearchCase::IgnoreCase))
			return SimStage->Script;
	}

	return nullptr;
}

// Bump a UNiagaraGraph's ChangeId so the compilation digest cache is invalidated.
// The engine does this via MarkGraphRequiresSynchronization (not exported); ChangeId is a
// private UPROPERTY, so we set it via reflection. Without this, HLSL/graph edits made by
// direct property writes compile against the STALE cached digest — the edit is in the asset
// but the compiler never sees it (observed 2026-06-10: three HLSL updates ignored by compile).
static void BumpGraphChangeId(UNiagaraGraph* Graph, const TCHAR* Reason)
{
	if (!Graph) return;
	if (FStructProperty* ChangeIdProp = FindFProperty<FStructProperty>(Graph->GetClass(), TEXT("ChangeId")))
	{
		FGuid* ChangeId = ChangeIdProp->ContainerPtrToValuePtr<FGuid>(Graph);
		*ChangeId = FGuid::NewGuid();
		UE_LOG(LogUnrealClaude, Log, TEXT("BumpGraphChangeId: %s (%s)"), *Graph->GetPathName(), Reason);
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("BumpGraphChangeId: ChangeId property not found on %s"), *Graph->GetPathName());
	}
}

// UNiagaraNodeWithDynamicPins keeps its "Add" pin (PinCategoryMisc / "DynamicAddPin") as the
// LAST pin of each direction; the engine converts the Add pin into new data pins and re-appends
// a fresh Add pin (RequestNewTypedPin — not exported, so we replicate). This ordering is load-
// bearing: UNiagaraNodeCustomHlsl::BuildParameterMapHistory (NiagaraNodeCustomHlsl.cpp:488)
// indexes Signature.Inputs[i] by raw pin index and asserts OOB if a data pin sits after the
// Add pin. UEdGraphNode::CreatePin appends at the end, so every pin we add must be moved back
// in front of the Add pin.
static void MovePinBeforeAddPin(UEdGraphNode* Node, UEdGraphPin* NewPin)
{
	if (!Node || !NewPin) return;
	static const FName AddPinSubCategory(TEXT("DynamicAddPin"));
	for (int32 i = 0; i < Node->Pins.Num(); ++i)
	{
		UEdGraphPin* P = Node->Pins[i];
		if (P && P != NewPin && P->Direction == NewPin->Direction &&
			P->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc &&
			P->PinType.PinSubCategory == AddPinSubCategory)
		{
			Node->Pins.Remove(NewPin);
			Node->Pins.Insert(NewPin, i);
			return;
		}
	}
}

// Registers a parameter in a graph's VariableToScriptVariable map — what the editor does via
// UNiagaraGraph::AddParameter (not exported). Without this the param is invisible to the
// Parameters panel and to metadata GUID lookups (GetAllMetaData).
static void RegisterGraphParameter(UNiagaraGraph* Graph, const FNiagaraVariable& Var)
{
	if (!Graph || Graph->GetAllMetaData().Contains(Var)) return;
	UNiagaraScriptVariable* ScriptVar = NewObject<UNiagaraScriptVariable>(Graph, NAME_None, RF_Transactional);
	ScriptVar->Init(Var, FNiagaraVariableMetaData());
	Graph->GetAllMetaData().Add(Var, ScriptVar);
}

UNiagaraGraph* FMCPTool_NiagaraModify::GetScriptGraph(UNiagaraScript* Script)
{
	if (!Script) return nullptr;
	// In UE 5.7, all stage scripts share the same UNiagaraScriptSource / NodeGraph.
	// GetLatestSource() is the reliable path (GetSource(FGuid()) may miss versioned data).
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Source)
		Source = Cast<UNiagaraScriptSource>(Script->GetSource(FGuid()));
	return Source ? Source->NodeGraph : nullptr;
}

// ── TODO-35 helpers ──────────────────────────────────────────────────────────

ENiagaraScriptUsage FMCPTool_NiagaraModify::StageNameToUsage(const FString& StageName)
{
	const FString Lower = StageName.ToLower();
	if (Lower == TEXT("emitterspawn"))                          return ENiagaraScriptUsage::EmitterSpawnScript;
	if (Lower == TEXT("emitterupdate"))                         return ENiagaraScriptUsage::EmitterUpdateScript;
	if (Lower == TEXT("particlespawn") || Lower == TEXT("spawn")) return ENiagaraScriptUsage::ParticleSpawnScript;
	if (Lower == TEXT("particleupdate") || Lower == TEXT("update")) return ENiagaraScriptUsage::ParticleUpdateScript;
	return ENiagaraScriptUsage::Module; // sentinel: unrecognized
}

UNiagaraNodeOutput* FMCPTool_NiagaraModify::GetStageOutputNode(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage)
{
	if (!Graph) return nullptr;
	TArray<UNiagaraNodeOutput*> OutputNodes;
	Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
	for (UNiagaraNodeOutput* Out : OutputNodes)
	{
		if (Out && Out->GetUsage() == Usage)
			return Out;
	}
	return nullptr;
}

void FMCPTool_NiagaraModify::CollectStageChainModules(
	UNiagaraNodeOutput* OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
{
	if (!OutputNode) return;

	// Walk backwards: OutputNode's param-map input → previous module's param-map output → ...
	UEdGraphPin* CurPin = FindParamMapPin(OutputNode, EGPD_Input);
	while (CurPin && CurPin->LinkedTo.Num() > 0)
	{
		UEdGraphPin* PrevOutPin = CurPin->LinkedTo[0];
		if (!PrevOutPin) break;
		UEdGraphNode* PrevNode = PrevOutPin->GetOwningNode();
		UNiagaraNodeFunctionCall* FuncCall = Cast<UNiagaraNodeFunctionCall>(PrevNode);
		if (!FuncCall) break; // hit the emitter/system input node
		OutModules.Add(FuncCall);
		CurPin = FindParamMapPin(FuncCall, EGPD_Input);
	}
	// OutModules is in reverse order (last module first); reverse to get logical order
	Algo::Reverse(OutModules);
}

// ── TODO-36 helper ──────────────────────────────────────────────────────────

int32 FMCPTool_NiagaraModify::CleanupOrphanedMapNodes(UNiagaraGraph* Graph)
{
	if (!Graph) return 0;
	int32 Destroyed = 0;

	// Collect candidates first (avoid modifying Graph->Nodes while iterating)
	TArray<UEdGraphNode*> ToDestroy;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!IsValid(Node)) continue;
		const FString ClassName = Node->GetClass()->GetName();
		if (!ClassName.Contains(TEXT("ParameterMapGet")) && !ClassName.Contains(TEXT("ParameterMapSet")))
			continue;
		// Orphaned = Source input pin exists and has no connections
		UEdGraphPin* SourcePin = Node->FindPin(TEXT("Source"), EGPD_Input);
		if (SourcePin && SourcePin->LinkedTo.Num() == 0)
			ToDestroy.Add(Node);
	}

	for (UEdGraphNode* Node : ToDestroy)
	{
		Node->BreakAllNodeLinks();
		Graph->RemoveNode(Node);
		++Destroyed;
	}

	UE_LOG(LogUnrealClaude, Log,
		TEXT("CleanupOrphanedMapNodes: destroyed %d orphaned ParameterMap node(s)"), Destroyed);
	return Destroyed;
}

UNiagaraNodeFunctionCall* FMCPTool_NiagaraModify::FindModuleByName(
	UNiagaraGraph* Graph, const FString& ModuleName)
{
	if (!Graph) return nullptr;

	TArray<UNiagaraNodeFunctionCall*> Nodes;
	Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Nodes);

	for (UNiagaraNodeFunctionCall* Node : Nodes)
	{
		if (!Node) continue;
		// Match by function name or node title (case-insensitive)
		if (Node->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
		{
			return Node;
		}
		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		if (Title.Equals(ModuleName, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphPin* FMCPTool_NiagaraModify::FindParamMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	if (!Node) return nullptr;
	// FNiagaraParameterMap is the struct type that flows between Niagara modules.
	// Match by PinSubCategoryObject == FNiagaraParameterMap::StaticStruct()
	UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction
			&& Pin->PinType.PinSubCategoryObject.Get() == MapStruct)
		{
			return Pin;
		}
	}

	// NI009: Fallback — match by struct name. Scratchpad module FunctionCall nodes (inline
	// FunctionScript, no asset path) may have map pins whose SubCategoryObject pointer differs
	// from the current GC-live struct pointer (e.g. after inline FunctionScript compilation).
	// Matching by name is safe: "NiagaraParameterMap" is unique to param-map pins.
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != Direction) continue;
		UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
		if (SubObj && SubObj->GetName().Equals(TEXT("NiagaraParameterMap"), ESearchCase::IgnoreCase))
			return Pin;
	}

	// Diagnostic: log pin layout on failure to aid debugging (TODO-42)
	UE_LOG(LogUnrealClaude, Verbose,
		TEXT("FindParamMapPin: no ParameterMap pin found on %s (dir=%d, %d pins, MapStruct=%s)"),
		*Node->GetClass()->GetName(), (int32)Direction, Node->Pins.Num(),
		MapStruct ? *MapStruct->GetName() : TEXT("null"));
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
		UE_LOG(LogUnrealClaude, Verbose,
			TEXT("  pin '%s' dir=%d cat='%s' subcat='%s' subcatobj=%s"),
			*Pin->PinName.ToString(), (int32)Pin->Direction,
			*Pin->PinType.PinCategory.ToString(),
			*Pin->PinType.PinSubCategory.ToString(),
			SubObj ? *SubObj->GetName() : TEXT("null"));
	}
	return nullptr;
}

// ============================================================
//  GetInfo
// ============================================================

FMCPToolInfo FMCPTool_NiagaraModify::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("niagara_modify");
	Info.Description = TEXT(
		"Modify a NiagaraSystem asset: inspect emitters/stages/modules and add, remove,\n"
		"or configure modules in a stage's script graph.\n\n"
		"Operations:\n"
		"  list_emitters         — list emitter handles (name, enabled, asset path)\n"
		"  list_stages           — list script stages for a named emitter\n"
		"  list_modules          — list function-call modules inside a stage graph\n"
		"  add_module            — insert a module from a Niagara module script asset path\n"
		"  remove_module         — remove a named module and break all its connections\n"
		"  set_module_input      — set a default-value override on a module's input pin\n"
		"  set_system_user_param — set the default value of a user-exposed parameter in the asset\n"
		"  compile               — force-recompile the system and mark it dirty\n\n"
		"Stage names accepted by list_stages / list_modules / add_module / remove_module:\n"
		"  EmitterSpawn, EmitterUpdate, ParticleSpawn (or Spawn), ParticleUpdate (or Update),\n"
		"  EventHandler_0, EventHandler_1, ...\n\n"
		"Example (list stages):\n"
		"  { \"operation\": \"list_stages\",\n"
		"    \"system_path\": \"/Game/OceanWater/Particles/FX_Syst_Readback_iFFT\",\n"
		"    \"emitter_name\": \"FX_ReadbackEmitter_01\" }\n\n"
		"Example (add module):\n"
		"  { \"operation\": \"add_module\",\n"
		"    \"system_path\": \"/Game/OceanWater/Particles/FX_Syst_Readback_iFFT\",\n"
		"    \"emitter_name\": \"FX_ReadbackEmitter_01\",\n"
		"    \"stage\": \"ParticleUpdate\",\n"
		"    \"module_path\": \"/Niagara/Modules/Update/Location/CurlNoiseForce\" }\n\n"
		"Example (set module input):\n"
		"  { \"operation\": \"set_module_input\",\n"
		"    \"system_path\": \"/Game/OceanWater/Particles/FX_Syst_Readback_iFFT\",\n"
		"    \"emitter_name\": \"FX_ReadbackEmitter_01\",\n"
		"    \"stage\": \"ParticleUpdate\",\n"
		"    \"module_name\": \"CurlNoiseForce\",\n"
		"    \"input_name\": \"NoisePanSpeed\",\n"
		"    \"value\": \"1.0\" }\n\n"
		"Example (set_system_user_param — float):\n"
		"  { \"operation\": \"set_system_user_param\",\n"
		"    \"system_path\": \"/Game/Blueprints/ShallowWater/FX_ShallowWater\",\n"
		"    \"param_name\": \"User.WaterDepth\", \"value\": 25.0 }\n\n"
		"Example (set_system_user_param — vec2, e.g. WorldGridSize):\n"
		"  { \"operation\": \"set_system_user_param\",\n"
		"    \"system_path\": \"/Game/Blueprints/ShallowWater/FX_ShallowWater\",\n"
		"    \"param_name\": \"User.WorldGridSize\", \"value\": {\"X\": 1000.0, \"Y\": 500.0} }\n\n"
		"Example (set_system_user_param — vec3):\n"
		"  { \"operation\": \"set_system_user_param\",\n"
		"    \"system_path\": \"/Game/Blueprints/ShallowWater/FX_ShallowWater\",\n"
		"    \"param_name\": \"User.GravityDir\", \"value\": {\"X\": 0.0, \"Y\": 0.0, \"Z\": -1.0} }\n\n"
		"Example (set_system_user_param — UObject ref, NI003):\n"
		"  { \"operation\": \"set_system_user_param\",\n"
		"    \"system_path\": \"/Game/OceanWater/Particles/FX_Syst_Readback_iFFT\",\n"
		"    \"param_name\": \"User.iFFT_RT\",\n"
		"    \"value\": \"/Game/OceanWater/RenderTargets/Vertex/RT_OceanWater_VertAttribs\" }"
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("Operation: list_emitters | list_stages | list_modules | add_module | "
			 "remove_module | set_module_input | set_system_user_param | compile | "
			 "get_module_source | get_emitter_properties | add_system_user_parameter | "
			 "remove_system_user_parameter | create_scratchpad_module | set_module_hlsl | "
			 "create_assignment_module | rebuild_scratchpad_inner_graph | reorder_module | "
			 "bind_module_input | configure_di_parameter | add_scratchpad_module_param | dump_stage_graph"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("system_path"), TEXT("string"),
		TEXT("Asset path to the NiagaraSystem, e.g. /Game/Particles/FX_MySystem"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("emitter_name"), TEXT("string"),
		TEXT("Emitter handle display name or asset name (required for stage/module ops)"), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("stage"), TEXT("string"),
		TEXT("Stage name: EmitterSpawn | EmitterUpdate | ParticleSpawn | ParticleUpdate | "
			 "EventHandler_N | <GPU simulation stage name> (required for module ops). "
			 "GPU sim stages (e.g. \"Sample And Export Wave\") are matched by UsageId."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("module_path"), TEXT("string"),
		TEXT("Asset path to the Niagara module script (required for add_module)"), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("module_name"), TEXT("string"),
		TEXT("Module function name or node title (required for remove_module, set_module_input)"), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("input_name"), TEXT("string"),
		TEXT("Input pin name on the module (required for set_module_input)"), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("value"), TEXT("string or number"),
		TEXT("Default value string for the input pin (required for set_module_input). "
			 "Float: \"1.0\"; Vector: \"(X=0.0,Y=0.0,Z=1.0)\"; Bool: \"true\". "
			 "For set_system_user_param: any JSON value (number, bool, or {X,Y,Z}/{R,G,B,A} object)."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("param_name"), TEXT("string"),
		TEXT("User parameter name for set_system_user_param, e.g. \"User.WaterDepth\""), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("param_type"), TEXT("string"),
		TEXT("Type hint for set_system_user_param: \"float\"|\"int\"|\"bool\"|\"vec2\"|\"vec3\"|\"color\". "
			 "Auto-detected from the system's exposed parameter store if omitted. "
			 "UObject ref params (added via add_system_user_parameter) are also settable — "
			 "pass an asset path string as value (e.g. '/Game/.../RT_OceanWater_VertAttribs')."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("hlsl"), TEXT("string"),
		TEXT("HLSL body for create_scratchpad_module / set_module_hlsl operations."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("pos_x"), TEXT("number"),
		TEXT("Node X position in stage graph (optional). Default: auto before OutputNode."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("pos_y"), TEXT("number"),
		TEXT("Node Y position in stage graph (optional). Default: 0."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("writes"), TEXT("array"),
		TEXT("create_assignment_module: array of { \"param\": \"Particles.SpriteSize\", \"type\": \"vec2\", \"default\": \"\" } "
			 "entries to write. Types: float | vec2 | vec3 | vec4 | int | bool."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("input_param"), TEXT("string"),
		TEXT("bind_module_input: full Module.* parameter name to bind, "
			 "e.g. \"Module.Float To Send (As Struct Size)\". Required for bind_module_input."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("bind_to"), TEXT("string"),
		TEXT("bind_module_input: full Niagara attribute name to bind the input to, "
			 "e.g. \"Particles.WaveHeight\". Required for bind_module_input."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("bind_type"), TEXT("string"),
		TEXT("bind_module_input: optional type hint: float | vec2 | vec3 | vec4 | int | bool. "
			 "Auto-detected from the parameter store if omitted."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("inputs"), TEXT("array"),
		TEXT("rebuild_scratchpad_inner_graph: IGNORED. Use add_scratchpad_module_param instead."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("outputs"), TEXT("array"),
		TEXT("rebuild_scratchpad_inner_graph: IGNORED. Use add_scratchpad_module_param instead."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("direction"), TEXT("string"),
		TEXT("add_scratchpad_module_param: 'input' or 'output'. "
			 "input  — adds a typed Map Get node in the module inner graph; "
			 "output — adds a typed Map Set node. "
			 "PREFERRED: pass a dotted param_name (e.g. \"Particles.Position\", \"User.iFFT_RT\", "
			 "\"Particles.WaveHeight\") for DIRECT attribute access — the Map Get/Set reads/writes "
			 "that exact stage attribute and NO stack-level bind_module_input is needed. "
			 "The HLSL variable is the leaf name (\"Position\", \"WaveHeight\"). "
			 "Undotted names create the classic Module.<name> interface requiring a stack binding. "
			 "After adding all params, call rebuild_scratchpad_inner_graph to set the HLSL body."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("inherit_user_param_settings"), TEXT("bool"),
		TEXT("configure_di_parameter (TODO-53): set bInheritUserParameterSettings on the DI instance. "
			 "When true, the DI reads RT size/format from the bound user parameter instead of its own settings. "
			 "Required for User.iFFT_RT to read from RT_OceanWater_VertAttribs."), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("user_param_binding"), TEXT("string"),
		TEXT("configure_di_parameter (TODO-53): full Niagara parameter name to bind to the DI's "
			 "RenderTargetUserParameter (or analogous binding field), e.g. 'User.RT_VertAttribs'. "
			 "Tells the DI which user parameter holds the external RT asset reference at runtime."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

// ============================================================
//  Execute dispatcher
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
		return Error.GetValue();

	const FString Lower = Operation.ToLower();

	if (Lower == TEXT("list_emitters"))              return ExecuteListEmitters(Params);
	if (Lower == TEXT("list_stages"))                return ExecuteListStages(Params);
	if (Lower == TEXT("list_modules"))               return ExecuteListModules(Params);
	if (Lower == TEXT("add_module"))                 return ExecuteAddModule(Params);
	if (Lower == TEXT("remove_module"))              return ExecuteRemoveModule(Params);
	if (Lower == TEXT("set_module_input"))           return ExecuteSetModuleInput(Params);
	if (Lower == TEXT("set_system_user_param"))      return ExecuteSetSystemUserParam(Params);
	if (Lower == TEXT("compile"))                    return ExecuteCompile(Params);
	if (Lower == TEXT("get_module_source"))          return ExecuteGetModuleSource(Params);
	if (Lower == TEXT("get_emitter_properties"))     return ExecuteGetEmitterProperties(Params);
	if (Lower == TEXT("add_system_user_parameter"))  return ExecuteAddSystemUserParameter(Params);
	if (Lower == TEXT("remove_system_user_parameter")) return ExecuteRemoveSystemUserParameter(Params);
	if (Lower == TEXT("create_scratchpad_module"))   return ExecuteCreateScratchpadModule(Params);
	if (Lower == TEXT("set_module_hlsl"))            return ExecuteSetModuleHlsl(Params);
	if (Lower == TEXT("create_assignment_module"))       return ExecuteCreateAssignmentModule(Params);
	if (Lower == TEXT("rebuild_scratchpad_inner_graph")) return ExecuteRebuildScratchpadInnerGraph(Params);
	if (Lower == TEXT("reorder_module"))                 return ExecuteReorderModule(Params);
	if (Lower == TEXT("bind_module_input"))              return ExecuteBindModuleInput(Params);
	if (Lower == TEXT("configure_di_parameter"))         return ExecuteConfigureDIParameter(Params);
	if (Lower == TEXT("add_scratchpad_module_param"))    return ExecuteAddScratchpadModuleParam(Params);
	if (Lower == TEXT("dump_stage_graph"))               return ExecuteDumpStageGraph(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation '%s'. Valid: list_emitters, list_stages, list_modules, "
			 "add_module, remove_module, set_module_input, set_system_user_param, compile, "
			 "get_module_source, get_emitter_properties, add_system_user_parameter, "
			 "remove_system_user_parameter, create_scratchpad_module, set_module_hlsl, "
			 "create_assignment_module, rebuild_scratchpad_inner_graph, reorder_module, "
			 "bind_module_input, configure_di_parameter, add_scratchpad_module_param, dump_stage_graph"), *Operation));
}

// ============================================================
//  list_emitters
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteListEmitters(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                          return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmittersArray;

	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		if (!Handle.IsValid()) continue;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		Obj->SetStringField(TEXT("handle_name"), Handle.GetName().ToString());
		Obj->SetBoolField(TEXT("is_enabled"), Handle.GetIsEnabled());

		// In UE 5.7, GetEmitterData() returns FVersionedNiagaraEmitterData* directly
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			TArray<FString> Stages = GetStageNames(Data);
			Obj->SetArrayField(TEXT("stages"), StringArrayToJsonArray(Stages));
		}

		EmittersArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("system_name"), System->GetName());
	Result->SetNumberField(TEXT("emitter_count"), EmittersArray.Num());
	Result->SetArrayField(TEXT("emitters"), EmittersArray);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("NiagaraSystem '%s' has %d emitter(s)"),
			*System->GetName(), EmittersArray.Num()), Result);
}

// ============================================================
//  list_stages
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteListStages(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName,  Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                             return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found in NiagaraSystem '%s'. "
				 "Use list_emitters to see available emitter names."),
			*EmitterName, *System->GetName()));
	}

	// Collect standard stage names for type-labelling
	TSet<FString> StandardStageNames = {
		TEXT("EmitterSpawn"), TEXT("EmitterUpdate"), TEXT("ParticleSpawn"), TEXT("ParticleUpdate")
	};

	TArray<FString> StageNames = GetStageNames(Data);
	TArray<TSharedPtr<FJsonValue>> StagesArray;
	for (const FString& StageName : StageNames)
	{
		UNiagaraScript* Script = GetStageScript(Data, StageName);
		UNiagaraGraph* Graph = GetScriptGraph(Script);

		// Determine stage type
		const bool bIsSimStage = !StandardStageNames.Contains(StageName) &&
			!StageName.StartsWith(TEXT("EventHandler_"));
		const FString StageType = bIsSimStage ? TEXT("SimulationStage") :
			(StageName.StartsWith(TEXT("EventHandler_")) ? TEXT("EventHandler") : TEXT("Standard"));

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage"), StageName);
		StageObj->SetStringField(TEXT("type"), StageType);
		StageObj->SetBoolField(TEXT("has_graph"), Graph != nullptr);

		if (Graph)
		{
			// Use chain-walker for accurate per-stage count (TODO-35 fix)
			const ENiagaraScriptUsage Usage = StageNameToUsage(StageName);
			TArray<UNiagaraNodeFunctionCall*> ChainModules;
			if (Usage != ENiagaraScriptUsage::Module)
			{
				UNiagaraNodeOutput* OutNode = GetStageOutputNode(Graph, Usage);
				if (OutNode) CollectStageChainModules(OutNode, ChainModules);
			}
			else
			{
				// Simulation stage: find its output node by iterating all UNiagaraNodeOutput nodes
				// whose ScriptUsage == ParticleSimulationStageScript
				TArray<UNiagaraNodeOutput*> AllOutputs;
				Graph->GetNodesOfClass<UNiagaraNodeOutput>(AllOutputs);
				for (UNiagaraNodeOutput* Out : AllOutputs)
				{
					if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
					{
						// Match by usage ID to the sim stage's script usage ID
						if (Script && Out->GetUsageId() == Script->GetUsageId())
						{
							CollectStageChainModules(Out, ChainModules);
							break;
						}
					}
				}
				// Fallback if no match: count all sim-stage output nodes' chains
				if (ChainModules.Num() == 0)
				{
					for (UNiagaraNodeOutput* Out : AllOutputs)
					{
						if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
						{
							CollectStageChainModules(Out, ChainModules);
							break; // take first sim stage for now
						}
					}
				}
			}
			StageObj->SetNumberField(TEXT("module_count"), ChainModules.Num());
		}

		StagesArray.Add(MakeShared<FJsonValueObject>(StageObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetNumberField(TEXT("stage_count"), StagesArray.Num());
	Result->SetArrayField(TEXT("stages"), StagesArray);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Emitter '%s' has %d stage(s)"), *EmitterName, StagesArray.Num()), Result);
}

// ============================================================
//  list_modules
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteListModules(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,    Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                             return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));
	}

	UNiagaraScript* Script = GetStageScript(Data, StageName);
	if (!Script)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' not found or has no script. "
				 "Valid stages: %s"),
			*StageName, *FString::Join(GetStageNames(Data), TEXT(", "))));
	}

	UNiagaraGraph* Graph = GetScriptGraph(Script);
	if (!Graph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not get graph for stage '%s' (script source is not a UNiagaraScriptSource)."),
			*StageName));
	}

	// TODO-35/38: Walk only the chain that terminates at this stage's output node.
	// All stage scripts share the same source graph; filtering by UNiagaraNodeOutput usage
	// ensures we return only modules in the requested stage.
	const ENiagaraScriptUsage StageUsage = StageNameToUsage(StageName);
	TArray<UNiagaraNodeFunctionCall*> FuncNodes;
	if (StageUsage != ENiagaraScriptUsage::Module)
	{
		// Standard stage (EmitterSpawn/Update, ParticleSpawn/Update)
		UNiagaraNodeOutput* OutputNode = GetStageOutputNode(Graph, StageUsage);
		if (OutputNode)
			CollectStageChainModules(OutputNode, FuncNodes);
		else
			Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FuncNodes); // fallback
	}
	else
	{
		// GPU simulation stage — find the UNiagaraNodeOutput whose UsageId matches this script.
		// NI009: In GPU sim stages, scratchpad module FunctionCall nodes are NOT wired in a
		// linear param-map chain. Each connects independently to the stage input, so
		// CollectStageChainModules (param-map walk) only finds asset-based modules that happen
		// to be at the tail of the chain. We therefore do a full BFS through ALL input edges
		// from the stage output node after the chain walk, collecting any reachable FunctionCall.
		UNiagaraNodeOutput* SimStageOut = nullptr;
		TArray<UNiagaraNodeOutput*> AllOutputs;
		Graph->GetNodesOfClass<UNiagaraNodeOutput>(AllOutputs);
		for (UNiagaraNodeOutput* Out : AllOutputs)
		{
			if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript
				&& Out->GetUsageId() == Script->GetUsageId())
			{
				SimStageOut = Out;
				break;
			}
		}
		if (!SimStageOut)
		{
			// Fallback: take first ParticleSimulationStageScript output
			for (UNiagaraNodeOutput* Out : AllOutputs)
			{
				if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
				{
					SimStageOut = Out;
					break;
				}
			}
		}

		if (SimStageOut)
		{
			// BFS through ALL input-pin edges from the stage output node.
			// This finds both chained (param-map) and independently-connected modules
			// (scratchpad FunctionCall nodes wired directly to the stage input node).
			TSet<UEdGraphNode*> Visited;
			TQueue<UEdGraphNode*> Queue;
			Queue.Enqueue(SimStageOut);
			Visited.Add(SimStageOut);

			while (!Queue.IsEmpty())
			{
				UEdGraphNode* Current = nullptr;
				Queue.Dequeue(Current);
				if (!Current) continue;

				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input) continue;
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (!Linked) continue;
						UEdGraphNode* Pred = Linked->GetOwningNode();
						if (!Pred || Visited.Contains(Pred)) continue;
						Visited.Add(Pred);

						UNiagaraNodeFunctionCall* FuncCall = Cast<UNiagaraNodeFunctionCall>(Pred);
						if (FuncCall)
							FuncNodes.Add(FuncCall);

						// Keep traversing back — scratchpad modules may have PMGet/PMSet
						// or other intermediate nodes before the stage input
						Queue.Enqueue(Pred);
					}
				}
			}
			// Reverse so output is in logical execution order (first module first)
			Algo::Reverse(FuncNodes);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ModulesArray;
	for (int32 i = 0; i < FuncNodes.Num(); ++i)
	{
		UNiagaraNodeFunctionCall* Node = FuncNodes[i];
		if (!Node) continue;

		TSharedPtr<FJsonObject> Mod = MakeShared<FJsonObject>();
		Mod->SetNumberField(TEXT("index"), i);
		Mod->SetStringField(TEXT("function_name"), Node->GetFunctionName());
		Mod->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		Mod->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		Mod->SetNumberField(TEXT("pos_y"), Node->NodePosY);

		// Collect input pin names (skip the parameter map wire pin)
		UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input
				&& Pin->PinType.PinSubCategoryObject.Get() != MapStruct)
			{
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), Pin->GetName());
				PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
				Inputs.Add(MakeShared<FJsonValueObject>(PinObj));
			}
		}
		Mod->SetArrayField(TEXT("inputs"), Inputs);

		// Script asset reference (FName in UE 5.7)
		if (!Node->FunctionScriptAssetObjectPath.IsNone())
		{
			Mod->SetStringField(TEXT("script_path"), Node->FunctionScriptAssetObjectPath.ToString());
		}

		ModulesArray.Add(MakeShared<FJsonValueObject>(Mod));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetNumberField(TEXT("module_count"), ModulesArray.Num());
	Result->SetArrayField(TEXT("modules"), ModulesArray);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Stage '%s' has %d module(s)"), *StageName, ModulesArray.Num()), Result);
}

// ============================================================
//  add_module
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteAddModule(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModulePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,    Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_path"),  ModulePath,   Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                             return Error.GetValue();
	if (!ValidateBlueprintPathParam(ModulePath, Error))                             return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));
	}

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' not found or has no script."), *StageName));
	}

	UNiagaraGraph* Graph = GetScriptGraph(StageScript);
	if (!Graph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not access script graph for stage '%s'."), *StageName));
	}

	// Load the module script asset
	UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModulePath);
	if (!ModuleScript)
	{
		const FString FullPath = ModulePath + TEXT(".") + FPackageName::GetShortName(ModulePath);
		ModuleScript = LoadObject<UNiagaraScript>(nullptr, *FullPath);
	}
	if (!ModuleScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraScript (module) at '%s'. "
				 "Confirm the path is correct and the asset is a Niagara module script."),
			*ModulePath));
	}

	// Find the output node for THIS stage (TODO-35: graph is shared across all stages).
	const ENiagaraScriptUsage AddStageUsage = StageNameToUsage(StageName);
	UNiagaraNodeOutput* OutputNode = (AddStageUsage != ENiagaraScriptUsage::Module)
		? GetStageOutputNode(Graph, AddStageUsage)
		: nullptr;
	if (!OutputNode)
	{
		// Fallback: first output node (legacy behaviour for unrecognized stage names)
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		OutputNode = OutputNodes.Num() > 0 ? OutputNodes[0] : nullptr;
	}
	if (!OutputNode)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' graph has no UNiagaraNodeOutput node — cannot insert module."),
			*StageName));
	}

	// Find the parameter-map input pin on the output node
	UEdGraphPin* OutputMapInPin = FindParamMapPin(OutputNode, EGPD_Input);
	if (!OutputMapInPin)
	{
		return FMCPToolResult::Error(TEXT(
			"Could not locate parameter-map input pin on the output node. "
			"This may indicate the Niagara graph is using a different pin type format."));
	}

	// Discover what currently drives the output's map input (the "last" module in the chain)
	UEdGraphPin* PrevModuleOutPin = nullptr;
	if (OutputMapInPin->LinkedTo.Num() > 0)
	{
		PrevModuleOutPin = OutputMapInPin->LinkedTo[0];
	}

	// Create the new function call node
	UNiagaraNodeFunctionCall* NewNode = NewObject<UNiagaraNodeFunctionCall>(Graph);

	// Position it just before the output node
	NewNode->NodePosX = OutputNode->NodePosX - 300;
	NewNode->NodePosY = OutputNode->NodePosY;

	Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

	// Set the script reference and allocate pins.
	// InitializeFromAsset does not exist in UE 5.7 — set fields directly.
	NewNode->FunctionScript = ModuleScript;
	NewNode->FunctionScriptAssetObjectPath = FName(*ModuleScript->GetPathName());
	NewNode->AllocateDefaultPins();

	// Rewire: [previous last module] → [new node] → [output node]
	UEdGraphPin* NewMapInPin  = FindParamMapPin(NewNode, EGPD_Input);
	UEdGraphPin* NewMapOutPin = FindParamMapPin(NewNode, EGPD_Output);

	FString WiringReport = TEXT("module inserted without parameter-map wiring");

	if (NewMapInPin && NewMapOutPin)
	{
		// Connect previous module's output → new node's map input
		if (PrevModuleOutPin)
		{
			OutputMapInPin->BreakLinkTo(PrevModuleOutPin);
			NewMapInPin->MakeLinkTo(PrevModuleOutPin);
		}
		// Connect new node's map output → output node's map input
		NewMapOutPin->MakeLinkTo(OutputMapInPin);
		WiringReport = TEXT("parameter-map wire connected");
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning,
			TEXT("niagara_modify add_module: could not find parameter-map pins on new node '%s'. "
				 "Module was added to the graph but is not connected — compile may still work if "
				 "Niagara resolves parameter names implicitly."),
			*ModuleScript->GetName());
	}

	// Mark dirty and request recompile
	Graph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetStringField(TEXT("module_added"), ModuleScript->GetName());
	Result->SetStringField(TEXT("wiring"), WiringReport);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added module '%s' to stage '%s' (%s); system recompile requested."),
			*ModuleScript->GetName(), *StageName, *WiringReport), Result);
}

// ============================================================
//  remove_module
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteRemoveModule(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));
	}

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' not found or has no script."), *StageName));
	}

	UNiagaraGraph* Graph = GetScriptGraph(StageScript);
	if (!Graph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not access script graph for stage '%s'."), *StageName));
	}

	UNiagaraNodeFunctionCall* TargetNode = FindModuleByName(Graph, ModuleName);
	if (!TargetNode)
	{
		// List available modules to help
		TArray<UNiagaraNodeFunctionCall*> AllNodes;
		Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllNodes);
		TArray<FString> Names;
		for (auto* N : AllNodes)
		{
			if (N) Names.Add(N->GetFunctionName());
		}
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' not found in stage '%s'. Available modules: %s"),
			*ModuleName, *StageName, *FString::Join(Names, TEXT(", "))));
	}

	// Attempt to bridge the parameter-map chain before removing:
	// If PrevOut → TargetIn and TargetOut → NextIn, reconnect PrevOut → NextIn.
	UEdGraphPin* TargetMapIn  = FindParamMapPin(TargetNode, EGPD_Input);
	UEdGraphPin* TargetMapOut = FindParamMapPin(TargetNode, EGPD_Output);

	UEdGraphPin* PrevOut = (TargetMapIn && TargetMapIn->LinkedTo.Num() > 0)
		? TargetMapIn->LinkedTo[0] : nullptr;
	UEdGraphPin* NextIn  = (TargetMapOut && TargetMapOut->LinkedTo.Num() > 0)
		? TargetMapOut->LinkedTo[0] : nullptr;

	if (PrevOut && NextIn)
	{
		PrevOut->BreakLinkTo(NextIn);    // remove old connection from previous to next
		PrevOut->MakeLinkTo(NextIn);     // bridge: previous → next (skipping the removed node)
	}

	// Break all remaining connections and remove the node
	const FString RemovedName = TargetNode->GetFunctionName();

	// TODO-48: Remove orphaned FunctionScript from ScratchPadScripts before removing the node.
	// Each create_scratchpad_module cycle adds one UNiagaraScript to ScratchPadScripts.
	// Without this cleanup, orphaned scripts accumulate — up to 8× duplicates were observed.
	UNiagaraScript* OwnedScript = TargetNode->FunctionScript;
	bool bScriptRemoved = false;
	if (OwnedScript && System->ScratchPadScripts.Contains(OwnedScript))
	{
		System->ScratchPadScripts.Remove(OwnedScript);
		bScriptRemoved = true;
		UE_LOG(LogUnrealClaude, Log,
			TEXT("remove_module (TODO-48): removed '%s' from ScratchPadScripts"),
			*OwnedScript->GetName());

		// NI007: Move the script to the transient package so its FName is freed under System's outer.
		// Without this, NewObject(..., System, FName(*SameName)) produces a _0 suffix variant on the
		// next create_scratchpad_module call, causing the duplicate-name check to succeed (no match)
		// but the FunctionCall to reference a weirdly-named script.
		OwnedScript->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
	}

	TargetNode->BreakAllNodeLinks();
	Graph->RemoveNode(TargetNode);

	// TODO-36: Clean up orphaned ParameterMapGet/Set nodes left behind by the removal.
	// Each removed module can leave Map Get nodes whose Source pin is now disconnected;
	// these cause "Parameter Maps must be created via an Input Node" compile errors.
	const int32 OrphansDestroyed = CleanupOrphanedMapNodes(Graph);

	Graph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetStringField(TEXT("module_removed"), RemovedName);
	Result->SetNumberField(TEXT("orphaned_map_nodes_destroyed"), OrphansDestroyed);
	Result->SetBoolField  (TEXT("scratchpad_script_removed"), bScriptRemoved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed module '%s' from stage '%s'; %d orphaned map node(s) cleaned up; recompile requested."),
			*RemovedName, *StageName, OrphansDestroyed), Result);
}

// ============================================================
//  set_module_input
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteSetModuleInput(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, InputName, Value;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("input_name"),   InputName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("value"),        Value,       Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));
	}

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' not found or has no script."), *StageName));
	}

	UNiagaraGraph* Graph = GetScriptGraph(StageScript);
	if (!Graph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not access script graph for stage '%s'."), *StageName));
	}

	UNiagaraNodeFunctionCall* TargetNode = FindModuleByName(Graph, ModuleName);
	if (!TargetNode)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' not found in stage '%s'."), *ModuleName, *StageName));
	}

	// Find the named input pin (case-insensitive, skip parameter-map pin)
	UEdGraphPin* FoundPin = nullptr;
	UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
	for (UEdGraphPin* Pin : TargetNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinSubCategoryObject.Get() == MapStruct) continue;
		if (Pin->GetName().Equals(InputName, ESearchCase::IgnoreCase))
		{
			FoundPin = Pin;
			break;
		}
	}

	if (!FoundPin)
	{
		// List available inputs to help
		TArray<FString> AvailableInputs;
		for (UEdGraphPin* Pin : TargetNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input
				&& Pin->PinType.PinSubCategoryObject.Get() != MapStruct)
			{
				AvailableInputs.Add(Pin->GetName());
			}
		}
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Input pin '%s' not found on module '%s'. Available inputs: %s"),
			*InputName, *ModuleName, *FString::Join(AvailableInputs, TEXT(", "))));
	}

	// If the pin is linked, break the link so the default value takes effect
	if (FoundPin->LinkedTo.Num() > 0)
	{
		FoundPin->BreakAllPinLinks();
	}
	const FString OldValue = FoundPin->DefaultValue;
	FoundPin->DefaultValue = Value;

	Graph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetStringField(TEXT("module_name"), ModuleName);
	Result->SetStringField(TEXT("input_name"), FoundPin->GetName());
	Result->SetStringField(TEXT("old_value"), OldValue);
	Result->SetStringField(TEXT("new_value"), Value);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = \"%s\" (was \"%s\"); system recompile requested."),
			*ModuleName, *FoundPin->GetName(), *Value, *OldValue), Result);
}

// ============================================================
//  compile
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteCompile(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                          return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("system_name"), System->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Recompile requested for NiagaraSystem '%s'."), *System->GetName()), Result);
}

// ============================================================
//  set_system_user_param
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteSetSystemUserParam(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, ParamName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("param_name"),  ParamName,  Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                          return Error.GetValue();

	// value is required — any JSON type
	const TSharedPtr<FJsonValue>* ValueFieldPtr = Params->Values.Find(TEXT("value"));
	if (!ValueFieldPtr || !ValueFieldPtr->IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: value"));
	}
	const TSharedPtr<FJsonValue>& JsonVal = *ValueFieldPtr;

	// Optional explicit type hint ("float", "int", "bool", "vec3", "color")
	FString TypeHint = ExtractOptionalString(Params, TEXT("param_type"), TEXT("")).ToLower();

	// --- Load system ---
	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));
	}

	// --- Find the parameter in the exposed store to get its type ---
	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	TArray<FNiagaraVariable> Variables;
	Store.GetParameters(Variables);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& V : Variables)
	{
		if (V.GetName().ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			FoundVar = &V;
			break;
		}
	}

	// Build list of available params for error messages
	auto BuildParamList = [&]() -> FString
	{
		TArray<FString> Names;
		for (const FNiagaraVariable& V : Variables)
		{
			Names.Add(V.GetName().ToString());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none — use niagara_query to inspect)");
	};

	if (!FoundVar)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("User parameter '%s' not found in NiagaraSystem '%s'. "
				 "Available parameters: %s"),
			*ParamName, *System->GetName(), *BuildParamList()));
	}

	// --- Normalise: re-parse string values (happens when schema type is "any") ---
	TSharedPtr<FJsonValue> ReparsedVal;
	if (JsonVal->Type == EJson::String)
	{
		FString Str = JsonVal->AsString().TrimStartAndEnd();
		if (Str.StartsWith(TEXT("{")) || Str.StartsWith(TEXT("[")))
		{
			TSharedPtr<FJsonObject> ParsedObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
			if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
			{
				ReparsedVal = MakeShared<FJsonValueObject>(ParsedObj);
			}
		}
		else if (Str.ToLower() == TEXT("true"))  { ReparsedVal = MakeShared<FJsonValueBoolean>(true);  }
		else if (Str.ToLower() == TEXT("false")) { ReparsedVal = MakeShared<FJsonValueBoolean>(false); }
		else if (!Str.IsEmpty() && (FChar::IsDigit(Str[0]) || Str[0] == TEXT('-') || Str[0] == TEXT('.')))
		{
			ReparsedVal = MakeShared<FJsonValueNumber>(FCString::Atod(*Str));
		}
	}
	const TSharedPtr<FJsonValue>& ActualVal = ReparsedVal.IsValid() ? ReparsedVal : JsonVal;

	// --- Dispatch on type ---
	const FNiagaraTypeDefinition& TypeDef = FoundVar->GetType();
	FString TypeApplied;

	// FoundVar is the FNiagaraVariable retrieved directly from the store — use it as-is
	// for SetParameterValue so its type and name match exactly what the store expects.
	const FNiagaraVariable& StoredVar = *FoundVar;
	EJson ValKind = ActualVal->Type;

	// Float
	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef() || TypeHint == TEXT("float"))
	{
		double NumVal = 0.0;
		if (!ActualVal->TryGetNumber(NumVal))
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is float — value must be a JSON number."), *ParamName));
		}
		Store.SetParameterValue<float>((float)NumVal, StoredVar);
		TypeApplied = FString::Printf(TEXT("float = %.6f"), (float)NumVal);
	}
	// Int
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef() || TypeHint == TEXT("int"))
	{
		double NumVal = 0.0;
		if (!ActualVal->TryGetNumber(NumVal))
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is int — value must be a JSON number."), *ParamName));
		}
		Store.SetParameterValue<int32>((int32)NumVal, StoredVar);
		TypeApplied = FString::Printf(TEXT("int = %d"), (int32)NumVal);
	}
	// Bool
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef() || TypeHint == TEXT("bool"))
	{
		bool bVal = false;
		if (ValKind == EJson::Boolean)
		{
			bVal = ActualVal->AsBool();
		}
		else
		{
			double NumVal = 0.0;
			if (!ActualVal->TryGetNumber(NumVal))
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Parameter '%s' is bool — value must be JSON true/false or 0/1."), *ParamName));
			}
			bVal = (NumVal != 0.0);
		}
		FNiagaraBool NiagaraBool(bVal);
		Store.SetParameterValue<FNiagaraBool>(NiagaraBool, StoredVar);
		TypeApplied = FString::Printf(TEXT("bool = %s"), bVal ? TEXT("true") : TEXT("false"));
	}
	// Vec2 / Vector2f (e.g. WorldGridSize, BodyForce on shallow water systems)
	else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def()
		|| TypeHint == TEXT("vec2")
		|| TypeDef.GetName() == TEXT("Vector2f"))
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		double X = 0.0, Y = 0.0;
		if (ActualVal->TryGetObject(ObjPtr) && ObjPtr && (*ObjPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
			Obj->TryGetNumberField(TEXT("X"), X); Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("Y"), Y); Obj->TryGetNumberField(TEXT("y"), Y);
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is Vector2f — value must be a JSON object {\"X\":...,\"Y\":...}."),
				*ParamName));
		}
		Store.SetParameterValue<FVector2f>(FVector2f((float)X, (float)Y), StoredVar);
		TypeApplied = FString::Printf(TEXT("vec2 = (%.3f, %.3f)"), X, Y);
	}
	// Vec3
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def() || TypeHint == TEXT("vec3"))
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!ActualVal->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is vec3 — value must be a JSON object {\"X\":...,\"Y\":...,\"Z\":...}."),
				*ParamName));
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		double X = 0.0, Y = 0.0, Z = 0.0;
		Obj->TryGetNumberField(TEXT("X"), X); Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("Y"), Y); Obj->TryGetNumberField(TEXT("y"), Y);
		Obj->TryGetNumberField(TEXT("Z"), Z); Obj->TryGetNumberField(TEXT("z"), Z);
		// In UE5, Niagara vec3 is stored as FVector3f (single precision)
		Store.SetParameterValue<FVector3f>(FVector3f((float)X, (float)Y, (float)Z), StoredVar);
		TypeApplied = FString::Printf(TEXT("vec3 = (%.3f, %.3f, %.3f)"), X, Y, Z);
	}
	// Vec4 (stored as FVector4f)
	else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!ActualVal->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is vec4 — value must be {\"X\":...,\"Y\":...,\"Z\":...,\"W\":...}."),
				*ParamName));
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		double X = 0.0, Y = 0.0, Z = 0.0, W = 0.0;
		Obj->TryGetNumberField(TEXT("X"), X); Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("Y"), Y); Obj->TryGetNumberField(TEXT("y"), Y);
		Obj->TryGetNumberField(TEXT("Z"), Z); Obj->TryGetNumberField(TEXT("z"), Z);
		Obj->TryGetNumberField(TEXT("W"), W); Obj->TryGetNumberField(TEXT("w"), W);
		Store.SetParameterValue<FVector4f>(FVector4f((float)X, (float)Y, (float)Z, (float)W), StoredVar);
		TypeApplied = FString::Printf(TEXT("vec4 = (%.3f, %.3f, %.3f, %.3f)"), X, Y, Z, W);
	}
	// Color (stored as FLinearColor)
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef() || TypeHint == TEXT("color"))
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!ActualVal->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' is color — value must be {\"R\":...,\"G\":...,\"B\":...,\"A\":...}."),
				*ParamName));
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
		Obj->TryGetNumberField(TEXT("R"), R); Obj->TryGetNumberField(TEXT("r"), R);
		Obj->TryGetNumberField(TEXT("G"), G); Obj->TryGetNumberField(TEXT("g"), G);
		Obj->TryGetNumberField(TEXT("B"), B); Obj->TryGetNumberField(TEXT("b"), B);
		Obj->TryGetNumberField(TEXT("A"), A); Obj->TryGetNumberField(TEXT("a"), A);
		Store.SetParameterValue<FLinearColor>(FLinearColor((float)R, (float)G, (float)B, (float)A), StoredVar);
		TypeApplied = FString::Printf(TEXT("color = (R=%.3f, G=%.3f, B=%.3f, A=%.3f)"), R, G, B, A);
	}
	// NI003: UObject reference type (created via add_system_user_parameter with a UClass type
	// such as TextureRenderTarget, Texture2D, etc.). Value must be an asset path string.
	else if (!TypeDef.IsDataInterface() && TypeDef.GetClass() != nullptr)
	{
		// Allow explicit null assignment
		bool bSetNull = (ActualVal->Type == EJson::Null);
		FString ValStr;
		if (!bSetNull && ActualVal->TryGetString(ValStr))
			bSetNull = (ValStr.TrimStartAndEnd().IsEmpty() || ValStr.ToLower() == TEXT("null"));

		if (bSetNull)
		{
			Store.SetUObject(nullptr, StoredVar);
			TypeApplied = TEXT("UObject = null (cleared)");
		}
		else
		{
			FString AssetPath;
			if (!ActualVal->TryGetString(AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Parameter '%s' is a UObject ref — value must be an asset path string "
					     "(e.g. '/Game/OceanWater/RenderTargets/Vertex/RT_OceanWater_VertAttribs')."),
					*ParamName));

			// Try bare path first, then with explicit object-name suffix
			UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
			if (!Asset)
			{
				const FString FullPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
				Asset = LoadObject<UObject>(nullptr, *FullPath);
			}
			if (!Asset)
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Failed to load asset at '%s' for UObject ref parameter '%s'. "
					     "Ensure the full /Game/ path is correct."),
					*AssetPath, *ParamName));

			// Validate class compatibility
			UClass* ExpectedClass = TypeDef.GetClass();
			if (ExpectedClass && !Asset->IsA(ExpectedClass))
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Asset '%s' (class %s) is not compatible with parameter '%s' (expected %s)."),
					*AssetPath, *Asset->GetClass()->GetName(), *ParamName, *ExpectedClass->GetName()));
			}

			Store.SetUObject(Asset, StoredVar);
			TypeApplied = FString::Printf(TEXT("UObject = '%s' (%s)"),
				*Asset->GetPathName(), *Asset->GetClass()->GetName());
		}
	}
	else
	{
		// Data interface or unknown type — not settable via simple value
		FString TypeName = TypeDef.GetName();
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' has type '%s' which is not settable via set_system_user_param. "
				 "Settable types: float, int, bool, vec2, vec3, vec4, color, UObject asset path. "
				 "Data interface parameters (is_data_interface:true from niagara_query) must be "
				 "set in the editor."),
			*ParamName, *TypeName));
	}

	// Mark dirty — no full recompile needed for a default value change
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("system_name"), System->GetName());
	Result->SetStringField(TEXT("param_name"), ParamName);
	Result->SetStringField(TEXT("value_applied"), TypeApplied);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set user parameter '%s' on NiagaraSystem '%s': %s"),
			*ParamName, *System->GetName(), *TypeApplied), Result);
}

// ============================================================
//  NI006: Helper — resolve an EdGraphPin type to a human-readable string.
//  PinCategory.ToString() returns "struct" for all struct types (Vector, LinearColor,
//  NiagaraParameterMap, etc.) — useless for readback. This helper checks
//  PinSubCategoryObject->GetName() for struct/object categories so we return
//  "Vector", "Vector2D", "LinearColor", "NiagaraDataInterfaceRenderTarget2DArray", etc.
// ============================================================
static FString NiagaraPinTypeToString(const FEdGraphPinType& PinType)
{
	const FString Cat = PinType.PinCategory.ToString();
	// "struct"/"object"/"class" — Blueprint/standard UE types (e.g. FVector, UObject subclasses)
	// "Type" — Niagara-specific typed pins (NiagaraFloat, FVector/FNiagaraFloat3, DI types, etc.)
	if (Cat == TEXT("struct") || Cat == TEXT("object") || Cat == TEXT("class") || Cat == TEXT("Type"))
	{
		if (UObject* SubObj = PinType.PinSubCategoryObject.Get())
			return SubObj->GetName(); // e.g. "NiagaraFloat", "Vector", "NiagaraDataInterfaceRenderTarget2DArray"
		return Cat; // fallback if SubCategoryObject is null
	}
	return Cat; // "float", "int", "bool", "exec", "Misc", etc.
}

// ============================================================
//  get_module_source (TODO-25)
//  Read inline scratch-pad HLSL from a named module node.
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteGetModuleSource(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(FString::Printf(TEXT("Could not get graph for stage '%s'."), *StageName));

	UNiagaraNodeFunctionCall* ModuleNode = FindModuleByName(StageGraph, ModuleName);
	if (!ModuleNode)
	{
		TArray<UNiagaraNodeFunctionCall*> AllNodes;
		StageGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllNodes);
		TArray<FString> Names;
		for (auto* N : AllNodes) { if (N) Names.Add(N->GetFunctionName()); }
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' not found in stage '%s'. Available: %s"),
			*ModuleName, *StageName, *FString::Join(Names, TEXT(", "))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());

	const bool bIsInline = ModuleNode->FunctionScriptAssetObjectPath.IsNone();
	Result->SetBoolField(TEXT("is_inline"), bIsInline);

	if (!bIsInline)
	{
		Result->SetStringField(TEXT("script_path"), ModuleNode->FunctionScriptAssetObjectPath.ToString());
		return FMCPToolResult::Success(
			FString::Printf(TEXT("Module '%s' is asset-based (not inline). Script path returned."), *ModuleName), Result);
	}

	// Case: the module node IS a CustomHlsl node directly in the stage graph
	// (created by create_scratchpad_module — has no FunctionScript sub-graph)
	if (UNiagaraNodeCustomHlsl* DirectHlsl = Cast<UNiagaraNodeCustomHlsl>(ModuleNode))
	{
		FString HlslText;
		if (FStrProperty* Prop = FindFProperty<FStrProperty>(DirectHlsl->GetClass(), TEXT("CustomHlsl")))
			HlslText = *Prop->ContainerPtrToValuePtr<FString>(DirectHlsl);

		TSharedPtr<FJsonObject> HObj = MakeShared<FJsonObject>();
		HObj->SetStringField(TEXT("hlsl"), HlslText);

		UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
		TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
		for (UEdGraphPin* Pin : ModuleNode->Pins)
		{
			if (!Pin || Pin->PinType.PinSubCategoryObject.Get() == MapStruct) continue;
			TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Pin->GetName());
			PObj->SetStringField(TEXT("type"), NiagaraPinTypeToString(Pin->PinType)); // NI006
			if (Pin->Direction == EGPD_Input) Inputs.Add(MakeShared<FJsonValueObject>(PObj));
			else Outputs.Add(MakeShared<FJsonValueObject>(PObj));
		}
		HObj->SetArrayField(TEXT("inputs"),  Inputs);
		HObj->SetArrayField(TEXT("outputs"), Outputs);

		TArray<TSharedPtr<FJsonValue>> HlslNodesArr;
		HlslNodesArr.Add(MakeShared<FJsonValueObject>(HObj));
		Result->SetArrayField(TEXT("hlsl_nodes"),     HlslNodesArr);
		Result->SetArrayField(TEXT("function_calls"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("param_reads"),    TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("param_writes"),   TArray<TSharedPtr<FJsonValue>>());

		return FMCPToolResult::Success(
			FString::Printf(TEXT("Module '%s' is a direct CustomHlsl node: HLSL body returned."), *ModuleName), Result);
	}

	// Inline (scratch-pad) sub-script module — navigate to the inner script source graph
	if (!ModuleNode->FunctionScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' is inline but FunctionScript is null — cannot read source."), *ModuleName));
	}

	UNiagaraGraph* InnerGraph = GetScriptGraph(ModuleNode->FunctionScript);
	if (!InnerGraph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' is inline but inner script has no graph."), *ModuleName));
	}

	TArray<TSharedPtr<FJsonValue>> HlslNodes;
	TArray<TSharedPtr<FJsonValue>> FunctionCalls;
	TArray<TSharedPtr<FJsonValue>> ParamReads;
	TArray<TSharedPtr<FJsonValue>> ParamWrites;

	TArray<UEdGraphNode*> AllInnerNodes;
	InnerGraph->GetNodesOfClass<UEdGraphNode>(AllInnerNodes);

	for (UEdGraphNode* Node : AllInnerNodes)
	{
		if (!Node) continue;
		const FString ClassName = Node->GetClass()->GetName();

		if (UNiagaraNodeCustomHlsl* HlslNode = Cast<UNiagaraNodeCustomHlsl>(Node))
		{
			TSharedPtr<FJsonObject> HObj = MakeShared<FJsonObject>();
			FString HlslText;
			if (FStrProperty* Prop = FindFProperty<FStrProperty>(HlslNode->GetClass(), TEXT("CustomHlsl")))
				HlslText = *Prop->ContainerPtrToValuePtr<FString>(HlslNode);
			HObj->SetStringField(TEXT("hlsl"), HlslText);

			// Collect non-parameter-map input/output pins as declared signature
			UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
			TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->PinType.PinSubCategoryObject.Get() == MapStruct) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Pin->GetName());
				PObj->SetStringField(TEXT("type"), NiagaraPinTypeToString(Pin->PinType)); // NI006
				if (Pin->Direction == EGPD_Input) Inputs.Add(MakeShared<FJsonValueObject>(PObj));
				else Outputs.Add(MakeShared<FJsonValueObject>(PObj));
			}
			HObj->SetArrayField(TEXT("inputs"), Inputs);
			HObj->SetArrayField(TEXT("outputs"), Outputs);
			HlslNodes.Add(MakeShared<FJsonValueObject>(HObj));
		}
		else if (ClassName.Contains(TEXT("ParameterMapGet")))
		{
			UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				// Skip the parameter map pass-through pin; only report typed data outputs
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinSubCategoryObject.Get() == MapStruct) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("param"), Pin->GetName());
				PObj->SetStringField(TEXT("type"), NiagaraPinTypeToString(Pin->PinType)); // NI006
				ParamReads.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		else if (ClassName.Contains(TEXT("ParameterMapSet")))
		{
			UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinSubCategoryObject.Get() == MapStruct) continue;
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("param"), Pin->GetName());
				PObj->SetStringField(TEXT("type"), NiagaraPinTypeToString(Pin->PinType)); // NI006
				ParamWrites.Add(MakeShared<FJsonValueObject>(PObj));
			}
		}
		else if (UNiagaraNodeFunctionCall* FCall = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
			FObj->SetStringField(TEXT("function"), FCall->GetFunctionName());
			if (!FCall->FunctionScriptAssetObjectPath.IsNone())
				FObj->SetStringField(TEXT("script_path"), FCall->FunctionScriptAssetObjectPath.ToString());
			FunctionCalls.Add(MakeShared<FJsonValueObject>(FObj));
		}
	}

	Result->SetArrayField(TEXT("hlsl_nodes"),      HlslNodes);
	Result->SetArrayField(TEXT("function_calls"),  FunctionCalls);
	Result->SetArrayField(TEXT("param_reads"),     ParamReads);
	Result->SetArrayField(TEXT("param_writes"),    ParamWrites);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Module '%s' is inline: %d HLSL node(s), %d function call(s), %d param reads, %d param writes."),
			*ModuleName, HlslNodes.Num(), FunctionCalls.Num(), ParamReads.Num(), ParamWrites.Num()), Result);
}

// ============================================================
//  get_emitter_properties (TODO-27)
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteGetEmitterProperties(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));

	// Sim target
	FString SimTargetStr;
	switch (Data->SimTarget)
	{
		case ENiagaraSimTarget::CPUSim:        SimTargetStr = TEXT("CPU"); break;
		case ENiagaraSimTarget::GPUComputeSim: SimTargetStr = TEXT("GPU"); break;
		default:                               SimTargetStr = TEXT("Dynamic"); break;
	}

	// Renderers
	TArray<TSharedPtr<FJsonValue>> RendererArray;
	for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
	{
		if (!Renderer) continue;
		TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
		RObj->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
		RObj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
		RendererArray.Add(MakeShared<FJsonValueObject>(RObj));
	}

	// Spawn count — scan ALL EmitterSpawn modules for any input pin containing "SpawnCount"
	// (keyword-based module name matching was unreliable; pin name is more stable)
	FString SpawnCountInfo = TEXT("unknown");
	UNiagaraScript* SpawnScript = Data->EmitterSpawnScriptProps.Script;
	if (SpawnScript)
	{
		UNiagaraGraph* SpawnGraph = GetScriptGraph(SpawnScript);
		if (SpawnGraph)
		{
			TArray<UNiagaraNodeFunctionCall*> Nodes;
			SpawnGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Nodes);
			for (UNiagaraNodeFunctionCall* Node : Nodes)
			{
				if (!Node) continue;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input
						&& Pin->GetName().Contains(TEXT("SpawnCount"), ESearchCase::IgnoreCase))
					{
						SpawnCountInfo = FString::Printf(TEXT("%s (pin '%s' default: %s)"),
							*Node->GetFunctionName(), *Pin->GetName(), *Pin->DefaultValue);
						break;
					}
				}
				if (SpawnCountInfo != TEXT("unknown")) break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"),  SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("sim_target"),   SimTargetStr);
	Result->SetBoolField  (TEXT("is_local_space"), Data->bLocalSpace);
	Result->SetNumberField(TEXT("renderer_count"), RendererArray.Num());
	Result->SetArrayField (TEXT("renderers"),    RendererArray);
	Result->SetStringField(TEXT("spawn_count_info"), SpawnCountInfo);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Emitter '%s': sim=%s, local=%s, renderers=%d"),
			*EmitterName, *SimTargetStr,
			Data->bLocalSpace ? TEXT("true") : TEXT("false"),
			RendererArray.Num()), Result);
}

// ============================================================
//  Shared helper: build FNiagaraTypeDefinition from string
// ============================================================

static FNiagaraTypeDefinition BuildNiagaraTypeDef(const FString& TypeStr)
{
	const FString Lower = TypeStr.ToLower().TrimStartAndEnd();
	if (Lower == TEXT("float"))  return FNiagaraTypeDefinition::GetFloatDef();
	if (Lower == TEXT("int") || Lower == TEXT("int32")) return FNiagaraTypeDefinition::GetIntDef();
	if (Lower == TEXT("bool"))   return FNiagaraTypeDefinition::GetBoolDef();
	if (Lower == TEXT("vec2"))   return FNiagaraTypeDefinition::GetVec2Def();
	if (Lower == TEXT("vec3"))   return FNiagaraTypeDefinition::GetVec3Def();
	if (Lower == TEXT("vec4"))   return FNiagaraTypeDefinition::GetVec4Def();
	if (Lower == TEXT("color"))  return FNiagaraTypeDefinition::GetColorDef();

	// ── DI class resolution (TODO-34) ───────────────────────────────────────────────────────
	// Try "NiagaraDataInterface" + TypeStr prefix (e.g. "RenderTarget2DArray" → "NiagaraDataInterfaceRenderTarget2DArray")
	// then bare TypeStr (e.g. "NiagaraDataInterfaceRenderTargetArray" passed directly).
	const FString Trimmed = TypeStr.TrimStartAndEnd();
	UClass* DIClass = nullptr;
	if (!DIClass)
		DIClass = FindFirstObject<UClass>(*FString::Printf(TEXT("NiagaraDataInterface%s"), *Trimmed), EFindFirstObjectOptions::NativeFirst);
	if (!DIClass)
		DIClass = FindFirstObject<UClass>(*Trimmed, EFindFirstObjectOptions::NativeFirst);
	if (DIClass && DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
		return FNiagaraTypeDefinition(DIClass);

	// TODO-52: Plain UObject reference types (not DI subclasses) — e.g. "TextureRenderTarget",
	// "Texture2D", "UTexture2DArray". The Niagara parameter store supports FNiagaraTypeDefinition(UClass*)
	// for any UObject subclass. Used for Step 1 of the RT sampling plan (User.RT_VertAttribs).
	{
		UClass* ObjClass = FindFirstObject<UClass>(*Trimmed, EFindFirstObjectOptions::NativeFirst);
		// Also try with "U" prefix in case user omitted it (e.g. "TextureRenderTarget" → "UTextureRenderTarget")
		if (!ObjClass)
			ObjClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *Trimmed), EFindFirstObjectOptions::NativeFirst);
		if (ObjClass && ObjClass->IsChildOf(UObject::StaticClass()))
			return FNiagaraTypeDefinition(ObjClass);
	}

	// Unknown — return invalid def
	return FNiagaraTypeDefinition();
}

// ============================================================
//  add_system_user_parameter (TODO-28)
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteAddSystemUserParameter(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, ParamName, TypeStr;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("param_name"),  ParamName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("param_type"),  TypeStr,    Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                          return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	FNiagaraTypeDefinition TypeDef = BuildNiagaraTypeDef(TypeStr);
	if (!TypeDef.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unknown param_type '%s'. Supported scalars: float, int, bool, vec2, vec3, vec4, color. "
				 "Supported DI types (NiagaraDataInterface prefix optional): RenderTarget2DArray, RenderTarget2D, "
				 "Texture2DArray, ArrayFloat3, ArrayFloat, ArrayFloat2, ArrayFloat4. "
				 "Supported UObject refs (TODO-52): TextureRenderTarget, Texture2D, Texture2DArray, StaticMesh, etc. — "
				 "or any full UClass name with or without U prefix."), *TypeStr));
	}

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

	// Check if it already exists
	TArray<FNiagaraVariable> Existing;
	Store.GetParameters(Existing);
	for (const FNiagaraVariable& V : Existing)
	{
		if (V.GetName().ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parameter '%s' already exists in NiagaraSystem '%s'. Use remove_system_user_parameter first to replace it."),
				*ParamName, *System->GetName()));
		}
	}

	FNiagaraVariable NewVar(TypeDef, FName(*ParamName));
	const bool bAdded = Store.AddParameter(NewVar, /*bInitialize=*/true);
	if (!bAdded)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to add parameter '%s' to NiagaraSystem '%s'. Check type validity."),
			*ParamName, *System->GetName()));
	}

	// For DI types, create and bind a default DI instance (TODO-34)
	bool bIsDI = TypeDef.IsDataInterface();
	bool bIsObjRef = (!bIsDI && TypeDef.GetClass() != nullptr);
	if (bIsDI)
	{
		UClass* DIClass = TypeDef.GetClass();
		if (DIClass)
		{
			UNiagaraDataInterface* DI = NewObject<UNiagaraDataInterface>(System, DIClass, NAME_None, RF_Transactional);
			if (DI)
			{
				Store.SetDataInterface(DI, NewVar);
				UE_LOG(LogUnrealClaude, Log,
					TEXT("add_system_user_parameter: DI instance created (%s) and bound to '%s'"),
					*DIClass->GetName(), *ParamName);
			}
		}
	}
	else if (bIsObjRef)
	{
		// TODO-52: Plain UObject reference — no DI instance needed; the store holds a null UObject* by default.
		UE_LOG(LogUnrealClaude, Log,
			TEXT("add_system_user_parameter (TODO-52): added UObject ref parameter '%s' (class: %s)"),
			*ParamName, *TypeDef.GetClass()->GetName());
	}

	System->MarkPackageDirty();

	// Apply the initial value when provided (NI016 root cause: 'value' used to be silently
	// ignored — User.iFFT_PatchLength stayed 0, producing NaN UVs and zero RT samples).
	bool bValueApplied = false;
	if (!bIsDI && Params->HasField(TEXT("value")))
	{
		FMCPToolResult SetResult = ExecuteSetSystemUserParam(Params);
		bValueApplied = SetResult.bSuccess;
		if (!SetResult.bSuccess)
		{
			UE_LOG(LogUnrealClaude, Warning,
				TEXT("add_system_user_parameter: 'value' provided but applying it failed: %s"),
				*SetResult.Message);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"),   SystemPath);
	Result->SetStringField(TEXT("param_name"),    ParamName);
	Result->SetStringField(TEXT("param_type"),    TypeStr);
	Result->SetBoolField  (TEXT("is_data_interface"), bIsDI);
	Result->SetBoolField  (TEXT("is_object_ref"),     bIsObjRef);
	Result->SetBoolField  (TEXT("value_applied"),     bValueApplied);
	if ((bIsDI || bIsObjRef) && TypeDef.GetClass())
	{
		Result->SetStringField(TEXT("class"),    TypeDef.GetClass()->GetName()); // canonical name
		Result->SetStringField(TEXT("di_class"), TypeDef.GetClass()->GetName()); // backward compat (TC-56)
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added user parameter '%s' (%s%s) to NiagaraSystem '%s'%s."),
			*ParamName, *TypeStr, bIsDI ? TEXT(" [DataInterface]") : TEXT(""), *System->GetName(),
			bValueApplied ? TEXT("; initial value applied") : TEXT("")), Result);
}

// ============================================================
//  remove_system_user_parameter (TODO-29)
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteRemoveSystemUserParameter(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, ParamName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("param_name"),  ParamName,  Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                          return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	TArray<FNiagaraVariable> Variables;
	Store.GetParameters(Variables);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& V : Variables)
	{
		if (V.GetName().ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			FoundVar = &V;
			break;
		}
	}

	if (!FoundVar)
	{
		TArray<FString> Names;
		for (const FNiagaraVariable& V : Variables) Names.Add(V.GetName().ToString());
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' not found in NiagaraSystem '%s'. Available: %s"),
			*ParamName, *System->GetName(),
			Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)")));
	}

	Store.RemoveParameter(*FoundVar);
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("param_name"),  ParamName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed user parameter '%s' from NiagaraSystem '%s'."),
			*ParamName, *System->GetName()), Result);
}

// ============================================================
//  create_scratchpad_module (TODO-30, fixed by TODO-32)
//
//  Creates a proper inline scratch-pad module using the sub-script pattern:
//    Stage graph: UNiagaraNodeFunctionCall → UNiagaraScript (Usage=Module)
//    Inner graph: UNiagaraNodeInput(ParameterMap) → UNiagaraNodeCustomHlsl → UNiagaraNodeOutput
//
//  UNiagaraNodeInput is in a Private NiagaraEditor header; we create it via
//  FindObject<UClass> runtime lookup and set its properties via reflection.
//  The inner ParameterMap Input→Output flow is required by the Niagara compiler
//  ("Parameter Maps must be created via an Input Node").
// ============================================================


FMCPToolResult FMCPTool_NiagaraModify::ExecuteCreateScratchpadModule(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, HlslBody;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("hlsl"),         HlslBody,    Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	const int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : -400;
	const int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 0;

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* Graph = GetScriptGraph(StageScript);
	if (!Graph) return FMCPToolResult::Error(FString::Printf(TEXT("Could not get graph for stage '%s'."), *StageName));

	if (FindModuleByName(Graph, ModuleName))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("A module named '%s' already exists in stage '%s'. Use set_module_hlsl to update it, or remove_module first."),
			*ModuleName, *StageName));
	}

	// ── Step 0b: Evict orphaned ScratchPadScripts ─────────────────────────────────────────────
	// When the user manually deletes modules in the editor UI, the FunctionScript objects remain
	// in System->ScratchPadScripts as orphans (no FunctionCall references them). The Niagara
	// compiler crashes with "Array index out of bounds: N into array of size N" when it encounters
	// orphaned scripts alongside a newly-created one. Clean them out before proceeding.
	{
		TSet<UNiagaraScript*> Referenced;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* EData = Handle.GetEmitterData();
			if (!EData) continue;
			TArray<UNiagaraScript*> Scripts;
			Scripts.Add(EData->EmitterSpawnScriptProps.Script);
			Scripts.Add(EData->EmitterUpdateScriptProps.Script);
			Scripts.Add(EData->SpawnScriptProps.Script);
			Scripts.Add(EData->UpdateScriptProps.Script);
			for (const FNiagaraEventScriptProperties& Ev : EData->GetEventHandlers())
				Scripts.Add(Ev.Script);
			for (UNiagaraSimulationStageBase* SimStage : EData->GetSimulationStages())
				if (SimStage) Scripts.Add(SimStage->Script);
			for (UNiagaraScript* S : Scripts)
			{
				if (!S) continue;
				UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(S->GetLatestSource());
				UNiagaraGraph* G = Src ? Src->NodeGraph : nullptr;
				if (!G) continue;
				TArray<UNiagaraNodeFunctionCall*> Calls;
				G->GetNodesOfClass<UNiagaraNodeFunctionCall>(Calls);
				for (UNiagaraNodeFunctionCall* Call : Calls)
					if (Call && Call->FunctionScript)
						Referenced.Add(Call->FunctionScript);
			}
		}
		int32 Evicted = 0;
		for (int32 i = System->ScratchPadScripts.Num() - 1; i >= 0; --i)
		{
			UNiagaraScript* S = System->ScratchPadScripts[i];
			if (!S || !Referenced.Contains(S))
			{
				if (S) S->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
				System->ScratchPadScripts.RemoveAt(i);
				++Evicted;
			}
		}
		if (Evicted > 0)
			UE_LOG(LogUnrealClaude, Log,
				TEXT("create_scratchpad_module: evicted %d orphaned ScratchPadScript(s) before creating '%s'"),
				Evicted, *ModuleName);
	}

	// ── Step 1: Create inline UNiagaraScript (Module usage, owned by system) ─────────────────
	UNiagaraScript* InlineScript = NewObject<UNiagaraScript>(System, FName(*ModuleName), RF_Transactional);
	InlineScript->SetUsage(ENiagaraScriptUsage::Module);
	// Register in system's ScratchPadScripts list so the compiler can resolve FunctionCall references
	System->ScratchPadScripts.Add(InlineScript);

	// ── Step 2: Bootstrap inner script by duplicating an existing module from the stage graph ──
	// UNiagaraScriptFactoryNew::InitializeScript is not exported (no NIAGARAEDITOR_API).
	// Instead, find any valid UNiagaraNodeFunctionCall in the stage graph, duplicate its
	// FunctionScript — this gives us a properly initialized UNiagaraScript with a valid
	// VersionData[0].Source and inner graph.  We then rename the duplicate and own it.
	{
		TArray<UNiagaraNodeFunctionCall*> ExistingCalls;
		Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(ExistingCalls);
		for (UNiagaraNodeFunctionCall* EC : ExistingCalls)
		{
			if (EC && EC->FunctionScript && EC->FunctionScript->GetLatestSource())
			{
				UNiagaraScript* DupScript = Cast<UNiagaraScript>(
					StaticDuplicateObject(EC->FunctionScript, System, FName(*ModuleName), RF_Transactional));
				if (DupScript)
				{
					// Replace InlineScript with the properly initialised duplicate
					InlineScript = DupScript;
					InlineScript->SetUsage(ENiagaraScriptUsage::Module);
					InlineScript->ClearFlags(RF_Public | RF_Standalone);
					// Replace in ScratchPadScripts
					System->ScratchPadScripts.Last() = InlineScript;
					UE_LOG(LogUnrealClaude, Log,
						TEXT("create_scratchpad_module: Bootstrapped from '%s' via StaticDuplicateObject"),
						*EC->FunctionScript->GetName());
				}
				break;
			}
		}
	}

	// ── Step 3: Update HLSL in bootstrapped inner graph (TODO-40) ──────────────────────────────
	// StaticDuplicateObject preserves the full inner graph (InputNode→CustomHlsl→OutputNode).
	// We update the existing CustomHlsl text in-place to avoid breaking the Input Node chain.
	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(InlineScript->GetLatestSource());
	UNiagaraGraph* InnerGraph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
	bool bInputNodeCreated = (ScriptSource != nullptr && InnerGraph != nullptr);
	bool bHlslInjected = false;

	UE_LOG(LogUnrealClaude, Log,
		TEXT("create_scratchpad_module: ScriptSource=%s InnerGraph=%s"),
		ScriptSource ? TEXT("ok") : TEXT("null"), InnerGraph ? TEXT("ok") : TEXT("null"));

	if (InnerGraph)
	{
		// ── TODO-46 (redesigned): Clean bootstrapped inner graph ────────────────────────────
		// StaticDuplicateObject copies the ENTIRE inner graph of the source module, including
		// Map Get/Set nodes with stale parameter references, and a bootstrapped CustomHlsl
		// whose map pins become null-type after neighbor nodes are removed.
		//
		// Redesigned strategy (NI005 fix, 2026-06-09):
		//   Keep ONLY: InputMap source node + UNiagaraNodeOutput (Module usage)
		//   Remove:    EVERYTHING else including any bootstrapped CustomHlsl
		//   Then:      Create a FRESH UNiagaraNodeCustomHlsl with ParameterMap in
		//              Signature.Inputs/Outputs so AllocateDefaultPins produces valid
		//              typed map pins (not null-type).
		//
		// This matches the editor-created pattern and avoids stale bootstrap state entirely.
		{
			TArray<UEdGraphNode*> NodesToRemove;
			UEdGraphNode* InputMapNode  = nullptr;
			UNiagaraNodeOutput* OutNode = nullptr;

			for (UEdGraphNode* Node : InnerGraph->Nodes)
			{
				if (!Node) continue;

				if (UNiagaraNodeOutput* AsOut = Cast<UNiagaraNodeOutput>(Node))
				{
					if (AsOut->GetUsage() == ENiagaraScriptUsage::Module)
					{ OutNode = AsOut; continue; }
				}

				// InputMap heuristic: param-map OUT exists, param-map IN does not
				bool bHasMapOut = (FindParamMapPin(Node, EGPD_Output) != nullptr);
				bool bHasMapIn  = (FindParamMapPin(Node, EGPD_Input)  != nullptr);
				if (bHasMapOut && !bHasMapIn)
				{ InputMapNode = Node; continue; }

				// Remove everything else — including bootstrapped CustomHlsl, Map Get/Set, etc.
				NodesToRemove.Add(Node);
			}

			int32 NumRemoved = 0;
			for (UEdGraphNode* Node : NodesToRemove)
			{
				Node->BreakAllNodeLinks();
				InnerGraph->RemoveNode(Node);
				++NumRemoved;
			}

			// Wire InputMap → OutNode directly for now; CustomHlsl will be spliced in below.
			if (InputMapNode && OutNode)
			{
				UEdGraphPin* SrcMapOut = FindParamMapPin(InputMapNode, EGPD_Output);
				UEdGraphPin* DstMapIn  = FindParamMapPin(OutNode,      EGPD_Input);
				if (SrcMapOut && DstMapIn) SrcMapOut->MakeLinkTo(DstMapIn);
			}

			UE_LOG(LogUnrealClaude, Log,
				TEXT("create_scratchpad_module (TODO-46 redesign): removed %d bootstrapped nodes; "
				     "InputMapNode=%s OutNode=%s"),
				NumRemoved,
				InputMapNode ? TEXT("ok") : TEXT("null"),
				OutNode      ? TEXT("ok") : TEXT("null"));
		}
		// ────────────────────────────────────────────────────────────────────────────────────

		// After cleanup there are no CustomHlsl nodes. Always create a fresh one.
		// NI005 fix: add ParameterMap to Signature.Inputs/Outputs BEFORE AllocateDefaultPins
		// so the map pins are created with valid NiagaraParameterMap type (not null-type).
		if (!HlslBody.IsEmpty())
		{
			UNiagaraNodeCustomHlsl* NewHlsl = NewObject<UNiagaraNodeCustomHlsl>(
				InnerGraph, NAME_None, RF_Transactional);
			if (NewHlsl)
			{
				NewHlsl->CreateNewGuid();

				// Set Signature: Name + ParameterMap in/out + GPU support (TODO-50)
				NewHlsl->Signature.Name         = FName(*ModuleName);
				NewHlsl->Signature.bSupportsGPU = true;
				NewHlsl->Signature.bSupportsCPU = false;
				// Add ParameterMap to Signature so AllocateDefaultPins creates valid typed map pins
				NewHlsl->Signature.Inputs.Add( FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map")));
				NewHlsl->Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map")));

				// Inject HLSL body
				if (FStrProperty* P = FindFProperty<FStrProperty>(NewHlsl->GetClass(), TEXT("CustomHlsl")))
					*P->ContainerPtrToValuePtr<FString>(NewHlsl) = HlslBody;

				InnerGraph->AddNode(NewHlsl, false, false);
				NewHlsl->AllocateDefaultPins(); // creates map pins with proper NiagaraParameterMap type

				UEdGraphPin* HlslMapIn  = FindParamMapPin(NewHlsl, EGPD_Input);
				UEdGraphPin* HlslMapOut = FindParamMapPin(NewHlsl, EGPD_Output);

				// Find inner OutputNode and splice NewHlsl into the InputMap → OutputNode chain
				TArray<UNiagaraNodeOutput*> InnerOutputs;
				InnerGraph->GetNodesOfClass<UNiagaraNodeOutput>(InnerOutputs);
				UNiagaraNodeOutput* InnerOutNode = nullptr;
				for (UNiagaraNodeOutput* O : InnerOutputs)
				{
					if (O && O->GetUsage() == ENiagaraScriptUsage::Module) { InnerOutNode = O; break; }
				}

				if (InnerOutNode && HlslMapIn && HlslMapOut)
				{
					UEdGraphPin* InnerOutMapIn = FindParamMapPin(InnerOutNode, EGPD_Input);
					if (InnerOutMapIn)
					{
						UEdGraphPin* PrevChainOut = InnerOutMapIn->LinkedTo.Num() > 0
							? InnerOutMapIn->LinkedTo[0] : nullptr;
						if (PrevChainOut)
						{
							InnerOutMapIn->BreakLinkTo(PrevChainOut);
							HlslMapIn->MakeLinkTo(PrevChainOut);
						}
						HlslMapOut->MakeLinkTo(InnerOutMapIn);
						bHlslInjected = true;
						UE_LOG(LogUnrealClaude, Log,
							TEXT("create_scratchpad_module (NI005): Created fresh CustomHlsl with valid map pins, spliced into inner graph"));
					}
				}

				InnerGraph->MarkPackageDirty();
				InnerGraph->NotifyGraphChanged();

				if (!bHlslInjected)
					UE_LOG(LogUnrealClaude, Warning,
						TEXT("create_scratchpad_module (NI005): CustomHlsl created but wiring failed (no OutputNode or map pins)"));
			}
		}
		else
		{
			UE_LOG(LogUnrealClaude, Log,
				TEXT("create_scratchpad_module: no hlsl param supplied — inner graph left as InputMap → OutputNode only"));
		}
	}

	// ── Step 4: Create UNiagaraNodeFunctionCall in stage graph ───────────────────────────────
	UNiagaraNodeFunctionCall* FuncCall = NewObject<UNiagaraNodeFunctionCall>(Graph, NAME_None, RF_Transactional);
	FuncCall->CreateNewGuid();            // fixes NiagaraParameterMapHistory ensure
	FuncCall->FunctionScript = InlineScript;
	FuncCall->NodePosX = PosX;
	FuncCall->NodePosY = PosY;
	Graph->AddNode(FuncCall, false, false);
	FuncCall->AllocateDefaultPins();

	// If AllocateDefaultPins missed the input ParameterMap pin (inner script uncompiled),
	// force-create it so the stage chain wiring can proceed.
	UScriptStruct* MapStruct = FNiagaraTypeDefinition::GetParameterMapDef().GetScriptStruct();
	if (!FindParamMapPin(FuncCall, EGPD_Input))
	{
		UEdGraphPin* ForcedIn = FuncCall->CreatePin(EGPD_Input, TEXT("struct"), FName("Map"));
		if (ForcedIn) ForcedIn->PinType.PinSubCategoryObject = MapStruct;
	}

	// ── Step 8: Wire FuncCall into stage's parameter-map chain ────────────────────────────────
	// TODO-39 FIX: For GPU simulation stages, StageNameToUsage returns the Module sentinel.
	// We match the correct UNiagaraNodeOutput by UsageId (StageScript->GetUsageId()) rather
	// than falling back to OutputNodes[0] which would land in EmitterSpawn.
	UNiagaraNodeOutput* StageOutput = nullptr;
	bool bUsedSimStageMatch = false;
	{
		const ENiagaraScriptUsage CreateStageUsage = StageNameToUsage(StageName);
		if (CreateStageUsage != ENiagaraScriptUsage::Module)
		{
			// Standard stage: match by usage enum
			StageOutput = GetStageOutputNode(Graph, CreateStageUsage);
		}
		else
		{
			// GPU simulation stage: match by UsageId so we target the correct stage output node.
			// All emitter stages share the same UNiagaraGraph; without UsageId matching we would
			// wire into the wrong stage (historically EmitterSpawn — the old TODO-39 bug).
			TArray<UNiagaraNodeOutput*> AllOutputs;
			Graph->GetNodesOfClass<UNiagaraNodeOutput>(AllOutputs);
			for (UNiagaraNodeOutput* Out : AllOutputs)
			{
				if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript
					&& Out->GetUsageId() == StageScript->GetUsageId())
				{
					StageOutput = Out;
					bUsedSimStageMatch = true;
					break;
				}
			}
			// Fallback: first sim stage output (if UsageId didn't match — e.g. uncompiled system)
			if (!StageOutput)
			{
				for (UNiagaraNodeOutput* Out : AllOutputs)
				{
					if (Out && Out->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
					{
						StageOutput = Out;
						UE_LOG(LogUnrealClaude, Warning,
							TEXT("create_scratchpad_module: UsageId match failed for stage '%s'; using first sim stage output as fallback"),
							*StageName);
						break;
					}
				}
			}
		}
	}
	FString WiringReport = TEXT("no stage output node found — module not wired");

	if (StageOutput)
	{
		UEdGraphPin* StageMapIn  = FindParamMapPin(StageOutput, EGPD_Input);
		UEdGraphPin* NewMapIn    = FindParamMapPin(FuncCall,    EGPD_Input);
		UEdGraphPin* NewMapOut   = FindParamMapPin(FuncCall,    EGPD_Output);

		if (StageMapIn && NewMapIn && NewMapOut)
		{
			UEdGraphPin* PrevOut = StageMapIn->LinkedTo.Num() > 0 ? StageMapIn->LinkedTo[0] : nullptr;
			if (PrevOut)
			{
				StageMapIn->BreakLinkTo(PrevOut);
				NewMapIn->MakeLinkTo(PrevOut);
			}
			NewMapOut->MakeLinkTo(StageMapIn);
			WiringReport = TEXT("parameter-map chain connected");
		}
		else
		{
			WiringReport = FString::Printf(
				TEXT("pin missing (StageMapIn=%s MapIn=%s MapOut=%s) — wiring skipped"),
				StageMapIn ? TEXT("ok") : TEXT("null"),
				NewMapIn   ? TEXT("ok") : TEXT("null"),
				NewMapOut  ? TEXT("ok") : TEXT("null"));
		}
	}

	// ── Diagnostics before compile ────────────────────────────────────────────────────────────
	UNiagaraScriptSource* DiagSource = Cast<UNiagaraScriptSource>(InlineScript->GetLatestSource());
	bool bHasSource     = DiagSource != nullptr;
	bool bHasGraph      = bHasSource && DiagSource->NodeGraph != nullptr;
	bool bValidAndGraph = FuncCall->HasValidScriptAndGraph();
	UE_LOG(LogUnrealClaude, Log,
		TEXT("create_scratchpad_module DIAG: HasSource=%d HasGraph=%d HasValidScriptAndGraph=%d HlslInjected=%d MapIn=%s MapOut=%s"),
		(int)bHasSource, (int)bHasGraph, (int)bValidAndGraph, (int)bHlslInjected,
		FindParamMapPin(FuncCall, EGPD_Input)  ? TEXT("ok") : TEXT("null"),
		FindParamMapPin(FuncCall, EGPD_Output) ? TEXT("ok") : TEXT("null"));

	Graph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"),  SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"),        StageName);
	Result->SetStringField(TEXT("module_name"),  ModuleName);
	Result->SetStringField(TEXT("wiring"),       WiringReport);
	Result->SetBoolField  (TEXT("input_node_created"),   bInputNodeCreated);
	Result->SetBoolField  (TEXT("hlsl_injected"),        bHlslInjected);
	Result->SetBoolField  (TEXT("sim_stage_targeted"),   bUsedSimStageMatch);
	Result->SetBoolField  (TEXT("diag_has_source"),      bHasSource);
	Result->SetBoolField  (TEXT("diag_has_graph"),       bHasGraph);
	Result->SetBoolField  (TEXT("diag_valid_and_graph"), bValidAndGraph);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created scratchpad module '%s' in stage '%s' (%s); recompile requested."),
			*ModuleName, *StageName, *WiringReport), Result);
}

// ============================================================
//  set_module_hlsl (TODO-31)
//  Update the HLSL body of an existing UNiagaraNodeCustomHlsl.
// ============================================================

FMCPToolResult FMCPTool_NiagaraModify::ExecuteSetModuleHlsl(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, HlslBody;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("hlsl"),         HlslBody,    Error)) return Error.GetValue();
	if (!ValidateBlueprintPathParam(SystemPath, Error))                            return Error.GetValue();

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load NiagaraSystem at '%s'."), *SystemPath));

	int32 HandleIndex = INDEX_NONE;
	FVersionedNiagaraEmitterData* Data = GetEmitterDataByName(System, EmitterName, HandleIndex);
	if (!Data) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(Data, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(FString::Printf(TEXT("Could not get graph for stage '%s'."), *StageName));

	UNiagaraNodeFunctionCall* ModuleNode = FindModuleByName(StageGraph, ModuleName);
	if (!ModuleNode)
	{
		TArray<UNiagaraNodeFunctionCall*> AllNodes;
		StageGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllNodes);
		TArray<FString> Names;
		for (auto* N : AllNodes) { if (N) Names.Add(N->GetFunctionName()); }
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' not found in stage '%s'. Available: %s"),
			*ModuleName, *StageName, *FString::Join(Names, TEXT(", "))));
	}

	// Case 1: the module node itself is a CustomHlsl node
	if (UNiagaraNodeCustomHlsl* DirectHlsl = Cast<UNiagaraNodeCustomHlsl>(ModuleNode))
	{
		FString OldHlsl;
		FStrProperty* HlslProp = FindFProperty<FStrProperty>(DirectHlsl->GetClass(), TEXT("CustomHlsl"));
		if (HlslProp) OldHlsl = *HlslProp->ContainerPtrToValuePtr<FString>(DirectHlsl);
		if (HlslProp) *HlslProp->ContainerPtrToValuePtr<FString>(DirectHlsl) = HlslBody;

		// TODO-37: Invalidate the Niagara HLSL diagnostic scan cache so the compiler
		// re-reads the live HlslText rather than a stale cached intermediate.
		DirectHlsl->Modify();
		DirectHlsl->RefreshFromExternalChanges();
		BumpGraphChangeId(StageGraph, TEXT("set_module_hlsl (direct)"));
		StageGraph->NotifyGraphChanged();

		StageGraph->MarkPackageDirty();
		System->MarkPackageDirty();
		System->RequestCompile(false);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("module_name"), ModuleName);
		Result->SetStringField(TEXT("old_hlsl_preview"), OldHlsl.Left(200));
		return FMCPToolResult::Success(
			FString::Printf(TEXT("Updated HLSL on module '%s' (direct CustomHlsl node); recompile requested."), *ModuleName), Result);
	}

	// Case 2: inline scratch-pad module — find the CustomHlsl node in the inner graph
	const bool bIsInline = ModuleNode->FunctionScriptAssetObjectPath.IsNone();
	if (!bIsInline)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' is asset-based (script_path='%s'). Only inline (scratch-pad) or custom-hlsl modules can be edited."),
			*ModuleName, *ModuleNode->FunctionScriptAssetObjectPath.ToString()));
	}

	if (!ModuleNode->FunctionScript)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' is inline but FunctionScript is null."), *ModuleName));
	}

	UNiagaraGraph* InnerGraph = GetScriptGraph(ModuleNode->FunctionScript);
	if (!InnerGraph)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' inner script has no graph."), *ModuleName));
	}

	TArray<UNiagaraNodeCustomHlsl*> InnerHlslNodes;
	InnerGraph->GetNodesOfClass<UNiagaraNodeCustomHlsl>(InnerHlslNodes);
	if (InnerHlslNodes.Num() == 0)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' inner graph has no CustomHlsl node to update."), *ModuleName));
	}

	FString OldHlsl;
	FStrProperty* HlslPropInner = FindFProperty<FStrProperty>(InnerHlslNodes[0]->GetClass(), TEXT("CustomHlsl"));
	if (HlslPropInner) OldHlsl = *HlslPropInner->ContainerPtrToValuePtr<FString>(InnerHlslNodes[0]);
	if (HlslPropInner) *HlslPropInner->ContainerPtrToValuePtr<FString>(InnerHlslNodes[0]) = HlslBody;

	// TODO-37: Invalidate the Niagara HLSL diagnostic scan cache.
	// Mark the inner CustomHlsl node dirty, notify both the inner graph and the stage graph,
	// and also invalidate the inline FunctionScript itself — this forces the translator to
	// re-derive from live nodes rather than reading a stale cached diagnostic intermediate.
	InnerHlslNodes[0]->Modify();
	InnerHlslNodes[0]->RefreshFromExternalChanges();
	BumpGraphChangeId(InnerGraph, TEXT("set_module_hlsl (inner)"));
	BumpGraphChangeId(StageGraph, TEXT("set_module_hlsl (stage)"));
	InnerGraph->NotifyGraphChanged();
	if (ModuleNode->FunctionScript)
	{
		ModuleNode->FunctionScript->Modify();
	}
	StageGraph->NotifyGraphChanged();

	InnerGraph->MarkPackageDirty();
	StageGraph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"), ModuleName);
	Result->SetStringField(TEXT("old_hlsl_preview"), OldHlsl.Left(200));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Updated HLSL on inline module '%s'; recompile requested."), *ModuleName), Result);
}

// ============================================================
//  create_assignment_module
// ============================================================
//
// Adds a UNiagaraNodeAssignment ("Set Parameters") node to a Niagara stage graph.
// Uses only fully-exported NiagaraEditor APIs — no RequestNewTypedPin calls.
//
// UNiagaraNodeAssignment::AddAssignmentTarget() + RefreshFromExternalChanges() auto-
// generate a correct module inner graph (InputNode → PMGet → PMSet → OutputNode).
//
// Required params:
//   system_path   — asset path of UNiagaraSystem
//   emitter_name  — emitter handle display-name or asset name
//   stage         — stage name: "ParticleSpawn", "ParticleUpdate", "EmitterUpdate", etc.
//   module_name   — label for the new node (informational; not stored on the node itself)
//   writes[]      — [{ "param": "Particles.SpriteSize", "type": "vec2", "default": "" }]
//
// Optional:
//   pos_x, pos_y  — stage-graph node position (default: auto before OutputNode)
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteCreateAssignmentModule(const TSharedRef<FJsonObject>& Params)
{
	// ── Parse required params ──────────────────────────────────────────────────────────────
	FString SystemPath, EmitterName, StageName, ModuleName;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Err)) return Err.GetValue();
	}

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	// ── Parse writes[] array ──────────────────────────────────────────────────────────────
	auto ParseTypeString = [](const FString& TypeStr) -> FNiagaraTypeDefinition
	{
		FString Lower = TypeStr.ToLower();
		if (Lower == TEXT("float"))  return FNiagaraTypeDefinition::GetFloatDef();
		if (Lower == TEXT("vec2"))   return FNiagaraTypeDefinition::GetVec2Def();
		if (Lower == TEXT("vec3"))   return FNiagaraTypeDefinition::GetVec3Def();
		if (Lower == TEXT("vec4"))   return FNiagaraTypeDefinition::GetVec4Def();
		if (Lower == TEXT("int"))    return FNiagaraTypeDefinition::GetIntDef();
		if (Lower == TEXT("bool"))   return FNiagaraTypeDefinition::GetBoolDef();
		return FNiagaraTypeDefinition::GetFloatDef();
	};

	struct FWriteEntry { FString ParamName; FNiagaraTypeDefinition TypeDef; FString DefaultValue; };
	TArray<FWriteEntry> WriteEntries;

	// Accept writes either as a native JSON array OR as a JSON-encoded string
	// (the latter happens when the MCP schema hasn't propagated the array type yet).
	TArray<TSharedPtr<FJsonValue>> LocalWritesArr;
	const TArray<TSharedPtr<FJsonValue>>* WritesArrPtr = nullptr;
	{
		// Try native array first
		if (!Params->TryGetArrayField(TEXT("writes"), WritesArrPtr))
		{
			// Fallback: try as JSON string containing an array
			FString WritesJson;
			if (Params->TryGetStringField(TEXT("writes"), WritesJson) && !WritesJson.IsEmpty())
			{
				TSharedPtr<FJsonValue> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WritesJson);
				if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()
					&& Parsed->Type == EJson::Array)
				{
					LocalWritesArr = Parsed->AsArray();
					WritesArrPtr = &LocalWritesArr;
				}
			}
		}
	}
	if (WritesArrPtr)
	{
		for (const TSharedPtr<FJsonValue>& V : *WritesArrPtr)
		{
			TSharedPtr<FJsonObject> Obj = V->AsObject();
			if (!Obj) continue;
			FWriteEntry E;
			E.ParamName    = Obj->GetStringField(TEXT("param"));
			E.TypeDef      = ParseTypeString(Obj->GetStringField(TEXT("type")));
			Obj->TryGetStringField(TEXT("default"), E.DefaultValue);
			WriteEntries.Add(E);
		}
	}

	if (WriteEntries.Num() == 0)
		return FMCPToolResult::Error(TEXT("create_assignment_module requires at least one entry in 'writes[]'."));

	// ── Optional node position ─────────────────────────────────────────────────────────────
	int32 PosX = 0, PosY = 0;
	{
		double Tmp;
		if (Params->TryGetNumberField(TEXT("pos_x"), Tmp)) PosX = (int32)Tmp;
		if (Params->TryGetNumberField(TEXT("pos_y"), Tmp)) PosY = (int32)Tmp;
	}

	// ── Find OutputNode to determine insert position ───────────────────────────────────────
	ENiagaraScriptUsage Usage = StageNameToUsage(StageName);
	UNiagaraNodeOutput* OutputNode = GetStageOutputNode(StageGraph, Usage);
	if (!OutputNode && Usage == ENiagaraScriptUsage::Module)
	{
		// GPU simulation stage fallback: OutputNode uses ParticleSimulationStageScript
		TArray<UNiagaraNodeOutput*> OutNodes;
		StageGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutNodes);
		for (UNiagaraNodeOutput* O : OutNodes)
		{
			if (O && O->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
			{ OutputNode = O; break; }
		}
		// Last resort: first available output node
		if (!OutputNode && OutNodes.Num() > 0) OutputNode = OutNodes[0];
	}
	if (!OutputNode)
		return FMCPToolResult::Error(FString::Printf(TEXT("No OutputNode found in stage '%s'."), *StageName));

	// Auto-position: place just before OutputNode if pos_x not specified
	if (PosX == 0)
		PosX = OutputNode->NodePosX - 350;

	// ── Create UNiagaraNodeAssignment in STAGE graph ───────────────────────────────────────
	// UNiagaraNodeAssignment is the "Set Parameters" module — fully exported (NIAGARAEDITOR_API).
	// AddAssignmentTarget() + RefreshFromExternalChanges() auto-builds the correct inner graph:
	//   InputNode → PMGet → PMSet → OutputNode (module usage)
	// No manual inner graph wiring needed.
	UNiagaraNodeAssignment* AssignNode = NewObject<UNiagaraNodeAssignment>(
		StageGraph, NAME_None, RF_Transactional);
	AssignNode->CreateNewGuid();
	AssignNode->NodePosX = PosX;
	AssignNode->NodePosY = PosY;

	for (const FWriteEntry& W : WriteEntries)
	{
		FNiagaraVariable TargetVar(W.TypeDef, *W.ParamName);
		const FString* DefPtr = W.DefaultValue.IsEmpty() ? nullptr : &W.DefaultValue;
		AssignNode->AddAssignmentTarget(TargetVar, DefPtr);
		UE_LOG(LogUnrealClaude, Log, TEXT("create_assignment_module: AddAssignmentTarget '%s'"),
			*W.ParamName);
	}

	StageGraph->AddNode(AssignNode, false, false);
	AssignNode->AllocateDefaultPins();

	// RefreshFromExternalChanges triggers GenerateScript() which builds the correct inner graph
	AssignNode->RefreshFromExternalChanges();

	// ── Wire AssignNode into the param-map chain before OutputNode ────────────────────────
	// OutputNode's map-in may already be connected to the previous last module.
	// New chain: ... → PrevModule → AssignNode → OutputNode
	UEdGraphPin* OutMapIn     = FindParamMapPin(OutputNode,  EGPD_Input);
	UEdGraphPin* AssignMapIn  = FindParamMapPin(AssignNode,  EGPD_Input);
	UEdGraphPin* AssignMapOut = FindParamMapPin(AssignNode,  EGPD_Output);

	bool bChained = false;
	if (OutMapIn && AssignMapOut)
	{
		// Redirect existing connection from OutputNode's map-in to AssignNode's map-in
		if (OutMapIn->LinkedTo.Num() > 0)
		{
			UEdGraphPin* PrevMapOut = OutMapIn->LinkedTo[0];
			OutMapIn->BreakLinkTo(PrevMapOut);
			if (AssignMapIn)
			{
				PrevMapOut->MakeLinkTo(AssignMapIn);
				bChained = true;
			}
		}
		// Connect AssignNode's map-out to OutputNode's map-in
		AssignMapOut->MakeLinkTo(OutMapIn);
	}

	// ── Mark dirty and recompile ───────────────────────────────────────────────────────────
	StageGraph->MarkPackageDirty();
	StageGraph->NotifyGraphChanged();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	UE_LOG(LogUnrealClaude, Log,
		TEXT("create_assignment_module: '%s' added to stage '%s' (%d targets, chained=%s)"),
		*ModuleName, *StageName, WriteEntries.Num(), bChained ? TEXT("yes") : TEXT("no"));

	// ── Build result ───────────────────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"),     ModuleName);
	Result->SetStringField(TEXT("stage"),           StageName);
	Result->SetNumberField(TEXT("writes_added"),    WriteEntries.Num());
	Result->SetBoolField  (TEXT("chained"),         bChained);
	Result->SetNumberField(TEXT("pos_x"),           PosX);
	Result->SetNumberField(TEXT("pos_y"),           PosY);

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("create_assignment_module: added '%s' to '%s' stage '%s' "
				 "with %d write target(s); inner graph auto-built by RefreshFromExternalChanges; "
				 "chained into param-map=%s; recompile requested."),
			*ModuleName, *SystemPath, *StageName,
			WriteEntries.Num(),
			bChained ? TEXT("yes") : TEXT("no (was first module)")),
		Result);
}

// ============================================================
//  rebuild_scratchpad_inner_graph (TODO-47, supersedes TODO-42)
// ============================================================
//
// Updates the HLSL body of an existing scratch-pad FunctionScript module IN-PLACE,
// preserving the UNiagaraNodeFunctionCall in the stage graph and keeping the
// FunctionScript's inner graph structure intact (including DI inputs, Map Get/Set
// wiring, and parameter metadata).
//
// Required params:
//   system_path, emitter_name, stage  — locate stage graph
//   module_name  — name of the existing scratchpad module to update
//   hlsl         — new HLSL body to set in the module's CustomHlsl node
//
// Note on inputs[]/outputs[] params (optional):
//   These are IGNORED for FunctionScript modules. The module's inputs/outputs are
//   defined by its inner graph parameter metadata (added via the Niagara Parameters tab
//   or future MCP support). They are not controlled by this function.
//   This avoids the previous behaviour of destroying the FunctionScript and creating a
//   stage-level UNiagaraNodeCustomHlsl which had no DI access.
//
// TODO-47: The previous implementation replaced the FunctionCall with a stage-level
// UNiagaraNodeCustomHlsl, which permanently removed DI capability. This version keeps
// the FunctionCall and updates the inner graph's CustomHlsl text only.
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteRebuildScratchpadInnerGraph(const TSharedRef<FJsonObject>& Params)
{
	// ── Parse required params ──────────────────────────────────────────────────────────────
	FString SystemPath, EmitterName, StageName, ModuleName, HlslBody;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("hlsl"),         HlslBody,    Err)) return Err.GetValue();
	}

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	UNiagaraNodeFunctionCall* FuncCall = FindModuleByName(StageGraph, ModuleName);
	if (!FuncCall) return FMCPToolResult::Error(FString::Printf(
		TEXT("Module '%s' not found in stage '%s'. Use create_scratchpad_module first."),
		*ModuleName, *StageName));

	// ── Get inner graph from FunctionScript ────────────────────────────────────────────────
	UNiagaraScript* InnerScript = FuncCall->FunctionScript;
	if (!InnerScript)
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' has no FunctionScript — it may be a stage-level node (from the old "
			     "rebuild_scratchpad_inner_graph). Use remove_module + create_scratchpad_module to recreate it."),
			*ModuleName));

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(InnerScript->GetLatestSource());
	UNiagaraGraph* InnerGraph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
	if (!InnerGraph)
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Module '%s' FunctionScript has no inner graph."), *ModuleName));

	// ── Locate and update the CustomHlsl node in the inner graph ──────────────────────────
	TArray<UNiagaraNodeCustomHlsl*> HlslNodes;
	InnerGraph->GetNodesOfClass<UNiagaraNodeCustomHlsl>(HlslNodes);

	bool bHlslUpdated = false;
	if (HlslNodes.Num() > 0)
	{
		UNiagaraNodeCustomHlsl* HlslNode = HlslNodes[0];
		HlslNode->Modify();
		if (FStrProperty* HlslProp = FindFProperty<FStrProperty>(HlslNode->GetClass(), TEXT("CustomHlsl")))
		{
			*HlslProp->ContainerPtrToValuePtr<FString>(HlslNode) = HlslBody;
			bHlslUpdated = true;
			UE_LOG(LogUnrealClaude, Log,
				TEXT("rebuild_scratchpad (TODO-47): updated CustomHlsl text in '%s' (inner graph preserved)"),
				*ModuleName);
		}
		// TODO-50: Ensure Signature.Name is set correctly.
		// Bootstrap-created nodes may have the wrong name (from the source module).
		// Setting it here ensures the compiler can resolve the function on next compile.
		HlslNode->Signature.Name        = FName(*ModuleName);
		HlslNode->Signature.bSupportsGPU = true;
		HlslNode->Signature.bSupportsCPU = false;
		// Rebuild pins/tokens from the new text + invalidate the compilation digest cache —
		// without the ChangeId bump the compiler silently reuses the stale digest.
		HlslNode->RefreshFromExternalChanges();
		BumpGraphChangeId(InnerGraph, TEXT("rebuild_scratchpad_inner_graph"));
		BumpGraphChangeId(StageGraph, TEXT("rebuild_scratchpad_inner_graph (stage)"));
	}
	else
	{
		// No CustomHlsl node in the inner graph — this is an unexpected state.
		// The inner graph should always have a CustomHlsl node after create_scratchpad_module.
		// Log a warning; the HLSL was not applied.
		UE_LOG(LogUnrealClaude, Warning,
			TEXT("rebuild_scratchpad (TODO-47): no CustomHlsl node found in '%s' inner graph — HLSL not set"),
			*ModuleName);
	}

	// ── Mark dirty and recompile ───────────────────────────────────────────────────────────
	InnerGraph->MarkPackageDirty();
	StageGraph->MarkPackageDirty();
	System->MarkPackageDirty();

	bool bSkipCompile = false;
	Params->TryGetBoolField(TEXT("skip_compile"), bSkipCompile);
	if (!bSkipCompile)
	{
		InnerGraph->NotifyGraphChanged();
		StageGraph->NotifyGraphChanged();
		System->RequestCompile(false);
	}

	// ── Build result ───────────────────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"),       ModuleName);
	Result->SetStringField(TEXT("stage"),             StageName);
	Result->SetBoolField  (TEXT("hlsl_updated"),      bHlslUpdated);
	Result->SetBoolField  (TEXT("functionscript_preserved"), true);
	Result->SetNumberField(TEXT("inner_hlsl_nodes"),  HlslNodes.Num());
	Result->SetBoolField  (TEXT("skip_compile"),      bSkipCompile);

	FString StatusMsg = bHlslUpdated
		? TEXT("HLSL updated in inner graph; FunctionScript preserved")
		: TEXT("WARNING: no CustomHlsl node found — HLSL not set; use create_scratchpad_module to rebuild");

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("rebuild_scratchpad_inner_graph (TODO-47): module '%s' in stage '%s' — %s; recompile requested."),
			*ModuleName, *StageName, *StatusMsg),
		Result);
}

// ============================================================
//  reorder_module (TODO-43 sub-step)
// ============================================================
//
// Moves a UNiagaraNodeFunctionCall to a new position in the stage param-map chain.
//
// Required params:
//   system_path, emitter_name, stage, module_name
//   after_module  — name of the module this one should run AFTER (or "first" to move to front)
//
// Limitation: does NOT move override PMSet nodes that may precede the module.
// Call before bind_module_input so override nodes don't exist yet.
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteReorderModule(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, AfterModule;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("after_module"), AfterModule, Err)) return Err.GetValue();
	}

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	// Find the node to move
	UNiagaraNodeFunctionCall* MoveNode = FindModuleByName(StageGraph, ModuleName);
	if (!MoveNode) return FMCPToolResult::Error(FString::Printf(TEXT("Module '%s' not found."), *ModuleName));

	// Find the target anchor node (nullptr = insert at front of chain)
	UNiagaraNodeFunctionCall* AnchorNode = nullptr;
	bool bInsertFirst = AfterModule.TrimStartAndEnd().ToLower() == TEXT("first");
	if (!bInsertFirst)
	{
		AnchorNode = FindModuleByName(StageGraph, AfterModule);
		if (!AnchorNode) return FMCPToolResult::Error(
			FString::Printf(TEXT("after_module '%s' not found. Use 'first' to insert at front."), *AfterModule));
		if (AnchorNode == MoveNode) return FMCPToolResult::Error(TEXT("after_module cannot be the same as module_name."));
	}

	// Get current map pins of MoveNode
	UEdGraphPin* MoveMapIn  = FindParamMapPin(MoveNode, EGPD_Input);
	UEdGraphPin* MoveMapOut = FindParamMapPin(MoveNode, EGPD_Output);
	if (!MoveMapIn || !MoveMapOut)
		return FMCPToolResult::Error(FString::Printf(TEXT("Module '%s' has no param-map pins."), *ModuleName));

	// Capture current predecessor and successor
	UEdGraphPin* PredMapOut = (MoveMapIn->LinkedTo.Num()  > 0) ? MoveMapIn->LinkedTo[0]  : nullptr;
	UEdGraphPin* SuccMapIn  = (MoveMapOut->LinkedTo.Num() > 0) ? MoveMapOut->LinkedTo[0] : nullptr;

	if (!PredMapOut && !SuccMapIn)
		return FMCPToolResult::Error(TEXT("Module is not connected to the param-map chain."));

	// Step 1: Detach MoveNode from current position; bridge the gap
	if (PredMapOut) MoveMapIn->BreakLinkTo(PredMapOut);
	if (SuccMapIn)  MoveMapOut->BreakLinkTo(SuccMapIn);
	if (PredMapOut && SuccMapIn) PredMapOut->MakeLinkTo(SuccMapIn);

	// Step 2: Find the insertion point after AnchorNode (or at front of chain)
	UEdGraphPin* InsertAfterOut = nullptr; // map-out of the node we insert after
	UEdGraphPin* InsertBeforeIn = nullptr; // map-in of the node currently after the insertion point

	if (bInsertFirst)
	{
		// Find the stage's OutputNode, walk backwards to the first node
		ENiagaraScriptUsage Usage = StageNameToUsage(StageName);
		UNiagaraNodeOutput* OutputNode = GetStageOutputNode(StageGraph, Usage);
		if (!OutputNode && Usage == ENiagaraScriptUsage::Module)
		{
			TArray<UNiagaraNodeOutput*> OutNodes;
			StageGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutNodes);
			for (UNiagaraNodeOutput* O : OutNodes)
			{
				if (O && O->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript)
				{ OutputNode = O; break; }
			}
		}
		if (!OutputNode) return FMCPToolResult::Error(TEXT("Could not find stage OutputNode for front-insert."));

		// Walk the chain backwards from OutputNode to find the furthest node — that node's predecessor is the "head"
		// Or simpler: find a node whose map-in is not connected to a FunctionCall (it connects to the InputNode or nothing)
		// Actually just find the stage InputNode's map-out
		// Simplest: collect all FunctionCalls in chain order by following links backward from OutputNode
		UEdGraphPin* OutMapIn = FindParamMapPin(OutputNode, EGPD_Input);
		if (!OutMapIn || OutMapIn->LinkedTo.Num() == 0)
			return FMCPToolResult::Error(TEXT("OutputNode has no incoming param-map link."));

		// Walk back to find the very first node in the chain
		UEdGraphPin* Current = OutMapIn->LinkedTo[0]; // map-out of last FuncCall
		while (true)
		{
			UEdGraphNode* CurrentNode = Current->GetOwningNode();
			UEdGraphPin* CurrentMapIn = FindParamMapPin(CurrentNode, EGPD_Input);
			if (!CurrentMapIn || CurrentMapIn->LinkedTo.Num() == 0) break;
			UEdGraphNode* PrevNode = CurrentMapIn->LinkedTo[0]->GetOwningNode();
			if (!Cast<UNiagaraNodeFunctionCall>(PrevNode)) break; // hit InputNode or non-funcCall
			Current = FindParamMapPin(PrevNode, EGPD_Output);
			if (!Current) break;
		}
		// 'Current' is now the map-out of the first FuncCall (or some other chain-start)
		// We want to insert BEFORE it — so find what feeds into it
		UEdGraphNode* FirstNode = Current->GetOwningNode();
		UEdGraphPin* FirstMapIn = FindParamMapPin(FirstNode, EGPD_Input);
		if (FirstMapIn && FirstMapIn->LinkedTo.Num() > 0)
		{
			InsertAfterOut = FirstMapIn->LinkedTo[0]; // what was before the first node
			InsertBeforeIn = FirstMapIn;
		}
		else
		{
			// First node has nothing before it — wire MoveNode before it
			InsertAfterOut = nullptr;
			InsertBeforeIn = FirstMapIn;
		}
	}
	else
	{
		// Insert after AnchorNode
		UEdGraphPin* AnchorMapOut = FindParamMapPin(AnchorNode, EGPD_Output);
		if (!AnchorMapOut) return FMCPToolResult::Error(FString::Printf(
			TEXT("AnchorNode '%s' has no param-map output pin."), *AfterModule));

		InsertAfterOut = AnchorMapOut;
		InsertBeforeIn = (AnchorMapOut->LinkedTo.Num() > 0) ? AnchorMapOut->LinkedTo[0] : nullptr;
	}

	// Step 3: Insert MoveNode at insertion point
	if (InsertAfterOut) InsertAfterOut->BreakLinkTo(InsertBeforeIn ? InsertBeforeIn : nullptr);
	if (InsertAfterOut) InsertAfterOut->MakeLinkTo(MoveMapIn);
	if (InsertBeforeIn) MoveMapOut->MakeLinkTo(InsertBeforeIn);

	StageGraph->MarkPackageDirty();
	StageGraph->NotifyGraphChanged();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"),  ModuleName);
	Result->SetStringField(TEXT("after_module"), AfterModule);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("reorder_module: moved '%s' to after '%s' in stage '%s'; recompile requested."),
			*ModuleName, *AfterModule, *StageName),
		Result);
}

// ============================================================
//  bind_module_input (TODO-43)
// ============================================================
//
// Binds a module's Module.* input parameter to a stage-level Niagara attribute,
// creating the "override PMSet + PMGet" pattern used by the Niagara stack editor.
//
// Mechanism (no RequestNewTypedPin needed):
//   1. Create UNiagaraNodeParameterMapSet ("override node") before the FunctionCall
//   2. Add a typed input pin to it for `input_param` using UEdGraphNode::CreatePin
//   3. Create UNiagaraNodeParameterMapGet, add output pin for `bind_to` using CreatePin
//   4. Wire: PrevMapOut → OverrideNode MapIn
//          : PrevMapOut → PMGet MapIn  (fork — one out, two ins is valid)
//          : PMGet output → OverrideNode input pin
//          : OverrideNode MapOut → FuncCall MapIn
//   5. Set BoundPinNames via FMapProperty reflection (bypasses protected access)
//
// Required params:
//   system_path, emitter_name, stage, module_name
//   input_param  — full Module.* parameter name, e.g. "Module.Float To Send (As Struct Size)"
//   bind_to      — full attribute name to read, e.g. "Particles.WaveHeight"
//   bind_type    — type: float | vec2 | vec3 | vec4 | int | bool (default: float)
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteBindModuleInput(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, InputParam, BindTo, BindTypeStr;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("input_param"),  InputParam,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("bind_to"),      BindTo,      Err)) return Err.GetValue();
	}
	Params->TryGetStringField(TEXT("bind_type"), BindTypeStr);
	if (BindTypeStr.IsEmpty()) BindTypeStr = TEXT("float");

	// Resolve type
	auto ResolveType = [](const FString& T) -> FNiagaraTypeDefinition
	{
		FString Lower = T.ToLower();
		if (Lower == TEXT("vec2")) return FNiagaraTypeDefinition::GetVec2Def();
		if (Lower == TEXT("vec3")) return FNiagaraTypeDefinition::GetVec3Def();
		if (Lower == TEXT("vec4")) return FNiagaraTypeDefinition::GetVec4Def();
		if (Lower == TEXT("int"))  return FNiagaraTypeDefinition::GetIntDef();
		if (Lower == TEXT("bool")) return FNiagaraTypeDefinition::GetBoolDef();
		return FNiagaraTypeDefinition::GetFloatDef();
	};
	FNiagaraTypeDefinition BindTypeDef = ResolveType(BindTypeStr);

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	UNiagaraNodeFunctionCall* FuncCallNode = FindModuleByName(StageGraph, ModuleName);
	if (!FuncCallNode) return FMCPToolResult::Error(FString::Printf(TEXT("Module '%s' not found."), *ModuleName));

	// ── Step 1: Look up script variable GUID from module inner graph ──────────────────────
	// TODO-49: Also detect whether the parameter is a DI type (UClass != nullptr on TypeDef).
	// DI-typed parameters cannot use the PMSet/PMGet override pattern — they need a direct
	// BoundPinNames entry only. Skipping the PMSet creation for DI types prevents the broken
	// override node that caused compile errors in previous sessions.
	FGuid ScriptVarGuid;
	bool bIsDIType = false;
	FNiagaraTypeDefinition InputTypeDef;
	bool bTypeFromMetadata = false;
	UNiagaraGraph* InnerGraph = FuncCallNode->GetCalledGraph(); // NIAGARAEDITOR_API
	if (InnerGraph)
	{
		FName InputParamName(*InputParam);
		// Fallback: try with "Module." prefix if the raw name doesn't match
		FName InputParamNamePrefixed = InputParam.StartsWith(TEXT("Module."))
			? InputParamName
			: FName(*(FString(TEXT("Module.")) + InputParam));

		for (const auto& Pair : InnerGraph->GetAllMetaData()) // NIAGARAEDITOR_API
		{
			const FName VarName = Pair.Key.GetName();
			if (VarName == InputParamName || VarName == InputParamNamePrefixed)
			{
				ScriptVarGuid = Pair.Value->Metadata.GetVariableGuid();
				// DI types have a UClass associated with their FNiagaraTypeDefinition
				bIsDIType = (Pair.Key.GetType().GetClass() != nullptr);
				InputTypeDef = Pair.Key.GetType();
				bTypeFromMetadata = true;
				UE_LOG(LogUnrealClaude, Log,
					TEXT("bind_module_input: found script var '%s' guid=%s bIsDIType=%s"),
					*VarName.ToString(), *ScriptVarGuid.ToString(),
					bIsDIType ? TEXT("true") : TEXT("false"));
				break;
			}
		}
		if (!ScriptVarGuid.IsValid())
		{
			// Log available variables to aid debugging
			UE_LOG(LogUnrealClaude, Warning,
				TEXT("bind_module_input: input_param '%s' (tried '%s') not found in inner graph metadata. "
					 "BoundPinNames will NOT be set. Available vars:"),
				*InputParam, *InputParamNamePrefixed.ToString());
			for (const auto& Pair : InnerGraph->GetAllMetaData())
				UE_LOG(LogUnrealClaude, Warning, TEXT("  '%s' (type_class=%s)"),
					*Pair.Key.GetName().ToString(),
					Pair.Key.GetType().GetClass() ? *Pair.Key.GetType().GetClass()->GetName() : TEXT("none"));
		}
	}
	else
	{
		UE_LOG(LogUnrealClaude, Warning,
			TEXT("bind_module_input: could not get inner graph for '%s'; GUID lookup skipped."), *ModuleName);
	}

	// ── Stack-override binding (replicates FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput) ──
	// The real data-flow mechanism for module inputs is an override PMSet node in the stage
	// graph with a pin named "<FunctionCallName>.<InputName>", fed by a PMGet that reads the
	// linked parameter. NOTE: BoundPinNames is declared SkipForCompileHash — it is pure UI
	// bookkeeping and does NOT affect compilation; the earlier BoundPinNames-only approach
	// (TODO-49) compiled but never flowed any data.
	if (!bTypeFromMetadata)
		InputTypeDef = ResolveType(BindTypeStr);

	// Canonical override pin name: "<FunctionCallName>.<LeafInputName>".
	// Accept input_param as "X", "Module.X", or "<FuncName>.X".
	FString LeafName = InputParam;
	const FString FuncName = FuncCallNode->GetFunctionName();
	if (LeafName.StartsWith(TEXT("Module.")))            LeafName = LeafName.Mid(7);
	else if (LeafName.StartsWith(FuncName + TEXT("."))) LeafName = LeafName.Mid(FuncName.Len() + 1);
	const FString OverridePinName = FuncName + TEXT(".") + LeafName;

	UClass* PMSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
	UClass* PMGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
	if (!PMSetClass || !PMGetClass)
		return FMCPToolResult::Error(TEXT("Could not locate NiagaraNodeParameterMapSet/Get classes."));

	UEdGraphPin* FuncCallMapIn = FindParamMapPin(FuncCallNode, EGPD_Input);
	if (!FuncCallMapIn)
		return FMCPToolResult::Error(FString::Printf(TEXT("Module '%s' has no param-map input pin."), *ModuleName));

	// Find or create the override PMSet directly before the FunctionCall node.
	// Only reuse a PMSet that is EXCLUSIVELY this module's override — its map-out must feed
	// nothing but this module. Reusing a shared/pre-existing PMSet pollutes it with another
	// module's pins and (observed 2026-06-10) can close parameter-map cycles that stack-overflow
	// the recursive BuildParameterMapHistory.
	UEdGraphNode* OverrideNode = nullptr;
	bool bOverrideWasExisting = false;
	if (FuncCallMapIn->LinkedTo.Num() > 0)
	{
		UEdGraphNode* PrevNode = FuncCallMapIn->LinkedTo[0]->GetOwningNode();
		if (PrevNode && PrevNode->GetClass() == PMSetClass)
		{
			UEdGraphPin* PrevPMSetOut = FindParamMapPin(PrevNode, EGPD_Output);
			const bool bExclusive = PrevPMSetOut && PrevPMSetOut->LinkedTo.Num() == 1 &&
				PrevPMSetOut->LinkedTo[0] == FuncCallMapIn;
			if (bExclusive)
			{
				OverrideNode = PrevNode;
				bOverrideWasExisting = true;
			}
		}
	}
	UEdGraphPin* PrevMapOut = (FuncCallMapIn->LinkedTo.Num() > 0) ? FuncCallMapIn->LinkedTo[0] : nullptr;

	if (!OverrideNode)
	{
		OverrideNode = NewObject<UEdGraphNode>(StageGraph, PMSetClass, NAME_None, RF_Transactional);
		OverrideNode->CreateNewGuid();
		OverrideNode->NodePosX = FuncCallNode->NodePosX - 300;
		OverrideNode->NodePosY = FuncCallNode->NodePosY;
		StageGraph->AddNode(OverrideNode, false, false);
		OverrideNode->AllocateDefaultPins();

		UEdGraphPin* OvMapIn  = FindParamMapPin(OverrideNode, EGPD_Input);
		UEdGraphPin* OvMapOut = FindParamMapPin(OverrideNode, EGPD_Output);
		if (PrevMapOut && OvMapIn && OvMapOut)
		{
			// Replicates GetOrCreateStackFunctionOverrideNode: reroute ALL of PrevMapOut's
			// downstream links (this module AND any siblings sharing the source) through the
			// override node, then feed PrevMapOut exclusively into it. Keeps the map chain
			// linear even in GPU-stage graphs with fork wiring.
			TArray<UEdGraphPin*> Downstream = PrevMapOut->LinkedTo;
			for (UEdGraphPin* Down : Downstream)
			{
				PrevMapOut->BreakLinkTo(Down);
				OvMapOut->MakeLinkTo(Down);
			}
			PrevMapOut->MakeLinkTo(OvMapIn);
		}
		else if (OvMapOut)
		{
			OvMapOut->MakeLinkTo(FuncCallMapIn);
		}
	}

	// PrevMapOut after potential override insertion: the pin feeding the override node's map-in
	UEdGraphPin* OvMapIn = FindParamMapPin(OverrideNode, EGPD_Input);
	UEdGraphPin* StackSourceMapOut = (OvMapIn && OvMapIn->LinkedTo.Num() > 0) ? OvMapIn->LinkedTo[0] : nullptr;

	const FEdGraphPinType BindPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(InputTypeDef);

	// Reuse an existing override pin for this input (rebind) or create a new one
	UEdGraphPin* OverridePin = OverrideNode->FindPin(FName(*OverridePinName), EGPD_Input);
	if (OverridePin)
	{
		OverridePin->BreakAllPinLinks();
	}
	else
	{
		OverridePin = OverrideNode->CreatePin(EGPD_Input, BindPinType, FName(*OverridePinName));
		if (!OverridePin)
			return FMCPToolResult::Error(TEXT("CreatePin failed on override PMSet node."));
		MovePinBeforeAddPin(OverrideNode, OverridePin);
	}

	// PMGet reading the bound parameter, map-in forked from the stack source
	UEdGraphNode* GetNode = NewObject<UEdGraphNode>(StageGraph, PMGetClass, NAME_None, RF_Transactional);
	GetNode->CreateNewGuid();
	GetNode->NodePosX = OverrideNode->NodePosX - 300;
	GetNode->NodePosY = OverrideNode->NodePosY + 150;
	StageGraph->AddNode(GetNode, false, false);
	GetNode->AllocateDefaultPins();

	UEdGraphPin* GetOutputPin = GetNode->CreatePin(EGPD_Output, BindPinType, FName(*BindTo));
	if (!GetOutputPin)
		return FMCPToolResult::Error(TEXT("CreatePin failed on PMGet node."));
	MovePinBeforeAddPin(GetNode, GetOutputPin);

	bool bMapWired = false;
	UEdGraphPin* GetMapIn = FindParamMapPin(GetNode, EGPD_Input);
	if (GetMapIn && StackSourceMapOut)
	{
		StackSourceMapOut->MakeLinkTo(GetMapIn);
		bMapWired = true;
	}
	GetOutputPin->MakeLinkTo(OverridePin);

	// ── Cycle guard ────────────────────────────────────────────────────────────────────────
	// A revisited node on the current upstream path means this bind closed a loop —
	// BuildParameterMapHistory recurses forever on cycles (EXCEPTION_STACK_OVERFLOW observed
	// 2026-06-10). Detect it NOW, roll back this bind's wiring, and fail loudly instead of
	// leaving a poisoned graph that crashes the editor on the next compile.
	{
		TSet<UEdGraphNode*> OnPath;
		TFunction<bool(UEdGraphNode*, int32)> HasCycle;
		HasCycle = [&HasCycle, &OnPath](UEdGraphNode* Node, int32 Depth) -> bool
		{
			if (!Node) return false;
			if (Depth > 512) return true; // pathologically deep == treat as cycle
			if (OnPath.Contains(Node)) return true;
			OnPath.Add(Node);
			bool bCycle = false;
			for (UEdGraphPin* P : Node->Pins)
			{
				if (!P || P->Direction != EGPD_Input) continue;
				for (UEdGraphPin* L : P->LinkedTo)
				{
					if (L && HasCycle(L->GetOwningNode(), Depth + 1)) { bCycle = true; break; }
				}
				if (bCycle) break;
			}
			OnPath.Remove(Node);
			return bCycle;
		};

		if (HasCycle(FuncCallNode, 0))
		{
			// Roll back: remove the PMGet, restore the override insertion if we created it
			GetOutputPin->BreakAllPinLinks();
			if (UEdGraphPin* GetMapInPin = FindParamMapPin(GetNode, EGPD_Input))
				GetMapInPin->BreakAllPinLinks();
			StageGraph->RemoveNode(GetNode);

			if (!bOverrideWasExisting)
			{
				UEdGraphPin* OvMapIn2  = FindParamMapPin(OverrideNode, EGPD_Input);
				UEdGraphPin* OvMapOut2 = FindParamMapPin(OverrideNode, EGPD_Output);
				if (OvMapIn2 && OvMapOut2 && PrevMapOut)
				{
					TArray<UEdGraphPin*> Down = OvMapOut2->LinkedTo;
					for (UEdGraphPin* D : Down)
					{
						OvMapOut2->BreakLinkTo(D);
						PrevMapOut->MakeLinkTo(D);
					}
				}
				StageGraph->RemoveNode(OverrideNode);
			}
			else if (OverridePin)
			{
				OverrideNode->RemovePin(OverridePin);
			}

			return FMCPToolResult::Error(FString::Printf(
				TEXT("bind_module_input: wiring '%s' ← '%s' would create a parameter-map cycle — "
				     "rolled back, nothing changed. The stage graph topology around '%s' needs inspection "
				     "(use get_module_source / open the stage graph in editor)."),
				*OverridePinName, *BindTo, *ModuleName));
		}
	}

	// Register the read parameter in the stage graph (editor does Graph->AddParameter here)
	RegisterGraphParameter(StageGraph, FNiagaraVariable(InputTypeDef, FName(*BindTo)));

	// BoundPinNames nicety for the stack UI (no compile effect — SkipForCompileHash)
	bool bBoundNameSet = false;
	if (ScriptVarGuid.IsValid())
	{
		if (FMapProperty* BoundPinProp = FindFProperty<FMapProperty>(
				FuncCallNode->GetClass(), TEXT("BoundPinNames")))
		{
			void* RawMapPtr = BoundPinProp->ContainerPtrToValuePtr<void>(FuncCallNode);
			TMap<FGuid, FName>* BoundMap = reinterpret_cast<TMap<FGuid, FName>*>(RawMapPtr);
			BoundMap->Add(ScriptVarGuid, FName(*BindTo));
			bBoundNameSet = true;
		}
	}

	BumpGraphChangeId(StageGraph, TEXT("bind_module_input"));
	StageGraph->MarkPackageDirty();
	StageGraph->NotifyGraphChanged();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_name"),        ModuleName);
	Result->SetStringField(TEXT("input_param"),        InputParam);
	Result->SetStringField(TEXT("override_pin"),       OverridePinName);
	Result->SetStringField(TEXT("bind_to"),            BindTo);
	Result->SetStringField(TEXT("bind_type"),          bTypeFromMetadata ? InputTypeDef.GetNameText().ToString() : BindTypeStr);
	Result->SetBoolField  (TEXT("di_type"),            bIsDIType);
	Result->SetBoolField  (TEXT("override_node_new"),  !bOverrideWasExisting);
	Result->SetBoolField  (TEXT("map_wired"),          bMapWired);
	Result->SetBoolField  (TEXT("bound_pin_name_set"), bBoundNameSet);

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("bind_module_input: override pin '%s' ← PMGet '%s' (%s); map_wired=%s; recompile requested."),
			*OverridePinName, *BindTo,
			bIsDIType ? TEXT("DI") : TEXT("value"),
			bMapWired ? TEXT("ok") : TEXT("FAILED")),
		Result);
}

// ============================================================
//  configure_di_parameter (TODO-53)
// ============================================================
//
// Sets properties on a NiagaraDataInterface instance stored as a user parameter in a NiagaraSystem.
// This is the MCP path for Step 2 of the RT2DArray sampling plan:
//   - Set bInheritUserParameterSettings = true  (DI reads RT size/format from bound user param)
//   - Set RenderTargetUserParameter binding → User.RT_VertAttribs
//
// Required params:
//   system_path  — path to NiagaraSystem asset
//   param_name   — name of the user parameter, e.g. "User.iFFT_RT"
//
// Optional:
//   inherit_user_param_settings  (bool) — set bInheritUserParameterSettings on the DI
//   user_param_binding           (string) — bind RenderTargetUserParameter to this param name,
//                                           e.g. "User.RT_VertAttribs"
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteConfigureDIParameter(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, ParamName;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("param_name"),  ParamName,  Err)) return Err.GetValue();
	}

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	TArray<FNiagaraVariable> Variables;
	Store.GetParameters(Variables);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& V : Variables)
	{
		if (V.GetName().ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{ FoundVar = &V; break; }
	}
	if (!FoundVar)
	{
		TArray<FString> Names;
		for (const FNiagaraVariable& V : Variables) Names.Add(V.GetName().ToString());
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' not found. Available: %s"),
			*ParamName, *FString::Join(Names, TEXT(", "))));
	}

	// Must be a DI parameter
	if (!FoundVar->GetType().IsDataInterface())
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' is not a DataInterface type — configure_di_parameter requires a DI user param."),
			*ParamName));

	UNiagaraDataInterface* DI = Store.GetDataInterface(*FoundVar);
	if (!DI)
		return FMCPToolResult::Error(FString::Printf(
			TEXT("No DI instance found for parameter '%s'. Was it added with add_system_user_parameter?"),
			*ParamName));

	DI->Modify();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("param_name"),  ParamName);
	Result->SetStringField(TEXT("di_class"),    DI->GetClass()->GetName());

	TArray<FString> ActionsApplied;

	// ── inherit_user_param_settings ──────────────────────────────────────────────────────
	bool bInherit = false;
	if (Params->TryGetBoolField(TEXT("inherit_user_param_settings"), bInherit))
	{
		bool bSet = false;
		if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(DI->GetClass(), TEXT("bInheritUserParameterSettings")))
		{
			Prop->SetPropertyValue_InContainer(DI, bInherit);
			bSet = true;
			ActionsApplied.Add(FString::Printf(TEXT("bInheritUserParameterSettings=%s"), bInherit ? TEXT("true") : TEXT("false")));
			UE_LOG(LogUnrealClaude, Log, TEXT("configure_di_parameter: set bInheritUserParameterSettings=%s on '%s'"),
				bInherit ? TEXT("true") : TEXT("false"), *ParamName);
		}
		else
		{
			UE_LOG(LogUnrealClaude, Warning,
				TEXT("configure_di_parameter: 'bInheritUserParameterSettings' not found on %s"),
				*DI->GetClass()->GetName());
		}
		Result->SetBoolField(TEXT("inherit_user_param_settings_set"), bSet);
	}

	// ── user_param_binding (RenderTargetUserParameter or similar) ─────────────────────────
	FString UserParamBinding;
	if (Params->TryGetStringField(TEXT("user_param_binding"), UserParamBinding) && !UserParamBinding.IsEmpty())
	{
		// The property is "RenderTargetUserParameter" of struct type FNiagaraUserParameterBinding.
		// FNiagaraUserParameterBinding contains a single FNiagaraVariable "Parameter".
		// We set Parameter.Name and Parameter type to match the target user param.
		bool bBound = false;
		FString BindReport = TEXT("not set");

		// Find property by name — try "RenderTargetUserParameter" first, then a generic scan
		// for any FStructProperty whose struct is FNiagaraUserParameterBinding.
		FStructProperty* BindingProp = nullptr;
		if (FStructProperty* P = FindFProperty<FStructProperty>(DI->GetClass(), TEXT("RenderTargetUserParameter")))
		{
			BindingProp = P;
		}
		else
		{
			// Fallback: scan for first FStructProperty with struct name "NiagaraUserParameterBinding"
			for (TFieldIterator<FStructProperty> It(DI->GetClass()); It; ++It)
			{
				if (It->Struct && It->Struct->GetName().Contains(TEXT("NiagaraUserParameterBinding")))
				{
					BindingProp = *It;
					break;
				}
			}
		}

		if (BindingProp)
		{
			void* BindingPtr = BindingProp->ContainerPtrToValuePtr<void>(DI);
			// FNiagaraUserParameterBinding layout: FNiagaraVariable Parameter (only field)
			// FNiagaraVariable = FNiagaraTypeDefinition Type + FName Name
			// We look up the target variable to get its type, then set Name.
			FNiagaraVariable* TargetVar = nullptr;
			for (FNiagaraVariable& V : Variables)
			{
				if (V.GetName().ToString().Equals(UserParamBinding, ESearchCase::IgnoreCase))
				{ TargetVar = &V; break; }
			}

			// Set the binding's Parameter (FNiagaraVariable) — TYPE AND NAME.
			// A name-only binding does NOT resolve (NI016: the DI silently fell back to its
			// black default texture and all samples returned 0). When the target variable is
			// found in the system's exposed parameters, assign the whole FNiagaraVariable so
			// the type matches exactly; name-only is kept as a last-resort fallback.
			if (FStructProperty* VarProp = FindFProperty<FStructProperty>(BindingProp->Struct, TEXT("Parameter")))
			{
				void* VarPtr = VarProp->ContainerPtrToValuePtr<void>(BindingPtr);
				if (TargetVar)
				{
					*reinterpret_cast<FNiagaraVariable*>(VarPtr) = *TargetVar;
					bBound = true;
					BindReport = FString::Printf(TEXT("%s.Parameter = '%s' (type '%s')"),
						*BindingProp->GetName(), *UserParamBinding,
						*TargetVar->GetType().GetNameText().ToString());
				}
				else if (FNameProperty* NameProp = FindFProperty<FNameProperty>(VarProp->Struct, TEXT("Name")))
				{
					NameProp->SetPropertyValue_InContainer(VarPtr, FName(*UserParamBinding));
					bBound = true;
					BindReport = FString::Printf(
						TEXT("%s.Parameter.Name = '%s' (WARNING: target var not found in exposed params — "
						     "type not set, binding may not resolve)"),
						*BindingProp->GetName(), *UserParamBinding);
				}
			}

			ActionsApplied.Add(BindReport);
		}
		else
		{
			BindReport = FString::Printf(
				TEXT("WARNING: RenderTargetUserParameter not found on %s — property name may differ"),
				*DI->GetClass()->GetName());
			ActionsApplied.Add(BindReport);
			// Log all struct properties to help diagnose
			for (TFieldIterator<FStructProperty> It(DI->GetClass()); It; ++It)
			{
				UE_LOG(LogUnrealClaude, Warning, TEXT("  StructProp: '%s' (struct: %s)"),
					*It->GetName(), It->Struct ? *It->Struct->GetName() : TEXT("null"));
			}
		}
		Result->SetBoolField(TEXT("user_param_binding_set"), bBound);
		Result->SetStringField(TEXT("user_param_binding_report"), BindReport);
	}

	// ── Finalize ──────────────────────────────────────────────────────────────────────────
	System->MarkPackageDirty();
	System->RequestCompile(false);

	const FString ActionsStr = ActionsApplied.Num() > 0
		? FString::Join(ActionsApplied, TEXT("; "))
		: TEXT("no actions (no optional params specified)");

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("configure_di_parameter (TODO-53): '%s' (%s) — %s; recompile requested."),
			*ParamName, *DI->GetClass()->GetName(), *ActionsStr),
		Result);
}

// ============================================================
//  add_scratchpad_module_param (NI010)
// ============================================================
//
// Adds a typed input or output parameter to an existing scratch-pad FunctionScript module.
// For an input: creates a UNiagaraNodeParameterMapGet in the inner graph reading
//   Module.<param_name>, wires it to a new typed input pin on the CustomHlsl node.
// For an output: creates a UNiagaraNodeParameterMapSet writing Output.Module.<param_name>,
//   wires it from a new typed output pin on the CustomHlsl node, spliced before OutputModule.
//
// This replaces the manual "Parameters panel → Module Inputs +" editor workflow.
// After all params are added, call rebuild_scratchpad_inner_graph to set the HLSL body.

// (MovePinBeforeAddPin and RegisterGraphParameter helpers are defined near the top of this
// file, next to BumpGraphChangeId — they are shared with bind_module_input.)
//
// Required params:
//   system_path, emitter_name, stage, module_name
//   param_name  — local name, e.g. "PatchLength" (NOT "Module.PatchLength")
//   param_type  — type string: float | vec3 | bool | RenderTarget2DArray | etc.
//   direction   — "input" | "output"
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteAddScratchpadModuleParam(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName, ModuleName, ParamName, ParamTypeStr, DirectionStr;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("module_name"),  ModuleName,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("param_name"),   ParamName,   Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("param_type"),   ParamTypeStr, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("direction"),    DirectionStr, Err)) return Err.GetValue();
	}
	{
		TOptional<FMCPToolResult> PathErr;
		if (!ValidateBlueprintPathParam(SystemPath, PathErr)) return PathErr.GetValue();
	}

	const bool bIsInput = DirectionStr.ToLower() != TEXT("output");

	// ── Load system / emitter / stage ───────────────────────────────────────────────────────
	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	// ── Find FunctionCall → FunctionScript → inner graph ────────────────────────────────────
	UNiagaraNodeFunctionCall* FuncCall = FindModuleByName(StageGraph, ModuleName);
	if (!FuncCall) return FMCPToolResult::Error(FString::Printf(TEXT("Module '%s' not found in stage '%s'."), *ModuleName, *StageName));

	UNiagaraScript* InnerScript = FuncCall->FunctionScript;
	if (!InnerScript) return FMCPToolResult::Error(FString::Printf(
		TEXT("Module '%s' has no FunctionScript — it may be an asset-based module, not a scratch-pad module."), *ModuleName));

	UNiagaraGraph* InnerGraph = GetScriptGraph(InnerScript);
	if (!InnerGraph) return FMCPToolResult::Error(FString::Printf(
		TEXT("Module '%s' FunctionScript has no inner graph."), *ModuleName));

	// ── Resolve type ─────────────────────────────────────────────────────────────────────────
	FNiagaraTypeDefinition TypeDef = BuildNiagaraTypeDef(ParamTypeStr);
	if (!TypeDef.IsValid()) return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown param_type '%s'. Supported: float, int, bool, vec2, vec3, vec4, color, "
		     "RenderTarget2DArray, Texture2DArray, or any DI class name."), *ParamTypeStr));

	const FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(TypeDef);

	// ── Find CustomHlsl node in inner graph ──────────────────────────────────────────────────
	TArray<UNiagaraNodeCustomHlsl*> HlslNodes;
	InnerGraph->GetNodesOfClass<UNiagaraNodeCustomHlsl>(HlslNodes);
	if (HlslNodes.Num() == 0) return FMCPToolResult::Error(FString::Printf(
		TEXT("No CustomHlsl node found in inner graph of '%s'. "
		     "Was create_scratchpad_module called first?"), *ModuleName));
	UNiagaraNodeCustomHlsl* HlslNode = HlslNodes[0];

	// ── Find InputMap node (has map-out, no map-in) ──────────────────────────────────────────
	UEdGraphNode* InputMapNode = nullptr;
	for (UEdGraphNode* Node : InnerGraph->Nodes)
	{
		if (!Node) continue;
		if (FindParamMapPin(Node, EGPD_Output) != nullptr && FindParamMapPin(Node, EGPD_Input) == nullptr)
		{ InputMapNode = Node; break; }
	}

	// ── Find OutputModule node ────────────────────────────────────────────────────────────────
	UNiagaraNodeOutput* OutNode = nullptr;
	{
		TArray<UNiagaraNodeOutput*> OutNodes;
		InnerGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutNodes);
		for (UNiagaraNodeOutput* O : OutNodes)
		{
			if (O && O->GetUsage() == ENiagaraScriptUsage::Module) { OutNode = O; break; }
		}
	}

	// ── PMGet / PMSet class lookup ────────────────────────────────────────────────────────────
	UClass* PMGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
	UClass* PMSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
	if (!PMGetClass || !PMSetClass) return FMCPToolResult::Error(
		TEXT("Could not locate NiagaraNodeParameterMapGet/Set classes."));

	FString WiringReport;

	// Dotted param_name = direct attribute access: the Map Get/Set reads/writes that exact
	// stage attribute (e.g. "Particles.Position", "User.iFFT_PatchLength", "Particles.WaveHeight").
	// This makes the module self-contained — no stack-level bind_module_input needed.
	// Undotted names keep the classic Module.* interface (bindable in the stack).
	// The HLSL-side variable is always the leaf name ("Position", "WaveHeight", ...).
	const bool bDirectAttribute = ParamName.Contains(TEXT("."));
	FString LocalName = ParamName;
	{
		int32 DotIdx;
		if (ParamName.FindLastChar(TCHAR('.'), DotIdx))
			LocalName = ParamName.Mid(DotIdx + 1);
	}

	if (bIsInput)
	{
		// ── Add to CustomHlsl Signature.Inputs + typed input pin ─────────────────────────────
		// BuildParameterMapHistory requires pin index i ↔ Signature.Inputs[i] alignment with the
		// Add pin last — hence MovePinBeforeAddPin after CreatePin (see helper above).
		HlslNode->Signature.Inputs.Add(FNiagaraVariable(TypeDef, FName(*LocalName)));
		UEdGraphPin* HlslInputPin = HlslNode->CreatePin(EGPD_Input, PinType, FName(*LocalName));
		MovePinBeforeAddPin(HlslNode, HlslInputPin);

		// ── Create Map Get node ────────────────────────────────────────────────────────────────
		UEdGraphNode* MapGetNode = NewObject<UEdGraphNode>(InnerGraph, PMGetClass, NAME_None, RF_Transactional);
		MapGetNode->CreateNewGuid();
		MapGetNode->NodePosX = HlslNode->NodePosX - 300;
		// Spread vertically by number of existing input params to avoid overlap
		int32 ExistingInputs = 0;
		for (UEdGraphPin* P : HlslNode->Pins) { if (P && P->Direction == EGPD_Input) ExistingInputs++; }
		MapGetNode->NodePosY = HlslNode->NodePosY + ExistingInputs * 80;
		InnerGraph->AddNode(MapGetNode, false, false);
		MapGetNode->AllocateDefaultPins();

		// ── Add typed output pin: direct attribute or Module.<ParamName> ──────────────────────
		const FString FullParamName = bDirectAttribute ? ParamName : (TEXT("Module.") + ParamName);
		UEdGraphPin* MapGetOutputPin = MapGetNode->CreatePin(EGPD_Output, PinType, FName(*FullParamName));
		MovePinBeforeAddPin(MapGetNode, MapGetOutputPin);
		RegisterGraphParameter(InnerGraph, FNiagaraVariable(TypeDef, FName(*FullParamName)));

		// ── Wire: InputMap map-out → MapGet map-in (fork, does not break existing connections) ─
		UEdGraphPin* InputMapOut = InputMapNode ? FindParamMapPin(InputMapNode, EGPD_Output) : nullptr;
		UEdGraphPin* MapGetMapIn = FindParamMapPin(MapGetNode, EGPD_Input);
		bool bMapGetWired = false;
		if (InputMapOut && MapGetMapIn)
		{
			InputMapOut->MakeLinkTo(MapGetMapIn);
			bMapGetWired = true;
		}

		// ── Wire: MapGet typed output → CustomHlsl typed input ────────────────────────────────
		bool bDataWired = false;
		if (MapGetOutputPin && HlslInputPin)
		{
			MapGetOutputPin->MakeLinkTo(HlslInputPin);
			bDataWired = true;
		}

		WiringReport = FString::Printf(
			TEXT("MapGet created (%s → %s input pin); map_wired=%s data_wired=%s"),
			*FullParamName, *LocalName,
			bMapGetWired ? TEXT("ok") : TEXT("FAILED (InputMapNode=%s)"),
			bDataWired   ? TEXT("ok") : TEXT("FAILED (pins null)"));

		UE_LOG(LogUnrealClaude, Log, TEXT("add_scratchpad_module_param (input): %s"), *WiringReport);
	}
	else
	{
		// ── Add to CustomHlsl Signature.Outputs + typed output pin ───────────────────────────
		// Same Add-pin-last invariant as the input case (see MovePinBeforeAddPin).
		HlslNode->Signature.Outputs.Add(FNiagaraVariable(TypeDef, FName(*LocalName)));
		UEdGraphPin* HlslOutputPin = HlslNode->CreatePin(EGPD_Output, PinType, FName(*LocalName));
		MovePinBeforeAddPin(HlslNode, HlslOutputPin);

		// ── Create Map Set node ────────────────────────────────────────────────────────────────
		UEdGraphNode* MapSetNode = NewObject<UEdGraphNode>(InnerGraph, PMSetClass, NAME_None, RF_Transactional);
		MapSetNode->CreateNewGuid();
		MapSetNode->NodePosX = OutNode ? OutNode->NodePosX - 200 : HlslNode->NodePosX + 300;
		MapSetNode->NodePosY = HlslNode->NodePosY;
		InnerGraph->AddNode(MapSetNode, false, false);
		MapSetNode->AllocateDefaultPins();

		// ── Add typed input pin: direct attribute or Output.Module.<ParamName> ────────────────
		const FString FullParamName = bDirectAttribute ? ParamName : (TEXT("Output.Module.") + ParamName);
		UEdGraphPin* MapSetInputPin = MapSetNode->CreatePin(EGPD_Input, PinType, FName(*FullParamName));
		MovePinBeforeAddPin(MapSetNode, MapSetInputPin);
		RegisterGraphParameter(InnerGraph, FNiagaraVariable(TypeDef, FName(*FullParamName)));

		// ── Wire: CustomHlsl typed output → MapSet typed input ────────────────────────────────
		bool bDataWired = false;
		if (HlslOutputPin && MapSetInputPin)
		{
			HlslOutputPin->MakeLinkTo(MapSetInputPin);
			bDataWired = true;
		}

		// ── Splice MapSet into the map chain just before OutputModule ─────────────────────────
		// Pattern: [Tail] → OutNode  becomes  [Tail] → MapSet → OutNode
		bool bMapSpliced = false;
		UEdGraphPin* MapSetMapIn  = FindParamMapPin(MapSetNode, EGPD_Input);
		UEdGraphPin* MapSetMapOut = FindParamMapPin(MapSetNode, EGPD_Output);
		UEdGraphPin* OutMapIn     = OutNode ? FindParamMapPin(OutNode, EGPD_Input) : nullptr;

		if (MapSetMapIn && MapSetMapOut && OutMapIn)
		{
			UEdGraphPin* TailMapOut = (OutMapIn->LinkedTo.Num() > 0) ? OutMapIn->LinkedTo[0] : nullptr;
			if (TailMapOut)
			{
				OutMapIn->BreakLinkTo(TailMapOut);
				TailMapOut->MakeLinkTo(MapSetMapIn);
			}
			else
			{
				// Nothing before OutputModule — find what's connected to HlslNode's map-out
				UEdGraphPin* HlslMapOut = FindParamMapPin(HlslNode, EGPD_Output);
				if (HlslMapOut && HlslMapOut->LinkedTo.Num() > 0)
				{
					UEdGraphPin* AfterHlsl = HlslMapOut->LinkedTo[0];
					HlslMapOut->BreakLinkTo(AfterHlsl);
					HlslMapOut->MakeLinkTo(MapSetMapIn);
					TailMapOut = MapSetMapOut; // will link to AfterHlsl below
					MapSetMapOut->MakeLinkTo(AfterHlsl);
					bMapSpliced = true;
				}
			}
			if (!bMapSpliced)
			{
				MapSetMapOut->MakeLinkTo(OutMapIn);
				bMapSpliced = true;
			}
		}

		WiringReport = FString::Printf(
			TEXT("MapSet created (%s ← %s output pin); data_wired=%s map_spliced=%s"),
			*FullParamName, *LocalName,
			bDataWired   ? TEXT("ok") : TEXT("FAILED"),
			bMapSpliced  ? TEXT("ok") : TEXT("FAILED"));

		UE_LOG(LogUnrealClaude, Log, TEXT("add_scratchpad_module_param (output): %s"), *WiringReport);
	}

	InnerGraph->MarkPackageDirty();
	InnerScript->Modify();
	System->MarkPackageDirty();
	BumpGraphChangeId(InnerGraph, TEXT("add_scratchpad_module_param"));

	bool bSkipCompile = false;
	Params->TryGetBoolField(TEXT("skip_compile"), bSkipCompile);
	if (!bSkipCompile)
	{
		InnerGraph->NotifyGraphChanged();
		System->RequestCompile(false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"),  SystemPath);
	Result->SetStringField(TEXT("module_name"),  ModuleName);
	Result->SetStringField(TEXT("param_name"),   ParamName);
	Result->SetStringField(TEXT("param_type"),   ParamTypeStr);
	Result->SetStringField(TEXT("direction"),    bIsInput ? TEXT("input") : TEXT("output"));
	Result->SetStringField(TEXT("full_param"),   bDirectAttribute ? ParamName : ((bIsInput ? TEXT("Module.") : TEXT("Output.Module.")) + ParamName));
	Result->SetStringField(TEXT("hlsl_name"),    LocalName);
	Result->SetStringField(TEXT("wiring"),       WiringReport);
	Result->SetBoolField  (TEXT("skip_compile"), bSkipCompile);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("add_scratchpad_module_param: added %s '%s' (type=%s) to module '%s'; %s."),
			bIsInput ? TEXT("input") : TEXT("output"), *ParamName, *ParamTypeStr, *ModuleName,
			bSkipCompile ? TEXT("compile skipped") : TEXT("recompile requested")),
		Result);
}

// ============================================================
//  dump_stage_graph (read-back for stage wiring)
// ============================================================
//
// Read-only: dumps every node and every pin link in a stage graph.
// Added 2026-06-10 after two blind-debugging editor crashes — the Feedback Loop
// Requirement demands a read-back tool for structural wiring work.
//
FMCPToolResult FMCPTool_NiagaraModify::ExecuteDumpStageGraph(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath, EmitterName, StageName;
	{
		TOptional<FMCPToolResult> Err;
		if (!ExtractRequiredString(Params, TEXT("system_path"),  SystemPath,  Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("emitter_name"), EmitterName, Err)) return Err.GetValue();
		if (!ExtractRequiredString(Params, TEXT("stage"),        StageName,   Err)) return Err.GetValue();
	}

	UNiagaraSystem* System = LoadNiagaraSystem(SystemPath);
	if (!System) return FMCPToolResult::Error(FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));

	int32 HandleIdx = INDEX_NONE;
	FVersionedNiagaraEmitterData* EmData = GetEmitterDataByName(System, EmitterName, HandleIdx);
	if (!EmData) return FMCPToolResult::Error(FString::Printf(TEXT("Emitter '%s' not found."), *EmitterName));

	UNiagaraScript* StageScript = GetStageScript(EmData, StageName);
	if (!StageScript) return FMCPToolResult::Error(FString::Printf(TEXT("Stage '%s' not found."), *StageName));

	UNiagaraGraph* StageGraph = GetScriptGraph(StageScript);
	if (!StageGraph) return FMCPToolResult::Error(TEXT("Stage has no graph."));

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (UEdGraphNode* Node : StageGraph->Nodes)
	{
		if (!Node) continue;
		TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
		NObj->SetStringField(TEXT("guid"),  Node->NodeGuid.ToString());
		NObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PObj->SetStringField(TEXT("dir"),  Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
			PObj->SetStringField(TEXT("type"), NiagaraPinTypeToString(Pin->PinType));
			if (Pin->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> LinksArr;
				for (UEdGraphPin* L : Pin->LinkedTo)
				{
					if (!L) continue;
					UEdGraphNode* LNode = L->GetOwningNode();
					LinksArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s:%s"),
						LNode ? *LNode->NodeGuid.ToString() : TEXT("null"), *L->PinName.ToString())));
				}
				PObj->SetArrayField(TEXT("linked_to"), LinksArr);
			}
			if (!Pin->DefaultValue.IsEmpty())
				PObj->SetStringField(TEXT("default"), Pin->DefaultValue);
			PinsArr.Add(MakeShared<FJsonValueObject>(PObj));
		}
		NObj->SetArrayField(TEXT("pins"), PinsArr);
		NodesArr.Add(MakeShared<FJsonValueObject>(NObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("stage"),       StageName);
	Result->SetNumberField(TEXT("node_count"),  NodesArr.Num());
	Result->SetArrayField (TEXT("nodes"),       NodesArr);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("dump_stage_graph: %d node(s) in stage '%s'."), NodesArr.Num(), *StageName),
		Result);
}
