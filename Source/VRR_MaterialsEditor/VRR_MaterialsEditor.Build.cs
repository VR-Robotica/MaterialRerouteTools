using UnrealBuildTool;

/** Editor-only tools for VRR_Materials (material graph context menu actions). Never ships with the game. */
public class VRR_MaterialsEditor : ModuleRules
{
	public VRR_MaterialsEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"ContentBrowser",	// save asset dialog
			"Slate",
			"SlateCore"
		});
	}
}
