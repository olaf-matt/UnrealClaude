// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Dom/JsonObject.h"

/**
 * Blueprint graph and node manipulation
 *
 * Responsibilities:
 * - Finding graphs (event graphs, function graphs)
 * - Creating/deleting Blueprint nodes
 * - Managing pin connections
 * - Node ID system for tracking
 *
 * Supported Node Types:
 * - Flow:        Branch, Sequence, ForEachLoop, ForEachLoopWithBreak
 * - Functions:   CallFunction, PrintString
 * - Variables:   VariableGet, VariableSet
 * - Events:      Event (BeginPlay, Tick, EndPlay)
 * - Math:        Add, Subtract, Multiply, Divide
 *                Clamp, Min, Max, Abs, Lerp, MapRangeClamped, MapRangeUnclamped
 *                (add optional "type": "int" param for integer variants)
 * - Vector math: VectorLength, Normalize, DotProduct, CrossProduct
 * - Comparisons: Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual
 *                (add optional "type": "int" or "type": "bool" for typed variants)
 * - Boolean:     BoolAND (alias AND), BoolOR (alias OR), BoolNOT (alias NOT), BoolXOR (alias XOR)
 * - Other:       IsValid
 * - Select:      { "type": "float", "num_options": 4 } — picks one of N values by index
 * - Cast:        { "class": "TargetClassName" } — output pin: "As TargetClassName", fail pin: "CastFailed"
 *                optional "pure": true for a pure cast (no exec pins, adds "bSuccess" output)
 * - MakeStruct:  { "struct": "StructName" } — makes struct from individual field pins
 * - BreakStruct: { "struct": "StructName" } — breaks struct into individual field pins
 *
 * Node ID System:
 * - Auto-generated descriptive IDs stored in NodeComment
 * - Format: "{NodeType}_{Context}_{Counter}"
 * - Thread-safe counter for uniqueness
 */
class FBlueprintGraphEditor
{
public:
	// ===== Graph Finding =====

	/**
	 * Find a graph in Blueprint by name
	 * @param Blueprint - Blueprint to search
	 * @param GraphName - Graph name (empty for default EventGraph)
	 * @param bFunctionGraph - true to search function graphs
	 * @param OutError - Error message if not found
	 * @return Graph or nullptr
	 */
	static UEdGraph* FindGraph(
		UBlueprint* Blueprint,
		const FString& GraphName,
		bool bFunctionGraph,
		FString& OutError
	);

	// ===== Node Management =====

	/**
	 * Create a Blueprint node in the specified graph
	 *
	 * Supported NodeTypes:
	 * - "CallFunction" - params: { function, target_class, target_variable (opt) }
	 *                   target_class: short C++ class name (e.g. "MaterialInstanceDynamic", "NiagaraComponent")
	 *                   target_variable: if set, auto-creates a VariableGet and wires it to the Target/self pin
	 * - "Branch" / "IfThenElse"
	 * - "Event" - params: { event: "BeginPlay"|"Tick"|"EndPlay" }
	 * - "VariableGet" / "VariableSet" - params: { variable }
	 * - "Sequence" - params: { num_outputs }
	 * - "PrintString"
	 * - "Add", "Subtract", "Multiply", "Divide"
	 *
	 * @param Graph - Graph to add node to
	 * @param NodeType - Type of node (case insensitive)
	 * @param NodeParams - JSON parameters for the node
	 * @param PosX - X position in editor
	 * @param PosY - Y position in editor
	 * @param OutNodeId - Generated unique node ID
	 * @param OutError - Error message if failed
	 * @return Created node or nullptr
	 */
	static UEdGraphNode* CreateNode(
		UEdGraph* Graph,
		const FString& NodeType,
		const TSharedPtr<FJsonObject>& NodeParams,
		int32 PosX,
		int32 PosY,
		FString& OutNodeId,
		FString& OutError
	);

	/**
	 * Delete a node from graph by ID
	 * @param Graph - Graph containing node
	 * @param NodeId - MCP-generated node ID
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool DeleteNode(UEdGraph* Graph, const FString& NodeId, FString& OutError);

	/**
	 * Find node by MCP-generated ID
	 * @param Graph - Graph to search
	 * @param NodeId - Node ID to find
	 * @return Node or nullptr
	 */
	static UEdGraphNode* FindNodeById(UEdGraph* Graph, const FString& NodeId);

	// ===== Pin & Connection Management =====

