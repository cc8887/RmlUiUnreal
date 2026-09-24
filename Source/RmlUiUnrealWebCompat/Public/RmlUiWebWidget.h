#pragma once

#include "CoreMinimal.h"
#include "RmlUiWidget.h"
#include "RmlUiWebWidget.generated.h"

UCLASS(BlueprintType, meta = (DisplayName = "RmlUi Web-Compatible Document"))
class RMLUIUNREALWEBCOMPAT_API URmlUiWebWidget : public URmlUiWidget
{
    GENERATED_BODY()

public:
    virtual ~URmlUiWebWidget() override;
    /** Empty or RawRml disables compatibility. Profile identifiers are versioned. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility")
    FName CompatibilityProfile = TEXT("WebModernV1");

    /** Compile browser-style CSS in inline/LLM documents once before RmlUi parses them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility")
    bool bCompileDynamicBrowserCss = true;

    /** Opt in for new documents: validate CSS against the selected renderer before loading it. RawRml remains unchanged. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility")
    bool bEnforceRendererCapabilities = false;

    /** Explicitly removable feature IDs, for example render.layers. Each removed declaration appears in diagnostics. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi|Web Compatibility", meta = (EditCondition = "bEnforceRendererCapabilities"))
    TArray<FString> AllowedCssDegradations;

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

    UFUNCTION(BlueprintPure, Category = "RmlUi|Web Compatibility|Diagnostics")
    FString GetCssAnimationDiagnostics() const;

    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

protected:
    virtual RmlUE_StyleSheet* GetBaseStyleSheet() const override;
    virtual bool PrepareDocumentMarkup(const FString& Markup, const FString& SourcePath, FString& OutMarkup) override;
    virtual void OnDocumentLoaded(bool bSuccess) override;

private:
    void AdvanceCssAnimationActivation(float DeltaSeconds);
    FString PendingMotionManifest;
    class IRmlUiCssAnimationSession* CssAnimationSession = nullptr;
    FDelegateHandle CssAnimationFrameHandle;
};
