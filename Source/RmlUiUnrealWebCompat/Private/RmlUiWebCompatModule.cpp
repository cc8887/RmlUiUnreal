#include "RmlUiWebCompatModule.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RmlUiBridge.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogRmlUiWebCompat);
IMPLEMENT_MODULE(FRmlUiWebCompatModule, RmlUiUnrealWebCompat)

namespace
{
constexpr int32 MaxCompiledDocumentCacheEntries = 32;

bool IsWithinDirectory(const FString& File, const FString& Directory)
{
    FString NormalFile = FPaths::ConvertRelativePathToFull(File);
    FString NormalDirectory = FPaths::ConvertRelativePathToFull(Directory);
    FPaths::NormalizeFilename(NormalFile);
    FPaths::NormalizeDirectoryName(NormalDirectory);
    return NormalFile.StartsWith(NormalDirectory + TEXT("/"), ESearchCase::IgnoreCase);
}

#if WITH_EDITOR
class FNodeWebDocumentCompiler final : public IRmlUiWebDocumentCompiler
{
public:
    explicit FNodeWebDocumentCompiler(FString InToolsRoot) : ToolsRoot(MoveTemp(InToolsRoot)) {}
    virtual FName GetCompilerId() const override { return TEXT("PostCssEditorV1"); }

    virtual bool Compile(const FString& Markup, const FString& SourcePath, FString& OutMarkup, FString& OutDiagnostics) override
    {
        return CompileWithOptions(Markup, SourcePath, FRmlUiCssCompileOptions(), OutMarkup, OutDiagnostics);
    }

    virtual bool CompileWithOptions(const FString& Markup, const FString& SourcePath,
        const FRmlUiCssCompileOptions& Options, FString& OutMarkup, FString& OutDiagnostics) override
    {
        FString IgnoredMotionManifest;
        return CompileWithMotion(Markup, SourcePath, Options, OutMarkup, OutDiagnostics, IgnoredMotionManifest);
    }

    virtual bool CompileWithMotion(const FString& Markup, const FString& SourcePath,
        const FRmlUiCssCompileOptions& Options, FString& OutMarkup, FString& OutDiagnostics,
        FString& OutMotionManifest) override
    {
        OutMarkup.Reset();
        OutDiagnostics.Reset();
        OutMotionManifest.Reset();
        const FString NodeExecutable = FindNodeExecutable();
        const FString Script = FPaths::Combine(ToolsRoot, TEXT("src/compile-string-cli.mjs"));
        if (NodeExecutable.IsEmpty() || !FPaths::FileExists(Script))
        {
            OutDiagnostics = TEXT("The editor WebCompat compiler requires node.exe and Tools/src/compile-string-cli.mjs.");
            return false;
        }

        const FString TemporaryDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiWebCompat/Dynamic"));
        IFileManager::Get().MakeDirectory(*TemporaryDirectory, true);
        const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString RequestPath = FPaths::Combine(TemporaryDirectory, RequestId + TEXT("-request.json"));
        const FString ResponsePath = FPaths::Combine(TemporaryDirectory, RequestId + TEXT("-response.json"));

        TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("markup"), Markup);
        Request->SetStringField(TEXT("sourcePath"), SourcePath);
        Request->SetStringField(TEXT("profile"), Options.CapabilityProfile);
        Request->SetStringField(TEXT("mode"), Options.CapabilityMode);
        TArray<TSharedPtr<FJsonValue>> Degradations;
        for (const FString& Feature : Options.AllowedDegradations) Degradations.Add(MakeShared<FJsonValueString>(Feature));
        Request->SetArrayField(TEXT("allowDegrade"), Degradations);
        FString RequestJson;
        FJsonSerializer::Serialize(Request, TJsonWriterFactory<>::Create(&RequestJson));
        if (!FFileHelper::SaveStringToFile(RequestJson, *RequestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            OutDiagnostics = FString::Printf(TEXT("Could not write WebCompat request file: %s"), *RequestPath);
            return false;
        }

        const FString Arguments = FString::Printf(TEXT("\"%s\" \"%s\" \"%s\""), *Script, *RequestPath, *ResponsePath);
        int32 ReturnCode = -1;
        FString StandardOutput;
        FString StandardError;
        const bool bExecuted = FPlatformProcess::ExecProcess(*NodeExecutable, *Arguments, &ReturnCode,
            &StandardOutput, &StandardError, *ToolsRoot);

        FString ResponseJson;
        const bool bReadResponse = FFileHelper::LoadFileToString(ResponseJson, *ResponsePath);
        IFileManager::Get().Delete(*RequestPath, false, true);
        IFileManager::Get().Delete(*ResponsePath, false, true);
        if (!bExecuted || !bReadResponse)
        {
            OutDiagnostics = FString::Printf(TEXT("WebCompat compiler process failed (%d): %s"), ReturnCode, *StandardError);
            return false;
        }

        TSharedPtr<FJsonObject> Response;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResponseJson), Response) || !Response.IsValid())
        {
            OutDiagnostics = TEXT("WebCompat compiler returned invalid JSON.");
            return false;
        }
        bool bSuccess = false;
        Response->TryGetBoolField(TEXT("success"), bSuccess);
        if (!bSuccess || ReturnCode != 0)
        {
            Response->TryGetStringField(TEXT("error"), OutDiagnostics);
            if (OutDiagnostics.IsEmpty()) OutDiagnostics = StandardError;
            return false;
        }
        if (!Response->TryGetStringField(TEXT("markup"), OutMarkup))
        {
            OutDiagnostics = TEXT("WebCompat compiler response did not contain markup.");
            return false;
        }
        const TSharedPtr<FJsonObject>* MotionManifest = nullptr;
        if (Response->TryGetObjectField(TEXT("motionManifest"), MotionManifest) && MotionManifest && MotionManifest->IsValid())
            FJsonSerializer::Serialize(MotionManifest->ToSharedRef(), TJsonWriterFactory<>::Create(&OutMotionManifest));

        const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
        if (Response->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics)
            FJsonSerializer::Serialize(*Diagnostics, TJsonWriterFactory<>::Create(&OutDiagnostics));
        return true;
    }

