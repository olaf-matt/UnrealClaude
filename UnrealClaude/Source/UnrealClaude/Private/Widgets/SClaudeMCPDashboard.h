// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Views/SListView.h"
#include "MCP/MCPActivityLog.h"

class STextBlock;
class SScrollBox;
class SVerticalBox;

/**
 * MCP Activity Dashboard — the primary Claude panel in UE editor.
 *
 * Layout:
 *   ┌─ Toolbar ──────────────────────────────── [● Live] [Clear] ─┐
 *   │  Log list (scrollable)                                       │
 *   │    ✓ 20:38  blueprint_transaction  8/8 ops  355ms           │
 *   │    ✓ 20:36  blueprint_modify/add_variable   353ms           │
 *   │    ✗ 20:35  blueprint_modify/add_node  err  412ms           │
 *   ├─ Detail panel (selected row JSON) ──────────────────────────┤
 *   ├─ Stats ──────────────────────────────────────────────────────┤
 *   │  blueprint_modify  12 calls  0 err  381ms avg               │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * Zero API cost — reads only from the in-process FMCPActivityLog singleton.
 */
class SClaudeMCPDashboard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaudeMCPDashboard)
	{}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SClaudeMCPDashboard();

private:
	// ── Widget builders ───────────────────────────────────────────
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildLogList();
	TSharedRef<SWidget> BuildDetailPanel();
	TSharedRef<SWidget> BuildStatsPanel();

	// ── List view callbacks ───────────────────────────────────────
	TSharedRef<ITableRow> GenerateLogRow(
		TSharedPtr<FMCPLogEntry> Entry,
		const TSharedRef<STableViewBase>& OwnerTable);

	void OnSelectionChanged(
		TSharedPtr<FMCPLogEntry> Entry,
		ESelectInfo::Type SelectType);

	// ── Activity log callbacks ────────────────────────────────────
	void OnEntryAdded(const FMCPLogEntry& Entry);

	// ── Helpers ───────────────────────────────────────────────────
	void PopulateFromLog();
	void RefreshStats();
	FReply OnClearClicked();
	FText GetLiveButtonText() const;
	FSlateColor GetLiveButtonColor() const;
	FReply OnLiveToggleClicked();

private:
	// Live mode — when true new entries auto-scroll the list
	bool bLiveEnabled = true;

	// Source data for the list view (shared ptrs so SListView can hold them)
	TArray<TSharedPtr<FMCPLogEntry>> DisplayEntries;

	// Widgets that need to be updated at runtime
	TSharedPtr<SListView<TSharedPtr<FMCPLogEntry>>> LogListView;
	TSharedPtr<STextBlock>                          DetailText;
	TSharedPtr<STextBlock>                          StatsText;
	TSharedPtr<STextBlock>                          EntryCountText;

	// Delegate handle — unregistered in destructor
	FDelegateHandle LogDelegateHandle;
};
