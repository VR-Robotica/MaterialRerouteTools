// Copyright VR Robotica. All Rights Reserved.

using UnrealBuildTool;

/** Material Reroute Tools: editor-only material graph context menu actions. Never ships with the game. */
public class MaterialRerouteTools : ModuleRules
{
	public MaterialRerouteTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",		// Substrate checks behind the Result node's visible pins
			"UnrealEd",			// UMaterialGraph, factories, saving, transactions
			"MaterialEditor",	// IMaterialEditor, FMaterialEditorUtilities, UMaterialEditingLibrary
			"ToolMenus",		// graph context menu extension
			"AssetTools",		// creating the material function asset
			"AssetRegistry",	// registering exported node textures
			"RHI",				// shader platform the node texture is compiled for
			"ContentBrowser",	// save asset dialog
			"DeveloperSettings",	// Project Settings page for prefixes and colors
			"InputCore",		// keys used by the export dialog's size combo box
			"Slate",
			"SlateCore"
		});
	}
}
