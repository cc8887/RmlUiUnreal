using UnrealBuildTool;
using System.IO;

public class RmlUiUnrealJS : ModuleRules
{
    public RmlUiUnrealJS(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RmlUiUnreal", "JsEnv" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "UMG", "Projects", "Json", "HTTP", "ApplicationCore", "RmlUiUnrealWebCompat" });
        PublicSystemLibraries.Add("bcrypt.lib");
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-adapter-puerts-fixture.js"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-completion-burst-puerts-fixture.js"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-plan-cache-workload-puerts-fixture.js"));
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "..", "Puerts", "Content", "JavaScript", "..."), StagedFileType.UFS);
    }
}
