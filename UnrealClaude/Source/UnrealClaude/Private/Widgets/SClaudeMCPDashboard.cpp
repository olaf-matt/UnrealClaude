// Copyright Natali Caggiano. All Rights Reserved.

#include "SClaudeMCPDashboard.h"
#include "MCP/MCPActivityLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Styling/AppStyle.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "SClaudeMCPDashboard"

// ─── Helpers ──────────────────────────────────────────────────────────────────

static FString FormatTimestamp(const FDateTime& Ts)
{
	// Display UTC time as HH:MM:SS
	return FString::Printf(TEXT("%02d:%02d:%02d"), Ts.GetHour(), Ts.GetMinute(), Ts.GetSecond());
}

static FString FormatToolLabel(const FMCPLogEntry& Entry)
{
	if (!Entry.Operation.IsEmpty())
		return Entry.ToolName + TEXT(" / ") + Entry.Operation;
	return Entry.ToolName;
}

static FText FormatDuration(int32 Ms)
{
	if (Ms >= 1000)
		return FText::FromString(FString::Printf(TEXT("%.1fs"), Ms / 1000.0f));
	return FText::FromString(FString::Printf(TEXT("%dms"), Ms));
}

// ─── Construct ───────────────────────────────────────────────────────────────

void SClaudeMCPDashboard::Construct(const FArguments& InArgs)
{
	// Subscribe to the activity log
	LogDelegateHandle = FMCPActivityLog::Get().OnEntryAdded.AddRaw(
		this, &SClaudeMCPDashboard::OnEntryAdded);

	// Seed from the log file on disk (last 200 entries)
	FString McpLogPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("claude"), TEXT("mcp-tool-log.jsonl"));
	if (FPaths::FileExists(McpLogPath))
		FMCPActivityLog::Get().LoadFromFile(McpLogPath);

	// Populate display list from current singleton state
	PopulateFromLog();

	// Build UI
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(0)
		[
			SNew(SVerticalBox)

			// Toolbar
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildToolbar()
			]

			// Log + detail in a vertical splitter (log gets most space)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(EOrientation::Orient_Vertical)

				+ SSplitter::Slot()
				.Value(0.6f)
				[
					BuildLogList()
				]

				+ SSplitter::Slot()
				.Value(0.2f)
				[
					BuildDetailPanel()
				]

				+ SSplitter::Slot()
				.Value(0.2f)
				[
					BuildStatsPanel()
				]
			]
		]
	];

	// Scroll to bottom on first open
	if (LogListView.IsValid() && DisplayEntries.Num() > 0)
		LogListView->RequestScrollIntoView(DisplayEntries.Last());

	RefreshStats();
}

SClaudeMCPDashboard::~SClaudeMCPDashboard()
{
	FMCPActivityLog::Get().OnEntryAdded.Remove(LogDelegateHandle);
}

// ─── Toolbar ─────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SClaudeMCPDashboard::BuildToolbar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("EditorViewportToolBar.Background"))
		.Padding(FMargin(4, 3))
		[
			SNew(SHorizontalBox)

			// Title
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "MCP Activity"))
				.Font(FAppStyle::GetFontStyle("BoldFont"))
			]

			// Entry count
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SAssignNew(EntryCountText, STextBlock)
				.Text(FText::FromString(TEXT("0 calls")))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
			]

			// Spacer
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)

			// Live toggle button
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton")
				.OnClicked(this, &SClaudeMCPDashboard::OnLiveToggleClicked)
				.ToolTipText(LOCTEXT("LiveTip", "Toggle live updates — when ON new calls auto-scroll the list"))
				[
					SNew(STextBlock)
					.Text(this, &SClaudeMCPDashboard::GetLiveButtonText)
					.ColorAndOpacity(this, &SClaudeMCPDashboard::GetLiveButtonColor)
				]
			]

			// Clear button
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton")
				.Text(LOCTEXT("ClearBtn", "Clear"))
				.ToolTipText(LOCTEXT("ClearTip", "Clear the in-memory activity log"))
				.OnClicked(this, &SClaudeMCPDashboard::OnClearClicked)
			]
		];
}

FText SClaudeMCPDashboard::GetLiveButtonText() const
{
	return bLiveEnabled
		? LOCTEXT("LiveOn",  "● Live")
		: LOCTEXT("LiveOff", "○ Paused");
}

FSlateColor SClaudeMCPDashboard::GetLiveButtonColor() const
{
	return bLiveEnabled
		? FLinearColor(0.2f, 0.9f, 0.2f)
		: FLinearColor(0.5f, 0.5f, 0.5f);
}

