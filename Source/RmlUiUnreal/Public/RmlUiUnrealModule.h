#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SRmlUiWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogRmlUiUnreal, Log, All);

class RMLUIUNREAL_API FRmlUiUnrealModule : public IModuleInterface
{
public:
    static FRmlUiUnrealModule& Get();
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    bool IsInitialized() const { return bInitialized; }
    const FString& GetInitializationError() const { return InitializationError; }
    FString GetContentRoot() const;
    FString GetDefaultDocumentPath() const;
    FString ResolveDocumentPath(const FString& Path) const;
    void RegisterWidget(const TSharedRef<SRmlUiWidget>& Widget);

private:
    void* BridgeDll = nullptr;
    bool bInitialized = false;
    FString PluginRoot;
    FString InitializationError;
    TArray<TWeakPtr<SRmlUiWidget>> Widgets;
};
