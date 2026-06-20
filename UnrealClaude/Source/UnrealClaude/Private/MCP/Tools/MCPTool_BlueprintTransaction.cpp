// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintTransaction.h"
#include "MCP/MCPBlueprintLoadContext.h"
#include "BlueprintUtils.h"

// ─── Tool metadata ────────────────────────────────────────────────────────────

FMCPToolInfo FMCPTool_BlueprintTransaction::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("blueprint_transaction");
	Info.Description = TEXT(
		"Execute a full Blueprint graph-wiring script in one call — compiles once at the end.\n\n"
		"Use this instead of sequential connect_pins calls.  A graph that previously required\n"
		"16 round-trips (~8 min) now takes 1 call (~2 sec).\n\n"
		"─── REQUIRED ─────────────────────────────────────────────────────────\n"
		"  blueprint_path   Full asset path (e.g. '/Game/Blueprints/BP_Example')\n"
		"  ops              Array of operations (see below)\n\n"
		"─── OPTIONAL ────────────────────────────────────────────────────────\n"
		"  graph_name       Target graph (omit = EventGraph)\n"
		"  is_function_graph  true when graph_name is a function graph\n"
		"  pre_existing_refs  Map of ref_name → real_node_id for nodes already in graph\n"
		"                     (e.g. EventBeginPlay, EventTick GUIDs from blueprint_query)\n"
		"  compile_once_at_end  bool, default true — skip to defer compile\n\n"
		"─── OP TYPES ────────────────────────────────────────────────────────\n\n"
		"  add_node\n"
		"    Required: node_type  (same types as blueprint_modify add_node)\n"
		"    Optional: ref        local name for this node (used in later ops)\n"
		"              node_params  object with function/variable/event/etc.\n"
		"              pos_x, pos_y  canvas position\n"
		"              pin_values  object of {pin_name: default_value} to set\n"
		"    Inline shorthand: function/variable/event/target_class as top-level fields\n\n"
		"  connect_pins\n"
		"    Required: from_ref, to_ref  (ref string or literal node_id)\n"
		"    Optional: from_pin, to_pin  (omit to auto-connect first exec pins)\n\n"
		"  set_pin_value\n"
		"    Required: node_ref, pin_name, pin_value\n\n"
		"  delete_node\n"
		"    Required: node_ref  (ref string or literal node_id)\n\n"
		"─── PIN NAMES ───────────────────────────────────────────────────────\n"
		"  Exec input: 'execute'   Exec output: 'then'\n"
		"  Branch inputs: 'Condition'  outputs: 'then' (true) / 'else' (false)\n"
		"  Sequence outputs: 'then_0', 'then_1', ...\n\n"
		"─── EXAMPLE ─────────────────────────────────────────────────────────\n"
		"  { \"blueprint_path\": \"/Game/BP_Example\",\n"
		"    \"pre_existing_refs\": { \"bp\": \"27FEE3E4419D4942C7BB6F982B67E220\" },\n"
		"    \"ops\": [\n"
		"      { \"op\": \"add_node\", \"ref\": \"b1\", \"node_type\": \"Branch\", \"pos_x\": 300, \"pos_y\": 0 },\n"
		"      { \"op\": \"add_node\", \"ref\": \"vget\", \"node_type\": \"VariableGet\",\n"
		"        \"node_params\": { \"variable\": \"bActive\" }, \"pos_x\": 100, \"pos_y\": 80 },\n"
		"      { \"op\": \"connect_pins\", \"from_ref\": \"bp\", \"from_pin\": \"then\",\n"
		"        \"to_ref\": \"b1\", \"to_pin\": \"execute\" },\n"
		"      { \"op\": \"connect_pins\", \"from_ref\": \"vget\", \"from_pin\": \"bActive\",\n"
		"        \"to_ref\": \"b1\", \"to_pin\": \"Condition\" }\n"
		"    ] }"
	);

	Info.Parameters = {
		FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
			TEXT("Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_MyActor')."), true),

		FMCPToolParameter(TEXT("ops"), TEXT("array"),
			TEXT("Array of operations to execute: [{\"op\":\"add_node\",\"ref\":\"b1\",\"node_type\":\"Branch\",...}, ...]"), true),

		FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
			TEXT("Target graph name.  Omit for EventGraph.  For function graphs set is_function_graph=true too."), false),

		FMCPToolParameter(TEXT("is_function_graph"), TEXT("boolean"),
			TEXT("Set true when graph_name refers to a function graph.  Default false."), false, TEXT("false")),

		FMCPToolParameter(TEXT("pre_existing_refs"), TEXT("object"),
			TEXT("Map of ref_name → real node_id for nodes already in the graph.  "
			     "Get real node_ids via blueprint_query get_nodes.  "
			     "Example: {\"begin_play\": \"27FEE3E4419D4942C7BB6F982B67E220\"}"), false),

		FMCPToolParameter(TEXT("compile_once_at_end"), TEXT("boolean"),
			TEXT("Compile the Blueprint after all ops complete.  Default true.  Set false to defer."), false, TEXT("true")),
	};

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