FReply SClaudeMCPDashboard::OnLiveToggleClicked()
{
	bLiveEnabled = !bLiveEnabled;
	return FReply::Handled();
}

FReply SClaudeMCPDashboard::OnClearClicked()
{
	FMCPActivityLog::Get().Clear();
	DisplayEntries.Empty();
	if (LogListView.IsValid())
		LogListView->RebuildList();
	if (DetailText.IsValid())
		DetailText->SetText(LOCTEXT("SelectHint", "Select a row to inspect the result JSON."));
	if (EntryCountText.IsValid())
		EntryCountText->SetText(FText::FromString(TEXT("0 calls")));
	RefreshStats();
	return FReply::Handled();
}

// ─── Log list ────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SClaudeMCPDashboard::BuildLogList()
{
	// Static column header — matches GenerateLogRow layout exactly
	const FLinearColor HeaderColor(0.55f, 0.55f, 0.55f);
	TSharedRef<SWidget> ColumnHeader =
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
		.Padding(FMargin(0, 2))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[SNew(SBox).WidthOverride(24)[SNew(STextBlock).Text(FText::GetEmpty())]]

			+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
			[SNew(SBox).WidthOverride(62)
				[SNew(STextBlock).Text(LOCTEXT("ColTime","Time")).ColorAndOpacity(HeaderColor)]]

			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0)
			[SNew(STextBlock).Text(LOCTEXT("ColTool","Tool / Operation")).ColorAndOpacity(HeaderColor)]

			+ SHorizontalBox::Slot().FillWidth(1.5f).Padding(2, 0)
			[SNew(STextBlock).Text(LOCTEXT("ColSummary","Summary")).ColorAndOpacity(HeaderColor)]

			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right).Padding(4, 0)
			[SNew(SBox).WidthOverride(60)
				[SNew(STextBlock).Text(LOCTEXT("ColDuration","Duration")).ColorAndOpacity(HeaderColor).Justification(ETextJustify::Right)]]
		];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[ ColumnHeader ]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SAssignNew(LogListView, SListView<TSharedPtr<FMCPLogEntry>>)
				.ListItemsSource(&DisplayEntries)
				.OnGenerateRow(this, &SClaudeMCPDashboard::GenerateLogRow)
				.OnSelectionChanged(this, &SClaudeMCPDashboard::OnSelectionChanged)
				.SelectionMode(ESelectionMode::Single)
			]
		];
}

TSharedRef<ITableRow> SClaudeMCPDashboard::GenerateLogRow(
	TSharedPtr<FMCPLogEntry> Entry,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const FLinearColor StatusColor = Entry->bSuccess
		? FLinearColor(0.2f, 0.85f, 0.2f)
		: FLinearColor(0.95f, 0.3f, 0.3f);

	const FString StatusGlyph = Entry->bSuccess ? TEXT("✓") : TEXT("✗");
	const FString ToolLabel   = FormatToolLabel(*Entry);
	const FString TimeStr     = FormatTimestamp(Entry->Timestamp);
	// Truncate summary at 80 chars for display
	const FString SummaryStr  = Entry->Summary.Len() > 80
		? Entry->Summary.Left(77) + TEXT("...")
		: Entry->Summary;

	return SNew(STableRow<TSharedPtr<FMCPLogEntry>>, OwnerTable)
	[
		SNew(SHorizontalBox)

		// Status icon (fixed 24px)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(24)
			[
				SNew(STextBlock)
				.Text(FText::FromString(StatusGlyph))
				.ColorAndOpacity(StatusColor)
				.Justification(ETextJustify::Center)
			]
		]

		// Time (fixed ~62px)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2, 0)
		[
			SNew(SBox)
			.WidthOverride(62)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TimeStr))
				.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f))
			]
		]

		// Tool / operation (fills remaining space equally with summary)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(2, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(ToolLabel))
		]

		// Summary
		+ SHorizontalBox::Slot()
		.FillWidth(1.5f)
		.VAlign(VAlign_Center)
		.Padding(2, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(SummaryStr))
			.ColorAndOpacity(Entry->bSuccess
				? FLinearColor(0.8f, 0.8f, 0.8f)
				: FLinearColor(1.0f, 0.6f, 0.6f))
		]

		// Duration (fixed ~60px, right-aligned)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Right)
		.Padding(4, 0)
		[
			SNew(SBox)
			.WidthOverride(60)
			[
				SNew(STextBlock)
				.Text(FormatDuration(Entry->DurationMs))
				.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.55f))
				.Justification(ETextJustify::Right)
			]
		]
	];
}