private:
    FString FindNodeExecutable()
    {
        if (!CachedNodeExecutable.IsEmpty()) return CachedNodeExecutable;
        int32 ReturnCode = -1;
        FString StandardOutput;
        if (!FPlatformProcess::ExecProcess(TEXT("where.exe"), TEXT("node.exe"), &ReturnCode, &StandardOutput, nullptr) || ReturnCode != 0)
            return FString();
        TArray<FString> Candidates;
        StandardOutput.ParseIntoArrayLines(Candidates, true);
        for (FString Candidate : Candidates)
        {
            Candidate.TrimStartAndEndInline();
            if (FPaths::FileExists(Candidate))
            {
                CachedNodeExecutable = FPaths::ConvertRelativePathToFull(Candidate);
                break;
            }
        }
        return CachedNodeExecutable;
    }

    FString ToolsRoot;
    FString CachedNodeExecutable;
};
#endif
}

FRmlUiWebCompatModule& FRmlUiWebCompatModule::Get()
{
    return FModuleManager::LoadModuleChecked<FRmlUiWebCompatModule>(TEXT("RmlUiUnrealWebCompat"));
}

void FRmlUiWebCompatModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"));
    if (!Plugin)
    {
        LastError = TEXT("RmlUiUnreal plugin directory was not found.");
        UE_LOG(LogRmlUiWebCompat, Error, TEXT("%s"), *LastError);
        return;
    }
    ContentRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content/RmlUi")));
