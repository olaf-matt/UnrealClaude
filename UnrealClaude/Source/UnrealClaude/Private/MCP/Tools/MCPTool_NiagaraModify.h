// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UNiagaraSystem;
class UNiagaraScript;
class UNiagaraGraph;
class UNiagaraNodeFunctionCall;
struct FVersionedNiagaraEmitterData;

/**
 * MCP Tool for modifying NiagaraSystem assets.
 *
 * Operations:
 *   list_emitters    — list emitter handles in a NiagaraSystem
 *   list_stages      — list script stages for a named emitter (EmitterSpawn, EmitterUpdate,
 *                      ParticleSpawn, ParticleUpdate, EventHandler_N)
 *   list_modules     — list module function-call nodes inside a stage's script graph
 *   add_module       — insert a module into a stage by Niagara module script asset path
 *   remove_module    — remove a named module from a stage graph and break its connections
 *   set_module_input — set a default-value override on a specific input pin of a module
 *   compile          — force-recompile a NiagaraSystem and mark its package dirty
 *
 * Example (list stages):
 *   { "operation": "list_stages",
 *     "system_path": "/Game/OceanWater/Particles/FX_Syst_Readback_iFFT",
 *     "emitter_name": "FX_ReadbackEmitter_01" }
 */
class FMCPTool_NiagaraModify : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteListEmitters(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListStages(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteListModules(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddModule(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveModule(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetModuleInput(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteCompile(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetSystemUserParam(const TSharedRef<FJsonObject>& Params);

	// --- Shared helpers ---

	// Load UNiagaraSystem with .AssetName suffix fallback
	static UNiagaraSystem* LoadNiagaraSystem(const FString& SystemPath);

	// Find mutable emitter data by handle display-name or emitter asset-name.
	// Returns nullptr and leaves OutHandleIndex = INDEX_NONE if not found.
	static FVersionedNiagaraEmitterData* GetEmitterDataByName(
		UNiagaraSystem* System, const FString& EmitterName, int32& OutHandleIndex);

	// Map a stage name string → UNiagaraScript* from emitter data.
	// Accepts: "EmitterSpawn", "EmitterUpdate", "ParticleSpawn" (alias "Spawn"),
	//          "ParticleUpdate" (alias "Update"), "EventHandler_N" (N = 0-based index).
	static UNiagaraScript* GetStageScript(FVersionedNiagaraEmitterData* Data, const FString& StageName);

	// Get the UNiagaraGraph from a script (via its UNiagaraScriptSource).
	static UNiagaraGraph* GetScriptGraph(UNiagaraScript* Script);

	// Find a UNiagaraNodeFunctionCall in Graph whose function name or node title
	// matches ModuleName (case-insensitive).
	static UNiagaraNodeFunctionCall* FindModuleByName(UNiagaraGraph* Graph, const FString& ModuleName);

	// List the stage names that have a non-null script in the emitter data.
	static TArray<FString> GetStageNames(FVersionedNiagaraEmitterData* Data);

	// Find the first pin of the FNiagaraParameterMap struct type in the given direction.
	// Used to chain module nodes via the parameter-map wire.
	static UEdGraphPin* FindParamMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction);
};
