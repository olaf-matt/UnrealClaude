// Copyright Natali Caggiano. All Rights Reserved.

#include "BlueprintGraphEditor.h"
#include "UnrealClaudeModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Select.h"
#include "EdGraphSchema_K2.h"
#include "BlueprintEditor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformAtomics.h"

// Static member initialization
volatile int32 FBlueprintGraphEditor::NodeIdCounter = 0;
const FString FBlueprintGraphEditor::NodeIdPrefix = TEXT("MCP_ID:");

// ===== Graph Finding =====

UEdGraph* FBlueprintGraphEditor::FindGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	bool bFunctionGraph,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Get the appropriate graph array (UE 5.7 uses TObjectPtr)
	auto& Graphs = bFunctionGraph ? Blueprint->FunctionGraphs : Blueprint->UbergraphPages;

	// If no name specified, return the first graph (default)
	if (GraphName.IsEmpty())
	{
		if (Graphs.Num() > 0 && Graphs[0])
		{
			return Graphs[0];
		}
		OutError = bFunctionGraph ? TEXT("No function graphs found") : TEXT("No event graphs found");
		return nullptr;
	}

	// Search by name
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Build list of available graphs for error message
	TArray<FString> AvailableGraphs;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			AvailableGraphs.Add(Graph->GetName());
		}
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found. Available: %s"),
		*GraphName,
		*FString::Join(AvailableGraphs, TEXT(", ")));
	return nullptr;
}

// ===== Node Management =====