// ─── Execute ──────────────────────────────────────────────────────────────────

FMCPToolResult FMCPTool_BlueprintTransaction::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Load Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
		return LoadError.GetValue();

	// Resolve graph
	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	bool bFunctionGraph = false;
	Params->TryGetBoolField(TEXT("is_function_graph"), bFunctionGraph);

	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
		return FMCPToolResult::Error(GraphError);

	// Seed ref map from pre_existing_refs
	TMap<FString, FString> RefToNodeId;
	const TSharedPtr<FJsonObject>* PreExistingRefs;
	if (Params->TryGetObjectField(TEXT("pre_existing_refs"), PreExistingRefs))
	{
		for (const auto& KV : (*PreExistingRefs)->Values)
		{
			FString GuidStr;
			if (KV.Value->TryGetString(GuidStr) && !GuidStr.IsEmpty())
				RefToNodeId.Add(KV.Key, GuidStr);
		}
	}

	// Get ops array
	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Params->TryGetArrayField(TEXT("ops"), OpsArray) || OpsArray->Num() == 0)
		return FMCPToolResult::Error(TEXT("'ops' array is required and must not be empty"));

	// Execute ops sequentially
	int32 OpsOk = 0, OpsFailed = 0;
	TArray<TSharedPtr<FJsonValue>> Results;

	for (int32 i = 0; i < OpsArray->Num(); ++i)
	{
		TSharedPtr<FJsonObject> OpResult = MakeShared<FJsonObject>();
		OpResult->SetNumberField(TEXT("index"), i);

		const TSharedPtr<FJsonObject>* OpSpec;
		if (!(*OpsArray)[i]->TryGetObject(OpSpec))
		{
			OpResult->SetStringField(TEXT("status"), TEXT("failed"));
			OpResult->SetStringField(TEXT("error"), TEXT("Op is not a valid JSON object"));
			OpsFailed++;
			Results.Add(MakeShared<FJsonValueObject>(OpResult));
			continue;
		}

		FString OpError;
		bool bOk = ProcessOp(Graph, *OpSpec, RefToNodeId, OpResult, OpError);
		if (bOk)
		{
			OpResult->SetStringField(TEXT("status"), TEXT("ok"));
			OpsOk++;
		}
		else
		{
			OpResult->SetStringField(TEXT("status"), TEXT("failed"));
			OpResult->SetStringField(TEXT("error"), OpError);
			OpsFailed++;
		}

		Results.Add(MakeShared<FJsonValueObject>(OpResult));
	}

	// Compile once at the end (default true)
	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile_once_at_end"), bCompile);

	FString CompileStatus = TEXT("skipped");
	bool bCompileOk = true;
	if (bCompile)
	{
		TOptional<FMCPToolResult> CompileError = Context.CompileAndFinalize(TEXT("blueprint_transaction"));
		bCompileOk = !CompileError.IsSet();
		CompileStatus = Context.CompileResult.bSuccess
			? Context.CompileResult.StatusString
			: FString::Printf(TEXT("error: %s"), *Context.CompileResult.VerboseOutput.Left(200));
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.BlueprintPath);
	ResultData->SetNumberField(TEXT("ops_total"), OpsArray->Num());
	ResultData->SetNumberField(TEXT("ops_ok"), OpsOk);
	ResultData->SetNumberField(TEXT("ops_failed"), OpsFailed);
	ResultData->SetStringField(TEXT("compile_status"), CompileStatus);
	ResultData->SetArrayField(TEXT("results"), Results);

	FString Summary = FString::Printf(TEXT("Transaction %d/%d ops ok, compile=%s"),
		OpsOk, OpsArray->Num(), *CompileStatus);

	// Return error only if ALL ops failed AND compile failed — partial success still returns Success
	if (OpsFailed > 0 && OpsOk == 0 && !bCompileOk)
		return FMCPToolResult::Error(Summary);

	return FMCPToolResult::Success(Summary, ResultData);
}

