using UnrealBuildTool;
using System.IO;

public class RmlUiUnreal : ModuleRules
{
    public RmlUiUnreal(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "UMG", "InputCore" });
        PrivateDependencyModuleNames.AddRange(new[] { "Projects", "ImageWrapper", "RenderCore", "RHI", "ApplicationCore", "TraceLog", "MovieScene" });
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "RmlUiBridge", "include"));
        if (Target.Platform != UnrealTargetPlatform.Win64)
        {
            throw new BuildException("RmlUiUnreal currently supports Win64.");
        }
        string NativeDirectory = Path.GetFullPath(Path.Combine(PluginDirectory, "Binaries", "ThirdParty", "Win64"));
        string ImportLibrary = Path.Combine(NativeDirectory, "RmlUiBridge.lib");
        if (!File.Exists(ImportLibrary))
        {
            throw new BuildException("RmlUiBridge.lib is missing. Run the project's Build.ps1 or the plugin's Source/ThirdParty/RmlUiBridge/BuildBridge.ps1 first.");
        }
        PublicAdditionalLibraries.Add(ImportLibrary);
        PublicDelayLoadDLLs.Add("RmlUiBridge.dll");
        RuntimeDependencies.Add(Path.Combine(NativeDirectory, "RmlUiBridge.dll"));
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Content", "RmlUi", "..."), StagedFileType.UFS);
    }
}
