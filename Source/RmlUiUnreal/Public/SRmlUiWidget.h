#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class FDeferredCleanupSlateBrush;
class UMaterialInterface;
class UTexture2D;
struct RmlUE_View;
struct RmlUE_StyleSheet;

DECLARE_DELEGATE_ThreeParams(FOnSlateRmlUiDocumentEvent, const FString&, const FString&, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRmlUiBeforeRender, float);
DECLARE_MULTICAST_DELEGATE(FOnRmlUiNativeShutdown);

class RMLUIUNREAL_API SRmlUiWidget : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SRmlUiWidget)
        : _DesiredSize(1280.0, 720.0), _MaxTextureDimension(2048), _UseSlateRenderer(false), _BaseStyleSheet(nullptr) {}
        SLATE_ARGUMENT(FString, DocumentPath)
        SLATE_ARGUMENT(FString, InlineDocument)
        SLATE_ARGUMENT(FString, SourcePath)
        SLATE_ARGUMENT(FVector2D, DesiredSize)
        SLATE_ARGUMENT(int32, MaxTextureDimension)
        SLATE_ARGUMENT(bool, UseSlateRenderer)
        SLATE_ARGUMENT(RmlUE_StyleSheet*, BaseStyleSheet)
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
    void SetUseSlateRenderer(bool bInUseSlateRenderer);
    bool RegisterMaterial(FName Alias, UMaterialInterface* Material);
    void UnregisterMaterial(FName Alias);
    void SetBaseStyleSheet(RmlUE_StyleSheet* InStyleSheet);
    void ShutdownNative();
    bool RenderFrame(int32 Width, int32 Height, float DpRatio = 1.0f);
    RmlUE_View* GetNativeView() const { return NativeView; }
    FString GetLastError() const { return DocumentError.IsEmpty() ? LastError : DocumentError; }
    uint64 GetFrameNumber() const { return FrameNumber; }
    uint64 GetResolvedMaterialDrawCount() const { return ResolvedMaterialDrawCount; }
    uint64 GetResolvedMaterialDrawCount(int32 MaterialSlot) const;
    uint64 GetSlateRhiDrawCount() const { return SlateRhiDrawCount; }
    uint64 GetSlateRhiMaskCount() const { return SlateRhiMaskCount; }
    uint64 GetSlateMaterialClipDrawCount() const { return SlateMaterialClipDrawCount; }
    uint64 GetSlateFallbackDrawCount() const { return SlateFallbackDrawCount; }
    uint32 GetUnsupportedSlateFeatures() const { return UnsupportedSlateFeatures; }
    int32 GetReadySlateRhiGeometryCount() const;
    bool IsUsingSlateRenderer() const { return bUseSlateRenderer; }
    FOnRmlUiBeforeRender OnBeforeRender;
    FOnRmlUiNativeShutdown OnNativeShutdown;
    RmlUE_View* ExchangeNativeView(RmlUE_View* Replacement);

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
    struct FNativeMask {
        uint64 GeometryId = 0;
        int32 Operation = 0;
        FVector2f Translation = FVector2f::ZeroVector;
        FMatrix2x2 Transform;
        FVector2f TransformTranslation = FVector2f::ZeroVector;
        FSlateRect Scissor;
        bool bTransform = false;
        bool bScissor = false;
    };
    struct FNativeDraw {
        uint64 GeometryId = 0;
        uint64 TextureId = 0;
        FVector2f Translation = FVector2f::ZeroVector;
        FMatrix2x2 Transform;
        FVector2f TransformTranslation = FVector2f::ZeroVector;
        FSlateRect Scissor;
        bool bTransform = false;
        bool bScissor = false;
        TArray<FNativeMask> ClipMasks;
        bool bMaterialClipSupported = true;
    };
    struct FGeometryResource {
        TArray<float> Vertices;
        TArray<uint32> Indices;
        uint64 RegistryId = 0;
        uint64 VertexBufferRegistryId = 0;
        uint64 IndexBufferRegistryId = 0;
        TSharedPtr<class FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> RhiGeometry;
        TArray<FVector2f> ConvexHull;
        bool bFilledConvex = false;
    };
    struct FTextureResource {
        TObjectPtr<UTexture2D> Texture = nullptr;
        TSharedPtr<FDeferredCleanupSlateBrush> Brush;
        uint64 RegistryId = 0;
    };
    struct FMaterialResource {
        TSharedPtr<FDeferredCleanupSlateBrush> Brush;
        uint64 RegistryId = 0;
    };
    struct FNativeMaterialResource {
        FName Alias;
        int32 Slot = -1;
    };
    bool EnsureNativeView();
    bool CheckResult(int Result);
    void UpdateMousePosition(const FGeometry& Geometry, const FPointerEvent& Event);
    FReply ForwardTouch(const FGeometry& Geometry, const FPointerEvent& Event, int32 Phase);
    void DispatchEvents();
    void ReleaseUnrealRenderResources(bool bIncludeMaterials);

    RmlUE_View* NativeView = nullptr;
    RmlUE_StyleSheet* BaseStyleSheet = nullptr;
    UTexture2D* Texture = nullptr;
    TSharedPtr<FDeferredCleanupSlateBrush> TextureBrush;
    uint64 TextureRegistryId = 0;
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
    TArray<FNativeDraw> NativeDraws;
    mutable TArray<TSharedPtr<class FRmlUiSlateRhiDraw, ESPMode::ThreadSafe>> SlateRhiDrawElements;
    TMap<uint64, FGeometryResource> NativeGeometries;
    TMap<uint64, FTextureResource> NativeTextures;
    TMap<uint64, FNativeMaterialResource> NativeMaterialResources;
    TMap<FName, FMaterialResource> Materials;
    bool bUseSlateRenderer = false;
    uint32 UnsupportedSlateFeatures = 0;
    mutable uint64 ResolvedMaterialDrawCount = 0;
    mutable uint64 ResolvedMaterialDrawCounts[3] = {};
    mutable uint64 SlateRhiDrawCount = 0;
    mutable uint64 SlateRhiMaskCount = 0;
    mutable uint64 SlateMaterialClipDrawCount = 0;
    mutable uint64 SlateFallbackDrawCount = 0;
    bool bNativeShutdown = false;
};
