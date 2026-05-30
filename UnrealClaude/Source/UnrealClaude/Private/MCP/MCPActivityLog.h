// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** A single recorded MCP tool call */
struct FMCPLogEntry
{
	FDateTime Timestamp;       // UTC time of the call
	FString   ToolName;        // e.g. "blueprint_transaction", "blueprint_modify"
	FString   Operation;       // For router tools: "add_node", "get_nodes", etc.
	FString   Summary;         // Short human-readable result message (≤120 chars)
	FString   DetailJson;      // Full result JSON (≤4 KB) for the detail panel
	int32     DurationMs = 0;
	bool      bSuccess    = false;
	int32     OutputChars = 0; // Approximate output size
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPLogEntryAdded, const FMCPLogEntry&);

/**
 * Singleton that records recent MCP tool calls and notifies the dashboard UI.
 *
 * AddEntry() is safe to call from any thread — it marshals to the game thread
 * before appending and firing OnEntryAdded so Slate is never touched off-thread.
 */
class FMCPActivityLog
{
public:
	static FMCPActivityLog& Get();

	/** Record a completed tool call. Thread-safe. */
	void AddEntry(FMCPLogEntry Entry);

	/** Clear the in-memory log. Game thread only. */
	void Clear();

	/** Read all entries in chronological order. Game thread only. */
	const TArray<FMCPLogEntry>& GetEntries() const { return Entries; }

	/**
	 * Seed initial history from mcp-tool-log.jsonl on disk.
	 * Only takes the last MaxEntries lines. Game thread only.
	 */
	void LoadFromFile(const FString& JsonlPath);

	/** Fired on game thread each time a new entry is appended. */
	FOnMCPLogEntryAdded OnEntryAdded;

	static constexpr int32 MaxEntries = 200;

private:
	TArray<FMCPLogEntry> Entries;
};