// Maps a user-facing node type name to a KismetMathLibrary function name.
// Returns empty string if the op is not a recognised math/comparison/boolean node.
static FString ResolveMathFuncName(const FString& Op, const FString& TypeHint)
{
	const bool bInt  = TypeHint.Equals(TEXT("int"),   ESearchCase::IgnoreCase)
	                || TypeHint.Equals(TEXT("int32"), ESearchCase::IgnoreCase);
	const bool bBool = TypeHint.Equals(TEXT("bool"),  ESearchCase::IgnoreCase);

	// Comparisons — float default, int/bool variants via 'type' param
	if (Op.Equals(TEXT("Equal"),        ESearchCase::IgnoreCase) ||
	    Op.Equals(TEXT("EqualEqual"),   ESearchCase::IgnoreCase))
		return bInt  ? TEXT("EqualEqual_IntInt")
		     : bBool ? TEXT("EqualEqual_BoolBool")
		     :         TEXT("EqualEqual_DoubleDouble");

	if (Op.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("NotEqual_IntInt") : TEXT("NotEqual_DoubleDouble");

	if (Op.Equals(TEXT("Less"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Less_IntInt")      : TEXT("Less_DoubleDouble");
	if (Op.Equals(TEXT("LessEqual"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("LessEqual_IntInt") : TEXT("LessEqual_DoubleDouble");
	if (Op.Equals(TEXT("Greater"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Greater_IntInt")      : TEXT("Greater_DoubleDouble");
	if (Op.Equals(TEXT("GreaterEqual"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("GreaterEqual_IntInt") : TEXT("GreaterEqual_DoubleDouble");

	// Boolean ops
	if (Op.Equals(TEXT("BoolAND"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("AND"), ESearchCase::IgnoreCase))
		return TEXT("BooleanAND");
	if (Op.Equals(TEXT("BoolOR"),  ESearchCase::IgnoreCase) || Op.Equals(TEXT("OR"),  ESearchCase::IgnoreCase))
		return TEXT("BooleanOR");
	if (Op.Equals(TEXT("BoolNOT"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("NOT"), ESearchCase::IgnoreCase))
		return TEXT("Not_PreBool");
	if (Op.Equals(TEXT("BoolXOR"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("XOR"), ESearchCase::IgnoreCase))
		return TEXT("BooleanXOR");

	// Extended math — float default, int variants via 'type': 'int'
	if (Op.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Clamp")   : TEXT("FClamp");
	if (Op.Equals(TEXT("Min"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Min")     : TEXT("FMin");
	if (Op.Equals(TEXT("Max"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Max")     : TEXT("FMax");
	if (Op.Equals(TEXT("Abs"), ESearchCase::IgnoreCase))
		return bInt ? TEXT("Abs_Int") : TEXT("Abs");
	if (Op.Equals(TEXT("Lerp"),              ESearchCase::IgnoreCase)) return TEXT("Lerp");
	if (Op.Equals(TEXT("MapRangeClamped"),   ESearchCase::IgnoreCase)) return TEXT("MapRangeClamped");
	if (Op.Equals(TEXT("MapRangeUnclamped"), ESearchCase::IgnoreCase)) return TEXT("MapRangeUnclamped");

	// Vector math
	if (Op.Equals(TEXT("VectorLength"), ESearchCase::IgnoreCase)) return TEXT("VSize");
	if (Op.Equals(TEXT("Normalize"),    ESearchCase::IgnoreCase)) return TEXT("Normal");
	if (Op.Equals(TEXT("DotProduct"),   ESearchCase::IgnoreCase)) return TEXT("Dot_VectorVector");
	if (Op.Equals(TEXT("CrossProduct"), ESearchCase::IgnoreCase)) return TEXT("Cross_VectorVector");

	return TEXT("");
}

UEdGraphNode* FBlueprintGraphEditor::CreateNode(
	UEdGraph* Graph,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& NodeParams,
	int32 PosX,
	int32 PosY,
	FString& OutNodeId,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	UEdGraphNode* NewNode = nullptr;
	FString Context;

	// Dispatch to appropriate creation function
	if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
	{
		FString FunctionName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("function")) : TEXT("");
		FString TargetClass = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("target_class")) : TEXT("");
		Context = FunctionName;
		NewNode = CreateCallFunctionNode(Graph, FunctionName, TargetClass, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateBranchNode(Graph, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		FString EventName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("event")) : TEXT("");
		Context = EventName;
		NewNode = CreateEventNode(Graph, EventName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableGetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableSetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
	{
		int32 NumOutputs = NodeParams.IsValid() ? (int32)NodeParams->GetNumberField(TEXT("num_outputs")) : 2;
		if (NumOutputs < 2) NumOutputs = 2;
		NewNode = CreateSequenceNode(Graph, NumOutputs, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		Context = NodeType;
		NewNode = CreateMathNode(Graph, NodeType, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase))
	{
		// Convenience alias for CallFunction with PrintString
		Context = TEXT("PrintString");
		NewNode = CreateCallFunctionNode(Graph, TEXT("PrintString"), TEXT("KismetSystemLibrary"), PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
	{
		FString ClassName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("class")) : TEXT("");
		bool bPure = false;
		if (NodeParams.IsValid()) NodeParams->TryGetBoolField(TEXT("pure"), bPure);
		Context = ClassName;
		NewNode = CreateCastNode(Graph, ClassName, bPure, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("MakeStruct"), ESearchCase::IgnoreCase))
	{
		FString StructName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("struct")) : TEXT("");
		Context = StructName;
		NewNode = CreateMakeStructNode(Graph, StructName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("BreakStruct"), ESearchCase::IgnoreCase))
	{
		FString StructName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("struct")) : TEXT("");
		Context = StructName;
		NewNode = CreateBreakStructNode(Graph, StructName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ForEachLoop"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateForEachLoopNode(Graph, false, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ForEachLoopWithBreak"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateForEachLoopNode(Graph, true, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Select"), ESearchCase::IgnoreCase))
	{
		FString TypeName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("type")) : TEXT("");
		int32 NumOptions = 4;
		if (NodeParams.IsValid() && NodeParams->HasField(TEXT("num_options")))
			NumOptions = (int32)NodeParams->GetNumberField(TEXT("num_options"));
		NewNode = CreateSelectNode(Graph, TypeName, NumOptions, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("IsValid"), ESearchCase::IgnoreCase))
	{
		Context = TEXT("IsValid");
		NewNode = CreateCallFunctionNode(Graph, TEXT("IsValid"), TEXT("KismetSystemLibrary"), PosX, PosY, OutError);
	}
	else
	{
		// Dynamic dispatch: comparisons, boolean ops, extended math, vector math
		FString TypeHint;
		if (NodeParams.IsValid()) NodeParams->TryGetStringField(TEXT("type"), TypeHint);
		FString MathFunc = ResolveMathFuncName(NodeType, TypeHint);
		if (!MathFunc.IsEmpty())
		{
			Context = MathFunc;
			NewNode = CreateCallFunctionNode(Graph, MathFunc, TEXT("KismetMathLibrary"), PosX, PosY, OutError);
		}
		else
		{
			OutError = FString::Printf(TEXT("Unknown node type: '%s'. Supported: "
				"CallFunction, Branch, Event, VariableGet, VariableSet, Sequence, "
				"Add, Subtract, Multiply, Divide, PrintString, Cast, MakeStruct, BreakStruct, "
				"ForEachLoop, ForEachLoopWithBreak, Select, IsValid | "
				"Comparisons (add type:'int' for int variant): Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual | "
				"Boolean: BoolAND, BoolOR, BoolNOT, BoolXOR | "
				"Math: Clamp, Min, Max, Abs, Lerp, MapRangeClamped, MapRangeUnclamped | "
				"Vector: VectorLength, Normalize, DotProduct, CrossProduct"),
				*NodeType);
			return nullptr;
		}
	}

	if (NewNode)
	{
		// Generate and set node ID
		OutNodeId = GenerateNodeId(NodeType, Context, Graph);
		SetNodeId(NewNode, OutNodeId);

		// Mark blueprint as modified
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		UE_LOG(LogUnrealClaude, Log, TEXT("Created node '%s' (type: %s) at (%d, %d)"), *OutNodeId, *NodeType, PosX, PosY);
	}

	return NewNode;
}

bool FBlueprintGraphEditor::DeleteNode(UEdGraph* Graph, const FString& NodeId, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);

	// Break all connections first
	Node->BreakAllNodeLinks();

	// Remove from graph
	Graph->RemoveNode(Node);

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Deleted node '%s'"), *NodeId);
	return true;
}

UEdGraphNode* FBlueprintGraphEditor::FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && GetNodeId(Node) == NodeId)
		{
			return Node;
		}
	}

	// Fallback: match against the node's native NodeGuid so pre-existing nodes
	// (created by the user or Engine, not by MCP) are also reachable by ID.
	// blueprint_query returns node_guid values for every node — this lets modify
	// ops (delete_node, connect_nodes, set_pin_default) accept the same IDs.
	FGuid ParsedGuid;
	if (FGuid::Parse(NodeId, ParsedGuid))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == ParsedGuid)
			{
				return Node;
			}
		}
	}

	return nullptr;
}

// ===== Pin & Connection Management =====

bool FBlueprintGraphEditor::ConnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find source node by MCP-generated ID
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	// Find target node
	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins - auto-detect exec pins if names are empty
	UEdGraphPin* SourcePin = nullptr;
	UEdGraphPin* TargetPin = nullptr;

	if (SourcePinName.IsEmpty())
	{
		// Auto-select first exec output
		SourcePin = GetExecPin(SourceNode, true);
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("No exec output pin found on node '%s'"), *SourceNodeId);
			return false;
		}
	}
	else
	{
		SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Output);
		if (!SourcePin)
		{
			// Try input direction for bidirectional data pins
			SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Input);
		}
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
			return false;
		}
	}

	if (TargetPinName.IsEmpty())
	{
		// Auto-select first exec input
		TargetPin = GetExecPin(TargetNode, false);
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("No exec input pin found on node '%s'"), *TargetNodeId);
			return false;
		}
	}
	else
	{
		TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Input);
		if (!TargetPin)
		{
			// Try output direction for bidirectional data pins
			TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Output);
		}
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
			return false;
		}
	}

	// Check if connection is valid
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			OutError = FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString());
			return false;
		}
	}

	if (Schema)
	{
		Schema->TryCreateConnection(SourcePin, TargetPin);
	}
	else
	{
		SourcePin->MakeLinkTo(TargetPin);
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Connected '%s.%s' -> '%s.%s'"),
		*SourceNodeId, *SourcePin->PinName.ToString(),
		*TargetNodeId, *TargetPin->PinName.ToString());

	return true;
}

bool FBlueprintGraphEditor::DisconnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find nodes
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins
	UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_MAX);
	if (!SourcePin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
		return false;
	}

	UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_MAX);
	if (!TargetPin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
		return false;
	}

	// Break the link
	SourcePin->BreakLinkTo(TargetPin);

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Disconnected '%s.%s' from '%s.%s'"),
		*SourceNodeId, *SourcePinName,
		*TargetNodeId, *TargetPinName);

	return true;
}

bool FBlueprintGraphEditor::SetPinDefaultValue(
	UEdGraph* Graph,
	const FString& NodeId,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *PinName, *NodeId);
		return false;
	}

	// Set the default value
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		Pin->DefaultValue = Value;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Set pin '%s.%s' default value to '%s'"), *NodeId, *PinName, *Value);
	return true;
}

