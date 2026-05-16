// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Query Blueprint information (read-only operations)
 *
 * Operations:
 *   - list: List all Blueprints in project (with optional filters)
 *   - inspect: Get detailed Blueprint info (variables, functions, parent class)
 *   - get_graph: Get graph information (node count, events)
 *   - get_nodes: Get all nodes in a specific graph
 *   - get_variables: Get all Blueprint variables (standalone)
 *   - get_functions: Get all Blueprint functions (standalone)
 *   - get_node_pins: Get detailed pin info for a specific node
 *   - search_nodes: Search nodes by name/class substring
 *   - find_references: Find references to a variable or function
 */
class FMCPTool_BlueprintQuery : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("blueprint_query");
		Info.Description = TEXT(
			"Query Blueprint information (read-only).\n\n"
			"OPERATION → PURPOSE:\n"
			"  list          → Find Blueprints by path/name/type filter\n"
			"  inspect       → Variables, functions, parent class (set include_variables/include_functions/include_graphs=true)\n"
			"  get_graph     → Graph structure: node count, events, connections\n"
			"  get_nodes     → All nodes in a graph — returns node IDs usable in blueprint_modify\n"
			"  get_variables → All variables (faster than inspect when you only need vars)\n"
			"  get_functions → All functions (faster than inspect when you only need funcs)\n"
			"  get_node_pins → Exact pin names, types, and connections for a specific node\n"
			"                  USE THIS before blueprint_modify 'connect_pins' to verify pin names\n"
			"  search_nodes  → Find nodes by title or class substring across graphs\n"
			"  find_references → Find all graph uses of a variable or function\n\n"
			"WORKFLOW:\n"
			"  1. list → discover asset paths\n"
			"  2. inspect or get_nodes → understand structure + get node IDs\n"
			"  3. get_node_pins (on a specific node) → verify exact pin names before wiring\n"
			"  4. blueprint_modify → make changes using node IDs from step 2\n\n"
			"Node IDs returned by 'get_nodes' are accepted directly by blueprint_modify\n"
			"operations: connect_pins, disconnect_pins, delete_node, set_pin_value.\n\n"
			"Example paths: '/Game/Blueprints/BP_Character', '/Game/Characters/ABP_Hero'"
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("operation"), TEXT("string"),
				TEXT("list | inspect | get_graph | get_nodes | get_variables | get_functions | get_node_pins | search_nodes | find_references"), true),
			FMCPToolParameter(TEXT("path_filter"), TEXT("string"),
				TEXT("Path prefix filter (e.g., '/Game/Blueprints/')"), false, TEXT("/Game/")),
			FMCPToolParameter(TEXT("type_filter"), TEXT("string"),
				TEXT("Blueprint type filter: 'Actor', 'Object', 'Widget', 'AnimBlueprint', etc."), false),
			FMCPToolParameter(TEXT("name_filter"), TEXT("string"),
				TEXT("Name substring filter"), false),
			FMCPToolParameter(TEXT("limit"), TEXT("number"),
				TEXT("Maximum results to return (1-1000, default: 25)"), false, TEXT("25")),
			FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
				TEXT("Full Blueprint asset path (required for inspect/get_graph)"), false),
			FMCPToolParameter(TEXT("include_variables"), TEXT("boolean"),
				TEXT("Include variable list in inspect result (default: false)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("include_functions"), TEXT("boolean"),
				TEXT("Include function list in inspect result (default: false)"), false, TEXT("false")),
			FMCPToolParameter(TEXT("include_graphs"), TEXT("boolean"),
				TEXT("Include graph info in inspect result"), false, TEXT("false")),
			FMCPToolParameter(TEXT("graph_name"), TEXT("string"),
				TEXT("Graph to target. For get_nodes/search_nodes: empty = all graphs. For get_node_pins: empty = search all graphs. For get_graph: empty = EventGraph. Use exact graph name (function name for function graphs, 'EventGraph' for event graph)."), false),
			FMCPToolParameter(TEXT("node_id"), TEXT("string"),
				TEXT("Node ID from a prior 'get_nodes' result. Required for 'get_node_pins'. Also accepted as NodeGuid fallback."), false),
			FMCPToolParameter(TEXT("query"), TEXT("string"),
				TEXT("Search query for search_nodes (matches node title and class, case-insensitive)"), false),
			FMCPToolParameter(TEXT("ref_name"), TEXT("string"),
				TEXT("Variable or function name (required for find_references)"), false),
			FMCPToolParameter(TEXT("ref_type"), TEXT("string"),
				TEXT("Reference type filter for find_references: 'variable', 'function', or empty for both"), false)
		};
		Info.Annotations = FMCPToolAnnotations::ReadOnly();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/** List Blueprints matching filters */
	FMCPToolResult ExecuteList(const TSharedRef<FJsonObject>& Params);

	/** Get detailed Blueprint info */
	FMCPToolResult ExecuteInspect(const TSharedRef<FJsonObject>& Params);

	/** Get graph information */
	FMCPToolResult ExecuteGetGraph(const TSharedRef<FJsonObject>& Params);

	/** Get all nodes in a specific graph */
	FMCPToolResult ExecuteGetNodes(const TSharedRef<FJsonObject>& Params);

	/** Get all Blueprint variables (standalone) */
	FMCPToolResult ExecuteGetVariables(const TSharedRef<FJsonObject>& Params);

	/** Get all Blueprint functions (standalone) */
	FMCPToolResult ExecuteGetFunctions(const TSharedRef<FJsonObject>& Params);

	/** Get detailed pin info for a specific node */
	FMCPToolResult ExecuteGetNodePins(const TSharedRef<FJsonObject>& Params);

	/** Search nodes by name/class substring */
	FMCPToolResult ExecuteSearchNodes(const TSharedRef<FJsonObject>& Params);

	/** Find references to a variable or function */
	FMCPToolResult ExecuteFindReferences(const TSharedRef<FJsonObject>& Params);

	// --- Shared helpers ---

	/** Cached error from LoadAndValidateBlueprint */
	FMCPToolResult LastError;

	/** Load and validate a blueprint from Params["blueprint_path"]. Returns nullptr on failure (sets LastError). */
	UBlueprint* LoadAndValidateBlueprint(const TSharedRef<FJsonObject>& Params);

	/** Collect graphs to search. If GraphName is non-empty, finds that specific graph. Otherwise returns UbergraphPages + FunctionGraphs + MacroGraphs. */
	static TArray<UEdGraph*> CollectGraphs(UBlueprint* Blueprint, const FString& GraphName);

	/** Find a node by MCP ID or NodeGuid fallback. */
	static UEdGraphNode* FindNodeInGraphs(const TArray<UEdGraph*>& Graphs, const FString& NodeId, FString& OutGraphName);
};
