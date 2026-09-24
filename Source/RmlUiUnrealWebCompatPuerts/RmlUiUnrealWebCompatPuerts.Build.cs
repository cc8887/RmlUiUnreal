using UnrealBuildTool;

public class RmlUiUnrealWebCompatPuerts : ModuleRules
{
    public RmlUiUnrealWebCompatPuerts(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(new[] { "JsEnv", "Projects", "Json", "RmlUiUnrealWebCompat" });
    }
}
