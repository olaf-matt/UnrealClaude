// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * MCP Tool for authoring a Widget Blueprint's widget tree (UMG Designer).
 *
 * Fills the BP002 gap: blueprint_modify can wire a widget's event graph/bindings, but there was
 * no way to ADD, ARRANGE, or READ BACK the widgets themselves (the Designer tree). This tool adds
 * the missing structural ops PLUS a mandatory read-back op.
 *
 * Operations:
 * - get_widget_tree    : (read-back) dump the widget tree — name, class, is_variable, parent,
 *                        slot type, and edit-visible numeric/bool/string properties per widget.
 * - add_widget         : construct a widget (Slider/TextBlock/Button/VerticalBox/HorizontalBox/
 *                        CanvasPanel/...), parent it, optionally mark as a BP variable, set initial
 *                        properties + CanvasPanel slot position/size. Recompiles the Blueprint.
 * - remove_widget      : remove a widget (and its descendants) from the tree. Recompiles.
 * - set_widget_property: set a property on an existing widget by name via reflection. Recompiles.
 * - add_widget_event   : create a widget bound-event node (e.g. Slider.OnValueChanged) in the
 *                        event graph — the K2Node_ComponentBoundEvent that blueprint_modify's
 *                        add_node cannot make. Idempotent; reports the node guid + delegate output
 *                        pins so blueprint_modify can wire downstream logic to it.
 *
 * All edits run on the editor-only UWidgetBlueprint->WidgetTree and recompile so the generated
 * class (and any BindWidget variables) reflect the change. Iterate on /Game/TestCases/ duplicates.
 */
class FMCPTool_Widget : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteGetWidgetTree(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddWidgetEvent(const TSharedRef<FJsonObject>& Params);
};
