// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPToolRegistry.h"
#include "MCPTaskQueue.h"
#include "MCPActivityLog.h"
#include "UnrealClaudeModule.h"
#include "UnrealClaudeConstants.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Include all tool implementations
#include "Tools/MCPTool_SpawnActor.h"
#include "Tools/MCPTool_GetLevelActors.h"
#include "Tools/MCPTool_SetProperty.h"
#include "Tools/MCPTool_RunConsoleCommand.h"
#include "Tools/MCPTool_DeleteActors.h"
#include "Tools/MCPTool_MoveActor.h"
#include "Tools/MCPTool_GetOutputLog.h"
#include "Tools/MCPTool_ExecuteScript.h"
#include "Tools/MCPTool_CleanupScripts.h"
#include "Tools/MCPTool_GetScriptHistory.h"
#include "Tools/MCPTool_CaptureViewport.h"
#include "Tools/MCPTool_BlueprintQuery.h"
#include "Tools/MCPTool_BlueprintModify.h"
#include "Tools/MCPTool_AnimBlueprintModify.h"
#include "Tools/MCPTool_AssetSearch.h"
#include "Tools/MCPTool_AssetDependencies.h"
#include "Tools/MCPTool_AssetReferencers.h"
#include "Tools/MCPTool_EnhancedInput.h"
#include "Tools/MCPTool_Character.h"
#include "Tools/MCPTool_CharacterData.h"
#include "Tools/MCPTool_Material.h"
#include "Tools/MCPTool_Asset.h"
#include "Tools/MCPTool_OpenLevel.h"
#include "Tools/MCPTool_NiagaraQuery.h"
#include "Tools/MCPTool_NiagaraModify.h"
#include "Tools/MCPTool_SetNiagaraVariable.h"
#include "Tools/MCPTool_SampleRenderTarget.h"
#include "Tools/MCPTool_BlueprintTransaction.h"

// Task queue tools
#include "Tools/MCPTool_TaskSubmit.h"
#include "Tools/MCPTool_TaskStatus.h"
#include "Tools/MCPTool_TaskResult.h"
#include "Tools/MCPTool_TaskList.h"
#include "Tools/MCPTool_TaskCancel.h"

FMCPToolRegistry::FMCPToolRegistry()
{
	RegisterBuiltinTools();
}

FMCPToolRegistry::~FMCPToolRegistry()
{
	StopTaskQueue();
	Tools.Empty();
}

void FMCPToolRegistry::StartTaskQueue()
{
	if (TaskQueue.IsValid())
	{
		TaskQueue->Start();
	}
}

void FMCPToolRegistry::StopTaskQueue()
{
	if (TaskQueue.IsValid())
	{
		TaskQueue->Shutdown();
	}
}

void FMCPToolRegistry::RegisterBuiltinTools()
{
	UE_LOG(LogUnrealClaude, Log, TEXT("Registering MCP tools..."));

	// Register all built-in tools
	RegisterTool(MakeShared<FMCPTool_SpawnActor>());
	RegisterTool(MakeShared<FMCPTool_GetLevelActors>());
	RegisterTool(MakeShared<FMCPTool_SetProperty>());
	RegisterTool(MakeShared<FMCPTool_RunConsoleCommand>());
	RegisterTool(MakeShared<FMCPTool_DeleteActors>());
	RegisterTool(MakeShared<FMCPTool_MoveActor>());
	RegisterTool(MakeShared<FMCPTool_GetOutputLog>());

	// Script execution tools
	RegisterTool(MakeShared<FMCPTool_ExecuteScript>());
	RegisterTool(MakeShared<FMCPTool_CleanupScripts>());
	RegisterTool(MakeShared<FMCPTool_GetScriptHistory>());

	// Viewport capture
	RegisterTool(MakeShared<FMCPTool_CaptureViewport>());

	// Blueprint tools
	RegisterTool(MakeShared<FMCPTool_BlueprintQuery>());
	RegisterTool(MakeShared<FMCPTool_BlueprintModify>());
	RegisterTool(MakeShared<FMCPTool_AnimBlueprintModify>());

	// Asset tools
	RegisterTool(MakeShared<FMCPTool_AssetSearch>());
	RegisterTool(MakeShared<FMCPTool_AssetDependencies>());
	RegisterTool(MakeShared<FMCPTool_AssetReferencers>());

	// Enhanced Input tools
	RegisterTool(MakeShared<FMCPTool_EnhancedInput>());

	// Character tools
	RegisterTool(MakeShared<FMCPTool_Character>());
	RegisterTool(MakeShared<FMCPTool_CharacterData>());

	// Material and Asset tools
	RegisterTool(MakeShared<FMCPTool_Material>());
	RegisterTool(MakeShared<FMCPTool_Asset>());

	// Level management tools
	RegisterTool(MakeShared<FMCPTool_OpenLevel>());

	// Niagara tools
	RegisterTool(MakeShared<FMCPTool_NiagaraQuery>());
	RegisterTool(MakeShared<FMCPTool_NiagaraModify>());
	RegisterTool(MakeShared<FMCPTool_SetNiagaraVariable>());

	// Render target tools
	RegisterTool(MakeShared<FMCPTool_SampleRenderTarget>());

	// Blueprint transaction (batch graph-wiring tool)
	RegisterTool(MakeShared<FMCPTool_BlueprintTransaction>());

	// Create and register async task queue tools
	// Task queue takes a raw pointer since the registry always outlives it
	TaskQueue = MakeShared<FMCPTaskQueue>(this);

	// Wire up execute_script to use the task queue for async execution
	// This allows script execution to handle permission dialogs without timing out
	if (TSharedPtr<IMCPTool>* ExecuteScriptToolPtr = Tools.Find(TEXT("execute_script")))
	{
		if (FMCPTool_ExecuteScript* ExecuteScriptTool = static_cast<FMCPTool_ExecuteScript*>(ExecuteScriptToolPtr->Get()))
		{
			ExecuteScriptTool->SetTaskQueue(TaskQueue);
			UE_LOG(LogUnrealClaude, Log, TEXT("  Wired up execute_script to task queue for async execution"));
		}
	}

	RegisterTool(MakeShared<FMCPTool_TaskSubmit>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskStatus>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskResult>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskList>(TaskQueue));
	RegisterTool(MakeShared<FMCPTool_TaskCancel>(TaskQueue));

	UE_LOG(LogUnrealClaude, Log, TEXT("Registered %d MCP tools"), Tools.Num());
}

