#pragma once

#include "CoreMinimal.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DECLARE_CATEGORY_MODULE_EXTERN(RMLUIUNREAL_API, RmlUi);

enum class ERmlUiPerformanceBackend : uint8
{
    DX11,
    Slate,
    Unattributed,
    Count
};

enum class ERmlUiPaintCacheEvent : uint8
{
    Evaluated,
    ResourceWait,
    Activated,
    Invalidated
};

enum class ERmlUiPaintCacheRejectReason : uint8
{
    None,
    Disabled,
    ComplexClipMask,
    LegacyColoredTranslucentTexture
};

enum class ERmlUiPerformanceStage : uint8
{
    Tick,
    RenderFrame,
    Resize,
    BeforeRender,
    BridgeRender,
    SlateFrameDecode,
    GeometryDeltas,
    TextureDeltas,
    DrawDecode,
    UploadPrepare,
    EventDispatch,
    OnPaint,
    PaintSlateRhi,
    PaintMaterial,
    PaintFallback,
    RenderThreadDraw,
    GeometryInit,
    GeometryRelease,
    JsAdvance,
    JsFrameCallbacks,
    JsNativeEvent,
    JsActivate,
    AnimationBind,
    AnimationAdvance,
    AnimationSchedule,
    AnimationScheduleHeapDrain,
    AnimationScheduleGroupRefresh,
    AnimationScheduleTagMutation,
    AnimationScheduleMaintenance,
    AnimationEvaluate,
    AnimationCollect,
    AnimationCommit,
    AnimationCommitPrepare,
    AnimationCommitValidate,
    AnimationCommitTransformPrepare,
    AnimationCommitApply,
    AnimationCommitSynchronize,
    AnimationCommitPublish,
    AnimationCommitFallback,
    AnimationCommitPostprocess,
    AnimationFinalize,
    AnimationFinalizePrepare,
    AnimationFinalizeRemove,
    AnimationFinalizeDetach,
    AnimationFinalizeEntityRelease,
    AnimationFinalizeEntityMark,
    AnimationFinalizeEntityFree,
    AnimationFinalizeCallbacks,
    AnimationCompletionEventSerialize,
    AnimationCompletionEventPack,
    AnimationCompletionEventDispatch,
    AnimationStartBatchParse,
    AnimationStartBatchPrepare,
    AnimationStartBatchPlay,
    AnimationPlanRegisterPacked,
    AnimationCompiledBatchPrepare,
    AnimationCompiledBatchPlay,
    Count
};

enum class ERmlUiPerformanceWork : uint8
{
    Frames,
    Draws,
    ClipMasks,
    DrawRecordsDecoded,
    DrawRecordsReused,
    VisualDeltaUpdates,
    SlateFullFrameRecords,
    SlateClipTopologyChanges,
    SlateClipTopologyDrawsDecoded,
    SlateClipTopologyMaskRefsBefore,
    SlateClipTopologyMaskRefsAfter,
    SlateClipTopologyDecodeCycles,
    ScheduledRenderSkips,
    ScheduledContentWakes,
    ScheduledVisualWakes,
    ScheduledDeadlineWakes,
    ScheduledExplicitWakes,
    ScheduledResizeWakes,
    ScheduledActiveTimerWakes,
    JsAdvanceSkips,
    JsTimerWakes,
    JsFrameWakes,
    JsDispatchWakes,
    GeometryCreates,
    GeometryDestroys,
    Vertices,
    Indices,
    TextureCreates,
    TextureDestroys,
    TextureUploadBytes,
    FullFrameUploadBytes,
    MaterialDraws,
    MaterialOpacitySections,
    FallbackDraws,
    RhiDraws,
    RhiMaskDraws,
    RhiTriangles,
    RhiSkippedDraws,
    RhiSubmissions,
    RhiGroupsCompiled,
    RhiRasterPasses,
    RhiClipBuilds,
    StencilTextures,
    StencilPixels,
    JsNodeCalls,
    JsonHostRequests,
    JsonHostBytes,
    AnimationTracksEvaluated,
    AnimationChangedProperties,
    AnimationCommittedProperties,
    AnimationCompletedInstances,
    AnimationLayeredGroupsRefreshed,
    AnimationLayeredRecordsScanned,
    AnimationLayeredBoundaryWakes,
    AnimationCompletionCallbacks,
    AnimationCompletionEventBatches,
    AnimationCompletionEvents,
    AnimationDefinitionsRegistered,
    AnimationDefinitionsReused,
    PaintCacheEvaluations,
    PaintCacheEligibleFrames,
    PaintCacheRejectedDisabled,
    PaintCacheRejectedClipMasks,
    PaintCacheRejectedLegacyTranslucentTexture,
    PaintCacheResourceWaits,
    PaintCacheActivations,
    PaintCacheInvalidations,
    Count
};

struct RMLUIUNREAL_API FRmlUiPerformanceSnapshot
{
    uint64 Cycles[static_cast<uint8>(ERmlUiPerformanceBackend::Count)]
        [static_cast<uint8>(ERmlUiPerformanceStage::Count)]{};
    uint64 Calls[static_cast<uint8>(ERmlUiPerformanceBackend::Count)]
        [static_cast<uint8>(ERmlUiPerformanceStage::Count)]{};
    uint64 Work[static_cast<uint8>(ERmlUiPerformanceBackend::Count)]
        [static_cast<uint8>(ERmlUiPerformanceWork::Count)]{};

    double Milliseconds(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage) const;
    uint64 CallCount(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage) const;
    uint64 WorkCount(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceWork Counter) const;
};

struct FRmlUiPerformanceFrame
{
    uint64 ViewId = 0;
    uint64 FrameId = 0;
    ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Unattributed;
    int32 Width = 0;
    int32 Height = 0;
    uint64 BridgeCycles = 0;
    uint64 BeforeRenderCycles = 0;
    uint64 DecodeCycles = 0;
    uint64 UploadBytes = 0;
    uint64 Draws = 0;
    uint64 ClipMasks = 0;
    uint32 UnsupportedFeatures = 0;
    uint64 ClipTopologyMaskRefsBefore = 0;
    uint64 ClipTopologyMaskRefsAfter = 0;
    bool bSlateReplayed = false;
    bool bClipTopologyChanged = false;
};

class RMLUIUNREAL_API FRmlUiPerformance
{
public:
    static bool IsEnabled();
    static void SetEnabled(bool bEnabled);
    static void Reset();
    static FRmlUiPerformanceSnapshot Snapshot();
    static void AddCycles(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage, uint64 Cycles);
    static void AddWork(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceWork Counter, uint64 Amount = 1);
    static void RecordFrame(const FRmlUiPerformanceFrame& Frame);
    static void RecordRenderThread(uint64 ViewId, uint64 FrameId, uint64 GeometryId, uint32 Draws,
        uint32 Masks, uint32 Triangles, uint32 StencilPixels, bool bSkipped);
    static void RecordPaintCache(uint64 ViewId, uint64 FrameId, ERmlUiPaintCacheEvent Event,
        ERmlUiPaintCacheRejectReason RejectReason, uint32 Draws);
};

class RMLUIUNREAL_API FScopedRmlUiPerformanceTimer
{
public:
    FScopedRmlUiPerformanceTimer(ERmlUiPerformanceBackend InBackend, ERmlUiPerformanceStage InStage);
    ~FScopedRmlUiPerformanceTimer();

private:
    ERmlUiPerformanceBackend Backend;
    ERmlUiPerformanceStage Stage;
    uint64 StartCycles = 0;
};
