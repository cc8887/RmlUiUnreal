using UnrealBuildTool;
using System.IO;

public class RmlUiUnrealJS : ModuleRules
{
    public RmlUiUnrealJS(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RmlUiUnreal" });
        PrivateDependencyModuleNames.AddRange(new[] { "JsEnv", "Slate", "SlateCore", "UMG", "Projects", "Json", "HTTP", "ApplicationCore" });
        PublicSystemLibraries.Add("bcrypt.lib");
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "Vue", "current.json"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "Chat", "current.json"));
        ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", "ActorObserver", "current.json"));
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "Chat", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "Vue", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "ActorObserver", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "..", "Puerts", "Content", "JavaScript", "..."), StagedFileType.UFS);
    }
}