void FMCPToolRegistry::RegisterTool(TSharedPtr<IMCPTool> Tool)
{
	if (!Tool.IsValid())
	{
		return;
	}

	FMCPToolInfo Info = Tool->GetInfo();
	if (Info.Name.IsEmpty())
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Cannot register tool with empty name"));
		return;
	}

	if (Tools.Contains(Info.Name))
	{
		UE_LOG(LogUnrealClaude, Warning, TEXT("Tool '%s' is already registered, replacing"), *Info.Name);
	}

	Tools.Add(Info.Name, Tool);
	UE_LOG(LogUnrealClaude, Log, TEXT("  Registered tool: %s"), *Info.Name);
}

void FMCPToolRegistry::UnregisterTool(const FString& ToolName)
{
	if (Tools.Remove(ToolName) > 0)
	{
		InvalidateToolCache();
		UE_LOG(LogUnrealClaude, Log, TEXT("Unregistered tool: %s"), *ToolName);
	}
}

void FMCPToolRegistry::InvalidateToolCache()
{
	bCacheValid = false;
	CachedToolInfo.Empty();
}

TArray<FMCPToolInfo> FMCPToolRegistry::GetAllTools() const
{
	// Return cached result if valid
	if (bCacheValid)
	{
		return CachedToolInfo;
	}

	// Rebuild cache
	CachedToolInfo.Empty(Tools.Num());
	for (const auto& Pair : Tools)
	{
		if (Pair.Value.IsValid())
		{
			CachedToolInfo.Add(Pair.Value->GetInfo());
		}
	}
	bCacheValid = true;

	return CachedToolInfo;
}