UEdGraphPin* FBlueprintGraphEditor::FindPinByName(
	UEdGraphNode* Node,
	const FString& PinName,
	EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
			{
				return Pin;
			}
		}
	}

	return nullptr;
}

UEdGraphPin* FBlueprintGraphEditor::GetExecPin(UEdGraphNode* Node, bool bOutput)
{
	if (!Node)
	{
		return nullptr;
	}

	EEdGraphPinDirection Direction = bOutput ? EGPD_Output : EGPD_Input;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}

	return nullptr;
}

// ===== Serialization =====

TSharedPtr<FJsonObject> FBlueprintGraphEditor::SerializeNodeInfo(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

	if (!Node)
	{
		return NodeObj;
	}

	NodeObj->SetStringField(TEXT("node_id"), GetNodeId(Node));
	NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

	// Serialize pins
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));

			// Convert pin type to string representation
			FString TypeStr;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				TypeStr = TEXT("bool");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			{
				TypeStr = TEXT("int32");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				TypeStr = (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			{
				TypeStr = TEXT("FString");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				TypeStr = TEXT("exec");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Struct->GetName();
				}
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
			{
				if (UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Class->GetName() + TEXT("*");
				}
			}
			else
			{
				TypeStr = Pin->PinType.PinCategory.ToString();
			}

			PinObj->SetStringField(TEXT("type"), TypeStr);
			if (!Pin->DefaultValue.IsEmpty())
			{
				PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			}
			PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
			Pins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}
	NodeObj->SetArrayField(TEXT("pins"), Pins);

	return NodeObj;
}

