using UnrealBuildTool;
using System.IO;

public class RmlUiUnrealSamples : ModuleRules
{
    public RmlUiUnrealSamples(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RmlUiUnreal", "RmlUiUnrealJS" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "HTTP", "UMG", "Slate", "SlateCore", "Projects", "ApplicationCore" });
        foreach (string Name in new[] { "Vue", "Chat", "ActorObserver" })
        {
            ExternalDependencies.Add(Path.Combine(PluginDirectory, "Content", Name, "current.json"));
            RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", Name, "..."), StagedFileType.UFS);
        }
    }
}
