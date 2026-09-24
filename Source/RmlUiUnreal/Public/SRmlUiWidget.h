#pragma once

#include "CoreMinimal.h"
#include "RmlUiPerformance.h"
#include "Widgets/SLeafWidget.h"

class FDeferredCleanupSlateBrush;
class UMaterialInterface;
class UTexture;
class UTexture2D;
struct RmlUE_View;
struct RmlUE_StyleSheet;
class ITextInputMethodContext;
class FRmlUiTextInputMethodContext;

DECLARE_DELEGATE_ThreeParams(FOnSlateRmlUiDocumentEvent, const FString&, const FString&, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRmlUiBeforeRender, float);
DECLARE_MULTICAST_DELEGATE(FOnRmlUiNativeShutdown);

class RMLUIUNREAL_API SRmlUiWidget : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SRmlUiWidget)
        : _DesiredSize(1280.0, 720.0), _MaxTextureDimension(2048), _UseSlateRenderer(false),
          _UsePaintCache(false), _BaseStyleSheet(nullptr) {}
        SLATE_ARGUMENT(FString, DocumentPath)
        SLATE_ARGUMENT(FString, InlineDocument)
        SLATE_ARGUMENT(FString, SourcePath)
        SLATE_ARGUMENT(FVector2D, DesiredSize)
        SLATE_ARGUMENT(int32, MaxTextureDimension)
        SLATE_ARGUMENT(bool, UseSlateRenderer)
        SLATE_ARGUMENT(bool, UsePaintCache)
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
    bool TrackMaterialTexture(FName Alias, FName Parameter, UTexture* Value);
    void SetBaseStyleSheet(RmlUE_StyleSheet* InStyleSheet);
    void ShutdownNative();
    bool RenderFrame(int32 Width, int32 Height, float DpRatio = 1.0f);
    void RequestScheduledRender();
    void SetExternalWakeDeadline(const void* Owner, double AbsoluteTimeSeconds);
    void ClearExternalWakeDeadline(const void* Owner);
    RmlUE_View* GetNativeView() const { return NativeView; }
    FString GetLastError() const { return DocumentError.IsEmpty() ? LastError : DocumentError; }
    uint64 GetFrameNumber() const { return FrameNumber; }
    uint64 GetResolvedMaterialDrawCount() const { return ResolvedMaterialDrawCount; }
    uint64 GetResolvedMaterialDrawCount(int32 MaterialSlot) const;
    uint64 GetSlateRhiDrawCount() const { return SlateRhiDrawCount; }
    uint64 GetSlateRhiMaskCount() const { return SlateRhiMaskCount; }
    uint64 GetSlateMaterialClipDrawCount() const { return SlateMaterialClipDrawCount; }
    uint64 GetSlateMaterialOpacitySectionDrawCount() const { return SlateMaterialOpacitySectionDrawCount; }
    uint64 GetSlateFallbackDrawCount() const { return SlateFallbackDrawCount; }
    uint32 GetUnsupportedSlateFeatures() const { return UnsupportedSlateFeatures; }
    int32 GetReadySlateRhiGeometryCount() const;
    bool IsUsingSlateRenderer() const { return bUseSlateRenderer; }
    bool HasSlateSchedule() const { return bHasSlateSchedule; }
    bool IsSlatePaintCacheEligible() const { return bSlatePaintCacheEligible; }
    bool IsSlatePaintCacheReady() const { return bSlatePaintCacheReady; }
    ERmlUiPaintCacheRejectReason GetSlatePaintCacheRejectReason() const { return SlatePaintCacheRejectReason; }
#if WITH_DEV_AUTOMATION_TESTS
    bool OverrideClipMaskOperationForTesting(uint32 OwnerNode, int32 Operation);
    bool GetClipMaskOperationsForTesting(uint32 VisualNode, TArray<int32>& OutOperations) const;
    bool HasScheduledActiveTimerForTesting() const { return ScheduledActiveTimer.IsValid(); }
