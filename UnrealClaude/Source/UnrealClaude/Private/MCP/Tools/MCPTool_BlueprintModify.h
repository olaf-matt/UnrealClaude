// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Modify Blueprints (write operations)
 *
 * Level 2 Operations (Variables/Functions):
 *   - create: Create a new Blueprint
 *   - add_variable: Add a variable to a Blueprint
 *   - remove_variable: Remove a variable from a Blueprint
 *   - set_variable_instance_editable: Enable/disable Instance Editable flag on a variable
 *   - add_function: Add an empty function to a Blueprint
 *   - add_function_input: Add an input pin to an existing function (Interface blueprints only)
 *   - remove_function: Remove a function from a Blueprint
 *
 * Level 3 Operations (Nodes):
 *   - add_node: Add a single node to a graph
 *   - add_nodes: Batch add multiple nodes with connections
 *   - delete_node: Remove a node from a graph
 *   - move_node: Reposition a node without touching connections
 *
 * Level 4 Operations (Connections):
 *   - connect_pins: Connect two pins
 *   - disconnect_pins: Disconnect two pins
 *   - set_pin_value: Set default value for an input pin
 *
 * All modification operations auto-compile the Blueprint after changes.
 */
class FMCPTool_BlueprintModify : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_modify");
		Info.Description = TEXT(
			"Create and modify Blueprints programmatically. Auto-compiles after each operation.\n\n"
			"OPERATION → REQUIRED PARAMS:\n"
			"  create                         → package_path, blueprint_name, parent_class\n"
			"  add_variable                   → blueprint_path, variable_name, variable_type\n"
			"  remove_variable                → blueprint_path, variable_name\n"
			"  set_variable_instance_editable → blueprint_path, variable_name, instance_editable\n"
			"  set_variable_default           → blueprint_path, variable_name, default_value (string)\n"
			"  set_variable_expose_on_spawn   → blueprint_path, variable_name, expose_on_spawn (bool)\n"
			"  rename_variable                → blueprint_path, variable_name, new_name\n"
			"  add_function                   → blueprint_path, function_name [, inputs]\n"
			"  add_function_input             → blueprint_path, function_name, input_name, input_type  (Interface BPs only)\n"
			"  remove_function                → blueprint_path, function_name\n"
			"  add_component     → blueprint_path, component_class, component_name [, asset]\n"
			"  remove_component  → blueprint_path, component_name\n"
			"  set_component_property → blueprint_path, component_name, property_name, value\n"
			"  add_interface     → blueprint_path, interface_name\n"
			"  add_node        → blueprint_path, node_type [, graph_name, is_function_graph, node_params, pos_x, pos_y]\n"
			"  add_nodes       → blueprint_path, nodes[] [, connections[], graph_name, is_function_graph]\n"
			"  delete_node     → blueprint_path, node_id [, graph_name, is_function_graph]\n"
			"  move_node       → blueprint_path, node_id, pos_x, pos_y [, graph_name, is_function_graph]\n"
			"  connect_pins    → blueprint_path, source_node_id, target_node_id [, source_pin, target_pin, graph_name, is_function_graph]\n"
			"  disconnect_pins → blueprint_path, source_node_id, source_pin, target_node_id, target_pin\n"
			"  set_pin_value   → blueprint_path, node_id, pin_name, pin_value [, graph_name, is_function_graph]\n\n"
			"NODE TYPES and node_params:\n"
			"  CallFunction  {\"function\":\"MyFunc\"}                               self-call\n"
			"                {\"function\":\"GetAllActorsOfClass\",\"target_class\":\"GameplayStatics\"}  library call\n"
			"                {\"function\":\"SetTimerByFunctionName\"}               Object pin auto-wired to self\n"
			"                {\"function\":\"MyFunc\",\"target_object\":\"self\"}      explicit self Object pin for any function\n"
			"  VariableGet   {\"variable\":\"MyVar\"}\n"
			"  VariableSet   {\"variable\":\"MyVar\"}\n"
			"  Event         {\"event\":\"BeginPlay\"}   or  {\"event\":\"Tick\"}\n"
			"  Branch        (no node_params needed)\n"
			"  Sequence      {\"num_outputs\":3}   (optional, default 2)\n"
			"  PrintString, Add, Subtract, Multiply, Divide — no required params\n\n"
			"PIN NAMES (use blueprint_query 'get_node_pins' to verify for any node):\n"
			"  Exec input='execute'  Exec output='then'\n"
			"  Branch: input='Condition', outputs='then' (true path) / 'else' (false path)\n"
			"  Sequence: outputs='then_0','then_1','then_2',...\n"
			"  VariableSet: data input matches variable name (e.g. 'MyVar')\n"
			"  Omit source_pin/target_pin to auto-connect first available exec pins.\n\n"
			"FUNCTION GRAPHS: Omitting graph_name targets EventGraph.\n"
			"  To target a function/event function graph, set BOTH:\n"
			"    graph_name='MyFunctionName'  AND  is_function_graph=true\n\n"
			"Node IDs returned by blueprint_query 'get_nodes' can be used directly in\n"
			"connect_pins, disconnect_pins, delete_node, and set_pin_value.\n\n"
			"Workflow: blueprint_query inspect/get_nodes → blueprint_modify. Returns node IDs."
		);
		Info.Parameters = {
			// Operation selector
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("create | add_variable | remove_variable | set_variable_instance_editable | add_function | add_function_input | remove_function | add_node | add_nodes | delete_node | move_node | connect_pins | disconnect_pins | set_pin_value"), true),

			// Common parameters
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Full asset path to the Blueprint (e.g., '/Game/Blueprints/BP_MyActor'). Required for all ops except 'create'."), false),

			// For 'create' operation
			FMCPToolParameter(TEXT("package_path"), TEXT("string"),
				TEXT("Folder path for new Blueprint (e.g., '/Game/Blueprints'). Required for 'create'."), false),
			FMCPToolParameter(TEXT("blueprint_name"), TEXT("string"),
				TEXT("Asset name for new Blueprint without extension (e.g., 'BP_MyActor'). Required for 'create'."), false),
			FMCPToolParameter(TEXT("parent_class"), TEXT("string"),
				TEXT("Parent class short name (e.g., 'Actor', 'Pawn', 'Character'). Required for 'create'."), false),
			FMCPToolParameter(TEXT("blueprint_type"), TEXT("string"),
				TEXT("Blueprint type: 'Normal' (default) | 'Interface' | 'FunctionLibrary' | 'MacroLibrary'"), false, TEXT("Normal")),

			// For variable operations
			FMCPToolParameter(TEXT("variable_name"), TEXT("string"),
				TEXT("Variable name. Required for add_variable / remove_variable."), false),
			FMCPToolParameter(TEXT("variable_type"), TEXT("string"),
				TEXT("Supported types: bool | int | float | byte | string | Vector | Rotator | Transform. Object reference types (Actor, Component, etc.) are NOT supported — add those manually in the editor."), false),
			FMCPToolParameter(TEXT("instance_editable"), TEXT("boolean"),
				TEXT("For set_variable_instance_editable: true to enable Instance Editable (required before set_property works on level instances), false to disable."), false, TEXT("true")),

			// For function operations
			FMCPToolParameter(TEXT("function_name"), TEXT("string"),
				TEXT("Function name. Required for add_function / remove_function."), false),
			FMCPToolParameter(TEXT("inputs"), TEXT("array"),
				TEXT("Optional function inputs for add_function: [{\"name\":\"ParamName\",\"type\":\"float\"}, ...]. Uses same type tokens as variable_type."), false),
			FMCPToolParameter(TEXT("input_name"), TEXT("string"),
				TEXT("Name of the input pin to add. Required for add_function_input."), false),
			FMCPToolParameter(TEXT("input_type"), TEXT("string"),
				TEXT("Type of the input pin for add_function_input. Same tokens as variable_type: bool | int | float | byte | string | Vector | Rotator | Transform."), false),

			// For node operations
			FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
				TEXT("Target graph name. Omit to target EventGraph. For a function graph, set this to the function name AND set is_function_graph=true."), false),
			FMCPToolParameter(TEXT("is_function_graph"), TEXT("boolean"),
				TEXT("Must be true when graph_name refers to a function/event function graph (not EventGraph). Default false."), false, TEXT("false")),
			FMCPToolParameter(TEXT("node_type"), TEXT("string"),
				TEXT("Node type to create: CallFunction | VariableGet | VariableSet | Event | Branch | Sequence | PrintString | Add | Subtract | Multiply | Divide"), false),
			FMCPToolParameter(TEXT("node_params"), TEXT("object"),
				TEXT("Per-type params. CallFunction: {\"function\":\"Name\"} or {\"function\":\"Name\",\"target_class\":\"GameplayStatics\"}. VariableGet/Set: {\"variable\":\"Name\"}. Event: {\"event\":\"BeginPlay\"}. Sequence: {\"num_outputs\":3}. Branch/math nodes need no params."), false),
			FMCPToolParameter(TEXT("pos_x"), TEXT("number"),
				TEXT("Node X position in graph canvas (pixel units, left to right)"), false, TEXT("0")),
			FMCPToolParameter(TEXT("pos_y"), TEXT("number"),
				TEXT("Node Y position in graph canvas (pixel units, top to bottom)"), false, TEXT("0")),
			FMCPToolParameter(TEXT("node_id"), TEXT("string"),
				TEXT("Node ID from blueprint_query 'get_nodes'. Required for delete_node and set_pin_value."), false),

			// For batch add_nodes operation
			FMCPToolParameter(TEXT("nodes"), TEXT("array"),
				TEXT("For add_nodes: array of node specs [{\"type\":\"CallFunction\",\"function\":\"MyFunc\",\"pos_x\":0,\"pos_y\":0,\"pin_values\":{\"PinName\":\"Value\"}}]"), false),
			FMCPToolParameter(TEXT("connections"), TEXT("array"),
				TEXT("For add_nodes: connections between created nodes [{\"from_node\":0,\"from_pin\":\"then\",\"to_node\":1,\"to_pin\":\"execute\"}]. from_node/to_node can be array index or node ID."), false),

			// For connection operations
			FMCPToolParameter(TEXT("source_node_id"), TEXT("string"),
				TEXT("ID of the source node (output side). Use node IDs from blueprint_query 'get_nodes'."), false),
			FMCPToolParameter(TEXT("source_pin"), TEXT("string"),
				TEXT("Source pin name. Exec output is typically 'then'. Omit to auto-connect first exec pin. Use blueprint_query 'get_node_pins' to find exact names."), false),
			FMCPToolParameter(TEXT("target_node_id"), TEXT("string"),
				TEXT("ID of the target node (input side)."), false),
			FMCPToolParameter(TEXT("target_pin"), TEXT("string"),
				TEXT("Target pin name. Exec input is typically 'execute'. Branch condition input is 'Condition'. Omit to auto-connect first exec pin."), false),

			// For set_pin_value operation
			FMCPToolParameter(TEXT("pin_name"), TEXT("string"),
				TEXT("Pin name to set a default value on. Use blueprint_query 'get_node_pins' to find exact names."), false),
			FMCPToolParameter(TEXT("pin_value"), TEXT("string"),
				TEXT("Default value as string (e.g., '3.14', 'true', 'Hello'). Set on unconnected input pins."), false)
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Level 2 Operations
	FMCPToolResult ExecuteCreate(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddVariable(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveVariable(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetVariableInstanceEditable(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddFunction(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddFunctionInput(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveFunction(const TSharedRef<FJsonObject>& Params);

	// Level 3 Operations (Nodes)
	FMCPToolResult ExecuteAddNode(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddNodes(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteDeleteNode(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteMoveNode(const TSharedRef<FJsonObject>& Params);

	// Level 4 Operations (Connections)
	FMCPToolResult ExecuteConnectPins(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteDisconnectPins(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetPinValue(const TSharedRef<FJsonObject>& Params);

	// Group A — Variable additions
	FMCPToolResult ExecuteSetVariableDefault(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetVariableExposeOnSpawn(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRenameVariable(const TSharedRef<FJsonObject>& Params);

	// Group B — Component management
	FMCPToolResult ExecuteAddComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveComponent(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetComponentProperty(const TSharedRef<FJsonObject>& Params);

	// Group C — Blueprint class management
	FMCPToolResult ExecuteAddInterface(const TSharedRef<FJsonObject>& Params);

	// Helpers
	EBlueprintType ParseBlueprintType(const FString& TypeString);

	// ExecuteAddNodes helper functions (reduces function complexity)
	bool CreateNodesFromSpec(
		UEdGraph* Graph,
		const TArray<TSharedPtr<FJsonValue>>& NodesArray,
		TArray<FString>& OutCreatedNodeIds,
		TArray<TSharedPtr<FJsonValue>>& OutCreatedNodes,
		FString& OutError
	);

	// LocalIdToGuid maps local "id" strings from the add_nodes call to real node_ids (TODO-22)
	TArray<TSharedPtr<FJsonValue>> ProcessNodeConnections(
		UEdGraph* Graph,
		const TArray<TSharedPtr<FJsonValue>>& ConnectionsArray,
		const TArray<FString>& CreatedNodeIds,
		const TMap<FString, FString>& LocalIdToGuid
	);
};
