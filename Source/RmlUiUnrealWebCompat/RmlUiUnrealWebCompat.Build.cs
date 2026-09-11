using UnrealBuildTool;
using System.IO;

public class RmlUiUnrealWebCompat : ModuleRules
{
    public RmlUiUnrealWebCompat(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "UMG", "RmlUiUnreal" });
        PrivateDependencyModuleNames.AddRange(new[] { "Engine", "Json", "Projects", "Slate", "SlateCore", "RenderCore" });
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "..."), StagedFileType.UFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "RuntimeCompiler", "..."), StagedFileType.UFS);
    }
}