void SClaudeMCPDashboard::OnSelectionChanged(
	TSharedPtr<FMCPLogEntry> Entry,
	ESelectInfo::Type /*SelectType*/)
{
	if (!DetailText.IsValid())
		return;

	if (!Entry.IsValid())
	{
		DetailText->SetText(LOCTEXT("SelectHint", "Select a row to inspect the result JSON."));
		return;
	}

	// Build detail string: header + JSON (or fallback message)
	FString Detail = FString::Printf(
		TEXT("[%s]  %s\nDuration: %dms  |  Success: %s\n\n"),
		*FormatTimestamp(Entry->Timestamp),
		*FormatToolLabel(*Entry),
		Entry->DurationMs,
		Entry->bSuccess ? TEXT("yes") : TEXT("no"));

	if (!Entry->DetailJson.IsEmpty())
		Detail += Entry->DetailJson;
	else
		Detail += TEXT("(no detail JSON — call was recorded from log file)");

	DetailText->SetText(FText::FromString(Detail));
}

// ─── Detail panel ─────────────────────────────────────────────────────────────

TSharedRef<SWidget> SClaudeMCPDashboard::BuildDetailPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(DetailText, STextBlock)
				.Text(LOCTEXT("SelectHint", "Select a row to inspect the result JSON."))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(FLinearColor(0.75f, 0.75f, 0.75f))
				.AutoWrapText(true)
			]
		];
}

// ─── Stats panel ─────────────────────────────────────────────────────────────

TSharedRef<SWidget> SClaudeMCPDashboard::BuildStatsPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(6, 4))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StatsTitle", "Stats"))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.55f))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0, 2, 0, 0)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(StatsText, STextBlock)
					.Text(LOCTEXT("StatsEmpty", "No calls recorded yet."))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
				]
			]
		];
}

void SClaudeMCPDashboard::RefreshStats()
{
	struct FToolStat
	{
		int32 Calls       = 0;
		int32 Errors      = 0;
		int64 TotalDurMs  = 0;
	};

	TMap<FString, FToolStat> Stats;
	for (const auto& E : DisplayEntries)
	{
		FToolStat& S = Stats.FindOrAdd(E->ToolName);
		S.Calls++;
		if (!E->bSuccess) S.Errors++;
		S.TotalDurMs += E->DurationMs;
	}

	// Sort by call count descending
	TArray<TPair<FString, FToolStat>> Sorted;
	for (auto& KV : Stats)
		Sorted.Add(KV);
	Sorted.Sort([](const TPair<FString, FToolStat>& A, const TPair<FString, FToolStat>& B)
	{
		return A.Value.Calls > B.Value.Calls;
	});

	FString StatsStr;
	const int32 MaxShow = FMath::Min(Sorted.Num(), 10);
	for (int32 i = 0; i < MaxShow; ++i)
	{
		const FString& Name  = Sorted[i].Key;
		const FToolStat& S   = Sorted[i].Value;
		const int32 AvgMs    = S.Calls > 0 ? (int32)(S.TotalDurMs / S.Calls) : 0;
		const FString ErrStr = S.Errors > 0
			? FString::Printf(TEXT("  %d err"), S.Errors)
			: TEXT("        ");

		StatsStr += FString::Printf(TEXT("%-32s  %3d call%s  %4dms avg%s\n"),
			*Name,
			S.Calls, S.Calls == 1 ? TEXT(" ") : TEXT("s"),
			AvgMs,
			*ErrStr);
	}
	if (StatsStr.IsEmpty())
		StatsStr = TEXT("No calls recorded yet.");

	if (StatsText.IsValid())
		StatsText->SetText(FText::FromString(StatsStr.TrimEnd()));

	// Update entry count label
	if (EntryCountText.IsValid())
	{
		const int32 Total = DisplayEntries.Num();
		EntryCountText->SetText(FText::FromString(
			FString::Printf(TEXT("%d call%s"), Total, Total == 1 ? TEXT("") : TEXT("s"))));
	}
}

// ─── Live entry arrival ───────────────────────────────────────────────────────

void SClaudeMCPDashboard::OnEntryAdded(const FMCPLogEntry& Entry)
{
	DisplayEntries.Add(MakeShared<FMCPLogEntry>(Entry));

	if (LogListView.IsValid())
	{
		LogListView->RebuildList();
		if (bLiveEnabled)
			LogListView->RequestScrollIntoView(DisplayEntries.Last());
	}

	RefreshStats();
}

void SClaudeMCPDashboard::PopulateFromLog()
{
	DisplayEntries.Empty();
	for (const FMCPLogEntry& E : FMCPActivityLog::Get().GetEntries())
		DisplayEntries.Add(MakeShared<FMCPLogEntry>(E));
}

#undef LOCTEXT_NAMESPACE
