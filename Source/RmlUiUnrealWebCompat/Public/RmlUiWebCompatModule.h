#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

struct RmlUE_StyleSheet;

DECLARE_LOG_CATEGORY_EXTERN(LogRmlUiWebCompat, Log, All);

struct RMLUIUNREALWEBCOMPAT_API FRmlUiCssCompileOptions
{
    FString CapabilityProfile = TEXT("legacy");
    FString CapabilityMode = TEXT("strict");
    TArray<FString> AllowedDegradations;
};

class RMLUIUNREALWEBCOMPAT_API IRmlUiWebDocumentCompiler
{
public:
    virtual ~IRmlUiWebDocumentCompiler() = default;
    virtual FName GetCompilerId() const = 0;
    virtual bool Compile(const FString& Markup, const FString& SourcePath, FString& OutMarkup, FString& OutDiagnostics) = 0;
    virtual bool CompileWithOptions(const FString& Markup, const FString& SourcePath,
        const FRmlUiCssCompileOptions& Options, FString& OutMarkup, FString& OutDiagnostics)
    {
        if (Options.CapabilityProfile != TEXT("legacy"))
        {
            OutDiagnostics = TEXT("This document compiler does not implement renderer capability profiles.");
            return false;
        }
        return Compile(Markup, SourcePath, OutMarkup, OutDiagnostics);
    }
    virtual bool CompileWithMotion(const FString& Markup, const FString& SourcePath,
        const FRmlUiCssCompileOptions& Options, FString& OutMarkup, FString& OutDiagnostics,
        FString& OutMotionManifest)
    {
        OutMotionManifest.Reset();
        return CompileWithOptions(Markup, SourcePath, Options, OutMarkup, OutDiagnostics);
    }
};

class RMLUIUNREALWEBCOMPAT_API FRmlUiWebCompatModule : public IModuleInterface
{
public:
    static FRmlUiWebCompatModule& Get();
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    RmlUE_StyleSheet* FindProfile(FName ProfileId) const;
    TArray<FName> GetProfileIds() const;
    bool ReloadProfiles();
    const FString& GetLastError() const { return LastError; }
    void RegisterDocumentCompiler(TSharedRef<IRmlUiWebDocumentCompiler> Compiler);
    void UnregisterDocumentCompiler(FName CompilerId);
    bool CompileDynamicDocument(const FString& Markup, const FString& SourcePath, FString& OutMarkup,
        FString& OutDiagnostics, bool& bOutCacheHit, const FRmlUiCssCompileOptions& Options = FRmlUiCssCompileOptions(),
        FString* OutMotionManifest = nullptr);
    void ClearCompiledDocumentCache();
    FName GetDocumentCompilerId() const;
    int32 GetDynamicCompileCount() const { return DynamicCompileCount; }
    int32 GetDynamicCacheHitCount() const { return DynamicCacheHitCount; }

private:
    struct FCompiledDocument
    {
        FString Markup;
        FString Diagnostics;
        FString MotionManifest;
    };
    FString ContentRoot;
    FString LastError;
    TMap<FName, RmlUE_StyleSheet*> Profiles;
    TArray<TSharedRef<IRmlUiWebDocumentCompiler>> DocumentCompilers;
    TMap<FString, FCompiledDocument> CompiledDocumentCache;
    TArray<FString> CompiledDocumentOrder;
    int32 DynamicCompileCount = 0;
    int32 DynamicCacheHitCount = 0;
};
