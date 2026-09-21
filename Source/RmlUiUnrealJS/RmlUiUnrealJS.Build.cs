using UnrealBuildTool;
using System.IO;

public class RmlUiUnrealJS : ModuleRules
{
    public RmlUiUnrealJS(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RmlUiUnreal", "JsEnv" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "UMG", "Projects", "Json", "HTTP", "ApplicationCore" });
        PublicSystemLibraries.Add("bcrypt.lib");
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "Vue", "current.json"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "Chat", "current.json"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "ActorObserver", "current.json"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-adapter-puerts-fixture.js"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-completion-burst-puerts-fixture.js"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "Tests", "animation-plan-cache-workload-puerts-fixture.js"));
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "Chat", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "Vue", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "ActorObserver", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "..", "Puerts", "Content", "JavaScript", "..."), StagedFileType.UFS);
    }
}