#if WITH_EDITOR
    RegisterDocumentCompiler(MakeShared<FNodeWebDocumentCompiler>(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tools"))));
#endif
    ReloadProfiles();
}

void FRmlUiWebCompatModule::ShutdownModule()
{
    for (const TPair<FName, RmlUE_StyleSheet*>& Entry : Profiles)
        RmlUE_ReleaseStyleSheet(Entry.Value);
    Profiles.Empty();
    DocumentCompilers.Empty();
    ClearCompiledDocumentCache();
}

void FRmlUiWebCompatModule::RegisterDocumentCompiler(TSharedRef<IRmlUiWebDocumentCompiler> Compiler)
{
    const FName CompilerId = Compiler->GetCompilerId();
    DocumentCompilers.RemoveAll([CompilerId](const TSharedRef<IRmlUiWebDocumentCompiler>& Existing)
    {
        return Existing->GetCompilerId() == CompilerId;
    });
    DocumentCompilers.Add(Compiler);
    ClearCompiledDocumentCache();
}

void FRmlUiWebCompatModule::UnregisterDocumentCompiler(FName CompilerId)
{
    const int32 Removed = DocumentCompilers.RemoveAll([CompilerId](const TSharedRef<IRmlUiWebDocumentCompiler>& Existing)
    {
        return Existing->GetCompilerId() == CompilerId;
    });
    if (Removed > 0)
        ClearCompiledDocumentCache();
}

FName FRmlUiWebCompatModule::GetDocumentCompilerId() const
{
    return DocumentCompilers.IsEmpty() ? NAME_None : DocumentCompilers.Last()->GetCompilerId();
}

void FRmlUiWebCompatModule::ClearCompiledDocumentCache()
{
    CompiledDocumentCache.Empty();
    CompiledDocumentOrder.Empty();
}

bool FRmlUiWebCompatModule::CompileDynamicDocument(const FString& Markup, const FString& SourcePath, FString& OutMarkup,
    FString& OutDiagnostics, bool& bOutCacheHit, const FRmlUiCssCompileOptions& Options, FString* OutMotionManifest)
{
    bOutCacheHit = false;
    OutMarkup.Reset();
    OutDiagnostics.Reset();
    if (OutMotionManifest) OutMotionManifest->Reset();
    if (DocumentCompilers.IsEmpty())
    {
        OutDiagnostics = TEXT("No dynamic WebCompat document compiler is registered. Precompile this document during the build.");
        LastError = OutDiagnostics;
        return false;
    }

    const TSharedRef<IRmlUiWebDocumentCompiler>& DocumentCompiler = DocumentCompilers.Last();
    // A legacy compile must never satisfy a strict profile request from the cache.
    TSharedRef<FJsonObject> CacheInput = MakeShared<FJsonObject>();
    CacheInput->SetStringField(TEXT("compiler"), DocumentCompiler->GetCompilerId().ToString() + TEXT("/rmlui-css-2.1.0"));
    CacheInput->SetStringField(TEXT("source"), SourcePath);
    CacheInput->SetStringField(TEXT("markup"), Markup);
    CacheInput->SetStringField(TEXT("profile"), Options.CapabilityProfile);
    CacheInput->SetStringField(TEXT("mode"), Options.CapabilityMode);
    TArray<TSharedPtr<FJsonValue>> Degradations;
    for (const FString& Feature : Options.AllowedDegradations) Degradations.Add(MakeShared<FJsonValueString>(Feature));
    CacheInput->SetArrayField(TEXT("allowDegrade"), Degradations);
    FString CacheJson;
    FJsonSerializer::Serialize(CacheInput, TJsonWriterFactory<>::Create(&CacheJson));
    const FString CacheKey = FMD5::HashAnsiString(*CacheJson);
    if (const FCompiledDocument* Cached = CompiledDocumentCache.Find(CacheKey))
    {
        OutMarkup = Cached->Markup;
        OutDiagnostics = Cached->Diagnostics;
        if (OutMotionManifest) *OutMotionManifest = Cached->MotionManifest;
        bOutCacheHit = true;
        ++DynamicCacheHitCount;
        return true;
    }

    ++DynamicCompileCount;
    FString MotionManifest;
    if (!DocumentCompiler->CompileWithMotion(Markup, SourcePath, Options, OutMarkup, OutDiagnostics, MotionManifest))
    {
        LastError = OutDiagnostics;
        UE_LOG(LogRmlUiWebCompat, Error, TEXT("Dynamic document compilation failed: %s"), *OutDiagnostics);
        return false;
    }
    LastError.Reset();
    if (CompiledDocumentOrder.Num() >= MaxCompiledDocumentCacheEntries)
        CompiledDocumentCache.Remove(CompiledDocumentOrder[0]);
    if (CompiledDocumentOrder.Num() >= MaxCompiledDocumentCacheEntries)
        CompiledDocumentOrder.RemoveAt(0, 1, EAllowShrinking::No);
    CompiledDocumentOrder.Add(CacheKey);
    if (OutMotionManifest) *OutMotionManifest = MotionManifest;
    CompiledDocumentCache.Add(CacheKey, {OutMarkup, OutDiagnostics, MotionManifest});
    return true;
}

RmlUE_StyleSheet* FRmlUiWebCompatModule::FindProfile(FName ProfileId) const
{
    if (const RmlUE_StyleSheet* const* Found = Profiles.Find(ProfileId))
        return const_cast<RmlUE_StyleSheet*>(*Found);
    return nullptr;
}

TArray<FName> FRmlUiWebCompatModule::GetProfileIds() const
{
    TArray<FName> Result;
    Profiles.GetKeys(Result);
    Result.Sort(FNameLexicalLess());
    return Result;
}

bool FRmlUiWebCompatModule::ReloadProfiles()
{
    LastError.Reset();
    const FString ProfileDirectory = FPaths::Combine(ContentRoot, TEXT("Profiles"));
    TArray<FString> Manifests;
    IFileManager::Get().FindFilesRecursive(Manifests, *ProfileDirectory, TEXT("*.json"), true, false);
    Manifests.Sort();

    TMap<FName, RmlUE_StyleSheet*> LoadedProfiles;
    for (const FString& ManifestPath : Manifests)
    {
        FString JsonText;
        TSharedPtr<FJsonObject> Manifest;
        if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Manifest) || !Manifest.IsValid())
        {
            LastError = FString::Printf(TEXT("Invalid compatibility profile manifest: %s"), *ManifestPath);
            break;
        }
        FString Id;
        double SchemaVersion = 0;
        const TArray<TSharedPtr<FJsonValue>>* BaseStyles = nullptr;
        if (!Manifest->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty() ||
            !Manifest->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) || SchemaVersion != 1 ||
            !Manifest->TryGetArrayField(TEXT("baseStyles"), BaseStyles) || BaseStyles->IsEmpty())
        {
            LastError = FString::Printf(TEXT("Compatibility profile has an invalid schema: %s"), *ManifestPath);
            break;
        }
        const FName ProfileId(*Id);
        if (ProfileId == TEXT("RawRml") || LoadedProfiles.Contains(ProfileId))
        {
            LastError = FString::Printf(TEXT("Duplicate or reserved compatibility profile id '%s'."), *Id);
            break;
        }

        FString CombinedRcss;
        for (const TSharedPtr<FJsonValue>& StyleValue : *BaseStyles)
        {
            FString RelativePath;
            if (!StyleValue.IsValid() || !StyleValue->TryGetString(RelativePath))
            {
                LastError = FString::Printf(TEXT("Profile '%s' contains a non-string baseStyles entry."), *Id);
                break;
            }
            const FString StylePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(ManifestPath), RelativePath));
            FString StyleText;
            if (!IsWithinDirectory(StylePath, ContentRoot) || !FFileHelper::LoadFileToString(StyleText, *StylePath))
            {
                LastError = FString::Printf(TEXT("Profile '%s' cannot read style '%s'."), *Id, *RelativePath);
                break;
            }
            CombinedRcss += StyleText;
            CombinedRcss += TEXT("\n");
        }
        if (!LastError.IsEmpty()) break;

        RmlUE_StyleSheet* StyleSheet = RmlUE_CreateStyleSheet(TCHAR_TO_UTF8(*CombinedRcss));
        if (!StyleSheet)
        {
            LastError = FString::Printf(TEXT("Profile '%s' RCSS failed to parse: %s"), *Id, UTF8_TO_TCHAR(RmlUE_GetLastError()));
            break;
        }
        LoadedProfiles.Add(ProfileId, StyleSheet);
    }

    if (!LastError.IsEmpty())
    {
        for (const TPair<FName, RmlUE_StyleSheet*>& Entry : LoadedProfiles)
            RmlUE_ReleaseStyleSheet(Entry.Value);
        UE_LOG(LogRmlUiWebCompat, Error, TEXT("%s"), *LastError);
        return false;
    }

    for (const TPair<FName, RmlUE_StyleSheet*>& Entry : Profiles)
        RmlUE_ReleaseStyleSheet(Entry.Value);
    Profiles = MoveTemp(LoadedProfiles);
    UE_LOG(LogRmlUiWebCompat, Display, TEXT("Loaded %d versioned Web compatibility profile(s)."), Profiles.Num());
    return true;
}