// ─── ProcessOp ───────────────────────────────────────────────────────────────

bool FMCPTool_BlueprintTransaction::ProcessOp(
	UEdGraph* Graph,
	const TSharedPtr<FJsonObject>& Op,
	TMap<FString, FString>& RefToNodeId,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutError)
{
	FString OpType;
	if (!Op->TryGetStringField(TEXT("op"), OpType))
	{
		OutError = TEXT("Missing 'op' field");
		return false;
	}

	// ── add_node ──────────────────────────────────────────────────────────────
	if (OpType == TEXT("add_node"))
	{
		FString NodeType;
		if (!Op->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
		{
			OutError = TEXT("add_node requires 'node_type'");
			return false;
		}

		int32 PosX = 0, PosY = 0;
		double PosXD = 0.0, PosYD = 0.0;
		if (Op->TryGetNumberField(TEXT("pos_x"), PosXD)) PosX = (int32)PosXD;
		if (Op->TryGetNumberField(TEXT("pos_y"), PosYD)) PosY = (int32)PosYD;

		TSharedPtr<FJsonObject> NodeParams = BuildNodeParams(Op);

		FString NodeId, CreateError;
		UEdGraphNode* NewNode = FBlueprintUtils::CreateNode(Graph, NodeType, NodeParams, PosX, PosY, NodeId, CreateError);
		if (!NewNode)
		{
			OutError = CreateError;
			return false;
		}

		// Apply pin_values if provided
		const TSharedPtr<FJsonObject>* PinValuesPtr;
		if (Op->TryGetObjectField(TEXT("pin_values"), PinValuesPtr))
		{
			for (const auto& PV : (*PinValuesPtr)->Values)
			{
				FString PinVal;
				if (PV.Value->TryGetString(PinVal))
				{
					FString PinError;
					FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PV.Key, PinVal, PinError);
					// Non-fatal: continue even if a pin value fails
				}
			}
		}

		// Register ref → node_id
		FString Ref;
		Op->TryGetStringField(TEXT("ref"), Ref);
		if (!Ref.IsEmpty())
			RefToNodeId.Add(Ref, NodeId);

		OutResult->SetStringField(TEXT("ref"), Ref);
		OutResult->SetStringField(TEXT("real_id"), NodeId);
		OutResult->SetStringField(TEXT("node_type"), NodeType);
		return true;
	}

	// ── connect_pins ──────────────────────────────────────────────────────────
	if (OpType == TEXT("connect_pins"))
	{
		FString FromRef, ToRef, FromPin, ToPin;
		Op->TryGetStringField(TEXT("from_ref"), FromRef);
		Op->TryGetStringField(TEXT("to_ref"),   ToRef);
		Op->TryGetStringField(TEXT("from_pin"), FromPin);
		Op->TryGetStringField(TEXT("to_pin"),   ToPin);

		FString SourceId = ResolveRef(FromRef, RefToNodeId);
		FString TargetId = ResolveRef(ToRef,   RefToNodeId);

		if (SourceId.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot resolve from_ref '%s' — not in pre_existing_refs and no node with that ref was created in this transaction"), *FromRef);
			return false;
		}
		if (TargetId.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot resolve to_ref '%s' — not in pre_existing_refs and no node with that ref was created in this transaction"), *ToRef);
			return false;
		}

		FString ConnectError;
		bool bOk = FBlueprintUtils::ConnectPins(Graph, SourceId, FromPin, TargetId, ToPin, ConnectError);
		if (!bOk)
		{
			OutError = ConnectError;
			return false;
		}

		OutResult->SetStringField(TEXT("connected"),
			FString::Printf(TEXT("%s.%s → %s.%s"), *SourceId, *FromPin, *TargetId, *ToPin));
		return true;
	}

	// ── set_pin_value ─────────────────────────────────────────────────────────
	if (OpType == TEXT("set_pin_value"))
	{
		FString NodeRef, PinName, PinValue;
		Op->TryGetStringField(TEXT("node_ref"),  NodeRef);
		Op->TryGetStringField(TEXT("pin_name"),  PinName);
		Op->TryGetStringField(TEXT("pin_value"), PinValue);

		FString NodeId = ResolveRef(NodeRef, RefToNodeId);
		if (NodeId.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot resolve node_ref '%s'"), *NodeRef);
			return false;
		}

		FString SetError;
		bool bOk = FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinName, PinValue, SetError);
		if (!bOk)
		{
			OutError = SetError;
			return false;
		}

		OutResult->SetStringField(TEXT("node_id"), NodeId);
		OutResult->SetStringField(TEXT("pin"),     PinName);
		OutResult->SetStringField(TEXT("value"),   PinValue);
		return true;
	}

	// ── delete_node ───────────────────────────────────────────────────────────
	if (OpType == TEXT("delete_node"))
	{
		FString NodeRef;
		Op->TryGetStringField(TEXT("node_ref"), NodeRef);

		FString NodeId = ResolveRef(NodeRef, RefToNodeId);
		if (NodeId.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot resolve node_ref '%s'"), *NodeRef);
			return false;
		}

		FString DelError;
		bool bOk = FBlueprintUtils::DeleteNode(Graph, NodeId, DelError);
		if (!bOk)
		{
			OutError = DelError;
			return false;
		}

		OutResult->SetStringField(TEXT("deleted"), NodeId);
		return true;
	}

	OutError = FString::Printf(
		TEXT("Unknown op type '%s'.  Supported: add_node, connect_pins, set_pin_value, delete_node"), *OpType);
	return false;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

