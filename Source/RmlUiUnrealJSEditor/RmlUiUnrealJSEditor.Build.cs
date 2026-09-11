using UnrealBuildTool;

public class RmlUiUnrealJSEditor : ModuleRules
{
    public RmlUiUnrealJSEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UMG", "UnrealEd",
            "WorkspaceMenuStructure", "Projects", "RmlUiUnreal", "RmlUiUnrealJS"
        });
    }
}
