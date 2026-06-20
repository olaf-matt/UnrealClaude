// Copyright Natali Caggiano. All Rights Reserved.

using UnrealBuildTool;

public class UnrealClaude : ModuleRules
{
	public UnrealClaude(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"UnrealEd",
				"ToolMenus",
				"Projects",
				"EditorFramework",
				"WorkspaceMenuStructure"
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Project Settings -> Plugins -> Unreal Claude (UUnrealClaudeSettings)
				"DeveloperSettings",
				"Json",
				"JsonUtilities",
				"HTTP",
				"HTTPServer",
				"Sockets",
				"Networking",
				"ImageWrapper",
				// Blueprint manipulation
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"GraphEditor",
				"AssetRegistry",
				"AssetTools",
				// Animation Blueprint manipulation
				"AnimGraph",
				"AnimGraphRuntime",
				// Asset saving
				"EditorScriptingUtilities",
				// Material graph editing (UMaterialEditingLibrary — create/connect/delete expressions,
				// material-function editing, recompile dependents)
				"MaterialEditor",
				// Enhanced Input
				"EnhancedInput",
				// UMG widget-tree authoring (widget tool — USlider/UTextBlock/UPanelWidget/UWidgetTree;
				// UMGEditor for UWidgetBlueprint). UnrealEd (public dep) provides WidgetTree/BaseWidgetBlueprint.
				"UMG",
				"UMGEditor",
				// Niagara runtime (niagara_query — UNiagaraSystem, FNiagaraParameterStore)
				"Niagara",
				// Niagara editor (niagara_modify — UNiagaraGraph, UNiagaraNodeFunctionCall,
				// UNiagaraScriptSource, UNiagaraNodeOutput, FNiagaraEditorUtilities)
				"NiagaraEditor",
				// RHI + RenderCore (for sample_render_target_2d_array —
				// FRHITexture, FReadSurfaceDataFlags, ENQUEUE_RENDER_COMMAND, FlushRenderingCommands)
				"RHI",
				"RenderCore"
			}
		);

		// Clipboard support (FPlatformApplicationMisc) on all platforms
		PrivateDependencyModuleNames.Add("ApplicationCore");

		// Windows only
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// LiveCoding is only available in editor builds on Windows
			if (Target.bBuildEditor)
			{
				PrivateDependencyModuleNames.Add("LiveCoding");
			}
		}
	}
}
