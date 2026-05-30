// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintModify.h"
#include "BlueprintUtils.h"
#include "MCP/MCPParamValidator.h"
#include "MCP/MCPBlueprintLoadContext.h"
#include "UnrealClaudeModule.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

// Operation name constants
namespace BlueprintModifyOps
{
	static const FString Create = TEXT("create");
	static const FString AddVariable = TEXT("add_variable");
	static const FString RemoveVariable = TEXT("remove_variable");
	static const FString SetVariableInstanceEditable = TEXT("set_variable_instance_editable");
	static const FString AddFunction = TEXT("add_function");
	static const FString AddFunctionInput = TEXT("add_function_input");
	static const FString RemoveFunction = TEXT("remove_function");
	static const FString AddNode = TEXT("add_node");
	static const FString AddNodes = TEXT("add_nodes");
	static const FString DeleteNode = TEXT("delete_node");
	static const FString MoveNode = TEXT("move_node");
	static const FString ConnectPins = TEXT("connect_pins");
	static const FString DisconnectPins = TEXT("disconnect_pins");
	static const FString SetPinValue = TEXT("set_pin_value");
	// Group A — Variable additions
	static const FString SetVariableDefault = TEXT("set_variable_default");
	static const FString SetVariableExposeOnSpawn = TEXT("set_variable_expose_on_spawn");
	static const FString RenameVariable = TEXT("rename_variable");
	// Group B — Component management
	static const FString AddComponent = TEXT("add_component");
	static const FString RemoveComponent = TEXT("remove_component");
	static const FString SetComponentProperty = TEXT("set_component_property");
	// Group C — Blueprint class management
	static const FString AddInterface = TEXT("add_interface");
}

