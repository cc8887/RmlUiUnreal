using UnrealBuildTool;

public class RmlUiUnrealSamplesEditor : ModuleRules
{
    public RmlUiUnrealSamplesEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UMG", "UnrealEd",
            "Projects", "RmlUiUnreal", "RmlUiUnrealJS", "RmlUiUnrealSamples"
        });
    }
}
