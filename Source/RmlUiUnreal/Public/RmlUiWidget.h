#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "RmlUiWidget.generated.h"

class SRmlUiWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRmlUiDocumentEvent,
    const FString&, Type, const FString&, ElementId, const FString&, Value);

UCLASS(BlueprintType, meta = (DisplayName = "RmlUi Document"))
class RMLUIUNREAL_API URmlUiWidget : public UWidget
{
    GENERATED_BODY()

public:
    /** Absolute path, or a path relative to Project/Content/RmlUi or the plugin RmlUi content directory. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi")
    FString DocumentPath;

    /** Optional inline RML or supported HTML. Overrides DocumentPath when nonempty. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi", meta = (MultiLine = "true"))
    FString InlineDocument;

    /** Source document path used to resolve stylesheets and images in inline markup. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi")
    FString InlineSourcePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi")
    FVector2D DesiredSize = FVector2D(1280.0, 720.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RmlUi", meta = (ClampMin = "64", ClampMax = "4096"))
    int32 MaxTextureDimension = 2048;

    UPROPERTY(BlueprintAssignable, Category = "RmlUi")
    FOnRmlUiDocumentEvent OnDocumentEvent;

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool LoadDocument(const FString& Path);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool LoadDocumentFromString(const FString& Markup, const FString& SourcePath);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool ReloadDocument();

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool SetElementInnerRml(const FString& Id, const FString& Rml);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool SetElementText(const FString& Id, const FString& Text);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool SetElementProperty(const FString& Id, const FString& Property, const FString& Value);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool SetElementAttribute(const FString& Id, const FString& Attribute, const FString& Value);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool GetElementAttribute(const FString& Id, const FString& Attribute, FString& Value);

    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    void SetDebuggerVisible(bool bVisible);

    /** Loads a font shared by all RmlUi documents. Set fallback for additional character coverage. */
    UFUNCTION(BlueprintCallable, Category = "RmlUi")
    bool LoadFontFace(const FString& FontPath, bool bFallback = true);

    UFUNCTION(BlueprintPure, Category = "RmlUi")
    FString GetLastError() const;

    TSharedPtr<SRmlUiWidget> GetSlateRmlWidget() const { return MyRmlWidget; }
    virtual void SynchronizeProperties() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
    virtual const FText GetPaletteCategory() override;
#endif

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;

private:
    void HandleDocumentEvent(const FString& Type, const FString& ElementId, const FString& Value);
    TSharedPtr<SRmlUiWidget> MyRmlWidget;
};
