#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "RmlUiPerformance.h"
#include "SRmlUiWidget.h"
#include "DynamicRHI.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SInvalidationPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"

namespace RmlUiPaintCacheRouteTests
{
constexpr int32 ViewWidth = 800;
constexpr int32 ViewHeight = 480;
constexpr double DefaultSampleSeconds = 1.0;
constexpr double StableWindowSeconds = 0.25;
constexpr double TimeoutSeconds = 30.0;

FString OutputDirectory()
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(), TEXT("RmlUiPerfOutput="), Directory) || Directory.IsEmpty())
        Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance"));
    return FPaths::ConvertRelativePathToFull(Directory);
}

FString RunLabel()
{
    return FString::Printf(TEXT("%s-%s"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"),
        FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen")) ? TEXT("Headless") : TEXT("Windowed"));
}

FString RejectReasonName(ERmlUiPaintCacheRejectReason Reason)
{
    switch (Reason)
    {
    case ERmlUiPaintCacheRejectReason::None: return TEXT("none");
    case ERmlUiPaintCacheRejectReason::Disabled: return TEXT("disabled");
    case ERmlUiPaintCacheRejectReason::ComplexClipMask: return TEXT("complex_clip_mask");
    case ERmlUiPaintCacheRejectReason::LegacyColoredTranslucentTexture:
        return TEXT("legacy_colored_translucent_texture");
    default: return TEXT("unknown");
    }
}

TSharedPtr<FJsonObject> StageJson(const FRmlUiPerformanceSnapshot& Snapshot, ERmlUiPerformanceStage Stage)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    const uint64 Calls = Snapshot.CallCount(ERmlUiPerformanceBackend::Slate, Stage);
    Result->SetNumberField(TEXT("calls"), static_cast<double>(Calls));
    Result->SetNumberField(TEXT("total_ms"), Snapshot.Milliseconds(ERmlUiPerformanceBackend::Slate, Stage));
    Result->SetNumberField(TEXT("mean_ms"), Calls ?
        Snapshot.Milliseconds(ERmlUiPerformanceBackend::Slate, Stage) / Calls : 0.0);
    return Result;
}

TSharedPtr<FJsonObject> SnapshotJson(const FRmlUiPerformanceSnapshot& Snapshot)
{
    const ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Slate;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Stages = MakeShared<FJsonObject>();
    Stages->SetObjectField(TEXT("tick"), StageJson(Snapshot, ERmlUiPerformanceStage::Tick));
    Stages->SetObjectField(TEXT("render_frame"), StageJson(Snapshot, ERmlUiPerformanceStage::RenderFrame));
    Stages->SetObjectField(TEXT("bridge_render"), StageJson(Snapshot, ERmlUiPerformanceStage::BridgeRender));
    Stages->SetObjectField(TEXT("draw_decode"), StageJson(Snapshot, ERmlUiPerformanceStage::DrawDecode));
    Stages->SetObjectField(TEXT("on_paint"), StageJson(Snapshot, ERmlUiPerformanceStage::OnPaint));
    Stages->SetObjectField(TEXT("render_thread_draw"),
        StageJson(Snapshot, ERmlUiPerformanceStage::RenderThreadDraw));
    Result->SetObjectField(TEXT("stages"), Stages);
    Result->SetNumberField(TEXT("frames"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Frames));
    Result->SetNumberField(TEXT("draws"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Draws));
    Result->SetNumberField(TEXT("rhi_draws"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiDraws));
    Result->SetNumberField(TEXT("rhi_mask_draws"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiMaskDraws));
    Result->SetNumberField(TEXT("rhi_triangles"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiTriangles));
    Result->SetNumberField(TEXT("rhi_submissions"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiSubmissions));
    Result->SetNumberField(TEXT("rhi_groups_compiled"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiGroupsCompiled));
    Result->SetNumberField(TEXT("rhi_raster_passes"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiRasterPasses));
    Result->SetNumberField(TEXT("rhi_clip_builds"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::RhiClipBuilds));
    Result->SetNumberField(TEXT("stencil_textures"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::StencilTextures));
    Result->SetNumberField(TEXT("stencil_pixels"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::StencilPixels));
    TSharedPtr<FJsonObject> ClipTopology = MakeShared<FJsonObject>();
    ClipTopology->SetNumberField(TEXT("full_frame_records"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateFullFrameRecords));
    ClipTopology->SetNumberField(TEXT("changes"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyChanges));
    ClipTopology->SetNumberField(TEXT("draws_decoded"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyDrawsDecoded));
    ClipTopology->SetNumberField(TEXT("mask_refs_before"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyMaskRefsBefore));
    ClipTopology->SetNumberField(TEXT("mask_refs_after"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyMaskRefsAfter));
    ClipTopology->SetNumberField(TEXT("decode_ms"), FPlatformTime::ToMilliseconds64(
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyDecodeCycles)));
    Result->SetObjectField(TEXT("clip_topology"), ClipTopology);
    Result->SetNumberField(TEXT("scheduled_render_skips"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::ScheduledRenderSkips));
    TSharedPtr<FJsonObject> Cache = MakeShared<FJsonObject>();
    Cache->SetNumberField(TEXT("evaluations"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheEvaluations));
    Cache->SetNumberField(TEXT("eligible_frames"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheEligibleFrames));
    Cache->SetNumberField(TEXT("rejected_disabled"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedDisabled));
    Cache->SetNumberField(TEXT("rejected_clip_masks"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedClipMasks));
    Cache->SetNumberField(TEXT("rejected_legacy_translucent_texture"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedLegacyTranslucentTexture));
    Cache->SetNumberField(TEXT("resource_waits"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheResourceWaits));
    Cache->SetNumberField(TEXT("activations"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheActivations));
    Cache->SetNumberField(TEXT("invalidations"),
        Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheInvalidations));
    Result->SetObjectField(TEXT("paint_cache"), Cache);
    return Result;
}

class FRouteMatrixCommand final : public IAutomationLatentCommand
{
public:
    explicit FRouteMatrixCommand(FAutomationTestBase* InTest)
        : Test(InTest), bPreviouslyEnabled(FRmlUiPerformance::IsEnabled())
    {
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPaintCacheSampleSeconds="), SampleSeconds);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPresentWindowMarker="), PresentWindowMarkerPath);
        if (!PresentWindowMarkerPath.IsEmpty())
            PresentWindowMarkerPath = FPaths::ConvertRelativePathToFull(PresentWindowMarkerPath);
        SampleSeconds = FMath::Max(0.25, SampleSeconds);
        FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(),
            TEXT("RmlUi/Tests/web-motion-libraries.html")));
    }

    virtual ~FRouteMatrixCommand() override
    {
        EndGpuProfileRegion();
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        DestroyWindow();
    }

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (!Window.IsValid())
        {
            StartScenario();
            return false;
        }
        if (bGpuProfilePending)
        {
            if (Now - GpuProfileStarted < 5.0) return false;
            bGpuProfilePending = false;
            EndGpuProfileRegion();
            DestroyWindow();
            ++Scenario;
            SaveReport();
            FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
            return true;
        }
        if (!bSampling)
        {
            bool bAllSettled = true;
            for (int32 Index = 0; Index < Widgets.Num(); ++Index)
            {
                const TSharedPtr<SRmlUiWidget>& Widget = Widgets[Index];
                const uint64 Frame = Widget->GetFrameNumber();
                const bool bRouteResolved = Frame > 0 && (Widget->IsSlatePaintCacheReady() ||
                    Widget->GetSlatePaintCacheRejectReason() != ERmlUiPaintCacheRejectReason::None);
                bAllSettled &= bRouteResolved && !Widget->GetCanTick() && Frame == LastFrames[Index];
                LastFrames[Index] = Frame;
            }
            if (bAllSettled)
            {
                if (StableSince <= 0.0) StableSince = Now;
                if (Now - StableSince >= StableWindowSeconds) BeginSample();
            }
            else
            {
                StableSince = 0.0;
            }
            if (!bSampling && Now - ScenarioStarted >= TimeoutSeconds)
            {
                Test->AddError(FString::Printf(TEXT("Paint-cache route matrix with %d views did not settle"),
                    ViewCounts[Scenario]));
                return CompleteScenario();
            }
            return false;
        }
        if (Now - SampleStarted < SampleSeconds) return false;
        return CompleteScenario();
    }

