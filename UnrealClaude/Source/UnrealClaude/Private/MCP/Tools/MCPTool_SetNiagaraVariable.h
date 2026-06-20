// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"

class UNiagaraComponent;

/**
 * MCP Tool: set_niagara_variable
 *
 * Set a user parameter override on a NiagaraComponent attached to a level actor.
 * Equivalent to calling SetNiagaraVariableFloat / Vec3 / Bool / Color / Int from Blueprint.
 * Useful for live-tuning Niagara system parameters during development without
 * requiring Blueprint graph changes or editor Details panel access.
 *
 * Type auto-detection from JSON value shape:
 *   number (no "type")     → float
 *   number + "type":"int"  → int32
 *   bool                   → bool
 *   {X, Y, Z}              → vec3 (FVector)
 *   {R, G, B} or {R,G,B,A} → color (FLinearColor)
 *
 * Parameters:
 *   actor_name     (string, required)  — Outliner label of the actor that has a NiagaraComponent
 *   variable_name  (string, required)  — Full user param name, e.g. "User.WaterDepth"
 *   value          (any, required)     — number, bool, or {X,Y,Z} / {R,G,B,A} object
 *   type           (string, optional)  — "float" | "int" | "bool" | "vec3" | "color"
 *                                        Inferred from value shape if omitted.
 *
 * Example (float):
 *   { "actor_name": "BP_ShallowWaterVolume_0", "variable_name": "User.WaterDepth", "value": 25.0 }
 *
 * Example (vec3):
 *   { "actor_name": "FX_ShallowWater_0", "variable_name": "User.GravityDir",
 *     "value": {"X": 0.1, "Y": 0.0, "Z": -0.99} }
 *
 * Example (color):
 *   { "actor_name": "FX_MySystem_0", "variable_name": "User.TintColor",
 *     "value": {"R": 0.0, "G": 0.5, "B": 1.0, "A": 1.0} }
 */
class FMCPTool_SetNiagaraVariable : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/** Find the first UNiagaraComponent on the actor (searches all components). */
	UNiagaraComponent* FindNiagaraComponent(AActor* Actor) const;
};
