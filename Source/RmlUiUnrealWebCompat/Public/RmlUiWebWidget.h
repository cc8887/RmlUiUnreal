#pragma once

#include "CoreMinimal.h"
#include "RmlUiWidget.h"
#include "RmlUiWebWidget.generated.h"

UCLASS(BlueprintType, meta = (DisplayName = "RmlUi Web-Compatible Document"))
class RMLUIUNREALWEBCOMPAT_API URmlUiWebWidget : public URmlUiWidget
{
    GENERATED_BODY()

public:
    /** Empty or RawRml disables compatibility. Profile identifiers are versioned. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility")
    FName CompatibilityProfile = TEXT("WebModernV1");

    /** Compile browser-style CSS in inline/LLM documents once before RmlUi parses them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility")
    bool bCompileDynamicBrowserCss = true;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "RmlUi|Web Compatibility")
    FString LastCompatibilityDiagnostics;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "RmlUi|Web Compatibility")
    bool bLastCompatibilityCompileCacheHit = false;

    UFUNCTION(BlueprintCallable, Category = "RmlUi|Web Compatibility")
    bool SetCompatibilityProfile(FName ProfileId);

    UFUNCTION(BlueprintPure, Category = "RmlUi|Web Compatibility")
    TArray<FName> GetAvailableCompatibilityProfiles() const;

    UFUNCTION(BlueprintPure, Category = "RmlUi|Web Compatibility")
    FName GetDynamicDocumentCompilerId() const;

protected:
    virtual RmlUE_StyleSheet* GetBaseStyleSheet() const override;
    virtual bool PrepareDocumentMarkup(const FString& Markup, const FString& SourcePath, FString& OutMarkup) override;
};
