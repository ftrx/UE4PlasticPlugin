// Copyright (c) 2016-2018 Codice Software - Sebastien Rombauts (sebastien.rombauts@gmail.com)

using UnrealBuildTool;

public class PlasticSourceControl : ModuleRules
{
	public PlasticSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
	    PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Slate",
				"SlateCore",
				"InputCore",
				"EditorStyle",
				"UnrealEd",
				"LevelEditor",
				"SourceControl",
				"XmlParser2",
				"Projects",
				"AssetRegistry",
			}
		);
	}
}
