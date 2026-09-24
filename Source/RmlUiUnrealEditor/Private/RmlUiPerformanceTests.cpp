#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "RmlUiPerformance.h"
#include "SRmlUiWidget.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SInvalidationPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Fonts/CompositeFont.h"
#include "Styling/CoreStyle.h"
#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogRmlUiPerformanceTest, Log, All);

namespace RmlUiPerformanceTests
{
constexpr double DefaultWarmupSeconds = 1.0;
constexpr double DefaultSampleSeconds = 3.0;
constexpr double ReadinessTimeoutSeconds = 30.0;
constexpr int32 RowCount = 100;
constexpr int32 ViewWidth = 1280;
constexpr int32 ViewHeight = 800;

const TCHAR* ScenarioName(int32 Scenario)
{
    static const TCHAR* Names[] = {TEXT("Slate"), TEXT("WebBrowser"), TEXT("RmlUiDX11"), TEXT("RmlUiSlate")};
    return Names[Scenario];
}

FString FontPath()
{
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(),
        TEXT("RmlUi/Fonts/LatoLatin-Regular.ttf")));
}

FString RunLabel()
{
    return FString::Printf(TEXT("%s-%s"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"),
        FParse::Param(FCommandLine::Get(), TEXT("RenderOffscreen")) ? TEXT("Headless") : TEXT("Windowed"));
}

FString OutputDirectory()
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(), TEXT("RmlUiPerfOutput="), Directory) || Directory.IsEmpty())
        Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance"));
    return FPaths::ConvertRelativePathToFull(Directory);
}

class SMeasuredWidget final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMeasuredWidget) {}
        SLATE_DEFAULT_SLOT(FArguments, Content)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args) { ChildSlot[Args._Content.Widget]; }
    void BeginCapture() { Samples.Reset(); Samples.Reserve(16384); bCapture = true; }
    void EndCapture() { bCapture = false; }

    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
        FSlateWindowElementList& Elements, int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override
    {
        const uint64 Start = FPlatformTime::Cycles64();
        const int32 Result = SCompoundWidget::OnPaint(Args, Geometry, CullingRect, Elements, LayerId, Style, bParentEnabled);
        if (bCapture) Samples.Add(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Start));
        return Result;
    }

    mutable TArray<double> Samples;
    mutable bool bCapture = false;
};

double Percentile(TArray<double> Values, double Fraction)
{
    if (Values.IsEmpty()) return 0.0;
    Values.Sort();
    return Values[FMath::Clamp(FMath::CeilToInt((Values.Num() - 1) * Fraction), 0, Values.Num() - 1)];
}