FMCPToolResult FMCPTool_BlueprintModify::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Get operation type
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	// Level 2: Variable/Function Operations
	if (Operation == BlueprintModifyOps::Create)
	{
		return ExecuteCreate(Params);
	}
	if (Operation == BlueprintModifyOps::AddVariable)
	{
		return ExecuteAddVariable(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveVariable)
	{
		return ExecuteRemoveVariable(Params);
	}
	if (Operation == BlueprintModifyOps::SetVariableInstanceEditable)
	{
		return ExecuteSetVariableInstanceEditable(Params);
	}
	if (Operation == BlueprintModifyOps::AddFunction)
	{
		return ExecuteAddFunction(Params);
	}
	if (Operation == BlueprintModifyOps::AddFunctionInput)
	{
		return ExecuteAddFunctionInput(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveFunction)
	{
		return ExecuteRemoveFunction(Params);
	}
	// Level 3: Node Operations
	if (Operation == BlueprintModifyOps::AddNode)
	{
		return ExecuteAddNode(Params);
	}
	if (Operation == BlueprintModifyOps::AddNodes)
	{
		return ExecuteAddNodes(Params);
	}
	if (Operation == BlueprintModifyOps::DeleteNode)
	{
		return ExecuteDeleteNode(Params);
	}
	if (Operation == BlueprintModifyOps::MoveNode)
	{
		return ExecuteMoveNode(Params);
	}
	// Level 4: Connection Operations
	if (Operation == BlueprintModifyOps::ConnectPins)
	{
		return ExecuteConnectPins(Params);
	}
	if (Operation == BlueprintModifyOps::DisconnectPins)
	{
		return ExecuteDisconnectPins(Params);
	}
	if (Operation == BlueprintModifyOps::SetPinValue)
	{
		return ExecuteSetPinValue(Params);
	}
	// Group A — Variable additions
	if (Operation == BlueprintModifyOps::SetVariableDefault)
	{
		return ExecuteSetVariableDefault(Params);
	}
	if (Operation == BlueprintModifyOps::SetVariableExposeOnSpawn)
	{
		return ExecuteSetVariableExposeOnSpawn(Params);
	}
	if (Operation == BlueprintModifyOps::RenameVariable)
	{
		return ExecuteRenameVariable(Params);
	}
	// Group B — Component management
	if (Operation == BlueprintModifyOps::AddComponent)
	{
		return ExecuteAddComponent(Params);
	}
	if (Operation == BlueprintModifyOps::RemoveComponent)
	{
		return ExecuteRemoveComponent(Params);
	}
	if (Operation == BlueprintModifyOps::SetComponentProperty)
	{
		return ExecuteSetComponentProperty(Params);
	}
	// Group C — Blueprint class management
	if (Operation == BlueprintModifyOps::AddInterface)
	{
		return ExecuteAddInterface(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid: create, add_variable, remove_variable, set_variable_instance_editable, "
		     "add_function, add_function_input, remove_function, add_node, add_nodes, delete_node, move_node, "
		     "connect_pins, disconnect_pins, set_pin_value, "
		     "set_variable_default, set_variable_expose_on_spawn, rename_variable, "
		     "add_component, remove_component, set_component_property, add_interface"),
		*Operation));
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteCreate(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	FString PackagePath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("package_path"), PackagePath, Error))
	{
		return Error.GetValue();
	}

	FString BlueprintName;
	if (!ExtractRequiredString(Params, TEXT("blueprint_name"), BlueprintName, Error))
	{
		return Error.GetValue();
	}

	FString ParentClassName;
	if (!ExtractRequiredString(Params, TEXT("parent_class"), ParentClassName, Error))
	{
		return Error.GetValue();
	}

	FString BlueprintTypeStr = ExtractOptionalString(Params, TEXT("blueprint_type"), TEXT("Normal"));

	// Validate package path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(PackagePath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Validate Blueprint name
	if (!FMCPParamValidator::ValidateBlueprintVariableName(BlueprintName, ValidationError))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Invalid Blueprint name: %s"), *ValidationError));
	}

	// Find parent class
	FString ClassError;
	UClass* ParentClass = FBlueprintUtils::FindParentClass(ParentClassName, ClassError);
	if (!ParentClass)
	{
		return FMCPToolResult::Error(ClassError);
	}

	// Parse Blueprint type
	EBlueprintType BlueprintType = ParseBlueprintType(BlueprintTypeStr);

	// Create the Blueprint
	FString CreateError;
	UBlueprint* NewBlueprint = FBlueprintUtils::CreateBlueprint(
		PackagePath,
		BlueprintName,
		ParentClass,
		BlueprintType,
		CreateError
	);

	if (!NewBlueprint)
	{
		return FMCPToolResult::Error(CreateError);
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_name"), NewBlueprint->GetName());
	ResultData->SetStringField(TEXT("blueprint_path"), NewBlueprint->GetPathName());
	ResultData->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	ResultData->SetStringField(TEXT("blueprint_type"), FBlueprintUtils::GetBlueprintTypeString(BlueprintType));
	ResultData->SetBoolField(TEXT("compiled"), true);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created Blueprint: %s"), *NewBlueprint->GetPathName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddVariable(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString VariableName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error))
	{
		return Error.GetValue();
	}

	FString VariableType;
	if (!ExtractRequiredString(Params, TEXT("variable_type"), VariableType, Error))
	{
		return Error.GetValue();
	}

	// Validate variable name
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintVariableName(VariableName, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Parse variable type
	FEdGraphPinType PinType;
	FString TypeError;
	if (!FBlueprintUtils::ParsePinType(VariableType, PinType, TypeError))
	{
		return FMCPToolResult::Error(TypeError);
	}

	// Extract optional category (default: keep "Default")
	FString Category = ExtractOptionalString(Params, TEXT("category"), TEXT(""));

	// Add the variable
	FString AddError;
	if (!FBlueprintUtils::AddVariable(Context.Blueprint, VariableName, PinType, AddError))
	{
		return FMCPToolResult::Error(AddError);
	}

	// Assign category if provided
	if (!Category.IsEmpty())
	{
		// bDontRecompile=true: we compile below in CompileAndFinalize
		FBlueprintEditorUtils::SetBlueprintVariableCategory(
			Context.Blueprint, FName(*VariableName), nullptr,
			FText::FromString(Category), /*bDontRecompile=*/true);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable added")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);
	ResultData->SetStringField(TEXT("variable_type"), VariableType);
	if (!Category.IsEmpty())
	{
		ResultData->SetStringField(TEXT("category"), Category);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added variable '%s' (%s) to Blueprint"), *VariableName, *VariableType),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveVariable(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString VariableName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error))
	{
		return Error.GetValue();
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Remove the variable
	FString RemoveError;
	if (!FBlueprintUtils::RemoveVariable(Context.Blueprint, VariableName, RemoveError))
	{
		return FMCPToolResult::Error(RemoveError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable removed")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed variable '%s' from Blueprint"), *VariableName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetVariableInstanceEditable(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;
	FString VariableName;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error))
	{
		return Error.GetValue();
	}

	bool bInstanceEditable = ExtractOptionalBool(Params, TEXT("instance_editable"), true);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString SetError;
	if (!FBlueprintUtils::SetVariableInstanceEditable(Context.Blueprint, VariableName, bInstanceEditable, SetError))
	{
		return FMCPToolResult::Error(SetError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Instance Editable flag updated")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);
	ResultData->SetBoolField(TEXT("instance_editable"), bInstanceEditable);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Variable '%s' instance_editable=%s on '%s'"),
			*VariableName,
			bInstanceEditable ? TEXT("true") : TEXT("false"),
			*Context.Blueprint->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddFunction(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error))
	{
		return Error.GetValue();
	}

	// Validate function name
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintFunctionName(FunctionName, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Parse optional inputs array: [{"name": "Foo", "type": "float"}, ...]
	TArray<FBlueprintFunctionParam> FunctionParams;
	const TArray<TSharedPtr<FJsonValue>>* InputsArray;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObj;
			if (!InputVal->TryGetObject(InputObj)) continue;

			FString ParamName, ParamType;
			if (!(*InputObj)->TryGetStringField(TEXT("name"), ParamName) ||
				!(*InputObj)->TryGetStringField(TEXT("type"), ParamType))
			{
				continue;
			}

			FEdGraphPinType PinType;
			FString TypeError;
			if (FBlueprintUtils::ParsePinType(ParamType, PinType, TypeError))
			{
				FBlueprintFunctionParam Param;
				Param.Name = ParamName;
				Param.PinType = PinType;
				FunctionParams.Add(Param);
			}
		}
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Add the function (with parameters if provided)
	FString AddError;
	if (!FBlueprintUtils::AddFunction(Context.Blueprint, FunctionName, FunctionParams, AddError))
	{
		return FMCPToolResult::Error(AddError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function added")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);
	ResultData->SetNumberField(TEXT("param_count"), FunctionParams.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added function '%s' (%d params) to Blueprint"), *FunctionName, FunctionParams.Num()),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddFunctionInput(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error))
	{
		return Error.GetValue();
	}

	FString InputName;
	if (!ExtractRequiredString(Params, TEXT("input_name"), InputName, Error))
	{
		return Error.GetValue();
	}

	FString InputTypeStr;
	if (!ExtractRequiredString(Params, TEXT("input_type"), InputTypeStr, Error))
	{
		return Error.GetValue();
	}

	// Parse pin type
	FEdGraphPinType PinType;
	FString TypeError;
	if (!FBlueprintUtils::ParsePinType(InputTypeStr, PinType, TypeError))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unknown input_type '%s': %s"), *InputTypeStr, *TypeError));
	}

	// Validate names
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintFunctionName(FunctionName, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Add the input pin
	FString AddError;
	if (!FBlueprintUtils::AddFunctionInput(Context.Blueprint, FunctionName, InputName, PinType, AddError))
	{
		return FMCPToolResult::Error(AddError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function input added")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);
	ResultData->SetStringField(TEXT("input_name"), InputName);
	ResultData->SetStringField(TEXT("input_type"), InputTypeStr);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added input '%s' (%s) to function '%s'"), *InputName, *InputTypeStr, *FunctionName),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveFunction(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString FunctionName;
	if (!ExtractRequiredString(Params, TEXT("function_name"), FunctionName, Error))
	{
		return Error.GetValue();
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Remove the function
	FString RemoveError;
	if (!FBlueprintUtils::RemoveFunction(Context.Blueprint, FunctionName, RemoveError))
	{
		return FMCPToolResult::Error(RemoveError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Function removed")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("function_name"), FunctionName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed function '%s' from Blueprint"), *FunctionName),
		ResultData
	);
}

EBlueprintType FMCPTool_BlueprintModify::ParseBlueprintType(const FString& TypeString)
{
	FString LowerType = TypeString.ToLower();

	if (LowerType == TEXT("normal") || LowerType == TEXT("actor") || LowerType == TEXT("object"))
	{
		return BPTYPE_Normal;
	}
	if (LowerType == TEXT("functionlibrary") || LowerType == TEXT("function_library"))
	{
		return BPTYPE_FunctionLibrary;
	}
	if (LowerType == TEXT("interface"))
	{
		return BPTYPE_Interface;
	}
	if (LowerType == TEXT("macrolibrary") || LowerType == TEXT("macro_library") || LowerType == TEXT("macro"))
	{
		return BPTYPE_MacroLibrary;
	}

	// Default to normal
	return BPTYPE_Normal;
}

// ===== Level 3: Node Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddNode(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeType;
	if (!ExtractRequiredString(Params, TEXT("node_type"), NodeType, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);
	int32 PosX = (int32)ExtractOptionalNumber(Params, TEXT("pos_x"), 0);
	int32 PosY = (int32)ExtractOptionalNumber(Params, TEXT("pos_y"), 0);

	// Get node params object
	TSharedPtr<FJsonObject> NodeParams;
	const TSharedPtr<FJsonObject>* NodeParamsPtr;
	if (Params->TryGetObjectField(TEXT("node_params"), NodeParamsPtr))
	{
		NodeParams = *NodeParamsPtr;
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// TODO-05: Ensure Blueprint is compiled before function-graph node operations.
	// Some manually-authored function graphs (e.g. BP_DynamicSky.SetSkyParams) have
	// uninitialised schema or stale GeneratedClass if the BP hasn't been compiled yet
	// in this editor session. Force a compile so graph schemas are fully initialised.
	if (bFunctionGraph && Context.Blueprint->Status != BS_UpToDate
	    && Context.Blueprint->Status != BS_UpToDateWithWarnings)
	{
		FString CompileErr;
		FBlueprintUtils::CompileBlueprint(Context.Blueprint, CompileErr);
		// Non-fatal — proceed even if compile reports warnings; only abort on hard errors
		if (Context.Blueprint->Status == BS_Error)
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Blueprint has compile errors — fix before adding nodes. %s"), *CompileErr));
		}
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Create the node
	FString NodeId;
	FString CreateError;
	UEdGraphNode* NewNode = FBlueprintUtils::CreateNode(Graph, NodeType, NodeParams, PosX, PosY, NodeId, CreateError);
	if (!NewNode)
	{
		return FMCPToolResult::Error(CreateError);
	}

	// Apply pin default values if provided
	if (NodeParams.IsValid())
	{
		const TSharedPtr<FJsonObject>* PinValuesPtr;
		if (NodeParams->TryGetObjectField(TEXT("pin_values"), PinValuesPtr))
		{
			for (const auto& PinValue : (*PinValuesPtr)->Values)
			{
				FString PinValueStr;
				if (PinValue.Value->TryGetString(PinValueStr))
				{
					FString PinError;
					FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinValue.Key, PinValueStr, PinError);
				}
			}
		}
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Node created")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = FBlueprintUtils::SerializeNodeInfo(NewNode);
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("graph_name"), Graph->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created node '%s' (type: %s)"), *NodeId, *NodeType),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddNodes(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Get nodes array
	const TArray<TSharedPtr<FJsonValue>>* NodesArray;
	if (!Params->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		return FMCPToolResult::Error(TEXT("'nodes' array is required"));
	}

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// TODO-05: Pre-compile for function graphs (same as ExecuteAddNode)
	if (bFunctionGraph && Context.Blueprint->Status != BS_UpToDate
	    && Context.Blueprint->Status != BS_UpToDateWithWarnings)
	{
		FString CompileErr;
		FBlueprintUtils::CompileBlueprint(Context.Blueprint, CompileErr);
		if (Context.Blueprint->Status == BS_Error)
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Blueprint has compile errors — fix before adding nodes. %s"), *CompileErr));
		}
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Create all nodes using helper
	TArray<FString> CreatedNodeIds;
	TArray<TSharedPtr<FJsonValue>> CreatedNodes;
	FString CreateError;
	if (!CreateNodesFromSpec(Graph, *NodesArray, CreatedNodeIds, CreatedNodes, CreateError))
	{
		return FMCPToolResult::Error(CreateError);
	}

	// Build local-id → real-guid map from this add_nodes call (TODO-22)
	// Lets connections array reference nodes by local "id" string instead of real GUIDs
	TMap<FString, FString> LocalIdToGuid;
	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* NodeSpec;
		FString LocalId;
		if ((*NodesArray)[i]->TryGetObject(NodeSpec)
			&& (*NodeSpec)->TryGetStringField(TEXT("id"), LocalId)
			&& !LocalId.IsEmpty()
			&& i < CreatedNodeIds.Num())
		{
			LocalIdToGuid.Add(LocalId, CreatedNodeIds[i]);
		}
	}

	// Process connections using helper — captures per-connection success/failure
	TArray<TSharedPtr<FJsonValue>> ConnectionResults;
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray;
	if (Params->TryGetArrayField(TEXT("connections"), ConnectionsArray))
	{
		ConnectionResults = ProcessNodeConnections(Graph, *ConnectionsArray, CreatedNodeIds, LocalIdToGuid);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Nodes created")))
	{
		return CompileError.GetValue();
	}

	// Count failed connections for the summary message
	int32 FailedConnections = 0;
	for (const TSharedPtr<FJsonValue>& ConnResult : ConnectionResults)
	{
		const TSharedPtr<FJsonObject>* ConnObj;
		FString Status;
		if (ConnResult->TryGetObject(ConnObj) && (*ConnObj)->TryGetStringField(TEXT("status"), Status) && Status == TEXT("failed"))
		{
			FailedConnections++;
		}
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("graph_name"), Graph->GetName());
	ResultData->SetArrayField(TEXT("nodes"), CreatedNodes);
	ResultData->SetNumberField(TEXT("node_count"), CreatedNodeIds.Num());
	if (ConnectionResults.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("connections"), ConnectionResults);
		ResultData->SetNumberField(TEXT("connections_ok"), ConnectionResults.Num() - FailedConnections);
		ResultData->SetNumberField(TEXT("connections_failed"), FailedConnections);
	}

	FString Summary = FString::Printf(TEXT("Created %d nodes"), CreatedNodeIds.Num());
	if (ConnectionResults.Num() > 0)
	{
		Summary += FString::Printf(TEXT(", %d/%d connections ok"),
			ConnectionResults.Num() - FailedConnections, ConnectionResults.Num());
	}

	return FMCPToolResult::Success(Summary, ResultData);
}

bool FMCPTool_BlueprintModify::CreateNodesFromSpec(
	UEdGraph* Graph,
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	TArray<FString>& OutCreatedNodeIds,
	TArray<TSharedPtr<FJsonValue>>& OutCreatedNodes,
	FString& OutError)
{
	for (int32 i = 0; i < NodesArray.Num(); i++)
	{
		const TSharedPtr<FJsonObject>* NodeSpec;
		if (!NodesArray[i]->TryGetObject(NodeSpec))
		{
			OutError = FString::Printf(TEXT("Node at index %d is not a valid object"), i);
			return false;
		}

		FString NodeType = (*NodeSpec)->GetStringField(TEXT("type"));
		if (NodeType.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Node at index %d missing 'type' field"), i);
			return false;
		}

		int32 PosX = (int32)(*NodeSpec)->GetNumberField(TEXT("pos_x"));
		int32 PosY = (int32)(*NodeSpec)->GetNumberField(TEXT("pos_y"));

		// Get params (could be inline or nested)
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject>* ParamsPtr;
		if ((*NodeSpec)->TryGetObjectField(TEXT("params"), ParamsPtr))
		{
			NodeParams = *ParamsPtr;
		}
		else
		{
			// Copy common fields to params
			if ((*NodeSpec)->HasField(TEXT("function")))
				NodeParams->SetStringField(TEXT("function"), (*NodeSpec)->GetStringField(TEXT("function")));
			if ((*NodeSpec)->HasField(TEXT("target_class")))
				NodeParams->SetStringField(TEXT("target_class"), (*NodeSpec)->GetStringField(TEXT("target_class")));
			if ((*NodeSpec)->HasField(TEXT("event")))
				NodeParams->SetStringField(TEXT("event"), (*NodeSpec)->GetStringField(TEXT("event")));
			if ((*NodeSpec)->HasField(TEXT("variable")))
				NodeParams->SetStringField(TEXT("variable"), (*NodeSpec)->GetStringField(TEXT("variable")));
			if ((*NodeSpec)->HasField(TEXT("num_outputs")))
				NodeParams->SetNumberField(TEXT("num_outputs"), (*NodeSpec)->GetNumberField(TEXT("num_outputs")));
			if ((*NodeSpec)->HasField(TEXT("class")))
				NodeParams->SetStringField(TEXT("class"), (*NodeSpec)->GetStringField(TEXT("class")));
			if ((*NodeSpec)->HasField(TEXT("struct")))
				NodeParams->SetStringField(TEXT("struct"), (*NodeSpec)->GetStringField(TEXT("struct")));
			bool bPureField = false;
			if ((*NodeSpec)->TryGetBoolField(TEXT("pure"), bPureField))
				NodeParams->SetBoolField(TEXT("pure"), bPureField);
		}

		// Create node
		FString NodeId;
		FString CreateError;
		UEdGraphNode* NewNode = FBlueprintUtils::CreateNode(Graph, NodeType, NodeParams, PosX, PosY, NodeId, CreateError);
		if (!NewNode)
		{
			OutError = FString::Printf(TEXT("Failed to create node %d: %s"), i, *CreateError);
			return false;
		}

		OutCreatedNodeIds.Add(NodeId);

		// Apply pin default values if provided
		const TSharedPtr<FJsonObject>* PinValuesPtr;
		if ((*NodeSpec)->TryGetObjectField(TEXT("pin_values"), PinValuesPtr))
		{
			for (const auto& PinValue : (*PinValuesPtr)->Values)
			{
				FString PinValueStr;
				if (PinValue.Value->TryGetString(PinValueStr))
				{
					FString PinError;
					FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinValue.Key, PinValueStr, PinError);
				}
			}
		}

		// Add to result
		TSharedPtr<FJsonObject> NodeInfo = FBlueprintUtils::SerializeNodeInfo(NewNode);
		NodeInfo->SetNumberField(TEXT("index"), i);
		OutCreatedNodes.Add(MakeShared<FJsonValueObject>(NodeInfo));
	}

	return true;
}

TArray<TSharedPtr<FJsonValue>> FMCPTool_BlueprintModify::ProcessNodeConnections(
	UEdGraph* Graph,
	const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray,
	const TArray<FString>& CreatedNodeIds,
	const TMap<FString, FString>& LocalIdToGuid)
{
	TArray<TSharedPtr<FJsonValue>> Results;

	for (int32 i = 0; i < ConnectionsArray.Num(); i++)
	{
		TSharedPtr<FJsonObject> ConnResult = MakeShared<FJsonObject>();
		ConnResult->SetNumberField(TEXT("index"), i);

		const TSharedPtr<FJsonObject>* ConnSpec;
		if (!ConnectionsArray[i]->TryGetObject(ConnSpec))
		{
			ConnResult->SetStringField(TEXT("status"), TEXT("failed"));
			ConnResult->SetStringField(TEXT("error"), TEXT("Connection spec is not a valid object"));
			Results.Add(MakeShared<FJsonValueObject>(ConnResult));
			continue;
		}

		// Get source - can be index or node_id
		FString SourceNodeId;
		if ((*ConnSpec)->HasTypedField<EJson::Number>(TEXT("from_node")))
		{
			int32 FromIndex = (int32)(*ConnSpec)->GetNumberField(TEXT("from_node"));
			if (FromIndex >= 0 && FromIndex < CreatedNodeIds.Num())
			{
				SourceNodeId = CreatedNodeIds[FromIndex];
			}
		}
		else if ((*ConnSpec)->HasTypedField<EJson::String>(TEXT("from_node")))
		{
			// TODO-22: try local id map first, then treat as real GUID
			FString FromStr = (*ConnSpec)->GetStringField(TEXT("from_node"));
			const FString* Mapped = LocalIdToGuid.Find(FromStr);
			SourceNodeId = Mapped ? *Mapped : FromStr;
		}

		// Get target - can be index or node_id
		FString TargetNodeId;
		if ((*ConnSpec)->HasTypedField<EJson::Number>(TEXT("to_node")))
		{
			int32 ToIndex = (int32)(*ConnSpec)->GetNumberField(TEXT("to_node"));
			if (ToIndex >= 0 && ToIndex < CreatedNodeIds.Num())
			{
				TargetNodeId = CreatedNodeIds[ToIndex];
			}
		}
		else if ((*ConnSpec)->HasTypedField<EJson::String>(TEXT("to_node")))
		{
			// TODO-22: try local id map first, then treat as real GUID
			FString ToStr = (*ConnSpec)->GetStringField(TEXT("to_node"));
			const FString* Mapped = LocalIdToGuid.Find(ToStr);
			TargetNodeId = Mapped ? *Mapped : ToStr;
		}

		FString SourcePin = (*ConnSpec)->GetStringField(TEXT("from_pin"));
		FString TargetPin = (*ConnSpec)->GetStringField(TEXT("to_pin"));

		ConnResult->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *SourceNodeId, *SourcePin));
		ConnResult->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *TargetNodeId, *TargetPin));

		if (SourceNodeId.IsEmpty() || TargetNodeId.IsEmpty())
		{
			ConnResult->SetStringField(TEXT("status"), TEXT("failed"));
			ConnResult->SetStringField(TEXT("error"), TEXT("Could not resolve from_node or to_node to a valid node ID"));
		}
		else
		{
			FString ConnectError;
			bool bConnected = FBlueprintUtils::ConnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, ConnectError);
			if (bConnected)
			{
				ConnResult->SetStringField(TEXT("status"), TEXT("ok"));
			}
			else
			{
				ConnResult->SetStringField(TEXT("status"), TEXT("failed"));
				ConnResult->SetStringField(TEXT("error"), ConnectError);
			}
		}

		Results.Add(MakeShared<FJsonValueObject>(ConnResult));
	}

	return Results;
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteDeleteNode(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Delete the node
	FString DeleteError;
	if (!FBlueprintUtils::DeleteNode(Graph, NodeId, DeleteError))
	{
		return FMCPToolResult::Error(DeleteError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Node deleted")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("node_id"), NodeId);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Deleted node '%s'"), *NodeId),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteMoveNode(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;
	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	int32 PosX = (int32)ExtractOptionalNumber(Params, TEXT("pos_x"), 0);
	int32 PosY = (int32)ExtractOptionalNumber(Params, TEXT("pos_y"), 0);
	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Move the node
	FString MoveError;
	if (!FBlueprintUtils::MoveNode(Graph, NodeId, PosX, PosY, MoveError))
	{
		return FMCPToolResult::Error(MoveError);
	}

	// Build result (no compile needed — positional change only)
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("blueprint_path"), Context.Blueprint->GetPathName());
	ResultData->SetStringField(TEXT("graph_name"), Graph->GetName());
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetNumberField(TEXT("pos_x"), PosX);
	ResultData->SetNumberField(TEXT("pos_y"), PosY);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Moved node '%s' to (%d, %d)"), *NodeId, PosX, PosY),
		ResultData
	);
}

// ===== Level 4: Connection Operations =====

FMCPToolResult FMCPTool_BlueprintModify::ExecuteConnectPins(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString SourceNodeId;
	if (!ExtractRequiredString(Params, TEXT("source_node_id"), SourceNodeId, Error))
	{
		return Error.GetValue();
	}

	FString TargetNodeId;
	if (!ExtractRequiredString(Params, TEXT("target_node_id"), TargetNodeId, Error))
	{
		return Error.GetValue();
	}

	FString SourcePin = ExtractOptionalString(Params, TEXT("source_pin"), TEXT(""));
	FString TargetPin = ExtractOptionalString(Params, TEXT("target_pin"), TEXT(""));
	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// TODO-05: Pre-compile for function graphs (same as ExecuteAddNode)
	if (bFunctionGraph && Context.Blueprint->Status != BS_UpToDate
	    && Context.Blueprint->Status != BS_UpToDateWithWarnings)
	{
		FString CompileErr;
		FBlueprintUtils::CompileBlueprint(Context.Blueprint, CompileErr);
		if (Context.Blueprint->Status == BS_Error)
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Blueprint has compile errors — fix before connecting pins. %s"), *CompileErr));
		}
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Connect the pins
	FString ConnectError;
	if (!FBlueprintUtils::ConnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, ConnectError))
	{
		return FMCPToolResult::Error(ConnectError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Pins connected")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("source_node_id"), SourceNodeId);
	ResultData->SetStringField(TEXT("source_pin"), SourcePin.IsEmpty() ? TEXT("(auto exec)") : SourcePin);
	ResultData->SetStringField(TEXT("target_node_id"), TargetNodeId);
	ResultData->SetStringField(TEXT("target_pin"), TargetPin.IsEmpty() ? TEXT("(auto exec)") : TargetPin);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Connected '%s' -> '%s'"), *SourceNodeId, *TargetNodeId),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteDisconnectPins(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString SourceNodeId;
	if (!ExtractRequiredString(Params, TEXT("source_node_id"), SourceNodeId, Error))
	{
		return Error.GetValue();
	}

	FString SourcePin;
	if (!ExtractRequiredString(Params, TEXT("source_pin"), SourcePin, Error))
	{
		return Error.GetValue();
	}

	FString TargetNodeId;
	if (!ExtractRequiredString(Params, TEXT("target_node_id"), TargetNodeId, Error))
	{
		return Error.GetValue();
	}

	FString TargetPin;
	if (!ExtractRequiredString(Params, TEXT("target_pin"), TargetPin, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Disconnect the pins
	FString DisconnectError;
	if (!FBlueprintUtils::DisconnectPins(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin, DisconnectError))
	{
		return FMCPToolResult::Error(DisconnectError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Pins disconnected")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("source_node_id"), SourceNodeId);
	ResultData->SetStringField(TEXT("source_pin"), SourcePin);
	ResultData->SetStringField(TEXT("target_node_id"), TargetNodeId);
	ResultData->SetStringField(TEXT("target_pin"), TargetPin);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Disconnected '%s.%s' from '%s.%s'"), *SourceNodeId, *SourcePin, *TargetNodeId, *TargetPin),
		ResultData
	);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetPinValue(const TSharedRef<FJsonObject>& Params)
{
	// Extract parameters
	TOptional<FMCPToolResult> Error;
	FString NodeId;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeId, Error))
	{
		return Error.GetValue();
	}

	FString PinName;
	if (!ExtractRequiredString(Params, TEXT("pin_name"), PinName, Error))
	{
		return Error.GetValue();
	}

	FString PinValue;
	if (!ExtractRequiredString(Params, TEXT("pin_value"), PinValue, Error))
	{
		return Error.GetValue();
	}

	FString GraphName = ExtractOptionalString(Params, TEXT("graph_name"), TEXT(""));
	bool bFunctionGraph = ExtractOptionalBool(Params, TEXT("is_function_graph"), false);

	// Load and validate Blueprint
	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	// Find graph
	FString GraphError;
	UEdGraph* Graph = FBlueprintUtils::FindGraph(Context.Blueprint, GraphName, bFunctionGraph, GraphError);
	if (!Graph)
	{
		return FMCPToolResult::Error(GraphError);
	}

	// Set the pin value
	FString SetError;
	if (!FBlueprintUtils::SetPinDefaultValue(Graph, NodeId, PinName, PinValue, SetError))
	{
		return FMCPToolResult::Error(SetError);
	}

	// Compile and finalize
	if (auto CompileError = Context.CompileAndFinalize(TEXT("Pin value set")))
	{
		return CompileError.GetValue();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetStringField(TEXT("pin_name"), PinName);
	ResultData->SetStringField(TEXT("pin_value"), PinValue);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = '%s'"), *NodeId, *PinName, *PinValue),
		ResultData
	);
}

// ============================================================
//  Group A — Variable additions
// ============================================================

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetVariableDefault(const TSharedRef<FJsonObject>& Params)
{
	FString VariableName, DefaultValue;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("default_value"), DefaultValue,  Error)) return Error.GetValue();

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::SetVariableDefault(Context.Blueprint, VariableName, DefaultValue, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable default set")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);
	ResultData->SetStringField(TEXT("default_value"), DefaultValue);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set default for '%s' = '%s' on '%s'"),
			*VariableName, *DefaultValue, *Context.Blueprint->GetName()),
		ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetVariableExposeOnSpawn(const TSharedRef<FJsonObject>& Params)
{
	FString VariableName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error)) return Error.GetValue();

	bool bExposeOnSpawn = true;
	Params->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn);

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::SetVariableExposeOnSpawn(Context.Blueprint, VariableName, bExposeOnSpawn, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable expose_on_spawn set")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("variable_name"), VariableName);
	ResultData->SetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Variable '%s' expose_on_spawn=%s on '%s'"),
			*VariableName, bExposeOnSpawn ? TEXT("true") : TEXT("false"), *Context.Blueprint->GetName()),
		ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRenameVariable(const TSharedRef<FJsonObject>& Params)
{
	FString VariableName, NewName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VariableName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("new_name"),      NewName,      Error)) return Error.GetValue();

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::RenameVariable(Context.Blueprint, VariableName, NewName, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Variable renamed")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("old_name"), VariableName);
	ResultData->SetStringField(TEXT("new_name"), NewName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Renamed variable '%s' to '%s' on '%s'"),
			*VariableName, *NewName, *Context.Blueprint->GetName()),
		ResultData);
}

// ============================================================
//  Group B — Component management
// ============================================================

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddComponent(const TSharedRef<FJsonObject>& Params)
{
	FString ComponentClass, ComponentName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_class"), ComponentClass, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("component_name"),  ComponentName,  Error)) return Error.GetValue();

	FString AssetPath = ExtractOptionalString(Params, TEXT("asset"), TEXT(""));

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::AddComponent(Context.Blueprint, ComponentClass, ComponentName, AssetPath, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Component added")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_class"), ComponentClass);
	ResultData->SetStringField(TEXT("component_name"),  ComponentName);
	if (!AssetPath.IsEmpty())
		ResultData->SetStringField(TEXT("asset"), AssetPath);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added component '%s' (%s) to '%s'"),
			*ComponentName, *ComponentClass, *Context.Blueprint->GetName()),
		ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteRemoveComponent(const TSharedRef<FJsonObject>& Params)
{
	FString ComponentName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName, Error)) return Error.GetValue();

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::RemoveComponent(Context.Blueprint, ComponentName, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Component removed")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_name"), ComponentName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed component '%s' from '%s'"),
			*ComponentName, *Context.Blueprint->GetName()),
		ResultData);
}

