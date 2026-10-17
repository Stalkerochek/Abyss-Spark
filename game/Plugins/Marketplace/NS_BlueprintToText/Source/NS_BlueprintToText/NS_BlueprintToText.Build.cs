// Copyright NodeSmith 2026

using UnrealBuildTool;

public class NS_BlueprintToText : ModuleRules
{
	public NS_BlueprintToText(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd"
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"AssetRegistry",
				"EditorStyle",
				"ToolMenus",
				"ContentBrowser",
				"ContentBrowserData",
				"BlueprintGraph",
				"KismetCompiler",
				"Kismet",
				"GraphEditor",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings"
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