#endif
    TSharedPtr<ITextInputMethodContext> GetTextInputMethodContext() const;
    void CancelTextComposition();
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
    virtual FReply OnFocusReceived(const FGeometry& Geometry, const FFocusEvent& Event) override;
    virtual void OnFocusLost(const FFocusEvent& Event) override;
    virtual FReply OnTouchStarted(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnTouchEnded(const FGeometry& Geometry, const FPointerEvent& Event) override;

private:
    struct FNativeMask {
        uint64 GeometryId = 0;
        uint32 OwnerNode = 0;
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
        uint32 VisualNode = 0;
        float VisualOpacity = 1.0f;
        int32 PaintRole = 0;
        bool bVisualColor = false;
        FLinearColor VisualColor = FLinearColor::Transparent;
    };
    struct FNativeMaskRef {
        int32 DrawIndex = INDEX_NONE;
        int32 MaskIndex = INDEX_NONE;
    };
    struct FMaterialOpacitySection {
        TArray<FVector2f> ConvexHull;
    };
    struct FGeometryResource {
        TArray<float> Vertices;
        TArray<uint32> Indices;
        uint64 RegistryId = 0;
        uint64 VertexBufferRegistryId = 0;
        uint64 IndexBufferRegistryId = 0;
        TSharedPtr<class FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> RhiGeometry;
        TArray<FVector2f> ConvexHull;
        TArray<FMaterialOpacitySection> MaterialOpacitySections;
        FVector2f MaterialBoundsMinimum = FVector2f::ZeroVector;
        FVector2f MaterialBoundsMaximum = FVector2f::ZeroVector;
        bool bFilledConvex = false;
        bool bMaterialOpacityAnalyzed = false;
        bool bMaterialOpacitySupported = false;
        bool bMaterialOpacityFullBox = false;
    };
    struct FTextureResource {
        TObjectPtr<UTexture2D> Texture = nullptr;
        TSharedPtr<FDeferredCleanupSlateBrush> Brush;
        uint64 RegistryId = 0;
        bool bHasColoredTranslucentPixels = false;
    };
    struct FMaterialResource {
        TSharedPtr<FDeferredCleanupSlateBrush> Brush;
        uint64 RegistryId = 0;
        TMap<FName, uint64> TextureParameterRegistryIds;
        bool bUsePremultipliedVertexColor = false;
        bool bSupportsInheritedOpacity = true;
    };
    struct FNativeMaterialResource {
        FName Alias;
        int32 Slot = -1;
    };
    bool AnalyzeMaterialOpacityGeometry(FGeometryResource& Geometry) const;
    bool CanApplyMaterialOpacityAsBoxes(const FNativeDraw& Draw, const FGeometryResource& Geometry,
        const FNativeMaterialResource& Binding, const FMaterialResource& Material) const;
    void CaptureSlateClipTopology(TArray<uint64>& OutTokens, uint64& OutMaskRefs) const;
    bool EnsureNativeView();
    bool CheckResult(int Result);
    bool RenderFrameInternal(int32 Width, int32 Height, float DpRatio,
        bool bBroadcastBeforeRender, uint64 PrecomputedBeforeRenderCycles = 0);
    uint64 BroadcastBeforeRender();
    void ResetSlateSchedule();
    void CaptureSlateSchedule(double NowSeconds);
    ERmlUiPaintCacheRejectReason EvaluateSlatePaintCache() const;
    bool AreSlatePaintResourcesReady() const;
    void MarkSlatePaintCachePending();
    void UpdateSlatePaintCacheReadiness();
    void ArmScheduledActiveTimer(double NowSeconds);
    EActiveTimerReturnType HandleScheduledActiveTimer(double CurrentTime, float DeltaTime);
    void UpdateMousePosition(const FGeometry& Geometry, const FPointerEvent& Event);
    FReply ForwardTouch(const FGeometry& Geometry, const FPointerEvent& Event, int32 Phase);
    void DispatchEvents();
    void ReleaseUnrealRenderResources(bool bIncludeMaterials);
    void ReparentMaterialResources(uint64 OwnerId);

    RmlUE_View* NativeView = nullptr;
    TSharedPtr<FRmlUiTextInputMethodContext> TextInputContext;
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
    uint64 LastScheduledContentRevision = 0;
    uint64 LastScheduledVisualRevision = 0;
    double NextScheduledUpdateTime = TNumericLimits<double>::Max();
    int32 LastScheduledWidth = 0;
    int32 LastScheduledHeight = 0;
    float LastScheduledDpRatio = 0.0f;
    bool bHasSlateSchedule = false;
    bool bScheduledRenderRequested = true;
    TMap<const void*, double> ExternalWakeDeadlines;
    TWeakPtr<FActiveTimerHandle> ScheduledActiveTimer;
    double ScheduledActiveTimerDeadline = TNumericLimits<double>::Max();
    bool bSlatePaintCacheEligible = true;
    bool bSlatePaintCacheReady = false;
    bool bSlatePaintResourcesWaiting = false;
    ERmlUiPaintCacheRejectReason SlatePaintCacheRejectReason = ERmlUiPaintCacheRejectReason::None;
    TCHAR PendingHighSurrogate = 0;
    TMap<int32, FVector2D> ActiveTouches;
    TSet<int32> PressedMouseButtons;
    TArray<FNativeDraw> NativeDraws;
    bool bHasSlateCommandSnapshot = false;
    TMap<uint32, TArray<int32>> NativeDrawIndicesByVisualNode;
    TMap<uint32, TArray<FNativeMaskRef>> NativeMaskIndicesByOwnerNode;
    mutable uint64 SlateRhiSubmissionPaintFrame = MAX_uint64;
    mutable int32 SlateRhiSubmissionIndex = 0;
    mutable TArray<TSharedPtr<class FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe>> SlateRhiSubmissions;
    TMap<uint64, FGeometryResource> NativeGeometries;
    TMap<uint64, FTextureResource> NativeTextures;
    TMap<uint64, FNativeMaterialResource> NativeMaterialResources;
    TMap<FName, FMaterialResource> Materials;
    bool bUseSlateRenderer = false;
    bool bUsePaintCache = false;
    uint32 UnsupportedSlateFeatures = 0;
    mutable uint64 ResolvedMaterialDrawCount = 0;
    mutable uint64 ResolvedMaterialDrawCounts[3] = {};
    mutable uint64 SlateRhiDrawCount = 0;
    mutable uint64 SlateRhiMaskCount = 0;
    mutable uint64 SlateMaterialClipDrawCount = 0;
    mutable uint64 SlateMaterialOpacitySectionDrawCount = 0;
    mutable uint64 SlateFallbackDrawCount = 0;
    bool bNativeShutdown = false;
};