	/**
	 * Connect two pins
	 * @param Graph - Graph containing nodes
	 * @param SourceNodeId - Source node ID
	 * @param SourcePinName - Source pin (empty for auto exec)
	 * @param TargetNodeId - Target node ID
	 * @param TargetPinName - Target pin (empty for auto exec)
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool ConnectPins(
		UEdGraph* Graph,
		const FString& SourceNodeId,
		const FString& SourcePinName,
		const FString& TargetNodeId,
		const FString& TargetPinName,
		FString& OutError
	);

	/**
	 * Disconnect two pins
	 * @param Graph - Graph containing nodes
	 * @param SourceNodeId - Source node ID
	 * @param SourcePinName - Source pin name
	 * @param TargetNodeId - Target node ID
	 * @param TargetPinName - Target pin name
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool DisconnectPins(
		UEdGraph* Graph,
		const FString& SourceNodeId,
		const FString& SourcePinName,
		const FString& TargetNodeId,
		const FString& TargetPinName,
		FString& OutError
	);

	/**
	 * Set default value for an input pin
	 * @param Graph - Graph containing node
	 * @param NodeId - Node ID
	 * @param PinName - Pin name
	 * @param Value - Default value as string
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool SetPinDefaultValue(
		UEdGraph* Graph,
		const FString& NodeId,
		const FString& PinName,
		const FString& Value,
		FString& OutError
	);

	/**
	 * Find pin on node by name
	 * @param Node - Node to search
	 * @param PinName - Pin name (case insensitive)
	 * @param Direction - Pin direction (EGPD_MAX for any)
	 * @return Pin or nullptr
	 */
	static UEdGraphPin* FindPinByName(
		UEdGraphNode* Node,
		const FString& PinName,
		EEdGraphPinDirection Direction = EGPD_MAX
	);

	/**
	 * Get first exec pin (input or output)
	 * @param Node - Node to search
	 * @param bOutput - true for output, false for input
	 * @return Exec pin or nullptr
	 */
	static UEdGraphPin* GetExecPin(UEdGraphNode* Node, bool bOutput);

	// ===== Serialization =====

	/**
	 * Serialize node info to JSON
	 * @param Node - Node to serialize
	 * @return JSON object with node info
	 */
	static TSharedPtr<FJsonObject> SerializeNodeInfo(UEdGraphNode* Node);

	// ===== Node ID System =====

	/**
	 * Generate unique descriptive node ID
	 * @param NodeType - Type of node
	 * @param Context - Additional context (function name, etc.)
	 * @param Graph - Graph to ensure uniqueness
	 * @return Unique ID string
	 */
	static FString GenerateNodeId(const FString& NodeType, const FString& Context, UEdGraph* Graph);

	/**
	 * Store node ID in node metadata
	 * @param Node - Node to tag
	 * @param NodeId - ID to store
	 */
	static void SetNodeId(UEdGraphNode* Node, const FString& NodeId);

	/**
	 * Get node ID from metadata
	 * @param Node - Node to query
	 * @return Node ID or empty string
	 */
	static FString GetNodeId(UEdGraphNode* Node);

	/**
	 * Resolve a short class name to a UClass*.
	 * Searches /Script/Engine, /Script/Niagara, /Script/UMG, and all loaded packages.
	 * Accepts names without the U prefix (e.g. "ExponentialHeightFogComponent", "MaterialInstanceDynamic").
	 * Also shared by BlueprintEditor::ParsePinType for object-reference variable types.
	 */
	static UClass* ResolveClassByName(const FString& ClassName);

private:
	// Thread-safe counter for unique IDs
	static volatile int32 NodeIdCounter;

	// Node creation helpers
	static UEdGraphNode* CreateCallFunctionNode(UEdGraph* Graph, const FString& FunctionName, const FString& TargetClass, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateBranchNode(UEdGraph* Graph, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateEventNode(UEdGraph* Graph, const FString& EventName, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateSequenceNode(UEdGraph* Graph, int32 NumOutputs, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateMathNode(UEdGraph* Graph, const FString& MathOp, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateCastNode(UEdGraph* Graph, const FString& ClassName, bool bPureCast, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateMakeStructNode(UEdGraph* Graph, const FString& StructName, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateBreakStructNode(UEdGraph* Graph, const FString& StructName, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateForEachLoopNode(UEdGraph* Graph, bool bWithBreak, int32 PosX, int32 PosY, FString& OutError);
	static UEdGraphNode* CreateSelectNode(UEdGraph* Graph, const FString& TypeName, int32 NumOptions, int32 PosX, int32 PosY, FString& OutError);

	/** Resolve a short struct name (e.g. "Vector") to a UScriptStruct*. Handles FVector/FRotator/FTransform directly; falls back to object search. */
	static UScriptStruct* ResolveStructByName(const FString& StructName);

	// ID prefix for node comments
	static const FString NodeIdPrefix;
};