private:
    void StartScenario()
    {
        Test->TestTrue(TEXT("Web motion fixture exists"), IFileManager::Get().FileExists(*FixturePath));
        const int32 ViewCount = ViewCounts[Scenario];
        Widgets.Reset();
        CacheRoots.Reset();
        LastFrames.Init(0, ViewCount);
        StableSince = 0.0;
        bSampling = false;
        FRmlUiPerformance::SetEnabled(true);
        FRmlUiPerformance::Reset();

        TSharedRef<SOverlay> Overlay = SNew(SOverlay);
        for (int32 Index = 0; Index < ViewCount; ++Index)
        {
            TSharedPtr<SRmlUiWidget> Widget;
            TSharedPtr<SInvalidationPanel> CacheRoot;
            SAssignNew(Widget, SRmlUiWidget)
                .DocumentPath(FixturePath)
                .UseSlateRenderer(true)
                .UsePaintCache(true)
                .DesiredSize(FVector2D(ViewWidth, ViewHeight))
                .Clipping(EWidgetClipping::ClipToBounds);
            SAssignNew(CacheRoot, SInvalidationPanel)
                .DebugName(FString::Printf(TEXT("RmlUi paint cache route view %d"), Index))
                [Widget.ToSharedRef()];
            CacheRoot->SetCanCache(true);
            Widgets.Add(Widget);
            CacheRoots.Add(CacheRoot);
            Overlay->AddSlot()[CacheRoot.ToSharedRef()];
        }
        TSharedRef<SWidget> Content = SNew(SBox)
            .WidthOverride(ViewWidth).HeightOverride(ViewHeight)[Overlay];
        if (!PresentWindowMarkerPath.IsEmpty())
            Content->SetVisibility(EVisibility::HitTestInvisible);
        const FVector2D WindowPosition = PresentWindowMarkerPath.IsEmpty() ?
            FVector2D(-10000, -10000) : FVector2D(32, 32);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi paint cache route matrix")))
            .ClientSize(FVector2D(ViewWidth, ViewHeight)).UseOSWindowBorder(false).CreateTitleBar(false)
            .AutoCenter(EAutoCenter::None).ScreenPosition(WindowPosition)
            .IsTopmostWindow(!PresentWindowMarkerPath.IsEmpty())
            .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
            .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[Content];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        ScenarioStarted = FPlatformTime::Seconds();
    }

    void BeginSample()
    {
        FlushRenderingCommands();
        WarmupSnapshot = FRmlUiPerformance::Snapshot();
        FRmlUiPerformance::Reset();
        SampleStarted = FPlatformTime::Seconds();
        bSampling = true;
    }

    bool CompleteScenario()
    {
        FlushRenderingCommands();
        const FRmlUiPerformanceSnapshot IdleSnapshot = FRmlUiPerformance::Snapshot();
        const int32 ViewCount = ViewCounts[Scenario];
        int32 Eligible = 0;
        int32 Ready = 0;
        TMap<ERmlUiPaintCacheRejectReason, int32> RejectCounts;
        TArray<TSharedPtr<FJsonValue>> Views;
        for (int32 Index = 0; Index < Widgets.Num(); ++Index)
        {
            const TSharedPtr<SRmlUiWidget>& Widget = Widgets[Index];
            Eligible += Widget->IsSlatePaintCacheEligible() ? 1 : 0;
            Ready += Widget->IsSlatePaintCacheReady() ? 1 : 0;
            const ERmlUiPaintCacheRejectReason Reason = Widget->GetSlatePaintCacheRejectReason();
            RejectCounts.FindOrAdd(Reason)++;
            TSharedPtr<FJsonObject> View = MakeShared<FJsonObject>();
            View->SetNumberField(TEXT("index"), Index);
            View->SetNumberField(TEXT("frame"), static_cast<double>(Widget->GetFrameNumber()));
            View->SetBoolField(TEXT("eligible"), Widget->IsSlatePaintCacheEligible());
            View->SetBoolField(TEXT("ready"), Widget->IsSlatePaintCacheReady());
            View->SetBoolField(TEXT("can_tick"), Widget->GetCanTick());
            View->SetStringField(TEXT("reject_reason"), RejectReasonName(Reason));
            View->SetNumberField(TEXT("unsupported_slate_features"), Widget->GetUnsupportedSlateFeatures());
            View->SetStringField(TEXT("error"), Widget->GetLastError());
            Views.Add(MakeShared<FJsonValueObject>(View));
            Test->TestTrue(FString::Printf(TEXT("View %d/%d loaded the real motion fixture"), Index, ViewCount),
                Widget->GetLastError().IsEmpty() && Widget->GetFrameNumber() > 0);
            Test->TestFalse(FString::Printf(TEXT("View %d/%d stopped ticking after motion settled"), Index, ViewCount),
                Widget->GetCanTick());
            Test->TestEqual(FString::Printf(TEXT("View %d/%d eligibility matches its route"), Index, ViewCount),
                Widget->IsSlatePaintCacheEligible(), Reason == ERmlUiPaintCacheRejectReason::None);
            if (Widget->IsSlatePaintCacheEligible())
                Test->TestTrue(FString::Printf(TEXT("View %d/%d activated its eligible cache"), Index, ViewCount),
                    Widget->IsSlatePaintCacheReady());
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("view_count"), ViewCount);
        Result->SetNumberField(TEXT("eligible_views"), Eligible);
        Result->SetNumberField(TEXT("ready_views"), Ready);
        Result->SetNumberField(TEXT("sample_seconds"), FPlatformTime::Seconds() - SampleStarted);
        Result->SetArrayField(TEXT("views"), Views);
        TSharedPtr<FJsonObject> Routes = MakeShared<FJsonObject>();
        for (const TPair<ERmlUiPaintCacheRejectReason, int32>& Pair : RejectCounts)
            Routes->SetNumberField(RejectReasonName(Pair.Key), Pair.Value);
        Result->SetObjectField(TEXT("route_distribution"), Routes);
        Result->SetObjectField(TEXT("load_and_motion"), SnapshotJson(WarmupSnapshot));
        Result->SetObjectField(TEXT("stable_idle"), SnapshotJson(IdleSnapshot));

        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size);
        int32 ContentPixels = 0;
        for (const FColor Pixel : Pixels)
            if (Pixel.R > 25 || Pixel.G > 35 || Pixel.B > 35) ++ContentPixels;
        const bool bPixelsVerified = bCaptured && Size.X > 0 && Size.Y > 0 && ContentPixels > 10000;
        Result->SetBoolField(TEXT("content_pixels_verified"), bPixelsVerified);
        Result->SetNumberField(TEXT("content_pixels"), ContentPixels);
        Test->TestTrue(FString::Printf(TEXT("%d-view fixture rendered visible pixels"), ViewCount), bPixelsVerified);
        if (bCaptured)
        {
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
                TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            const FString Screenshot = FPaths::Combine(OutputDirectory(),
                FString::Printf(TEXT("RmlUiPaintCacheRoute-%dViews-%s.png"), ViewCount, *RunLabel()));
            IFileManager::Get().MakeDirectory(*OutputDirectory(), true);
            Test->TestTrue(TEXT("Save route matrix screenshot"), FFileHelper::SaveArrayToFile(Png, *Screenshot));
            Result->SetStringField(TEXT("screenshot"), Screenshot);
        }

        Test->TestTrue(FString::Printf(TEXT("%d-view load phase evaluated every cache route"), ViewCount),
            WarmupSnapshot.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::PaintCacheEvaluations) >= static_cast<uint64>(ViewCount));
        Results.Add(MakeShared<FJsonValueObject>(Result));

        if (Scenario + 1 == UE_ARRAY_COUNT(ViewCounts) &&
            FParse::Param(FCommandLine::Get(), TEXT("RmlUiCaptureGpuProfile")))
        {
            bGpuProfilePending = true;
            GpuProfileStartQpc = FPlatformTime::Cycles64();
            WriteGpuProfileWindowMarker(false, 0);
            TRACE_BEGIN_REGION(TEXT("RmlUiGpuProfileWindow"), TEXT("RmlUi"));
            bGpuProfileRegionOpen = true;
            GpuProfileStarted = FPlatformTime::Seconds();
            IConsoleManager::Get().ProcessUserConsoleInput(TEXT("ProfileGPU"), *GLog, nullptr);
            Window->Invalidate(EInvalidateWidgetReason::Paint);
            return false;
        }

        DestroyWindow();
        ++Scenario;
        if (Scenario < UE_ARRAY_COUNT(ViewCounts)) return false;
        SaveReport();
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        return true;
    }

    void SaveReport()
    {
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema_version"), 1);
        Root->SetStringField(TEXT("fixture"), FixturePath);
        Root->SetStringField(TEXT("fixture_md5"), LexToString(FMD5Hash::HashFile(*FixturePath)));
        Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
        Root->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"));
        Root->SetNumberField(TEXT("viewport_width"), ViewWidth);
        Root->SetNumberField(TEXT("viewport_height"), ViewHeight);
        Root->SetStringField(TEXT("method"),
            TEXT("1/4/8 independent RmlUi views are overlaid at an identical 800x480 viewport. Each view owns an SInvalidationPanel. Metrics are split at the point where the one-shot CSS motion has stopped, the frame number is stable, Tick is disabled, and the cache route is resolved."));
        Root->SetStringField(TEXT("gpu_metric"),
            TEXT("RmlUiSlateRhi is emitted as an RDG GPU stat for Unreal Insights; this JSON does not estimate GPU time."));
        Root->SetStringField(TEXT("present_metric"),
            TEXT("Present is swap-chain scoped and is not attributed to individual RmlUi views by this test."));
        Root->SetArrayField(TEXT("scenarios"), Results);
        FString Json;
        FJsonSerializer::Serialize(Root.ToSharedRef(),
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
        const FString Directory = OutputDirectory();
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Path = FPaths::Combine(Directory,
            TEXT("RmlUiPaintCacheRouteMatrix-") + RunLabel() + TEXT(".json"));
        Test->TestTrue(TEXT("Save paint-cache route matrix report"), FFileHelper::SaveStringToFile(Json, *Path));
        Test->AddInfo(FString::Printf(TEXT("Paint-cache route matrix report: %s"), *Path));
    }

    void DestroyWindow()
    {
        if (Window.IsValid() && FSlateApplication::IsInitialized())
            FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Window.Reset();
        CacheRoots.Reset();
        Widgets.Reset();
    }

    void EndGpuProfileRegion()
    {
        if (!bGpuProfileRegionOpen) return;
        TRACE_END_REGION(TEXT("RmlUiGpuProfileWindow"));
        bGpuProfileRegionOpen = false;
        WriteGpuProfileWindowMarker(true, FPlatformTime::Cycles64());
    }

    void WriteGpuProfileWindowMarker(bool bComplete, uint64 EndQpc)
    {
        if (PresentWindowMarkerPath.IsEmpty()) return;
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema_version"), 1);
        Root->SetStringField(TEXT("clock"), TEXT("QueryPerformanceCounter"));
        Root->SetStringField(TEXT("start_qpc"), LexToString(GpuProfileStartQpc));
        Root->SetStringField(TEXT("end_qpc"), bComplete ? LexToString(EndQpc) : FString());
        Root->SetNumberField(TEXT("seconds_per_cycle"), FPlatformTime::GetSecondsPerCycle64());
        Root->SetBoolField(TEXT("complete"), bComplete);
        FString Json;
        FJsonSerializer::Serialize(Root.ToSharedRef(),
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(PresentWindowMarkerPath), true);
        if (!FFileHelper::SaveStringToFile(Json, *PresentWindowMarkerPath))
            Test->AddError(FString::Printf(TEXT("Failed to save PresentMon window marker: %s"),
                *PresentWindowMarkerPath));
    }

    static constexpr int32 ViewCounts[] = {1, 4, 8};
    FAutomationTestBase* Test = nullptr;
    bool bPreviouslyEnabled = false;
    bool bSampling = false;
    bool bGpuProfilePending = false;
    bool bGpuProfileRegionOpen = false;
    int32 Scenario = 0;
    double SampleSeconds = DefaultSampleSeconds;
    double ScenarioStarted = 0.0;
    double StableSince = 0.0;
    double SampleStarted = 0.0;
    double GpuProfileStarted = 0.0;
    uint64 GpuProfileStartQpc = 0;
    FString FixturePath;
    FString PresentWindowMarkerPath;
    FRmlUiPerformanceSnapshot WarmupSnapshot;
    TArray<uint64> LastFrames;
    TArray<TSharedPtr<FJsonValue>> Results;
    TArray<TSharedPtr<SRmlUiWidget>> Widgets;
    TArray<TSharedPtr<SInvalidationPanel>> CacheRoots;
    TSharedPtr<SWindow> Window;
};