FMCPToolResult FMCPTool_BlueprintModify::ExecuteSetComponentProperty(const TSharedRef<FJsonObject>& Params)
{
	FString ComponentName, PropertyName, PropertyValue;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("component_name"), ComponentName,  Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("property_name"),  PropertyName,   Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("value"),          PropertyValue,  Error)) return Error.GetValue();

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::SetComponentProperty(Context.Blueprint, ComponentName, PropertyName, PropertyValue, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Component property set")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("component_name"), ComponentName);
	ResultData->SetStringField(TEXT("property_name"),  PropertyName);
	ResultData->SetStringField(TEXT("value"),          PropertyValue);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set '%s.%s' = '%s' on '%s'"),
			*ComponentName, *PropertyName, *PropertyValue, *Context.Blueprint->GetName()),
		ResultData);
}

// ============================================================
//  Group C — Blueprint class management
// ============================================================

FMCPToolResult FMCPTool_BlueprintModify::ExecuteAddInterface(const TSharedRef<FJsonObject>& Params)
{
	FString InterfaceName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("interface_name"), InterfaceName, Error)) return Error.GetValue();

	FMCPBlueprintLoadContext Context;
	if (auto LoadError = Context.LoadAndValidate(Params))
	{
		return LoadError.GetValue();
	}

	FString OpError;
	if (!FBlueprintEditor::AddInterface(Context.Blueprint, InterfaceName, OpError))
		return FMCPToolResult::Error(OpError);

	if (auto CompileError = Context.CompileAndFinalize(TEXT("Interface added")))
	{
		return CompileError.GetValue();
	}

	TSharedPtr<FJsonObject> ResultData = Context.BuildResultJson();
	ResultData->SetStringField(TEXT("interface_name"), InterfaceName);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("'%s' now implements interface '%s'"),
			*Context.Blueprint->GetName(), *InterfaceName),
		ResultData);
}
