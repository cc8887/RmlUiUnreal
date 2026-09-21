#include "RmlUiPerformance.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Trace/Trace.inl"

#include <atomic>

CSV_DEFINE_CATEGORY_MODULE(RMLUIUNREAL_API, RmlUi, false);

UE_TRACE_CHANNEL_DEFINE(RmlUiPerfChannel, "RmlUi frame and render-stage performance events");

UE_TRACE_EVENT_BEGIN(RmlUiPerf, Frame)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, ViewId)
    UE_TRACE_EVENT_FIELD(uint64, FrameId)
    UE_TRACE_EVENT_FIELD(uint64, BridgeCycles)
    UE_TRACE_EVENT_FIELD(uint64, BeforeRenderCycles)
    UE_TRACE_EVENT_FIELD(uint64, DecodeCycles)
    UE_TRACE_EVENT_FIELD(uint64, UploadBytes)
    UE_TRACE_EVENT_FIELD(uint64, Draws)
    UE_TRACE_EVENT_FIELD(uint64, ClipMasks)
    UE_TRACE_EVENT_FIELD(uint64, ClipTopologyMaskRefsBefore)
    UE_TRACE_EVENT_FIELD(uint64, ClipTopologyMaskRefsAfter)
    UE_TRACE_EVENT_FIELD(uint32, UnsupportedFeatures)
    UE_TRACE_EVENT_FIELD(uint16, Width)
    UE_TRACE_EVENT_FIELD(uint16, Height)
    UE_TRACE_EVENT_FIELD(uint8, Backend)
    UE_TRACE_EVENT_FIELD(uint8, SlateReplayed)
    UE_TRACE_EVENT_FIELD(uint8, ClipTopologyChanged)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(RmlUiPerf, RenderThread)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, ViewId)
    UE_TRACE_EVENT_FIELD(uint64, FrameId)
    UE_TRACE_EVENT_FIELD(uint64, GeometryId)
    UE_TRACE_EVENT_FIELD(uint32, Draws)
    UE_TRACE_EVENT_FIELD(uint32, Masks)
    UE_TRACE_EVENT_FIELD(uint32, Triangles)
    UE_TRACE_EVENT_FIELD(uint32, StencilPixels)
    UE_TRACE_EVENT_FIELD(uint8, Skipped)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(RmlUiPerf, PaintCache)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, ViewId)
    UE_TRACE_EVENT_FIELD(uint64, FrameId)
    UE_TRACE_EVENT_FIELD(uint32, Draws)
    UE_TRACE_EVENT_FIELD(uint8, Event)
    UE_TRACE_EVENT_FIELD(uint8, RejectReason)
UE_TRACE_EVENT_END()

TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiFrames, TEXT("RmlUi/Frames"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiDraws, TEXT("RmlUi/Draws"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiClipMasks, TEXT("RmlUi/Clip Masks"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiUploadBytes, TEXT("RmlUi/Upload Bytes"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiRhiDraws, TEXT("RmlUi/RHI Draws"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiRhiMaskDraws, TEXT("RmlUi/RHI Mask Draws"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiRhiTriangles, TEXT("RmlUi/RHI Triangles"));
TRACE_DECLARE_ATOMIC_INT_COUNTER(RmlUiStencilPixels, TEXT("RmlUi/Stencil Pixels"));

namespace
{
constexpr uint8 BackendCount = static_cast<uint8>(ERmlUiPerformanceBackend::Count);
constexpr uint8 StageCount = static_cast<uint8>(ERmlUiPerformanceStage::Count);
constexpr uint8 WorkCount = static_cast<uint8>(ERmlUiPerformanceWork::Count);

std::atomic<uint64> GCycles[BackendCount][StageCount]{};
std::atomic<uint64> GCalls[BackendCount][StageCount]{};
std::atomic<uint64> GWork[BackendCount][WorkCount]{};

TAutoConsoleVariable<int32> CVarRmlUiPerformance(
    TEXT("r.RmlUi.Perf"), 0,
    TEXT("Collect RmlUi UE-side performance totals and RmlUiPerf trace events."),
    ECVF_Default);

uint8 BackendIndex(ERmlUiPerformanceBackend Backend)
{
    const uint8 Index = static_cast<uint8>(Backend);
    return Index < BackendCount ? Index : static_cast<uint8>(ERmlUiPerformanceBackend::Unattributed);
}
}

double FRmlUiPerformanceSnapshot::Milliseconds(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage) const
{
    return FPlatformTime::ToMilliseconds64(Cycles[BackendIndex(Backend)][static_cast<uint8>(Stage)]);
}

uint64 FRmlUiPerformanceSnapshot::CallCount(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage) const
{
    return Calls[BackendIndex(Backend)][static_cast<uint8>(Stage)];
}

uint64 FRmlUiPerformanceSnapshot::WorkCount(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceWork Counter) const
{
    return Work[BackendIndex(Backend)][static_cast<uint8>(Counter)];
}

bool FRmlUiPerformance::IsEnabled()
{
    return CVarRmlUiPerformance.GetValueOnAnyThread() != 0;
}

void FRmlUiPerformance::SetEnabled(bool bEnabled)
{
    CVarRmlUiPerformance->Set(bEnabled ? 1 : 0, ECVF_SetByCode);
}

void FRmlUiPerformance::Reset()
{
    for (uint8 Backend = 0; Backend < BackendCount; ++Backend)
    {
        for (uint8 Stage = 0; Stage < StageCount; ++Stage)
        {
            GCycles[Backend][Stage].store(0, std::memory_order_relaxed);
            GCalls[Backend][Stage].store(0, std::memory_order_relaxed);
        }
        for (uint8 Counter = 0; Counter < WorkCount; ++Counter)
            GWork[Backend][Counter].store(0, std::memory_order_relaxed);
    }
}

FRmlUiPerformanceSnapshot FRmlUiPerformance::Snapshot()
{
    FRmlUiPerformanceSnapshot Result;
    for (uint8 Backend = 0; Backend < BackendCount; ++Backend)
    {
        for (uint8 Stage = 0; Stage < StageCount; ++Stage)
        {
            Result.Cycles[Backend][Stage] = GCycles[Backend][Stage].load(std::memory_order_relaxed);
            Result.Calls[Backend][Stage] = GCalls[Backend][Stage].load(std::memory_order_relaxed);
        }
        for (uint8 Counter = 0; Counter < WorkCount; ++Counter)
            Result.Work[Backend][Counter] = GWork[Backend][Counter].load(std::memory_order_relaxed);
    }
    return Result;
}

void FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage, uint64 Cycles)
{
    if (!IsEnabled()) return;
    const uint8 BackendValue = BackendIndex(Backend);
    const uint8 StageValue = static_cast<uint8>(Stage);
    if (StageValue >= StageCount) return;
    GCycles[BackendValue][StageValue].fetch_add(Cycles, std::memory_order_relaxed);
    GCalls[BackendValue][StageValue].fetch_add(1, std::memory_order_relaxed);
}

void FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend Backend, ERmlUiPerformanceWork Counter, uint64 Amount)
{
    if (!IsEnabled()) return;
    const uint8 CounterValue = static_cast<uint8>(Counter);
    if (CounterValue >= WorkCount) return;
    GWork[BackendIndex(Backend)][CounterValue].fetch_add(Amount, std::memory_order_relaxed);
}

void FRmlUiPerformance::RecordFrame(const FRmlUiPerformanceFrame& Metrics)
{
    if (!IsEnabled()) return;
    AddWork(Metrics.Backend, ERmlUiPerformanceWork::Frames);
    if (Metrics.Backend == ERmlUiPerformanceBackend::Slate && !Metrics.bSlateReplayed)
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateFullFrameRecords);
    if (Metrics.bClipTopologyChanged)
    {
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateClipTopologyChanges);
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateClipTopologyDrawsDecoded, Metrics.Draws);
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateClipTopologyMaskRefsBefore,
            Metrics.ClipTopologyMaskRefsBefore);
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateClipTopologyMaskRefsAfter,
            Metrics.ClipTopologyMaskRefsAfter);
        AddWork(Metrics.Backend, ERmlUiPerformanceWork::SlateClipTopologyDecodeCycles, Metrics.DecodeCycles);
    }
    TRACE_COUNTER_SET(RmlUiFrames, static_cast<int64>(GWork[BackendIndex(Metrics.Backend)]
        [static_cast<uint8>(ERmlUiPerformanceWork::Frames)].load(std::memory_order_relaxed)));
    TRACE_COUNTER_SET(RmlUiDraws, static_cast<int64>(Metrics.Draws));
    TRACE_COUNTER_SET(RmlUiClipMasks, static_cast<int64>(Metrics.ClipMasks));
    TRACE_COUNTER_SET(RmlUiUploadBytes, static_cast<int64>(Metrics.UploadBytes));
    CSV_CUSTOM_STAT(RmlUi, Draws, static_cast<int32>(FMath::Min<uint64>(Metrics.Draws, MAX_int32)), ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(RmlUi, ClipMasks, static_cast<int32>(FMath::Min<uint64>(Metrics.ClipMasks, MAX_int32)), ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(RmlUi, UploadKiB, static_cast<float>(Metrics.UploadBytes) / 1024.0f, ECsvCustomStatOp::Set);
    UE_TRACE_LOG(RmlUiPerf, Frame, RmlUiPerfChannel)
        << Frame.Timestamp(FPlatformTime::Cycles64())
        << Frame.ViewId(Metrics.ViewId)
        << Frame.FrameId(Metrics.FrameId)
        << Frame.BridgeCycles(Metrics.BridgeCycles)
        << Frame.BeforeRenderCycles(Metrics.BeforeRenderCycles)
        << Frame.DecodeCycles(Metrics.DecodeCycles)
        << Frame.UploadBytes(Metrics.UploadBytes)
        << Frame.Draws(Metrics.Draws)
        << Frame.ClipMasks(Metrics.ClipMasks)
        << Frame.ClipTopologyMaskRefsBefore(Metrics.ClipTopologyMaskRefsBefore)
        << Frame.ClipTopologyMaskRefsAfter(Metrics.ClipTopologyMaskRefsAfter)
        << Frame.UnsupportedFeatures(Metrics.UnsupportedFeatures)
        << Frame.Width(static_cast<uint16>(FMath::Clamp(Metrics.Width, 0, 65535)))
        << Frame.Height(static_cast<uint16>(FMath::Clamp(Metrics.Height, 0, 65535)))
        << Frame.Backend(static_cast<uint8>(Metrics.Backend))
        << Frame.SlateReplayed(Metrics.bSlateReplayed ? 1 : 0)
        << Frame.ClipTopologyChanged(Metrics.bClipTopologyChanged ? 1 : 0);
}

void FRmlUiPerformance::RecordRenderThread(uint64 ViewId, uint64 FrameId, uint64 GeometryId, uint32 Draws,
    uint32 Masks, uint32 Triangles, uint32 StencilPixels, bool bSkipped)
{
    if (!IsEnabled()) return;
    const ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Slate;
    AddWork(Backend, ERmlUiPerformanceWork::RhiDraws, Draws);
    AddWork(Backend, ERmlUiPerformanceWork::RhiMaskDraws, Masks);
    AddWork(Backend, ERmlUiPerformanceWork::RhiTriangles, Triangles);
    AddWork(Backend, ERmlUiPerformanceWork::StencilPixels, StencilPixels);
    if (bSkipped) AddWork(Backend, ERmlUiPerformanceWork::RhiSkippedDraws);
    TRACE_COUNTER_SET(RmlUiRhiDraws, static_cast<int64>(Draws));
    TRACE_COUNTER_SET(RmlUiRhiMaskDraws, static_cast<int64>(Masks));
    TRACE_COUNTER_SET(RmlUiRhiTriangles, static_cast<int64>(Triangles));
    TRACE_COUNTER_SET(RmlUiStencilPixels, static_cast<int64>(StencilPixels));
    UE_TRACE_LOG(RmlUiPerf, RenderThread, RmlUiPerfChannel)
        << RenderThread.Timestamp(FPlatformTime::Cycles64())
        << RenderThread.ViewId(ViewId)
        << RenderThread.FrameId(FrameId)
        << RenderThread.GeometryId(GeometryId)
        << RenderThread.Draws(Draws)
        << RenderThread.Masks(Masks)
        << RenderThread.Triangles(Triangles)
        << RenderThread.StencilPixels(StencilPixels)
        << RenderThread.Skipped(bSkipped ? 1 : 0);
}

void FRmlUiPerformance::RecordPaintCache(uint64 ViewId, uint64 FrameId, ERmlUiPaintCacheEvent Event,
    ERmlUiPaintCacheRejectReason RejectReason, uint32 Draws)
{
    if (!IsEnabled()) return;
    const ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Slate;
    switch (Event)
    {
    case ERmlUiPaintCacheEvent::Evaluated:
        AddWork(Backend, ERmlUiPerformanceWork::PaintCacheEvaluations);
        switch (RejectReason)
        {
        case ERmlUiPaintCacheRejectReason::None:
            AddWork(Backend, ERmlUiPerformanceWork::PaintCacheEligibleFrames);
            break;
        case ERmlUiPaintCacheRejectReason::Disabled:
            AddWork(Backend, ERmlUiPerformanceWork::PaintCacheRejectedDisabled);
            break;
        case ERmlUiPaintCacheRejectReason::ComplexClipMask:
            AddWork(Backend, ERmlUiPerformanceWork::PaintCacheRejectedClipMasks);
            break;
        case ERmlUiPaintCacheRejectReason::LegacyColoredTranslucentTexture:
            AddWork(Backend, ERmlUiPerformanceWork::PaintCacheRejectedLegacyTranslucentTexture);
            break;
        }
        break;
    case ERmlUiPaintCacheEvent::ResourceWait:
        AddWork(Backend, ERmlUiPerformanceWork::PaintCacheResourceWaits);
        break;
    case ERmlUiPaintCacheEvent::Activated:
        AddWork(Backend, ERmlUiPerformanceWork::PaintCacheActivations);
        break;
    case ERmlUiPaintCacheEvent::Invalidated:
        AddWork(Backend, ERmlUiPerformanceWork::PaintCacheInvalidations);
        break;
    }
    UE_TRACE_LOG(RmlUiPerf, PaintCache, RmlUiPerfChannel)
        << PaintCache.Timestamp(FPlatformTime::Cycles64())
        << PaintCache.ViewId(ViewId)
        << PaintCache.FrameId(FrameId)
        << PaintCache.Draws(Draws)
        << PaintCache.Event(static_cast<uint8>(Event))
        << PaintCache.RejectReason(static_cast<uint8>(RejectReason));
}

FScopedRmlUiPerformanceTimer::FScopedRmlUiPerformanceTimer(
    ERmlUiPerformanceBackend InBackend, ERmlUiPerformanceStage InStage)
    : Backend(InBackend), Stage(InStage), StartCycles(FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0)
{
}

FScopedRmlUiPerformanceTimer::~FScopedRmlUiPerformanceTimer()
{
    if (StartCycles)
        FRmlUiPerformance::AddCycles(Backend, Stage, FPlatformTime::Cycles64() - StartCycles);
}
