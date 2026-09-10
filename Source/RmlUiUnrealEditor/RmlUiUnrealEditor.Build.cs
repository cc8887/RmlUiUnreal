using UnrealBuildTool;

public class RmlUiUnrealEditor : ModuleRules
{
    public RmlUiUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UMG", "RmlUiUnreal",
            "UnrealEd", "WorkspaceMenuStructure", "DesktopPlatform", "Projects", "RenderCore"
        });
    }
}