FString HtmlDocument()
{
    FString Rows;
    for (int32 Index = 0; Index < RowCount; ++Index)
        Rows += FString::Printf(TEXT("<div class='row'><span class='name'>Item %03d</span><span class='payload'>Shared benchmark payload</span><span class='value'>%d</span></div>"), Index, Index * 17);
    return FString::Printf(TEXT(R"HTML(<!doctype html><html><head><style>
@font-face{font-family:LatoLatin;src:url('LatoLatin-Regular.ttf')}
html,body{margin:0;width:100%%;height:100%%;overflow:hidden;background:#18232d;color:#eef2f4;font:14px LatoLatin}
.header{height:48px;padding:0 14px;display:flex;align-items:center;background:#176b59;font-size:20px}
.row{height:28px;display:flex;align-items:center;border-bottom:1px solid #40515d;padding:0 12px;overflow:hidden}
.name{width:110px;flex-shrink:0}.payload{flex:1;min-width:0}.value{width:80px;flex-shrink:0;color:#8fd3c2}
</style></head><body><div class='header'><span id='header'>Frame 0</span></div>%s<script>
document.fonts.ready.then(()=>{document.title=document.fonts.check('14px LatoLatin')?'RmlUiBenchmarkReady':'RmlUiBenchmarkFontFailed'});
</script></body></html>)HTML"), *Rows);
}

FString RmlDocument()
{
    FString Rows;
    for (int32 Index = 0; Index < RowCount; ++Index)
        Rows += FString::Printf(TEXT("<div class='row'><span class='name'>Item %03d</span><span class='payload'>Shared benchmark payload</span><span class='value'>%d</span></div>"), Index, Index * 17);
    return FString::Printf(TEXT(R"RML(<rml><head><style>
body{margin:0;background-color:#18232d;color:#eef2f4;font-family:LatoLatin;font-size:14px;width:100%%;height:100%%;overflow:hidden}
.header{height:48px;padding-left:14px;padding-right:14px;display:flex;align-items:center;background-color:#176b59;font-size:20px}
.row{height:28px;display:flex;align-items:center;border-bottom-width:1px;border-bottom-color:#40515d;padding-left:12px;padding-right:12px;overflow:hidden}
.name{width:110px;flex-shrink:0}.payload{flex:1;min-width:0}.value{width:80px;flex-shrink:0;color:#8fd3c2}
</style></head><body><div class='header'><span id='header'>Frame 0</span></div>%s</body></rml>)RML"), *Rows);
}

TSharedPtr<FJsonObject> StageJson(const FRmlUiPerformanceSnapshot& Snapshot,
    ERmlUiPerformanceBackend Backend, ERmlUiPerformanceStage Stage)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    const uint64 Calls = Snapshot.CallCount(Backend, Stage);
    Result->SetNumberField(TEXT("calls"), static_cast<double>(Calls));
    Result->SetNumberField(TEXT("total_ms"), Snapshot.Milliseconds(Backend, Stage));
    Result->SetNumberField(TEXT("mean_ms"), Calls ? Snapshot.Milliseconds(Backend, Stage) / Calls : 0.0);
    return Result;
}

class FComparisonCommand final : public IAutomationLatentCommand
{
public:
    explicit FComparisonCommand(FAutomationTestBase* InTest)
        : Test(InTest), bPreviouslyEnabled(FRmlUiPerformance::IsEnabled())
    {
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPerfWarmup="), WarmupSeconds);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPerfSeconds="), SampleSeconds);
        WarmupSeconds = FMath::Max(0.0, WarmupSeconds);
        SampleSeconds = FMath::Max(0.25, SampleSeconds);
    }

    virtual ~FComparisonCommand() override
    {
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        if (Window.IsValid() && FSlateApplication::IsInitialized())
            FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
    }

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (!Window.IsValid())
        {
            StartScenario();
            return false;
        }

        if (!bReady)
        {
            const FVector2D Size = Measured->GetCachedGeometry().GetLocalSize();
            const bool bGeometryReady = Size.X > 0 && Size.Y > 0;
            bReady = bGeometryReady && (!Browser.IsValid() || (*BrowserFontReady && Browser->IsLoaded())) &&
                (!RmlWidget.IsValid() || (RmlWidget->GetNativeView() && RmlWidget->GetFrameNumber() > 0 &&
                    RmlWidget->GetLastError().IsEmpty()));
            if (bReady)
            {
                ReadinessSeconds = Now - ScenarioStartTime;
                WarmupStartTime = Now;
                UE_LOG(LogRmlUiPerformanceTest, Display, TEXT("RmlUiPerf scenario=%s phase=warmup_start utc=%s startup_seconds=%.6f"),
                    ScenarioName(Scenario), *FDateTime::UtcNow().ToIso8601(), ReadinessSeconds);
            }
            else if (Now - ScenarioStartTime < ReadinessTimeoutSeconds)
                return false;
            else
            {
                Test->AddError(FString::Printf(TEXT("Performance scenario %d did not become ready within %.0fs"),
                    Scenario, ReadinessTimeoutSeconds));
                return CompleteScenario();
            }
        }

        ++Frame;
        if (NativeHeader.IsValid()) NativeHeader->SetText(FText::FromString(FString::Printf(TEXT("Frame %d"), Frame)));
        if (Browser.IsValid()) Browser->ExecuteJavascript(FString::Printf(
            TEXT("document.getElementById('header').textContent='Frame %d'"), Frame));
        if (RmlWidget.IsValid()) RmlWidget->SetElementText(TEXT("header"), FString::Printf(TEXT("Frame %d"), Frame));
        Measured->Invalidate(EInvalidateWidgetReason::Paint);

        if (Now - WarmupStartTime >= WarmupSeconds && !bSampling)
        {
            // Drain warmup rendering before resetting global counters; the first timed interval
            // starts after this flush. Snapshot is taken only once, after capture finishes.
            FlushRenderingCommands();
            bSampling = true;
            SampleStartTime = FPlatformTime::Seconds();
            LastSampleTime = SampleStartTime;
            FrameIntervals.Reset();
            Measured->BeginCapture();
            if (RmlWidget.IsValid()) FRmlUiPerformance::Reset();
            SampleStartUtc = FDateTime::UtcNow().ToIso8601();
            UE_LOG(LogRmlUiPerformanceTest, Display, TEXT("RmlUiPerf scenario=%s phase=sample_start utc=%s startup_seconds=%.6f warmup_seconds=%.6f"),
                ScenarioName(Scenario), *SampleStartUtc, SampleStartTime - ScenarioStartTime, SampleStartTime - WarmupStartTime);
        }
        else if (bSampling)
        {
            FrameIntervals.Add((Now - LastSampleTime) * 1000.0);
            LastSampleTime = Now;
        }

        if (!bSampling || Now - SampleStartTime < SampleSeconds) return false;
        ActualSampleSeconds = Now - SampleStartTime;
        SampleEndUtc = FDateTime::UtcNow().ToIso8601();
        UE_LOG(LogRmlUiPerformanceTest, Display, TEXT("RmlUiPerf scenario=%s phase=sample_end utc=%s sample_seconds=%.6f samples=%d"),
            ScenarioName(Scenario), *SampleEndUtc, ActualSampleSeconds, FrameIntervals.Num());
        return CompleteScenario();
    }

private:
    bool CompleteScenario()
    {
        FinishScenario();
        ++Scenario;
        if (Scenario < 4) return false;
        SaveReport();
        FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
        return true;
    }
    void StartScenario()
    {
        bSampling = false;
        bReady = false;
        BrowserFontReady = MakeShared<bool>(false);
        ReadinessSeconds = 0.0;
        ActualSampleSeconds = 0.0;
        SampleStartUtc.Reset();
        SampleEndUtc.Reset();
        FrameIntervals.Reset();
        FrameIntervals.Reserve(16384);
        Frame = 0;
        TSharedRef<SWidget> Content = BuildContent();
        Measured = SNew(SMeasuredWidget)[Content];
        Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi performance comparison")))
            .ClientSize(FVector2D(ViewWidth, ViewHeight)).UseOSWindowBorder(false).CreateTitleBar(false)
            .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
            .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
            .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[Measured.ToSharedRef()];
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        ScenarioStartTime = FPlatformTime::Seconds();
        UE_LOG(LogRmlUiPerformanceTest, Display, TEXT("RmlUiPerf scenario=%s phase=created utc=%s"),
            ScenarioName(Scenario), *FDateTime::UtcNow().ToIso8601());
    }

    TSharedRef<SWidget> BuildContent()
    {
        if (Scenario == 0)
        {
            // Slate font sizes use points at 96 DPI; HTML/RML font sizes use pixels.
            const TSharedPtr<const FCompositeFont> Typeface = MakeShared<FCompositeFont>(FName(TEXT("Regular")),
                FontPath(), EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
            const FSlateFontInfo RowFont(Typeface, 14.0f * 72.0f / FontConstants::RenderDPI);
            const FSlateFontInfo HeaderFont(Typeface, 20.0f * 72.0f / FontConstants::RenderDPI);
            const FLinearColor TextColor(FColor::FromHex(TEXT("EEF2F4")));
            const FLinearColor BodyColor(FColor::FromHex(TEXT("18232D")));
            const FLinearColor HeaderColor(FColor::FromHex(TEXT("176B59")));
            const FLinearColor LineColor(FColor::FromHex(TEXT("40515D")));
            const FLinearColor ValueColor(FColor::FromHex(TEXT("8FD3C2")));
            const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
            TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
            List->AddSlot().AutoHeight()
            [
                SNew(SBox).HeightOverride(48)
                [
                    SNew(SBorder).BorderImage(WhiteBrush).BorderBackgroundColor(HeaderColor)
                    .Padding(FMargin(14, 0)).VAlign(VAlign_Center)
                    [SAssignNew(NativeHeader, STextBlock).Font(HeaderFont).ColorAndOpacity(TextColor)
                        .Text(FText::FromString(TEXT("Frame 0")))]
                ]
            ];
            for (int32 Index = 0; Index < RowCount; ++Index)
            {
                List->AddSlot().AutoHeight()
                [
                    SNew(SBox).HeightOverride(29).Clipping(EWidgetClipping::ClipToBounds)
                    [
                        SNew(SBorder).BorderImage(WhiteBrush).BorderBackgroundColor(LineColor)
                        .Padding(FMargin(0, 0, 0, 1))
                        [
                            SNew(SBorder).BorderImage(WhiteBrush).BorderBackgroundColor(BodyColor)
                            .Padding(FMargin(12, 0))
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [SNew(SBox).WidthOverride(110)
                                    [SNew(STextBlock).Font(RowFont).ColorAndOpacity(TextColor)
                                        .Text(FText::FromString(FString::Printf(TEXT("Item %03d"), Index)))]]
                                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                                [SNew(STextBlock).Font(RowFont).ColorAndOpacity(TextColor)
                                    .Text(FText::FromString(TEXT("Shared benchmark payload")))]
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [SNew(SBox).WidthOverride(80)
                                    [SNew(STextBlock).Font(RowFont).ColorAndOpacity(ValueColor)
                                        .Text(FText::FromString(FString::FromInt(Index * 17)))]]
                            ]
                        ]
                    ]
                ];
            }
            return SNew(SBorder).BorderImage(WhiteBrush).BorderBackgroundColor(BodyColor)
                .Padding(0).Clipping(EWidgetClipping::ClipToBounds)[List];
        }
        if (Scenario == 1)
        {
            FModuleManager::LoadModuleChecked<IWebBrowserModule>(TEXT("WebBrowser"));
            const FString Directory = OutputDirectory();
            IFileManager::Get().MakeDirectory(*Directory, true);
            const FString HtmlPath = FPaths::ConvertRelativePathToFull(
                FPaths::Combine(Directory, TEXT("WebBrowserBenchmark.html")));
            Test->TestTrue(TEXT("Copy shared LatoLatin font for WebBrowser fixture"),
                IFileManager::Get().Copy(*FPaths::Combine(Directory, TEXT("LatoLatin-Regular.ttf")), *FontPath()) == COPY_OK);
            Test->TestTrue(TEXT("Save WebBrowser benchmark fixture"),
                FFileHelper::SaveStringToFile(HtmlDocument(), *HtmlPath));
            FString BrowserUrl = TEXT("file:///") + HtmlPath.Replace(TEXT("\\"), TEXT("/"));
            Browser = SNew(SWebBrowser).InitialURL(BrowserUrl).ShowControls(false).BrowserFrameRate(60)
                .OnTitleChanged_Lambda([Ready = BrowserFontReady](const FText& Title)
                {
                    *Ready = Title.ToString() == TEXT("RmlUiBenchmarkReady");
                });
            return Browser.ToSharedRef();
        }
        RmlWidget = SNew(SRmlUiWidget).InlineDocument(RmlDocument()).SourcePath(TEXT("F:///performance.rml"))
            .UseSlateRenderer(Scenario == 3).UsePaintCache(Scenario == 3)
            .DesiredSize(FVector2D(ViewWidth, ViewHeight))
            .Clipping(EWidgetClipping::ClipToBounds);
        FRmlUiPerformance::SetEnabled(true);
        if (Scenario == 3)
        {
            PaintCacheRoot = SNew(SInvalidationPanel)
                .DebugName(TEXT("RmlUi performance paint cache"))
                [
                    RmlWidget.ToSharedRef()
                ];
            PaintCacheRoot->SetCanCache(true);
            return PaintCacheRoot.ToSharedRef();
        }
        return RmlWidget.ToSharedRef();
    }

    void FinishScenario()
    {
        Measured->EndCapture();
        FlushRenderingCommands();
        // Capture counters before TakeScreenshot, which can itself traverse/render Slate.
        const FRmlUiPerformanceSnapshot Snapshot = RmlWidget.IsValid()
            ? FRmlUiPerformance::Snapshot() : FRmlUiPerformanceSnapshot{};
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        static const TCHAR* Names[] = {TEXT("Slate"), TEXT("WebBrowser"), TEXT("RmlUiDX11"), TEXT("RmlUiSlate")};
        Result->SetStringField(TEXT("name"), Names[Scenario]);
        Result->SetBoolField(TEXT("ready"), bReady);
        Result->SetNumberField(TEXT("readiness_seconds"), ReadinessSeconds);
        Result->SetNumberField(TEXT("actual_sample_seconds"), ActualSampleSeconds);
        Result->SetStringField(TEXT("sample_started_utc"), SampleStartUtc);
        Result->SetStringField(TEXT("sample_finished_utc"), SampleEndUtc);
        const FGeometry& Geometry = Measured->GetCachedGeometry();
        TSharedPtr<FJsonObject> GeometryJson = MakeShared<FJsonObject>();
        GeometryJson->SetNumberField(TEXT("local_width"), Geometry.GetLocalSize().X);
        GeometryJson->SetNumberField(TEXT("local_height"), Geometry.GetLocalSize().Y);
        GeometryJson->SetNumberField(TEXT("layout_scale"), Geometry.GetAccumulatedLayoutTransform().GetScale());
        GeometryJson->SetNumberField(TEXT("window_dpi_scale"), Window->GetDPIScaleFactor());
        GeometryJson->SetNumberField(TEXT("application_scale"), FSlateApplication::Get().GetApplicationScale());
        Result->SetObjectField(TEXT("geometry"), GeometryJson);
        {
            TArray<FColor> Pixels;
            FIntVector Size = FIntVector::ZeroValue;
            const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Measured.ToSharedRef(), Pixels, Size);
            int32 HeaderPixels = 0;
            int32 BodyPixels = 0;
            int32 HeaderTextPixels = 0;
            for (const FColor Pixel : Pixels)
            {
                const bool bHeader = Pixel.G > Pixel.R + 25 && Pixel.G > Pixel.B + 5;
                const bool bBody = Pixel.B > Pixel.R && Pixel.G > Pixel.R;
                if (bHeader) ++HeaderPixels;
                if (bBody) ++BodyPixels;
            }
            if (bCaptured && Size.X > 0 && Size.Y > 0)
            {
                // The changing title must actually be rendered, not only its green background.
                const int32 HeaderHeight = FMath::Min(Size.Y, FMath::RoundToInt(Size.Y * 48.0 / ViewHeight));
                const int32 HeaderWidth = FMath::Min(Size.X, FMath::RoundToInt(Size.X * 320.0 / ViewWidth));
                for (int32 Y = 0; Y < HeaderHeight; ++Y)
                    for (int32 X = 0; X < HeaderWidth; ++X)
                    {
                        const FColor Pixel = Pixels[Y * Size.X + X];
                        if (Pixel.R > 160 && Pixel.G > 160 && Pixel.B > 160) ++HeaderTextPixels;
                    }
            }
            const bool bHeaderTextVerified = HeaderTextPixels > 20;
            const bool bContentVerified = bCaptured && HeaderPixels > 1000 && BodyPixels > 1000 && bHeaderTextVerified;
            Result->SetBoolField(TEXT("content_pixels_verified"), bContentVerified);
            Result->SetBoolField(TEXT("header_text_pixels_verified"), bHeaderTextVerified);
            Result->SetNumberField(TEXT("header_text_pixels"), HeaderTextPixels);
            Result->SetNumberField(TEXT("screenshot_width"), Size.X);
            Result->SetNumberField(TEXT("screenshot_height"), Size.Y);
            if (bCaptured)
            {
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
                    TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                const FString Directory = OutputDirectory();
                IFileManager::Get().MakeDirectory(*Directory, true);
                const FString ScreenshotPath = FPaths::Combine(Directory,
                    FString(Names[Scenario]) + TEXT("Benchmark-") + RunLabel() + TEXT(".png"));
                Test->TestTrue(FString::Printf(TEXT("Save %s benchmark screenshot"), Names[Scenario]),
                    FFileHelper::SaveArrayToFile(Png, *ScreenshotPath));
                Result->SetStringField(TEXT("screenshot"), FPaths::ConvertRelativePathToFull(ScreenshotPath));
            }
            Test->TestTrue(FString::Printf(TEXT("%s rendered benchmark header and body pixels"), Names[Scenario]), bContentVerified);
        }
        Result->SetNumberField(TEXT("samples"), FrameIntervals.Num());
        Result->SetNumberField(TEXT("frame_p50_ms"), Percentile(FrameIntervals, 0.50));
        Result->SetNumberField(TEXT("frame_p95_ms"), Percentile(FrameIntervals, 0.95));
        Result->SetNumberField(TEXT("frame_p99_ms"), Percentile(FrameIntervals, 0.99));
        Result->SetNumberField(TEXT("widget_traversal_samples"), Measured->Samples.Num());
        Result->SetNumberField(TEXT("widget_traversal_p50_ms"), Percentile(Measured->Samples, 0.50));
        Result->SetNumberField(TEXT("widget_traversal_p95_ms"), Percentile(Measured->Samples, 0.95));
        Result->SetNumberField(TEXT("widget_traversal_p99_ms"), Percentile(Measured->Samples, 0.99));
        Result->SetNumberField(TEXT("process_used_physical_mib"),
            static_cast<double>(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0));
        if (RmlWidget.IsValid())
        {
            const ERmlUiPerformanceBackend Backend = Scenario == 3
                ? ERmlUiPerformanceBackend::Slate : ERmlUiPerformanceBackend::DX11;
            TSharedPtr<FJsonObject> Stages = MakeShared<FJsonObject>();
            Stages->SetObjectField(TEXT("render_frame"), StageJson(Snapshot, Backend, ERmlUiPerformanceStage::RenderFrame));
            Stages->SetObjectField(TEXT("bridge_render"), StageJson(Snapshot, Backend, ERmlUiPerformanceStage::BridgeRender));
            Stages->SetObjectField(TEXT("draw_decode"), StageJson(Snapshot, Backend, ERmlUiPerformanceStage::DrawDecode));
            Stages->SetObjectField(TEXT("upload_prepare"), StageJson(Snapshot, Backend, ERmlUiPerformanceStage::UploadPrepare));
            Stages->SetObjectField(TEXT("on_paint"), StageJson(Snapshot, Backend, ERmlUiPerformanceStage::OnPaint));
            Result->SetObjectField(TEXT("rmlui_stages"), Stages);
            Result->SetNumberField(TEXT("frames"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Frames));
            Result->SetNumberField(TEXT("draws"), Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Draws));
            Result->SetNumberField(TEXT("full_frame_upload_mib"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::FullFrameUploadBytes) / (1024.0 * 1024.0));
            TSharedPtr<FJsonObject> PaintCache = MakeShared<FJsonObject>();
            PaintCache->SetNumberField(TEXT("evaluations"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheEvaluations));
            PaintCache->SetNumberField(TEXT("eligible_frames"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheEligibleFrames));
            PaintCache->SetNumberField(TEXT("rejected_disabled"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedDisabled));
            PaintCache->SetNumberField(TEXT("rejected_clip_masks"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedClipMasks));
            PaintCache->SetNumberField(TEXT("rejected_legacy_translucent_texture"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheRejectedLegacyTranslucentTexture));
            PaintCache->SetNumberField(TEXT("resource_waits"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheResourceWaits));
            PaintCache->SetNumberField(TEXT("activations"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheActivations));
            PaintCache->SetNumberField(TEXT("invalidations"),
                Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::PaintCacheInvalidations));
            Result->SetObjectField(TEXT("paint_cache"), PaintCache);
            const bool bMetricsValid = Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Frames) > 10 &&
                Snapshot.CallCount(Backend, ERmlUiPerformanceStage::RenderFrame) > 10 &&
                Snapshot.CallCount(Backend, ERmlUiPerformanceStage::BridgeRender) > 10 &&
                Snapshot.Milliseconds(Backend, ERmlUiPerformanceStage::BridgeRender) > 0.0 &&
                (Scenario == 3 ? Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::Draws) > 0 :
                    Snapshot.WorkCount(Backend, ERmlUiPerformanceWork::FullFrameUploadBytes) > 0);
            Result->SetBoolField(TEXT("rmlui_metrics_verified"), bMetricsValid);
            Test->TestTrue(FString::Printf(TEXT("%s recorded nonzero frame, Bridge and backend work"), Names[Scenario]), bMetricsValid);
            Result->SetNumberField(TEXT("unsupported_slate_features"), RmlWidget->GetUnsupportedSlateFeatures());
        }
        Results.Add(MakeShared<FJsonValueObject>(Result));
        Test->TestTrue(FString::Printf(TEXT("%s produced frame samples"), Names[Scenario]), FrameIntervals.Num() > 10);
        Test->TestTrue(FString::Printf(TEXT("%s produced widget traversal samples"), Names[Scenario]), Measured->Samples.Num() > 10);
        if (RmlWidget.IsValid()) Test->TestTrue(TEXT("RmlUi benchmark document loaded"), RmlWidget->GetLastError().IsEmpty());
        FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Window.Reset(); Measured.Reset(); NativeHeader.Reset(); Browser.Reset(); PaintCacheRoot.Reset(); RmlWidget.Reset();
    }

    void SaveReport()
    {
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema_version"), 2);
        Root->SetStringField(TEXT("method"), FString::Printf(
            TEXT("1280x800 requested client pixels; 100 fixed 29px rows, three fields, 48px header; LatoLatin 14px/20px; clipping; per-UE-frame header update; readiness then %.3fs warmup + %.3fs sample"),
            WarmupSeconds, SampleSeconds));
        Root->SetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
        Root->SetNumberField(TEXT("sample_seconds"), SampleSeconds);
        Root->SetNumberField(TEXT("created_rows"), RowCount);
        Root->SetNumberField(TEXT("requested_client_width"), ViewWidth);
        Root->SetNumberField(TEXT("requested_client_height"), ViewHeight);
        Root->SetNumberField(TEXT("webbrowser_frame_rate"), 60);
        Root->SetStringField(TEXT("frame_metric"), TEXT("Elapsed wall time between automation updates in the UE host, not UI-only CPU time or browser presented-frame latency"));
        Root->SetStringField(TEXT("widget_traversal_metric"), TEXT("Outer SCompoundWidget::OnPaint including child traversal; child SWidget::Paint may execute Tick and OnPaint. This is not pure draw submission time. Browser work is asynchronous and CEF child CPU/GPU time is excluded."));
        Root->SetStringField(TEXT("workload_limitations"), TEXT("Same visible content and per-host-frame update requests; browser refresh capped at 60Hz and may coalesce updates. Native Slate and RmlUi process updates in the UE host. Font rasterizers and widget/DOM implementations differ. Results are steady-state Editor measurements, not input-to-photon latency or packaged-runtime totals."));
        Root->SetStringField(TEXT("readiness"), TEXT("Cached geometry; browser IsLoaded plus document.fonts.ready title acknowledgement; RmlUi native view plus a completed frame and no document error"));
        Root->SetStringField(TEXT("scope"), TEXT("UE host process; WebBrowser child-process CPU and memory excluded"));
        Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
        Root->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"));
        Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
        Root->SetStringField(TEXT("rhi_adapter"), GRHIAdapterName);
        Root->SetStringField(TEXT("primary_display_adapter"), FPlatformMisc::GetPrimaryGPUBrand());
        Root->SetStringField(TEXT("font"), FontPath());
        Root->SetArrayField(TEXT("scenarios"), Results);
        FString Json;
        FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
        const FString Directory = OutputDirectory();
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Path = FPaths::Combine(Directory, TEXT("RmlUiComparison-") + RunLabel() + TEXT(".json"));
        Test->TestTrue(TEXT("Save comparison report"), FFileHelper::SaveStringToFile(Json, *Path));
        Test->AddInfo(FString::Printf(TEXT("Performance report: %s"), *Path));
    }

    FAutomationTestBase* Test = nullptr;
    int32 Scenario = 0;
    int32 Frame = 0;
    bool bSampling = false;
    bool bReady = false;
    bool bPreviouslyEnabled = false;
    TSharedPtr<bool> BrowserFontReady;
    double WarmupSeconds = DefaultWarmupSeconds;
    double SampleSeconds = DefaultSampleSeconds;
    double ScenarioStartTime = 0.0;
    double WarmupStartTime = 0.0;
    double SampleStartTime = 0.0;
    double ReadinessSeconds = 0.0;
    double ActualSampleSeconds = 0.0;
    FString SampleStartUtc;
    FString SampleEndUtc;
    double LastSampleTime = 0.0;
    TArray<double> FrameIntervals;
    TArray<TSharedPtr<FJsonValue>> Results;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SMeasuredWidget> Measured;
    TSharedPtr<STextBlock> NativeHeader;
    TSharedPtr<SWebBrowser> Browser;
    TSharedPtr<SInvalidationPanel> PaintCacheRoot;
    TSharedPtr<SRmlUiWidget> RmlWidget;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiPerformanceComparisonTest, "RmlUiUnreal.Performance.Comparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiPerformanceComparisonTest::RunTest(const FString&)
{
    AddCommand(new RmlUiPerformanceTests::FComparisonCommand(this));
    return true;
}

#endif
