// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * MCP Tool for querying NiagaraSystem assets (read-only).
 *
 * Exposes the user-facing parameter schema of a NiagaraSystem: the names,
 * types, and data-interface flag for every entry in the system's exposed
 * parameter store.  Use this to confirm what inputs a readback system
 * expects (e.g. Pontoons, Blueprint, bExportBuoyancy) before wiring it up.
 *
 * Operations:
 * - inspect: list all exposed user parameters (name, type, is_data_interface)
 */
class FMCPTool_NiagaraQuery : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteInspect(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteInspectCollection(const TSharedRef<FJsonObject>& Params);
};