// ===== Node ID System =====

FString FBlueprintGraphEditor::GenerateNodeId(const FString& NodeType, const FString& Context, UEdGraph* Graph)
{
	FString BaseId;
	if (Context.IsEmpty())
	{
		BaseId = NodeType;
	}
	else
	{
		BaseId = FString::Printf(TEXT("%s_%s"), *NodeType, *Context);
	}

	// Ensure uniqueness with atomic increment for thread safety
	int32 Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
	FString NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);

	// Verify uniqueness in graph
	if (Graph)
	{
		while (FindNodeById(Graph, NodeId) != nullptr)
		{
			Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
			NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);
		}
	}

	return NodeId;
}

void FBlueprintGraphEditor::SetNodeId(UEdGraphNode* Node, const FString& NodeId)
{
	if (Node)
	{
		// Store ID in node comment (visible in editor, persisted)
		Node->NodeComment = NodeIdPrefix + NodeId;
	}
}

FString FBlueprintGraphEditor::GetNodeId(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}

	// Extract ID from node comment
	if (Node->NodeComment.StartsWith(NodeIdPrefix))
	{
		return Node->NodeComment.RightChop(NodeIdPrefix.Len());
	}

	// Engine-created nodes (BeginPlay, FunctionEntry, etc.) have no MCP comment.
	// Fall back to NodeGuid so blueprint_query and modify ops share the same ID space.
	return Node->NodeGuid.ToString();
}

// ===== Private Node Creation Helpers =====