class FTopologyChurnCommand final : public IAutomationLatentCommand
{
public:
    explicit FTopologyChurnCommand(FAutomationTestBase* InTest)
        : Test(InTest), bPreviouslyEnabled(FRmlUiPerformance::IsEnabled())
    {
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiTopologyChurnSeconds="), SampleSeconds);
        SampleSeconds = FMath::Max(0.5, SampleSeconds);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiTopologyChurnTeardownSeconds="), TeardownSeconds);
        TeardownSeconds = FMath::Clamp(TeardownSeconds, 0.0, 300.0);
        double FrequencyOverride = 0.0;
        if (FParse::Value(FCommandLine::Get(), TEXT("RmlUiTopologyChurnHz="), FrequencyOverride) &&
            FrequencyOverride > 0.0)
        {
            Frequencies.Add(FMath::Clamp(FrequencyOverride, 0.25, 120.0));
        }
        else
        {
            Frequencies = {2.0, 10.0, 30.0};
        }
        int32 ViewCountOverride = 0;
        if (FParse::Value(FCommandLine::Get(), TEXT("RmlUiTopologyChurnViewCount="), ViewCountOverride) &&
            ViewCountOverride > 0)
        {
            ViewCounts.Add(FMath::Clamp(ViewCountOverride, 1, 32));
        }
        else
        {
            ViewCounts = {1, 4, 8};
        }
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPresentWindowMarker="), PresentWindowMarkerPath);
        if (!PresentWindowMarkerPath.IsEmpty())
            PresentWindowMarkerPath = FPaths::ConvertRelativePathToFull(PresentWindowMarkerPath);
        FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(),
            TEXT("RmlUi/Tests/web-motion-libraries.html")));
    }

    virtual ~FTopologyChurnCommand() override
    {
        EndGpuProfileRegion();
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        DestroyWindow();
    }

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Phase == EPhase::Teardown)
        {
            SampleTeardownMemory(Now);
            if (Now - TeardownStarted >= TeardownSeconds)
            {
                FlushRenderingCommands();
                SampleTeardownMemory(Now, true);
                FinalizeTeardownMemory();
                Phase = EPhase::Settling;
                return AdvanceScenario();
            }
            return false;
        }
        if (!Window.IsValid())
        {
            StartScenario();
            return false;
        }

        if (Phase == EPhase::Settling)
        {
            if (ViewsSettled(Now))
            {
                BeginChurn(Now);
                return false;
            }
            if (Now - ScenarioStarted >= TimeoutSeconds)
            {
                Test->AddError(FString::Printf(TEXT("Topology churn fixture with %d views did not settle"),
                    CurrentViewCount()));
                return CompleteScenario(Now);
            }
            return false;
        }

        if (Phase == EPhase::Churning)
        {
            SampleMemory(Now);
            if (Now - SampleStarted >= SampleSeconds)
            {
                SampleMemory(Now, true);
                SampleEnded = Now;
                SampleEndMemory = FPlatformMemory::GetStats();
                EndGpuProfileRegion();
                Phase = EPhase::Draining;
                StableSince = 0.0;
                LastFrames.Init(0, Widgets.Num());
                return false;
            }
            if (Now >= NextToggle)
            {
                if (Now - NextToggle >= ToggleInterval)
                    MissedIntervals += FMath::FloorToInt((Now - NextToggle) / ToggleInterval);
                ToggleTopology();
                NextToggle = Now + ToggleInterval;
            }
            return false;
        }

        if (ViewsSettled(Now) || Now - SampleEnded >= DrainTimeoutSeconds)
            return CompleteScenario(Now);
        return false;
    }