FString FMCPTool_BlueprintTransaction::ResolveRef(
	const FString& RefOrId,
	const TMap<FString, FString>& RefToNodeId) const
{
	if (RefOrId.IsEmpty())
		return FString();

	// Check ref map first (local refs + pre_existing_refs)
	const FString* Mapped = RefToNodeId.Find(RefOrId);
	if (Mapped)
		return *Mapped;

	// Fall back to treating as a literal node_id (GUID or MCP comment ID)
	return RefOrId;
}

TSharedPtr<FJsonObject> FMCPTool_BlueprintTransaction::BuildNodeParams(
	const TSharedPtr<FJsonObject>& Op) const
{
	// Prefer explicit node_params object
	const TSharedPtr<FJsonObject>* ParamsPtr;
	if (Op->TryGetObjectField(TEXT("node_params"), ParamsPtr))
		return *ParamsPtr;

	// Otherwise copy recognized inline fields into a fresh params object
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();

	static const TCHAR* StringFields[] = {
		TEXT("function"), TEXT("target_class"), TEXT("target_variable"), TEXT("target_object"),
		TEXT("event"), TEXT("variable"), TEXT("class"), TEXT("struct"),
		nullptr
	};
	for (int32 fi = 0; StringFields[fi]; ++fi)
	{
		FString Val;
		if (Op->TryGetStringField(StringFields[fi], Val))
			NodeParams->SetStringField(StringFields[fi], Val);
	}

	double NumVal = 0.0;
	if (Op->TryGetNumberField(TEXT("num_outputs"), NumVal))
		NodeParams->SetNumberField(TEXT("num_outputs"), NumVal);

	bool BoolVal = false;
	if (Op->TryGetBoolField(TEXT("pure"), BoolVal))
		NodeParams->SetBoolField(TEXT("pure"), BoolVal);

	return NodeParams;
}
