// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Blueprint Transaction
 *
 * Accepts a full graph-wiring script as a single JSON call and executes it
 * server-side — compiling once at the end.  Eliminates all Claude round-trip
 * overhead that occurs when issuing sequential connect_pins calls.
 *
 * Typical speedup: 16 sequential calls (8 min) → 1 transaction call (~2 sec).
 *
 * ─── Op types ──────────────────────────────────────────────────────────────
 *
 *   add_node      Create a node; assigns a local "ref" for later ops.
 *   connect_pins  Wire two nodes using ref names or real node_ids.
 *   set_pin_value Set a default value on an unconnected input pin.
 *   delete_node   Remove a node by ref or real node_id.
 *
 * ─── Ref resolution ────────────────────────────────────────────────────────
 *
 *   "ref" strings name nodes created in this transaction.
 *   "pre_existing_refs" maps friendly names to real GUIDs for existing nodes
 *   (e.g. the EventBeginPlay or EventTick nodes already in the graph).
 *   If a ref/from_ref/to_ref/node_ref is not found in the map, it is treated
 *   as a literal node_id (GUID or MCP comment ID).
 *
 * ─── Usage example ─────────────────────────────────────────────────────────
 *
 *   {
 *     "blueprint_path": "/Game/BP_Example",
 *     "pre_existing_refs": { "begin_play": "27FEE3E4..." },
 *     "ops": [
 *       { "op": "add_node", "ref": "b1", "node_type": "Branch", "pos_x": 300, "pos_y": 0 },
 *       { "op": "add_node", "ref": "vget", "node_type": "VariableGet",
 *                           "node_params": { "variable": "bActive" }, "pos_x": 100, "pos_y": 80 },
 *       { "op": "connect_pins", "from_ref": "begin_play", "from_pin": "then",
 *                               "to_ref": "b1", "to_pin": "execute" },
 *       { "op": "connect_pins", "from_ref": "vget", "from_pin": "bActive",
 *                               "to_ref": "b1", "to_pin": "Condition" }
 *     ]
 *   }
 */
class FMCPTool_BlueprintTransaction : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/**
	 * Process one op entry.  Updates RefToNodeId when a new node is created.
	 * @return true on success; OutError set and returned result marked failed on failure.
	 */
	bool ProcessOp(
		UEdGraph* Graph,
		const TSharedPtr<FJsonObject>& Op,
		TMap<FString, FString>& RefToNodeId,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError
	);

	/** Resolve a ref string: checks RefToNodeId first, falls back to literal node_id. */
	FString ResolveRef(const FString& RefOrId, const TMap<FString, FString>& RefToNodeId) const;

	/**
	 * Build NodeParams from an add_node op spec — handles both nested "node_params"
	 * object and inline fields (function, variable, event, target_class, etc.).
	 */
	TSharedPtr<FJsonObject> BuildNodeParams(const TSharedPtr<FJsonObject>& Op) const;
};