private:
    enum class EPhase : uint8 { Settling, Churning, Draining, Teardown };

    int32 CurrentViewCount() const { return ViewCounts[ViewIndex]; }
    double CurrentFrequency() const { return Frequencies[FrequencyIndex]; }

    bool ViewsSettled(double Now)
    {
        bool bAllSettled = true;
        for (int32 Index = 0; Index < Widgets.Num(); ++Index)
        {
            const TSharedPtr<SRmlUiWidget>& Widget = Widgets[Index];
            const uint64 Frame = Widget->GetFrameNumber();
            const bool bRouteResolved = Frame > 0 && (Widget->IsSlatePaintCacheReady() ||
                Widget->GetSlatePaintCacheRejectReason() != ERmlUiPaintCacheRejectReason::None);
            bAllSettled &= bRouteResolved && !Widget->GetCanTick() && Frame == LastFrames[Index] &&
                Widget->GetLastError().IsEmpty();
            LastFrames[Index] = Frame;
        }
        if (!bAllSettled)
        {
            StableSince = 0.0;
            return false;
        }
        if (StableSince <= 0.0) StableSince = Now;
        return Now - StableSince >= StableWindowSeconds;
    }

    void StartScenario()
    {
        Test->TestTrue(TEXT("Topology churn uses the real web-motion fixture"),
            IFileManager::Get().FileExists(*FixturePath));
        Widgets.Reset();
        CacheRoots.Reset();
        LastFrames.Init(0, CurrentViewCount());
        StableSince = 0.0;
        Phase = EPhase::Settling;
        FRmlUiPerformance::SetEnabled(true);
        FRmlUiPerformance::Reset();

        TSharedRef<SOverlay> Overlay = SNew(SOverlay);
        for (int32 Index = 0; Index < CurrentViewCount(); ++Index)
        {
            TSharedPtr<SRmlUiWidget> Widget;
            TSharedPtr<SInvalidationPanel> CacheRoot;
            SAssignNew(Widget, SRmlUiWidget)
                .DocumentPath(FixturePath)
                .UseSlateRenderer(true)
                .UsePaintCache(true)
                .DesiredSize(FVector2D(ViewWidth, ViewHeight))
                .Clipping(EWidgetClipping::ClipToBounds);
            SAssignNew(CacheRoot, SInvalidationPanel)
                .DebugName(FString::Printf(TEXT("RmlUi topology churn view %d"), Index))
                [Widget.ToSharedRef()];
            CacheRoot->SetCanCache(true);
            Widgets.Add(Widget);
            CacheRoots.Add(CacheRoot);
            Overlay->AddSlot()[CacheRoot.ToSharedRef()];
        }
        TSharedRef<SWidget> Content = SNew(SBox).WidthOverride(ViewWidth).HeightOverride(ViewHeight)[Overlay];
        if (!PresentWindowMarkerPath.IsEmpty()) Content->SetVisibility(EVisibility::HitTestInvisible);
        const FVector2D WindowPosition = PresentWindowMarkerPath.IsEmpty() ?
            FVector2D(-10000, -10000) : FVector2D(32, 32);
        Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi clip topology churn")))
            .ClientSize(FVector2D(ViewWidth, ViewHeight)).UseOSWindowBorder(false).CreateTitleBar(false)
            .AutoCenter(EAutoCenter::None).ScreenPosition(WindowPosition)
            .IsTopmostWindow(!PresentWindowMarkerPath.IsEmpty())
            .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
            .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)
            [Content];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        ScenarioStarted = FPlatformTime::Seconds();
    }

    void BeginChurn(double Now)
    {
        FlushRenderingCommands();
        FRmlUiPerformance::Reset();
        MutationBatchMilliseconds.Reset();
        ToggleBatches = 0;
        SuccessfulMutations = 0;
        MissedIntervals = 0;
        bOverflowHidden = false;
        SampleStarted = Now;
        SampleEnded = 0.0;
        ToggleInterval = 1.0 / CurrentFrequency();
        NextToggle = Now;
        bGpuProfileTriggered = false;
        StartMemory = FPlatformMemory::GetStats();
        SampleEndMemory = StartMemory;
        PeakUsedPhysical = StartMemory.UsedPhysical;
        PeakUsedVirtual = StartMemory.UsedVirtual;
        MemorySampleSeconds.Reset();
        MemorySamplePhysicalMiB.Reset();
        MemorySampleVirtualMiB.Reset();
        NextMemorySample = Now;
        SampleMemory(Now, true);
        Phase = EPhase::Churning;
    }

    void ToggleTopology()
    {
        bOverflowHidden = !bOverflowHidden;
        const TCHAR* Value = bOverflowHidden ? TEXT("hidden") : TEXT("visible");
        const uint64 StartCycles = FPlatformTime::Cycles64();
        for (const TSharedPtr<SRmlUiWidget>& Widget : Widgets)
            if (Widget->SetElementProperty(TEXT("dropdown-surface"), TEXT("overflow"), Value))
                ++SuccessfulMutations;
        MutationBatchMilliseconds.Add(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles));
        ++ToggleBatches;
        if (!bGpuProfileTriggered && FParse::Param(FCommandLine::Get(), TEXT("RmlUiCaptureGpuProfile")) &&
            ViewIndex + 1 == ViewCounts.Num() && FrequencyIndex + 1 == Frequencies.Num() &&
            ToggleBatches >= FMath::Max(1, FMath::CeilToInt(CurrentFrequency() * 0.5)))
        {
            bGpuProfileTriggered = true;
            GpuProfileStartQpc = FPlatformTime::Cycles64();
            WriteGpuProfileWindowMarker(false, 0);
            TRACE_BEGIN_REGION(TEXT("RmlUiGpuProfileWindow"), TEXT("RmlUi"));
            bGpuProfileRegionOpen = true;
            IConsoleManager::Get().ProcessUserConsoleInput(TEXT("ProfileGPU"), *GLog, nullptr);
        }
    }

    void SampleMemory(double Now, bool bForce = false)
    {
        if (!bForce && Now < NextMemorySample) return;
        const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
        PeakUsedPhysical = FMath::Max(PeakUsedPhysical, Stats.UsedPhysical);
        PeakUsedVirtual = FMath::Max(PeakUsedVirtual, Stats.UsedVirtual);
        const double Elapsed = FMath::Max(0.0, Now - SampleStarted);
        constexpr double BytesToMiB = 1.0 / (1024.0 * 1024.0);
        if (!MemorySampleSeconds.IsEmpty() && FMath::IsNearlyEqual(MemorySampleSeconds.Last(), Elapsed))
        {
            MemorySamplePhysicalMiB.Last() = Stats.UsedPhysical * BytesToMiB;
            MemorySampleVirtualMiB.Last() = Stats.UsedVirtual * BytesToMiB;
        }
        else
        {
            MemorySampleSeconds.Add(Elapsed);
            MemorySamplePhysicalMiB.Add(Stats.UsedPhysical * BytesToMiB);
            MemorySampleVirtualMiB.Add(Stats.UsedVirtual * BytesToMiB);
        }
        NextMemorySample = Now + MemorySampleIntervalSeconds;
    }

    void BeginTeardown(double Now, const TSharedPtr<FJsonObject>& Memory,
        const FPlatformMemoryStats& DrainedMemory)
    {
        PendingMemory = Memory;
        BeforeTeardownMemory = DrainedMemory;
        TeardownStarted = Now;
        NextTeardownSample = Now;
        TeardownSampleSeconds.Reset();
        TeardownSamplePhysicalMiB.Reset();
        TeardownSampleVirtualMiB.Reset();
        TeardownSampleLiveWidgets.Reset();
        TeardownSampleWindowAlive.Reset();
        TeardownWidgetWeakRefs.Reset();
        for (const TSharedPtr<SRmlUiWidget>& Widget : Widgets)
            TeardownWidgetWeakRefs.Add(Widget);
        TeardownWindowWeak = Window;
        DestroyWindow();
        SampleTeardownMemory(Now, true);
        Phase = EPhase::Teardown;
    }

    void SampleTeardownMemory(double Now, bool bForce = false)
    {
        if (!bForce && Now < NextTeardownSample) return;
        const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
        int32 LiveWidgetCount = 0;
        for (const TWeakPtr<SRmlUiWidget>& Widget : TeardownWidgetWeakRefs)
            if (Widget.IsValid()) ++LiveWidgetCount;
        const bool bWindowAlive = TeardownWindowWeak.IsValid();
        PeakUsedPhysical = FMath::Max(PeakUsedPhysical, Stats.UsedPhysical);
        PeakUsedVirtual = FMath::Max(PeakUsedVirtual, Stats.UsedVirtual);
        const double Elapsed = FMath::Max(0.0, Now - TeardownStarted);
        constexpr double BytesToMiB = 1.0 / (1024.0 * 1024.0);
        if (!TeardownSampleSeconds.IsEmpty() && FMath::IsNearlyEqual(TeardownSampleSeconds.Last(), Elapsed))
        {
            TeardownSamplePhysicalMiB.Last() = Stats.UsedPhysical * BytesToMiB;
            TeardownSampleVirtualMiB.Last() = Stats.UsedVirtual * BytesToMiB;
            TeardownSampleLiveWidgets.Last() = LiveWidgetCount;
            TeardownSampleWindowAlive.Last() = bWindowAlive;
        }
        else
        {
            TeardownSampleSeconds.Add(Elapsed);
            TeardownSamplePhysicalMiB.Add(Stats.UsedPhysical * BytesToMiB);
            TeardownSampleVirtualMiB.Add(Stats.UsedVirtual * BytesToMiB);
            TeardownSampleLiveWidgets.Add(LiveWidgetCount);
            TeardownSampleWindowAlive.Add(bWindowAlive);
        }
        NextTeardownSample = Now + MemorySampleIntervalSeconds;
    }

    void FinalizeTeardownMemory()
    {
        if (!PendingMemory.IsValid() || TeardownSampleSeconds.IsEmpty()) return;
        constexpr double BytesToMiB = 1.0 / (1024.0 * 1024.0);
        const double FinalPhysicalMiB = TeardownSamplePhysicalMiB.Last();
        const double FinalVirtualMiB = TeardownSampleVirtualMiB.Last();
        double MinimumPhysicalMiB = TeardownSamplePhysicalMiB[0];
        double MinimumVirtualMiB = TeardownSampleVirtualMiB[0];
        double MaximumPhysicalMiB = TeardownSamplePhysicalMiB[0];
        double MaximumVirtualMiB = TeardownSampleVirtualMiB[0];
        double AllWidgetsReleasedSeconds = -1.0;
        double WindowReleasedSeconds = -1.0;
        for (const double Value : TeardownSamplePhysicalMiB)
        {
            MinimumPhysicalMiB = FMath::Min(MinimumPhysicalMiB, Value);
            MaximumPhysicalMiB = FMath::Max(MaximumPhysicalMiB, Value);
        }
        for (const double Value : TeardownSampleVirtualMiB)
        {
            MinimumVirtualMiB = FMath::Min(MinimumVirtualMiB, Value);
            MaximumVirtualMiB = FMath::Max(MaximumVirtualMiB, Value);
        }
        for (int32 Index = 0; Index < TeardownSampleSeconds.Num(); ++Index)
        {
            if (AllWidgetsReleasedSeconds < 0.0 && TeardownSampleLiveWidgets[Index] == 0)
                AllWidgetsReleasedSeconds = TeardownSampleSeconds[Index];
            if (WindowReleasedSeconds < 0.0 && !TeardownSampleWindowAlive[Index])
                WindowReleasedSeconds = TeardownSampleSeconds[Index];
        }

        TArray<TSharedPtr<FJsonValue>> Samples;
        for (int32 Index = 0; Index < TeardownSampleSeconds.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("seconds"), TeardownSampleSeconds[Index]);
            Sample->SetNumberField(TEXT("used_physical_mib"), TeardownSamplePhysicalMiB[Index]);
            Sample->SetNumberField(TEXT("used_virtual_mib"), TeardownSampleVirtualMiB[Index]);
            Sample->SetNumberField(TEXT("live_widget_count"), TeardownSampleLiveWidgets[Index]);
            Sample->SetBoolField(TEXT("window_alive"), TeardownSampleWindowAlive[Index]);
            Samples.Add(MakeShared<FJsonValueObject>(Sample));
        }
        PendingMemory->SetNumberField(TEXT("teardown_requested_seconds"), TeardownSeconds);
        PendingMemory->SetNumberField(TEXT("used_physical_after_destroy_request_mib"),
            TeardownSamplePhysicalMiB[0]);
        PendingMemory->SetNumberField(TEXT("used_virtual_after_destroy_request_mib"),
            TeardownSampleVirtualMiB[0]);
        PendingMemory->SetNumberField(TEXT("used_physical_after_teardown_mib"), FinalPhysicalMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_after_teardown_mib"), FinalVirtualMiB);
        PendingMemory->SetNumberField(TEXT("used_physical_teardown_min_mib"), MinimumPhysicalMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_teardown_min_mib"), MinimumVirtualMiB);
        PendingMemory->SetNumberField(TEXT("used_physical_teardown_max_mib"), MaximumPhysicalMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_teardown_max_mib"), MaximumVirtualMiB);
        PendingMemory->SetNumberField(TEXT("used_physical_teardown_range_mib"),
            MaximumPhysicalMiB - MinimumPhysicalMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_teardown_range_mib"),
            MaximumVirtualMiB - MinimumVirtualMiB);
        PendingMemory->SetNumberField(TEXT("all_widgets_released_at_seconds"), AllWidgetsReleasedSeconds);
        PendingMemory->SetNumberField(TEXT("window_released_at_seconds"), WindowReleasedSeconds);
        PendingMemory->SetNumberField(TEXT("live_widgets_after_teardown"), TeardownSampleLiveWidgets.Last());
        PendingMemory->SetBoolField(TEXT("window_alive_after_teardown"), TeardownSampleWindowAlive.Last());
        PendingMemory->SetNumberField(TEXT("used_physical_released_after_teardown_mib"),
            BeforeTeardownMemory.UsedPhysical * BytesToMiB - FinalPhysicalMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_released_after_teardown_mib"),
            BeforeTeardownMemory.UsedVirtual * BytesToMiB - FinalVirtualMiB);
        PendingMemory->SetNumberField(TEXT("used_physical_peak_mib"), PeakUsedPhysical * BytesToMiB);
        PendingMemory->SetNumberField(TEXT("used_physical_peak_delta_mib"),
            (static_cast<double>(PeakUsedPhysical) - StartMemory.UsedPhysical) * BytesToMiB);
        PendingMemory->SetNumberField(TEXT("used_virtual_peak_mib"), PeakUsedVirtual * BytesToMiB);
        const double TeardownMinutes =
            (TeardownSampleSeconds.Last() - TeardownSampleSeconds[0]) / 60.0;
        PendingMemory->SetNumberField(TEXT("teardown_endpoint_physical_slope_mib_per_min"),
            TeardownMinutes > 0.0 ?
            (FinalPhysicalMiB - TeardownSamplePhysicalMiB[0]) / TeardownMinutes : 0.0);
        PendingMemory->SetNumberField(TEXT("teardown_endpoint_virtual_slope_mib_per_min"),
            TeardownMinutes > 0.0 ?
            (FinalVirtualMiB - TeardownSampleVirtualMiB[0]) / TeardownMinutes : 0.0);
        PendingMemory->SetNumberField(TEXT("teardown_ols_physical_slope_mib_per_min"),
            LinearRegressionSlopePerMinute(TeardownSampleSeconds, TeardownSamplePhysicalMiB, 0));
        PendingMemory->SetNumberField(TEXT("teardown_ols_virtual_slope_mib_per_min"),
            LinearRegressionSlopePerMinute(TeardownSampleSeconds, TeardownSampleVirtualMiB, 0));
        PendingMemory->SetArrayField(TEXT("teardown_samples"), Samples);
        PendingMemory.Reset();
    }

    static double Percentile(TArray<double> Values, double Quantile)
    {
        if (Values.IsEmpty()) return 0.0;
        Values.Sort();
        const int32 Index = FMath::Clamp(FMath::CeilToInt(Quantile * Values.Num()) - 1, 0, Values.Num() - 1);
        return Values[Index];
    }

    static double LinearRegressionSlopePerMinute(const TArray<double>& Seconds,
        const TArray<double>& Values, int32 StartIndex)
    {
        const int32 Count = Seconds.Num() - StartIndex;
        if (Count < 2 || Values.Num() != Seconds.Num()) return 0.0;
        double SumX = 0.0;
        double SumY = 0.0;
        double SumXY = 0.0;
        double SumXX = 0.0;
        for (int32 Index = StartIndex; Index < Seconds.Num(); ++Index)
        {
            const double X = Seconds[Index];
            const double Y = Values[Index];
            SumX += X;
            SumY += Y;
            SumXY += X * Y;
            SumXX += X * X;
        }
        const double Denominator = Count * SumXX - SumX * SumX;
        return FMath::IsNearlyZero(Denominator) ? 0.0 :
            60.0 * (Count * SumXY - SumX * SumY) / Denominator;
    }

    bool CompleteScenario(double Now)
    {
        FlushRenderingCommands();
        const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
        const ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Slate;
        const uint64 FullRecords = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateFullFrameRecords);
        const uint64 Changes = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyChanges);
        const uint64 Draws = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyDrawsDecoded);
        const uint64 DecodeCycles = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::SlateClipTopologyDecodeCycles);
        const uint64 Invalidations = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheInvalidations);
        const uint64 Activations = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheActivations);
        const double ActualSeconds = FMath::Max(SampleEnded - SampleStarted, 0.000001);

        Test->TestEqual(TEXT("Every topology property mutation was accepted"), SuccessfulMutations,
            ToggleBatches * CurrentViewCount());
        Test->TestTrue(TEXT("Sustained property churn produced clip-topology changes"), Changes > 0);
        Test->TestTrue(TEXT("Topology changes are bounded by accepted mutations"),
            Changes <= static_cast<uint64>(SuccessfulMutations));
        Test->TestTrue(TEXT("Every observed topology change came from a complete frame"), FullRecords >= Changes);
        Test->TestTrue(TEXT("Topology churn invalidated Slate paint caches"), Invalidations > 0);
        Test->TestTrue(TEXT("Topology churn decoded draw records"), Draws > 0 && DecodeCycles > 0);
        for (int32 Index = 0; Index < Widgets.Num(); ++Index)
        {
            Test->TestTrue(FString::Printf(TEXT("Topology churn view %d remained valid"), Index),
                Widgets[Index]->GetLastError().IsEmpty() && Widgets[Index]->GetFrameNumber() > 0);
            Test->TestTrue(FString::Printf(TEXT("Topology churn view %d drained to a ready cache"), Index),
                Widgets[Index]->IsSlatePaintCacheReady());
        }

        double MutationTotalMs = 0.0;
        for (const double Value : MutationBatchMilliseconds) MutationTotalMs += Value;
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("view_count"), CurrentViewCount());
        Result->SetNumberField(TEXT("target_hz"), CurrentFrequency());
        Result->SetNumberField(TEXT("requested_seconds"), SampleSeconds);
        Result->SetNumberField(TEXT("actual_seconds"), ActualSeconds);
        Result->SetNumberField(TEXT("toggle_batches"), ToggleBatches);
        Result->SetNumberField(TEXT("successful_property_mutations"), SuccessfulMutations);
        Result->SetNumberField(TEXT("missed_intervals"), MissedIntervals);
        Result->SetNumberField(TEXT("achieved_hz"), ToggleBatches / ActualSeconds);
        Result->SetNumberField(TEXT("mutation_batch_mean_ms"),
            ToggleBatches ? MutationTotalMs / ToggleBatches : 0.0);
        Result->SetNumberField(TEXT("mutation_batch_p50_ms"), Percentile(MutationBatchMilliseconds, 0.50));
        Result->SetNumberField(TEXT("mutation_batch_p95_ms"), Percentile(MutationBatchMilliseconds, 0.95));
        Result->SetNumberField(TEXT("observed_topology_changes"), static_cast<double>(Changes));
        Result->SetNumberField(TEXT("topology_changes_per_second"), Changes / ActualSeconds);
        Result->SetNumberField(TEXT("changes_per_mutation"), SuccessfulMutations ?
            static_cast<double>(Changes) / SuccessfulMutations : 0.0);
        Result->SetNumberField(TEXT("decode_ms_per_change"), Changes ?
            FPlatformTime::ToMilliseconds64(DecodeCycles) / Changes : 0.0);
        Result->SetNumberField(TEXT("full_records_per_change"), Changes ?
            static_cast<double>(FullRecords) / Changes : 0.0);
        Result->SetNumberField(TEXT("paint_cache_invalidations_per_change"), Changes ?
            static_cast<double>(Invalidations) / Changes : 0.0);
        Result->SetNumberField(TEXT("paint_cache_activations"), static_cast<double>(Activations));
        Result->SetNumberField(TEXT("drain_seconds"), FMath::Max(0.0, Now - SampleEnded));
        const FPlatformMemoryStats DrainedMemory = FPlatformMemory::GetStats();
        PeakUsedPhysical = FMath::Max(PeakUsedPhysical, DrainedMemory.UsedPhysical);
        PeakUsedVirtual = FMath::Max(PeakUsedVirtual, DrainedMemory.UsedVirtual);
        constexpr double BytesToMiB = 1.0 / (1024.0 * 1024.0);
        TSharedPtr<FJsonObject> Memory = MakeShared<FJsonObject>();
        Memory->SetStringField(TEXT("source"), TEXT("FPlatformMemory::GetStats"));
        Memory->SetStringField(TEXT("used_physical_semantics"),
            TEXT("Platform-defined current-process resident memory; Windows uses WorkingSetSize."));
        Memory->SetStringField(TEXT("used_virtual_semantics"),
            TEXT("Platform-defined current-process virtual memory; Windows uses PagefileUsage (commit charge)."));
        Memory->SetNumberField(TEXT("used_physical_start_mib"), StartMemory.UsedPhysical * BytesToMiB);
        Memory->SetNumberField(TEXT("used_physical_sample_end_mib"), SampleEndMemory.UsedPhysical * BytesToMiB);
        Memory->SetNumberField(TEXT("used_physical_after_drain_mib"), DrainedMemory.UsedPhysical * BytesToMiB);
        Memory->SetNumberField(TEXT("used_physical_peak_mib"), PeakUsedPhysical * BytesToMiB);
        Memory->SetNumberField(TEXT("used_physical_peak_delta_mib"),
            (static_cast<double>(PeakUsedPhysical) - StartMemory.UsedPhysical) * BytesToMiB);
        Memory->SetNumberField(TEXT("used_virtual_start_mib"), StartMemory.UsedVirtual * BytesToMiB);
        Memory->SetNumberField(TEXT("used_virtual_sample_end_mib"), SampleEndMemory.UsedVirtual * BytesToMiB);
        Memory->SetNumberField(TEXT("used_virtual_after_drain_mib"), DrainedMemory.UsedVirtual * BytesToMiB);
        Memory->SetNumberField(TEXT("used_virtual_peak_mib"), PeakUsedVirtual * BytesToMiB);
        TArray<TSharedPtr<FJsonValue>> MemorySamples;
        for (int32 Index = 0; Index < MemorySampleSeconds.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("seconds"), MemorySampleSeconds[Index]);
            Sample->SetNumberField(TEXT("used_physical_mib"), MemorySamplePhysicalMiB[Index]);
            Sample->SetNumberField(TEXT("used_virtual_mib"), MemorySampleVirtualMiB[Index]);
            MemorySamples.Add(MakeShared<FJsonValueObject>(Sample));
        }
        Memory->SetArrayField(TEXT("samples"), MemorySamples);
        const double TailStartSeconds = ActualSeconds * 0.5;
        int32 TailStartIndex = 0;
        while (TailStartIndex + 1 < MemorySampleSeconds.Num() &&
            MemorySampleSeconds[TailStartIndex] < TailStartSeconds)
            ++TailStartIndex;
        if (MemorySampleSeconds.Num() > 1 && TailStartIndex < MemorySampleSeconds.Num() - 1)
        {
            const double TailMinutes =
                (MemorySampleSeconds.Last() - MemorySampleSeconds[TailStartIndex]) / 60.0;
            Memory->SetNumberField(TEXT("tail_endpoint_physical_slope_mib_per_min"), TailMinutes > 0.0 ?
                (MemorySamplePhysicalMiB.Last() - MemorySamplePhysicalMiB[TailStartIndex]) / TailMinutes : 0.0);
            Memory->SetNumberField(TEXT("tail_endpoint_virtual_slope_mib_per_min"), TailMinutes > 0.0 ?
                (MemorySampleVirtualMiB.Last() - MemorySampleVirtualMiB[TailStartIndex]) / TailMinutes : 0.0);
            Memory->SetNumberField(TEXT("tail_ols_physical_slope_mib_per_min"),
                LinearRegressionSlopePerMinute(MemorySampleSeconds, MemorySamplePhysicalMiB,
                    TailStartIndex));
            Memory->SetNumberField(TEXT("tail_ols_virtual_slope_mib_per_min"),
                LinearRegressionSlopePerMinute(MemorySampleSeconds, MemorySampleVirtualMiB,
                    TailStartIndex));
        }
        int32 LastMinuteStartIndex = 0;
        const double LastMinuteStartSeconds = FMath::Max(0.0, ActualSeconds - 60.0);
        while (LastMinuteStartIndex + 1 < MemorySampleSeconds.Num() &&
            MemorySampleSeconds[LastMinuteStartIndex] < LastMinuteStartSeconds)
            ++LastMinuteStartIndex;
        if (MemorySampleSeconds.Num() > 1 && LastMinuteStartIndex < MemorySampleSeconds.Num() - 1)
        {
            Memory->SetNumberField(TEXT("last_60s_ols_physical_slope_mib_per_min"),
                LinearRegressionSlopePerMinute(MemorySampleSeconds, MemorySamplePhysicalMiB,
                    LastMinuteStartIndex));
            Memory->SetNumberField(TEXT("last_60s_ols_virtual_slope_mib_per_min"),
                LinearRegressionSlopePerMinute(MemorySampleSeconds, MemorySampleVirtualMiB,
                    LastMinuteStartIndex));
        }
        Result->SetObjectField(TEXT("process_memory"), Memory);
        Result->SetObjectField(TEXT("churn_and_drain"), SnapshotJson(Snapshot));
        Results.Add(MakeShared<FJsonValueObject>(Result));

        Test->AddInfo(FString::Printf(
            TEXT("topology churn: views=%d target_hz=%.1f achieved_hz=%.2f mutations=%d changes=%llu full_records=%llu decode_ms=%.4f invalidations=%llu activations=%llu"),
            CurrentViewCount(), CurrentFrequency(), ToggleBatches / ActualSeconds, SuccessfulMutations,
            Changes, FullRecords, FPlatformTime::ToMilliseconds64(DecodeCycles), Invalidations, Activations));

        if (TeardownSeconds > 0.0)
        {
            BeginTeardown(Now, Memory, DrainedMemory);
            return false;
        }
        DestroyWindow();
        return AdvanceScenario();
    }

    bool AdvanceScenario()
    {
        ++FrequencyIndex;
        if (FrequencyIndex >= Frequencies.Num())
        {
            FrequencyIndex = 0;
            ++ViewIndex;
        }
        if (ViewIndex < ViewCounts.Num()) return false;
        SaveReport();
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        return true;
    }

    void SaveReport()
    {
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema_version"), 1);
        Root->SetStringField(TEXT("fixture"), FixturePath);
        Root->SetStringField(TEXT("fixture_md5"), LexToString(FMD5Hash::HashFile(*FixturePath)));
        Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
        Root->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"));
        Root->SetStringField(TEXT("target_element"), TEXT("dropdown-surface"));
        Root->SetStringField(TEXT("mutated_property"), TEXT("overflow"));
        Root->SetStringField(TEXT("method"),
            TEXT("The real web-motion-libraries dropdown is loaded into independently owned Slate views (default matrix: 1/4/8; command-line override supported). After its library-inspired one-shot motion and paint cache settle, overflow alternates between hidden and visible at a controlled frequency. Metrics cover mutation dispatch, complete command records, exact clip-topology changes, draw decode, paint-cache invalidation, and sampled process memory through final drain. An optional teardown window samples process memory after the Slate window and all owned views are released. Multiple mutations may coalesce before a rendered frame."));
        Root->SetArrayField(TEXT("scenarios"), Results);
        FString Json;
        FJsonSerializer::Serialize(Root.ToSharedRef(),
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
        const FString Directory = OutputDirectory();
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Path = FPaths::Combine(Directory,
            TEXT("RmlUiClipTopologyChurn-") + RunLabel() + TEXT(".json"));
        Test->TestTrue(TEXT("Save clip-topology churn report"), FFileHelper::SaveStringToFile(Json, *Path));
        Test->AddInfo(FString::Printf(TEXT("Clip-topology churn report: %s"), *Path));
    }

    void DestroyWindow()
    {
        if (Window.IsValid() && FSlateApplication::IsInitialized())
            FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Window.Reset();
        CacheRoots.Reset();
        Widgets.Reset();
    }

    void EndGpuProfileRegion()
    {
        if (!bGpuProfileRegionOpen) return;
        TRACE_END_REGION(TEXT("RmlUiGpuProfileWindow"));
        bGpuProfileRegionOpen = false;
        WriteGpuProfileWindowMarker(true, FPlatformTime::Cycles64());
    }

    void WriteGpuProfileWindowMarker(bool bComplete, uint64 EndQpc)
    {
        if (PresentWindowMarkerPath.IsEmpty()) return;
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema_version"), 1);
        Root->SetStringField(TEXT("clock"), TEXT("QueryPerformanceCounter"));
        Root->SetStringField(TEXT("start_qpc"), LexToString(GpuProfileStartQpc));
        Root->SetStringField(TEXT("end_qpc"), bComplete ? LexToString(EndQpc) : FString());
        Root->SetNumberField(TEXT("seconds_per_cycle"), FPlatformTime::GetSecondsPerCycle64());
        Root->SetBoolField(TEXT("complete"), bComplete);
        FString Json;
        FJsonSerializer::Serialize(Root.ToSharedRef(),
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(PresentWindowMarkerPath), true);
        if (!FFileHelper::SaveStringToFile(Json, *PresentWindowMarkerPath))
            Test->AddError(FString::Printf(TEXT("Failed to save topology churn profile marker: %s"),
                *PresentWindowMarkerPath));
    }

    FAutomationTestBase* Test = nullptr;
    bool bPreviouslyEnabled = false;
    bool bOverflowHidden = false;
    bool bGpuProfileTriggered = false;
    bool bGpuProfileRegionOpen = false;
    int32 ViewIndex = 0;
    int32 FrequencyIndex = 0;
    int32 ToggleBatches = 0;
    int32 SuccessfulMutations = 0;
    int32 MissedIntervals = 0;
    double SampleSeconds = 2.0;
    double ScenarioStarted = 0.0;
    double StableSince = 0.0;
    double SampleStarted = 0.0;
    double SampleEnded = 0.0;
    double NextToggle = 0.0;
    double ToggleInterval = 0.0;
    double NextMemorySample = 0.0;
    double TeardownSeconds = 0.0;
    double TeardownStarted = 0.0;
    double NextTeardownSample = 0.0;
    uint64 GpuProfileStartQpc = 0;
    uint64 PeakUsedPhysical = 0;
    uint64 PeakUsedVirtual = 0;
    static constexpr double DrainTimeoutSeconds = 5.0;
    static constexpr double MemorySampleIntervalSeconds = 0.5;
    EPhase Phase = EPhase::Settling;
    FString FixturePath;
    FString PresentWindowMarkerPath;
    FPlatformMemoryStats StartMemory;
    FPlatformMemoryStats SampleEndMemory;
    FPlatformMemoryStats BeforeTeardownMemory;
    TSharedPtr<FJsonObject> PendingMemory;
    TArray<int32> ViewCounts;
    TArray<double> Frequencies;
    TArray<double> MutationBatchMilliseconds;
    TArray<double> MemorySampleSeconds;
    TArray<double> MemorySamplePhysicalMiB;
    TArray<double> MemorySampleVirtualMiB;
    TArray<double> TeardownSampleSeconds;
    TArray<double> TeardownSamplePhysicalMiB;
    TArray<double> TeardownSampleVirtualMiB;
    TArray<int32> TeardownSampleLiveWidgets;
    TArray<bool> TeardownSampleWindowAlive;
    TArray<TWeakPtr<SRmlUiWidget>> TeardownWidgetWeakRefs;
    TWeakPtr<SWindow> TeardownWindowWeak;
    TArray<uint64> LastFrames;
    TArray<TSharedPtr<FJsonValue>> Results;
    TArray<TSharedPtr<SRmlUiWidget>> Widgets;
    TArray<TSharedPtr<SInvalidationPanel>> CacheRoots;
    TSharedPtr<SWindow> Window;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiPaintCacheRouteMatrixTest,
    "RmlUiUnreal.Performance.PaintCacheRouteMatrix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiPaintCacheRouteMatrixTest::RunTest(const FString&)
{
    AddCommand(new RmlUiPaintCacheRouteTests::FRouteMatrixCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiClipTopologyChurnTest,
    "RmlUiUnreal.Performance.ClipTopologyChurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiClipTopologyChurnTest::RunTest(const FString&)
{
    double FrequencyOverride = 0.0;
    const int32 FrequencyCount = FParse::Value(FCommandLine::Get(),
        TEXT("RmlUiTopologyChurnHz="), FrequencyOverride) && FrequencyOverride > 0.0 ? 1 : 3;
    int32 ViewCountOverride = 0;
    const int32 LoadedViewsPerFrequency = FParse::Value(FCommandLine::Get(),
        TEXT("RmlUiTopologyChurnViewCount="), ViewCountOverride) && ViewCountOverride > 0 ?
        FMath::Clamp(ViewCountOverride, 1, 32) : (1 + 4 + 8);
    const int32 LoadedViewCount = LoadedViewsPerFrequency * FrequencyCount;
    AddExpectedError(TEXT("Syntax error parsing media-query property declaration 'prefers-reduced-motion: reduce;'"),
        EAutomationExpectedErrorFlags::Contains, LoadedViewCount);
    AddExpectedError(TEXT("Media query list parsing yielded no properties"),
        EAutomationExpectedErrorFlags::Contains, LoadedViewCount);
    AddExpectedError(TEXT("Could not compile filter on element"),
        EAutomationExpectedErrorFlags::Contains, LoadedViewCount);
    AddExpectedError(TEXT("The experimental Slate renderer cannot reproduce RmlUi feature mask"),
        EAutomationExpectedErrorFlags::Contains, LoadedViewCount);
    AddCommand(new RmlUiPaintCacheRouteTests::FTopologyChurnCommand(this));
    return true;
}

#endif
