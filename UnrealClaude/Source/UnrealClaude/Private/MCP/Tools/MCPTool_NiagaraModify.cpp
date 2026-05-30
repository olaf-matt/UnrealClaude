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
#include "NiagaraEditorUtilities.h"

// Graph / package
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"

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
		{
			Names.Add(FString::Printf(TEXT("EventHandler_%d"), i));
		}
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
	return nullptr;
}

UNiagaraGraph* FMCPTool_NiagaraModify::GetScriptGraph(UNiagaraScript* Script)
{
	if (!Script) return nullptr;
	// In UE 5.7, GetSource takes a FGuid (version guid). FGuid() = default/base version.
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetSource(FGuid()));
	return Source ? Source->NodeGraph : nullptr;
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
		"Example (set_system_user_param — vec3):\n"
		"  { \"operation\": \"set_system_user_param\",\n"
		"    \"system_path\": \"/Game/Blueprints/ShallowWater/FX_ShallowWater\",\n"
		"    \"param_name\": \"User.GravityDir\", \"value\": {\"X\": 0.0, \"Y\": 0.0, \"Z\": -1.0} }"
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("Operation: list_emitters | list_stages | list_modules | add_module | "
			 "remove_module | set_module_input | set_system_user_param | compile"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("system_path"), TEXT("string"),
		TEXT("Asset path to the NiagaraSystem, e.g. /Game/Particles/FX_MySystem"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("emitter_name"), TEXT("string"),
		TEXT("Emitter handle display name or asset name (required for stage/module ops)"), false));
	Info.Parameters.Add(FMCPToolParameter(TEXT("stage"), TEXT("string"),
		TEXT("Stage name: EmitterSpawn | EmitterUpdate | ParticleSpawn | ParticleUpdate | "
			 "EventHandler_N (required for module ops)"), false));
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
		TEXT("Type hint for set_system_user_param: \"float\"|\"int\"|\"bool\"|\"vec3\"|\"color\". "
			 "Auto-detected from the system's exposed parameter store if omitted."), false));

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

	if (Lower == TEXT("list_emitters"))         return ExecuteListEmitters(Params);
	if (Lower == TEXT("list_stages"))           return ExecuteListStages(Params);
	if (Lower == TEXT("list_modules"))          return ExecuteListModules(Params);
	if (Lower == TEXT("add_module"))            return ExecuteAddModule(Params);
	if (Lower == TEXT("remove_module"))         return ExecuteRemoveModule(Params);
	if (Lower == TEXT("set_module_input"))      return ExecuteSetModuleInput(Params);
	if (Lower == TEXT("set_system_user_param")) return ExecuteSetSystemUserParam(Params);
	if (Lower == TEXT("compile"))               return ExecuteCompile(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation '%s'. Valid: list_emitters, list_stages, list_modules, "
			 "add_module, remove_module, set_module_input, set_system_user_param, compile"), *Operation));
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

	TArray<FString> StageNames = GetStageNames(Data);
	TArray<TSharedPtr<FJsonValue>> StagesArray;
	for (const FString& StageName : StageNames)
	{
		UNiagaraScript* Script = GetStageScript(Data, StageName);
		UNiagaraGraph* Graph = GetScriptGraph(Script);

		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		StageObj->SetStringField(TEXT("stage"), StageName);
		StageObj->SetBoolField(TEXT("has_graph"), Graph != nullptr);

		if (Graph)
		{
			TArray<UNiagaraNodeFunctionCall*> FuncNodes;
			Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FuncNodes);
			StageObj->SetNumberField(TEXT("module_count"), FuncNodes.Num());
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

	TArray<UNiagaraNodeFunctionCall*> FuncNodes;
	Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FuncNodes);

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

	// Find the output node — every stage graph has exactly one
	TArray<UNiagaraNodeOutput*> OutputNodes;
	Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
	if (OutputNodes.Num() == 0)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Stage '%s' graph has no UNiagaraNodeOutput node — cannot insert module."),
			*StageName));
	}
	UNiagaraNodeOutput* OutputNode = OutputNodes[0];

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
	TargetNode->BreakAllNodeLinks();
	Graph->RemoveNode(TargetNode);

	Graph->MarkPackageDirty();
	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_path"), SystemPath);
	Result->SetStringField(TEXT("emitter_name"), EmitterName);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetStringField(TEXT("module_removed"), RemovedName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed module '%s' from stage '%s'; system recompile requested."),
			*RemovedName, *StageName), Result);
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
	else
	{
		// Data interface or unknown type — not settable via simple value
		FString TypeName = TypeDef.GetName();
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Parameter '%s' has type '%s' which is not settable via set_system_user_param. "
				 "Settable types: float, int, bool, vec3, vec4, color. "
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
