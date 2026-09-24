#include "RmlUiWebCompatJSBridge.h"

#include "Interfaces/IPluginManager.h"
#include "JsEnv.h"
#include "JSModuleLoader.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RmlUiWebCompatModule.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
class FWebCompatModuleLoader final : public puerts::DefaultJSModuleLoader
{
public:
    explicit FWebCompatModuleLoader(const FString& Root) : DefaultJSModuleLoader(Root)
    {
        if (const TSharedPtr<IPlugin> PuertsPlugin = IPluginManager::Get().FindPlugin(TEXT("Puerts")))
            Bootstrap = FPaths::Combine(PuertsPlugin->GetContentDir(), TEXT("JavaScript"));
    }

    bool Search(const FString& RequiredDir, const FString& Module, FString& Path, FString& Absolute) override
    {
        return SearchModuleInDir(RequiredDir.IsEmpty() ? ScriptRoot : RequiredDir, Module, Path, Absolute) ||
            SearchModuleInDir(ScriptRoot, Module, Path, Absolute) ||
            (!Bootstrap.IsEmpty() && SearchModuleInDir(Bootstrap, Module, Path, Absolute));
    }

private:
    FString Bootstrap;
};

class FWebCompatLogger final : public puerts::ILogger
{
public:
    explicit FWebCompatLogger(URmlUiWebCompatJSBridge* InBridge) : Bridge(InBridge) {}
    void Log(const FString& Message) const override { UE_LOG(LogTemp, Display, TEXT("WebCompatJS: %s"), *Message); }
    void Info(const FString& Message) const override { Log(Message); }
    void Warn(const FString& Message) const override { UE_LOG(LogTemp, Warning, TEXT("WebCompatJS: %s"), *Message); }
    void Error(const FString& Message) const override
    {
        if (Bridge.IsValid()) Bridge->SetRuntimeError(Message);
        UE_LOG(LogTemp, Warning, TEXT("WebCompatJS: %s"), *Message);
    }

private:
    TWeakObjectPtr<URmlUiWebCompatJSBridge> Bridge;
};

class FPuertsWebDocumentCompiler final : public IRmlUiWebDocumentCompiler
{
public:
    explicit FPuertsWebDocumentCompiler(const FString& RuntimeRoot)
        : Bridge(NewObject<URmlUiWebCompatJSBridge>(GetTransientPackage()))
    {
        Environment = MakeShared<puerts::FJsEnv>(
            std::make_shared<FWebCompatModuleLoader>(RuntimeRoot),
            std::make_shared<FWebCompatLogger>(Bridge.Get()), -1);
        Environment->Start(TEXT("compiler.js"), {{TEXT("bridge"), Bridge.Get()}});
    }

    virtual FName GetCompilerId() const override { return TEXT("PuertsRuntimeV1"); }
    virtual bool Compile(const FString& Markup, const FString& SourcePath,
        FString& OutMarkup, FString& OutDiagnostics) override
    {
        return CompileWithOptions(Markup, SourcePath, FRmlUiCssCompileOptions(), OutMarkup, OutDiagnostics);
    }

    virtual bool CompileWithOptions(const FString& Markup, const FString& SourcePath,
        const FRmlUiCssCompileOptions& Options, FString& OutMarkup, FString& OutDiagnostics) override
    {
        if (!IsInGameThread())
        {
            OutDiagnostics = TEXT("Puerts WebCompat compilation must run on the game thread.");
            return false;
        }
        if (!Bridge.IsValid()) return false;
        Bridge->CapabilityProfile = Options.CapabilityProfile;
        Bridge->CapabilityMode = Options.CapabilityMode;
        TArray<TSharedPtr<FJsonValue>> Degradations;
        for (const FString& Feature : Options.AllowedDegradations) Degradations.Add(MakeShared<FJsonValueString>(Feature));
        Bridge->AllowedDegradationsJson.Reset();
        FJsonSerializer::Serialize(Degradations, TJsonWriterFactory<>::Create(&Bridge->AllowedDegradationsJson));
        return Bridge->Invoke(Markup, SourcePath, OutMarkup, OutDiagnostics);
    }

    bool IsReady() const { return Bridge.IsValid() && Bridge->IsReady(); }

private:
    TStrongObjectPtr<URmlUiWebCompatJSBridge> Bridge;
    TSharedPtr<puerts::FJsEnv> Environment;
};
}

class FRmlUiUnrealWebCompatPuertsModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        const TSharedPtr<IPlugin> WebCompatPlugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"));
        if (!WebCompatPlugin) return;
        const FString RuntimeRoot = FPaths::Combine(WebCompatPlugin->GetContentDir(), TEXT("RuntimeCompiler"));
        TSharedPtr<FPuertsWebDocumentCompiler> Candidate = MakeShared<FPuertsWebDocumentCompiler>(RuntimeRoot);
        if (!Candidate->IsReady())
        {
            UE_LOG(LogTemp, Warning, TEXT("Puerts WebCompat runtime compiler did not initialize from %s."), *RuntimeRoot);
            return;
        }
        RuntimeCompiler = Candidate;
        FRmlUiWebCompatModule::Get().RegisterDocumentCompiler(RuntimeCompiler.ToSharedRef());
        UE_LOG(LogTemp, Display, TEXT("Registered PuertsRuntimeV1 WebCompat document compiler."));
    }

    virtual void ShutdownModule() override
    {
        if (FModuleManager::Get().IsModuleLoaded(TEXT("RmlUiUnrealWebCompat")))
            FRmlUiWebCompatModule::Get().UnregisterDocumentCompiler(TEXT("PuertsRuntimeV1"));
        RuntimeCompiler.Reset();
    }

private:
    TSharedPtr<IRmlUiWebDocumentCompiler> RuntimeCompiler;
};

IMPLEMENT_MODULE(FRmlUiUnrealWebCompatPuertsModule, RmlUiUnrealWebCompatPuerts)