UEdGraphNode* FBlueprintGraphEditor::CreateCallFunctionNode(
	UEdGraph* Graph,
	const FString& FunctionName,
	const FString& TargetClass,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (FunctionName.IsEmpty())
	{
		OutError = TEXT("Function name is required");
		return nullptr;
	}

	// Find the function
	UFunction* Function = nullptr;
	UClass* FunctionOwner = nullptr;

	// Try to find class by name
	if (!TargetClass.IsEmpty())
	{
		FunctionOwner = FindObject<UClass>(nullptr, *TargetClass);
		if (!FunctionOwner)
		{
			// Try common library classes
			if (TargetClass.Equals(TEXT("KismetSystemLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetSystemLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("KismetMathLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetMathLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("KismetArrayLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetArrayLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("KismetStringLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetStringLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("GameplayStatics"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UGameplayStatics::StaticClass();
			}
		}
	}
	else
	{
		// Default to KismetSystemLibrary for common functions
		FunctionOwner = UKismetSystemLibrary::StaticClass();
	}

	if (FunctionOwner)
	{
		Function = FunctionOwner->FindFunctionByName(FName(*FunctionName));
	}

	// If not found in specified class, search common libraries
	if (!Function)
	{
		Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UKismetArrayLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UKismetStringLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UGameplayStatics::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}

	// Check Blueprint's own user-defined functions (self-call).
	// We check FunctionGraphs by name (reliable pre-compile), then fall back to
	// GeneratedClass lookup for inherited/compiled Blueprint functions.
	bool bIsSelfCall = false;
	if (!Function)
	{
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (Blueprint)
		{
			for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
			{
				if (FuncGraph && FuncGraph->GetName() == FunctionName)
				{
					bIsSelfCall = true;
					break;
				}
			}
			if (!bIsSelfCall && Blueprint->GeneratedClass)
			{
				Function = Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionName));
			}
		}
	}

	if (!Function && !bIsSelfCall)
	{
		OutError = FString::Printf(
			TEXT("Function '%s' not found in KismetSystemLibrary, KismetMathLibrary, KismetArrayLibrary, GameplayStatics, or this Blueprint's own functions"),
			*FunctionName);
		return nullptr;
	}

	// Functions with ArrayParm metadata (e.g. UKismetArrayLibrary) must use
	// UK2Node_CallArrayFunction so NotifyPinConnectionListChanged propagates wildcard types.
	if (!bIsSelfCall && Function && Function->HasMetaData(TEXT("ArrayParm")))
	{
		FGraphNodeCreator<UK2Node_CallArrayFunction> NodeCreator(*Graph);
		UK2Node_CallArrayFunction* CallNode = NodeCreator.CreateNode();
		CallNode->SetFromFunction(Function);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		NodeCreator.Finalize();
		return CallNode;
	}

	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();
	if (bIsSelfCall)
	{
		CallNode->FunctionReference.SetSelfMember(FName(*FunctionName));
	}
	else
	{
		CallNode->SetFromFunction(Function);
	}
	CallNode->NodePosX = PosX;
	CallNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return CallNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateBranchNode(
	UEdGraph* Graph,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
	UK2Node_IfThenElse* BranchNode = NodeCreator.CreateNode();
	BranchNode->NodePosX = PosX;
	BranchNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return BranchNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateEventNode(
	UEdGraph* Graph,
	const FString& EventName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (EventName.IsEmpty())
	{
		OutError = TEXT("Event name is required");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	// Find the event function
	UFunction* EventFunc = nullptr;

	// Check common events
	if (EventName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveBeginPlay"));
	}
	else if (EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveTick"));
	}
	else if (EventName.Equals(TEXT("EndPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveEndPlay"));
	}
	else
	{
		// Try to find in parent class
		if (Blueprint->ParentClass)
		{
			EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
		}
	}

	if (!EventFunc)
	{
		OutError = FString::Printf(TEXT("Event '%s' not found. Common events: BeginPlay, Tick, EndPlay"), *EventName);
		return nullptr;
	}

	// Create the event node
	FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
	UK2Node_Event* EventNode = NodeCreator.CreateNode();
	EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
	EventNode->bOverrideFunction = true;
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return EventNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateVariableGetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
	UK2Node_VariableGet* GetNode = NodeCreator.CreateNode();
	GetNode->VariableReference.SetSelfMember(VarName);
	GetNode->NodePosX = PosX;
	GetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return GetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateVariableSetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
	UK2Node_VariableSet* SetNode = NodeCreator.CreateNode();
	SetNode->VariableReference.SetSelfMember(VarName);
	SetNode->NodePosX = PosX;
	SetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return SetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateSequenceNode(
	UEdGraph* Graph,
	int32 NumOutputs,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
	UK2Node_ExecutionSequence* SeqNode = NodeCreator.CreateNode();
	SeqNode->NodePosX = PosX;
	SeqNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Add additional output pins if needed (default is 2)
	while (SeqNode->Pins.Num() < NumOutputs + 1) // +1 for input exec
	{
		SeqNode->AddInputPin();
	}

	return SeqNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateMathNode(
	UEdGraph* Graph,
	const FString& MathOp,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	// Find the appropriate math function
	FName FunctionName;
	if (MathOp.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Add_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Subtract_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Multiply_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Divide_FloatFloat");
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown math operation: '%s'. Supported: Add, Subtract, Multiply, Divide"), *MathOp);
		return nullptr;
	}

	UFunction* MathFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
	if (!MathFunc)
	{
		OutError = FString::Printf(TEXT("Math function '%s' not found"), *FunctionName.ToString());
		return nullptr;
	}

	// Create a call function node for the math operation
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* MathNode = NodeCreator.CreateNode();
	MathNode->SetFromFunction(MathFunc);
	MathNode->NodePosX = PosX;
	MathNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MathNode;
}

// ===== Class / Struct Resolver Helpers =====

UClass* FBlueprintGraphEditor::ResolveClassByName(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;

	// If user supplied a full path (contains "."), try it directly
	if (ClassName.Contains(TEXT(".")))
	{
		if (UClass* Class = FindObject<UClass>(nullptr, *ClassName))
		{
			return Class;
		}
	}

	// Try common engine module package paths (fast, no disk load)
	static const TCHAR* Modules[] = {
		TEXT("Engine"),
		TEXT("Niagara"),
		TEXT("AIModule"),
		TEXT("UMG"),
		TEXT("GameplayAbilities"),
		TEXT("Chaos"),
		nullptr
	};

	for (int32 i = 0; Modules[i]; ++i)
	{
		FString Path = FString::Printf(TEXT("/Script/%s.%s"), Modules[i], *ClassName);
		if (UClass* Class = FindObject<UClass>(nullptr, *Path))
		{
			return Class;
		}
	}

	// Slow scan across all currently-loaded packages
	return FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None);
}

UScriptStruct* FBlueprintGraphEditor::ResolveStructByName(const FString& StructName)
{
	if (StructName.IsEmpty()) return nullptr;

	// Common structs resolved via TBaseStructure (avoids any object search)
	FString Lower = StructName.ToLower();
	if (Lower == TEXT("vector")      || Lower == TEXT("fvector"))      return TBaseStructure<FVector>::Get();
	if (Lower == TEXT("rotator")     || Lower == TEXT("frotator"))     return TBaseStructure<FRotator>::Get();
	if (Lower == TEXT("transform")   || Lower == TEXT("ftransform"))   return TBaseStructure<FTransform>::Get();
	if (Lower == TEXT("linearcolor") || Lower == TEXT("flinearcolor")) return TBaseStructure<FLinearColor>::Get();
	if (Lower == TEXT("vector2d")    || Lower == TEXT("fvector2d"))    return TBaseStructure<FVector2D>::Get();
	if (Lower == TEXT("color")       || Lower == TEXT("fcolor"))       return TBaseStructure<FColor>::Get();

	// If user supplied a full path, try it directly
	if (StructName.Contains(TEXT(".")))
	{
		if (UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *StructName))
		{
			return Struct;
		}
	}

	// Try /Script/Engine and /Script/CoreUObject
	for (const FString& Pkg : { FString(TEXT("/Script/Engine.")), FString(TEXT("/Script/CoreUObject.")) })
	{
		if (UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *(Pkg + StructName)))
		{
			return Struct;
		}
	}

	// Slow scan across all currently-loaded packages
	return FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::None);
}

// ===== New Node Type Implementations =====

UEdGraphNode* FBlueprintGraphEditor::CreateCastNode(
	UEdGraph* Graph,
	const FString& ClassName,
	bool bPureCast,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (ClassName.IsEmpty())
	{
		OutError = TEXT("Cast requires 'class' param (e.g. 'ExponentialHeightFogComponent')");
		return nullptr;
	}

	UClass* TargetClass = ResolveClassByName(ClassName);
	if (!TargetClass)
	{
		OutError = FString::Printf(
			TEXT("Cast target class '%s' not found. "
			     "Tip: use the C++ class name without prefix (e.g. 'ExponentialHeightFogComponent' not 'UExponentialHeightFogComponent'). "
			     "Ensure the relevant module is loaded in the editor."),
			*ClassName);
		return nullptr;
	}

	// TargetType must be set BEFORE Finalize() — AllocateDefaultPins() uses it to name the output pin
	FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
	UK2Node_DynamicCast* CastNode = NodeCreator.CreateNode();
	CastNode->TargetType = TargetClass;
	CastNode->SetPurity(bPureCast);
	CastNode->NodePosX = PosX;
	CastNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return CastNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateMakeStructNode(
	UEdGraph* Graph,
	const FString& StructName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (StructName.IsEmpty())
	{
		OutError = TEXT("MakeStruct requires 'struct' param (e.g. 'Vector', 'Rotator', 'Transform')");
		return nullptr;
	}

	UScriptStruct* TargetStruct = ResolveStructByName(StructName);
	if (!TargetStruct)
	{
		OutError = FString::Printf(
			TEXT("Struct '%s' not found. Common structs: Vector, Rotator, Transform, LinearColor, Vector2D."),
			*StructName);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
	UK2Node_MakeStruct* MakeNode = NodeCreator.CreateNode();
	MakeNode->StructType = TargetStruct;
	MakeNode->NodePosX = PosX;
	MakeNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MakeNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateBreakStructNode(
	UEdGraph* Graph,
	const FString& StructName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (StructName.IsEmpty())
	{
		OutError = TEXT("BreakStruct requires 'struct' param (e.g. 'Vector', 'Rotator', 'Transform')");
		return nullptr;
	}

	UScriptStruct* TargetStruct = ResolveStructByName(StructName);
	if (!TargetStruct)
	{
		OutError = FString::Printf(
			TEXT("Struct '%s' not found. Common structs: Vector, Rotator, Transform, LinearColor, Vector2D."),
			*StructName);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
	UK2Node_BreakStruct* BreakNode = NodeCreator.CreateNode();
	BreakNode->StructType = TargetStruct;
	BreakNode->NodePosX = PosX;
	BreakNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return BreakNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateForEachLoopNode(
	UEdGraph* Graph,
	bool bWithBreak,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	// Load the engine standard macros blueprint
	UBlueprint* MacroBlueprint = Cast<UBlueprint>(StaticLoadObject(
		UBlueprint::StaticClass(), nullptr,
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")));

	if (!MacroBlueprint)
	{
		OutError = TEXT("Could not load /Engine/EditorBlueprintResources/StandardMacros — ForEachLoop unavailable");
		return nullptr;
	}

	const FString MacroName = bWithBreak ? TEXT("ForEachLoopWithBreak") : TEXT("ForEachLoop");
	UEdGraph* MacroGraph = nullptr;
	for (UEdGraph* G : MacroBlueprint->MacroGraphs)
	{
		if (G && G->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
		{
			MacroGraph = G;
			break;
		}
	}

	if (!MacroGraph)
	{
		OutError = FString::Printf(TEXT("Macro '%s' not found in StandardMacros"), *MacroName);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_MacroInstance> NodeCreator(*Graph);
	UK2Node_MacroInstance* MacroNode = NodeCreator.CreateNode();
	MacroNode->SetMacroGraph(MacroGraph);
	MacroNode->NodePosX = PosX;
	MacroNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MacroNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateSelectNode(
	UEdGraph* Graph,
	const FString& TypeName,
	int32 NumOptions,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (NumOptions < 2) NumOptions = 2;
	if (NumOptions > 16) NumOptions = 16;

	FGraphNodeCreator<UK2Node_Select> NodeCreator(*Graph);
	UK2Node_Select* SelectNode = NodeCreator.CreateNode();
	SelectNode->NodePosX = PosX;
	SelectNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Default is 2 options; call AddInputPin() for each additional one needed
	for (int32 i = 2; i < NumOptions; ++i)
	{
		if (SelectNode->CanAddPin())
		{
			SelectNode->AddInputPin();
		}
	}

	// If a type was specified, set the option and return value pin types explicitly.
	// Type will also resolve automatically when pins are connected.
	if (!TypeName.IsEmpty())
	{
		FEdGraphPinType PinType;
		FString TypeError;
		if (FBlueprintEditor::ParsePinType(TypeName, PinType, TypeError))
		{
			TArray<UEdGraphPin*> OptionPins;
			SelectNode->GetOptionPins(OptionPins);
			for (UEdGraphPin* Pin : OptionPins)
			{
				Pin->PinType = PinType;
			}
			if (UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin())
			{
				ReturnPin->PinType = PinType;
			}
		}
	}

	return SelectNode;
}
