// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPActivityLog.h"
#include "Containers/Ticker.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ─── Singleton ────────────────────────────────────────────────────────────────

FMCPActivityLog& FMCPActivityLog::Get()
{
	static FMCPActivityLog Instance;
	return Instance;
}

// ─── AddEntry ────────────────────────────────────────────────────────────────

void FMCPActivityLog::AddEntry(FMCPLogEntry InEntry)
{
	if (IsInGameThread())
	{
		if (Entries.Num() >= MaxEntries)
			Entries.RemoveAt(0, 1, /*bAllowShrinking=*/false);
		Entries.Add(InEntry);
		OnEntryAdded.Broadcast(Entries.Last());
	}
	else
	{
		// Marshal to game thread — copy the entry into the lambda
		FTSTicker::GetCoreTicker().AddTicker(TEXT("MCPActivity_Add"), 0.0f,
			[this, Entry = MoveTemp(InEntry)](float) mutable -> bool
		{
			if (Entries.Num() >= MaxEntries)
				Entries.RemoveAt(0, 1, false);
			Entries.Add(MoveTemp(Entry));
			OnEntryAdded.Broadcast(Entries.Last());
			return false; // one-shot
		});
	}
}

// ─── Clear ────────────────────────────────────────────────────────────────────

void FMCPActivityLog::Clear()
{
	check(IsInGameThread());
	Entries.Empty();
}

// ─── LoadFromFile ─────────────────────────────────────────────────────────────

void FMCPActivityLog::LoadFromFile(const FString& JsonlPath)
{
	check(IsInGameThread());

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *JsonlPath))
		return;

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	// Only take the last MaxEntries lines so startup is instant on large logs
	const int32 StartIdx = FMath::Max(0, Lines.Num() - MaxEntries);

	for (int32 i = StartIdx; i < Lines.Num(); ++i)
	{
		const FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty())
			continue;

		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
			continue;

		FMCPLogEntry Entry;

		FString TsStr;
		if (Obj->TryGetStringField(TEXT("ts"), TsStr))
			FDateTime::ParseIso8601(*TsStr, Entry.Timestamp);
		else
			Entry.Timestamp = FDateTime::UtcNow();

		Obj->TryGetStringField(TEXT("tool"),      Entry.ToolName);
		Obj->TryGetStringField(TEXT("operation"), Entry.Operation);

		double Dur = 0.0;
		if (Obj->TryGetNumberField(TEXT("duration_ms"), Dur))
			Entry.DurationMs = (int32)Dur;

		Obj->TryGetBoolField(TEXT("success"), Entry.bSuccess);

		double OutC = 0.0;
		if (Obj->TryGetNumberField(TEXT("output_chars"), OutC))
			Entry.OutputChars = (int32)OutC;

		// summary field added to mcp-tool-log.jsonl — present for entries logged after this fix
		Obj->TryGetStringField(TEXT("summary"), Entry.Summary);
		// Older entries without summary stay blank — tool name + duration is still useful

		if (!Entry.ToolName.IsEmpty())
			Entries.Add(MoveTemp(Entry));
	}
}
