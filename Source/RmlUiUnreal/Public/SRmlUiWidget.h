#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class FDeferredCleanupSlateBrush;
class UTexture2D;
struct RmlUE_View;

DECLARE_DELEGATE_ThreeParams(FOnSlateRmlUiDocumentEvent, const FString&, const FString&, const FString&);

class RMLUIUNREAL_API SRmlUiWidget : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SRmlUiWidget)
        : _DesiredSize(1280.0, 720.0), _MaxTextureDimension(2048) {}
        SLATE_ARGUMENT(FString, DocumentPath)
        SLATE_ARGUMENT(FString, InlineDocument)
        SLATE_ARGUMENT(FString, SourcePath)
        SLATE_ARGUMENT(FVector2D, DesiredSize)
        SLATE_ARGUMENT(int32, MaxTextureDimension)
        SLATE_EVENT(FOnSlateRmlUiDocumentEvent, OnDocumentEvent)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SRmlUiWidget() override;
    bool LoadDocument(const FString& Path);
    bool LoadDocumentFromString(const FString& Markup, const FString& SourcePath = FString());
    bool ReloadDocument();
    bool SetElementInnerRml(const FString& Id, const FString& Rml);
    bool SetElementText(const FString& Id, const FString& Text);
    bool SetElementProperty(const FString& Id, const FString& Property, const FString& Value);
    bool SetElementAttribute(const FString& Id, const FString& Attribute, const FString& Value);
    bool GetElementAttribute(const FString& Id, const FString& Attribute, FString& Value) const;
    void SetDebuggerVisible(bool bVisible);
    void SetDesiredSize(FVector2D InSize);
    void SetMaxTextureDimension(int32 InMaximum);
    void ShutdownNative();
    bool RenderFrame(int32 Width, int32 Height, float DpRatio = 1.0f);
    RmlUE_View* GetNativeView() const { return NativeView; }
    FString GetLastError() const { return DocumentError.IsEmpty() ? LastError : DocumentError; }
    uint64 GetFrameNumber() const { return FrameNumber; }

    virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;
    virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& CullingRect,
        FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& WidgetStyle, bool bParentEnabled) const override;
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual void OnMouseLeave(const FPointerEvent& Event) override;
    virtual void OnMouseCaptureLost(const FCaptureLostEvent& Event) override;
    virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FReply OnKeyUp(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FReply OnKeyChar(const FGeometry& Geometry, const FCharacterEvent& Event) override;
    virtual void OnFocusLost(const FFocusEvent& Event) override;
    virtual FReply OnTouchStarted(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnTouchEnded(const FGeometry& Geometry, const FPointerEvent& Event) override;

private:
    bool EnsureNativeView();
    bool CheckResult(int Result);
    void UpdateMousePosition(const FGeometry& Geometry, const FPointerEvent& Event);
    FReply ForwardTouch(const FGeometry& Geometry, const FPointerEvent& Event, int32 Phase);
    void DispatchEvents();

    RmlUE_View* NativeView = nullptr;
    UTexture2D* Texture = nullptr;
    TSharedPtr<FDeferredCleanupSlateBrush> TextureBrush;
    FOnSlateRmlUiDocumentEvent OnDocumentEvent;
    FString DocumentPath;
    FString InlineDocument;
    FString SourcePath;
    FString LastError;
    FString DocumentError;
    FVector2D DesiredSize = FVector2D(1280.0, 720.0);
    FIntPoint TextureSize = FIntPoint::ZeroValue;
    float PixelScale = 1.0f;
    int32 MaxTextureDimension = 2048;
    uint64 FrameNumber = 0;
    TCHAR PendingHighSurrogate = 0;
    TMap<int32, FVector2D> ActiveTouches;
    TSet<int32> PressedMouseButtons;
    bool bNativeShutdown = false;
};