FMCPToolResult FMCPToolRegistry::ExecuteTool(const FString& ToolName, const TSharedRef<FJsonObject>& Params)
{
	TSharedPtr<IMCPTool>* FoundTool = Tools.Find(ToolName);
	if (!FoundTool || !FoundTool->IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Tool '%s' not found"), *ToolName));
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Executing MCP tool: %s"), *ToolName);

	// Start timing for activity log
	const double StartTimeSec = FPlatformTime::Seconds();

	// Execute on game thread to ensure safe access to engine objects
	FMCPToolResult Result;

	if (IsInGameThread())
	{
		Result = (*FoundTool)->Execute(Params);
	}
	else
	{
		// If called from non-game thread, dispatch to game thread and wait with timeout
		// Use shared pointers for all state to avoid use-after-free if timeout occurs
		TSharedPtr<FMCPToolResult> SharedResult = MakeShared<FMCPToolResult>();
		TSharedPtr<FEvent, ESPMode::ThreadSafe> CompletionEvent = MakeShareable(FPlatformProcess::GetSynchEventFromPool(),
			[](FEvent* Event) { FPlatformProcess::ReturnSynchEventToPool(Event); });
		TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> bTaskCompleted = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

		// Use FTSTicker to dispatch to game thread at a safe point between subsystem ticks.
		// AsyncTask(GameThread) can fire during streaming manager iteration, causing
		// re-entrancy into LevelRenderAssetManagersLock (assertion crash).
		FTSTicker::GetCoreTicker().AddTicker(TEXT("MCPTool_Execute"), 0.0f,
			[SharedResult, FoundTool, Params, CompletionEvent, bTaskCompleted](float) -> bool
		{
			*SharedResult = (*FoundTool)->Execute(Params);
			*bTaskCompleted = true;
			CompletionEvent->Trigger();
			return false; // One-shot, don't reschedule
		});

		// Wait with timeout to prevent indefinite hangs
		const uint32 TimeoutMs = UnrealClaudeConstants::MCPServer::GameThreadTimeoutMs;
		const bool bSignaled = CompletionEvent->Wait(TimeoutMs);

		if (!bSignaled || !(*bTaskCompleted))
		{
			UE_LOG(LogUnrealClaude, Error, TEXT("Tool '%s' execution timed out after %d ms"), *ToolName, TimeoutMs);
			return FMCPToolResult::Error(FString::Printf(TEXT("Tool execution timed out after %d seconds"), TimeoutMs / 1000));
		}

		// Copy result from shared storage
		Result = *SharedResult;
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Tool '%s' execution %s: %s"),
		*ToolName,
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		*Result.Message);

	// ── Push to activity log ──────────────────────────────────────────────────────
	//
	// Two paths:
	//
	// 1. Synchronous tools (blueprint_query, blueprint_transaction called directly, etc.)
	//    → logged immediately with ToolName + timing from StartTimeSec.
	//
	// 2. Async tools (blueprint_transaction, blueprint_modify, etc. routed via task queue)
	//    → executed inside the task-queue worker (bypasses ExecuteTool).
	//    → task_result is the bridge that delivers the completed result.
	//    → We intercept task_result and log as the underlying tool using its stored metadata
	//      (tool_name, duration_ms, success, message, data — all present in task_result data).
	//
	// task_submit / task_status / task_list / task_cancel are skipped (pure machinery).
	//
	{
		// Pure-noise tools — never log
		const bool bSkip = (ToolName == TEXT("task_submit")
			|| ToolName == TEXT("task_status")
			|| ToolName == TEXT("task_list")
			|| ToolName == TEXT("task_cancel"));

		const bool bIsTaskResult = (ToolName == TEXT("task_result"));

		if (!bSkip)
		{
			FMCPLogEntry LogEntry;
			LogEntry.Timestamp = FDateTime::UtcNow();
			bool bShouldLog = false;

			if (bIsTaskResult && Result.bSuccess && Result.Data.IsValid())
			{
				// Unwrap: re-log as the underlying tool using its execution metadata
				FString UnderlyingTool;
				if (Result.Data->TryGetStringField(TEXT("tool_name"), UnderlyingTool)
					&& !UnderlyingTool.IsEmpty())
				{
					LogEntry.ToolName = UnderlyingTool;

					double Dur = 0.0;
					if (Result.Data->TryGetNumberField(TEXT("duration_ms"), Dur))
						LogEntry.DurationMs = (int32)Dur;

					bool bInnerSuccess = false;
					Result.Data->TryGetBoolField(TEXT("success"), bInnerSuccess);
					LogEntry.bSuccess = bInnerSuccess;

					FString Msg;
					Result.Data->TryGetStringField(TEXT("message"), Msg);
					LogEntry.Summary = Msg.Left(120);

					// Serialize inner data for the detail panel
					const TSharedPtr<FJsonObject>* InnerData;
					if (Result.Data->TryGetObjectField(TEXT("data"), InnerData)
						&& (*InnerData).IsValid())
					{
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&LogEntry.DetailJson);
						FJsonSerializer::Serialize((*InnerData).ToSharedRef(), Writer);
						LogEntry.OutputChars = LogEntry.DetailJson.Len();
						if (LogEntry.DetailJson.Len() > 4096)
							LogEntry.DetailJson = LogEntry.DetailJson.Left(4096) + TEXT("\n...(truncated)");
						// Router tools store "operation" in the inner result data
						(*InnerData)->TryGetStringField(TEXT("operation"), LogEntry.Operation);
					}
					bShouldLog = true;
				}
				// else: task_result without tool_name (error task) — skip
			}
			else if (!bIsTaskResult)
			{
				// Normal synchronous tool
				LogEntry.ToolName   = ToolName;
				LogEntry.DurationMs = (int32)((FPlatformTime::Seconds() - StartTimeSec) * 1000.0);
				LogEntry.bSuccess   = Result.bSuccess;
				LogEntry.Summary    = Result.Message.Left(120);
				Params->TryGetStringField(TEXT("operation"), LogEntry.Operation);

				if (Result.Data.IsValid())
				{
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&LogEntry.DetailJson);
					FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
					LogEntry.OutputChars = LogEntry.DetailJson.Len();
					if (LogEntry.DetailJson.Len() > 4096)
						LogEntry.DetailJson = LogEntry.DetailJson.Left(4096) + TEXT("\n...(truncated)");
				}
				bShouldLog = true;
			}

			if (bShouldLog)
				FMCPActivityLog::Get().AddEntry(MoveTemp(LogEntry));
		}
	}

	return Result;
}

bool FMCPToolRegistry::HasTool(const FString& ToolName) const
{
	return Tools.Contains(ToolName);
}
