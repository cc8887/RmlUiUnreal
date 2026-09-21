#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "RmlUiAnimationRuntime.h"
#include "RmlUiBridge.h"
#include "RmlUiPerformance.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

namespace RmlUiAnimationPerformanceTests
{
constexpr int32 WarmupFrames = 5;
constexpr int32 SampleFrames = 60;
constexpr float FrameDeltaSeconds = 1.0f / 60.0f;
constexpr int32 TrackCounts[] = {100, 1000, 10000};

double CyclesToMilliseconds(uint64 Cycles)
{
    return FPlatformTime::ToMilliseconds64(Cycles);
}

double Percentile(TArray<double> Values, double Fraction)
{
    if (Values.IsEmpty()) return 0.0;
    Values.Sort();
    return Values[FMath::Clamp(
        FMath::CeilToInt((Values.Num() - 1) * Fraction), 0, Values.Num() - 1)];
}

void AddFrameDistribution(TSharedPtr<FJsonObject>& Result, const TCHAR* Prefix, const TArray<double>& Samples)
{
    Result->SetNumberField(FString(Prefix) + TEXT("_samples"), Samples.Num());
    Result->SetNumberField(FString(Prefix) + TEXT("_p50_ms"), Percentile(Samples, 0.50));
    Result->SetNumberField(FString(Prefix) + TEXT("_p95_ms"), Percentile(Samples, 0.95));
    Result->SetNumberField(FString(Prefix) + TEXT("_p99_ms"), Percentile(Samples, 0.99));
}

TSharedPtr<FJsonObject> StageJson(
    const FRmlUiPerformanceSnapshot& Snapshot,
    ERmlUiPerformanceStage Stage)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    const ERmlUiPerformanceBackend Backend = ERmlUiPerformanceBackend::Unattributed;
    const uint64 Calls = Snapshot.CallCount(Backend, Stage);
    const double TotalMilliseconds = Snapshot.Milliseconds(Backend, Stage);
    Result->SetNumberField(TEXT("calls"), static_cast<double>(Calls));
    Result->SetNumberField(TEXT("total_ms"), TotalMilliseconds);
    Result->SetNumberField(TEXT("mean_ms"), Calls ? TotalMilliseconds / Calls : 0.0);
    return Result;
}

void AddCommitBreakdown(TSharedPtr<FJsonObject>& Result, const TCHAR* Prefix,
    const FRmlUiPerformanceSnapshot& Snapshot)
{
    const FString Name(Prefix);
    Result->SetObjectField(Name + TEXT("_commit"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommit));
    Result->SetObjectField(Name + TEXT("_commit_prepare"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitPrepare));
    Result->SetObjectField(Name + TEXT("_commit_validate"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitValidate));
    Result->SetObjectField(Name + TEXT("_commit_transform_prepare"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitTransformPrepare));
    Result->SetObjectField(Name + TEXT("_commit_apply"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitApply));
    Result->SetObjectField(Name + TEXT("_commit_synchronize"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitSynchronize));
    Result->SetObjectField(Name + TEXT("_commit_publish"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitPublish));
    Result->SetObjectField(Name + TEXT("_commit_fallback"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitFallback));
    Result->SetObjectField(Name + TEXT("_commit_postprocess"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommitPostprocess));
}

void AddScheduleBreakdown(TSharedPtr<FJsonObject>& Result, const TCHAR* Prefix,
    const FRmlUiPerformanceSnapshot& Snapshot)
{
    const FString Name(Prefix);
    Result->SetObjectField(Name + TEXT("_schedule"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationSchedule));
    Result->SetObjectField(Name + TEXT("_schedule_heap_drain"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationScheduleHeapDrain));
    Result->SetObjectField(Name + TEXT("_schedule_group_refresh"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationScheduleGroupRefresh));
    Result->SetObjectField(Name + TEXT("_schedule_tag_mutation"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationScheduleTagMutation));
    Result->SetObjectField(Name + TEXT("_schedule_maintenance"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationScheduleMaintenance));
}

void AddFinalizeBreakdown(TSharedPtr<FJsonObject>& Result, const TCHAR* Prefix,
    const FRmlUiPerformanceSnapshot& Snapshot)
{
    const FString Name(Prefix);
    Result->SetObjectField(Name + TEXT("_finalize"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalize));
    Result->SetObjectField(Name + TEXT("_finalize_prepare"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizePrepare));
    Result->SetObjectField(Name + TEXT("_finalize_remove"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeRemove));
    Result->SetObjectField(Name + TEXT("_finalize_detach"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeDetach));
    Result->SetObjectField(Name + TEXT("_finalize_entity_release"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeEntityRelease));
    Result->SetObjectField(Name + TEXT("_finalize_entity_mark"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeEntityMark));
    Result->SetObjectField(Name + TEXT("_finalize_entity_free"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeEntityFree));
    Result->SetObjectField(Name + TEXT("_finalize_callbacks"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalizeCallbacks));
}

TSharedPtr<FJsonObject> RunDenseBaseline(int32 TrackCount)
{
    struct FDenseTrack
    {
        float From = 0.2f;
        float To = 0.8f;
        float Previous = 0.0f;
        bool bPreviousValid = false;
    };

    TArray<FDenseTrack> Tracks;
    Tracks.SetNum(TrackCount);
    double TimeSeconds = 0.0;
    double Checksum = 0.0;
    uint64 ChangedProperties = 0;
    TArray<double> FrameSamples;
    FrameSamples.Reserve(SampleFrames);
    const uint64 StartCycles = FPlatformTime::Cycles64();
    for (int32 Frame = 0; Frame < SampleFrames; ++Frame)
    {
        const uint64 FrameStart = FPlatformTime::Cycles64();
        TimeSeconds += FrameDeltaSeconds;
        const float Alpha = static_cast<float>(TimeSeconds / 100.0);
        for (FDenseTrack& Track : Tracks)
        {
            const float Value = FMath::Lerp(Track.From, Track.To, Alpha);
            if (!Track.bPreviousValid || !FMath::IsNearlyEqual(Track.Previous, Value))
            {
                ++ChangedProperties;
                Track.Previous = Value;
                Track.bPreviousValid = true;
            }
            Checksum += Value;
        }
        FrameSamples.Add(CyclesToMilliseconds(FPlatformTime::Cycles64() - FrameStart));
    }
    const double TotalMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - StartCycles);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("total_ms"), TotalMilliseconds);
    Result->SetNumberField(TEXT("mean_frame_ms"), TotalMilliseconds / SampleFrames);
    Result->SetNumberField(TEXT("changed_properties"), static_cast<double>(ChangedProperties));
    Result->SetNumberField(TEXT("checksum"), Checksum);
    AddFrameDistribution(Result, TEXT("frame"), FrameSamples);
    Result->SetStringField(TEXT("scope"),
        TEXT("single-threaded contiguous-array linear sample plus changed-value detection; no entity management, callbacks or RmlUi commit"));
    return Result;
}

TSharedPtr<FJsonObject> RunMovieSceneScenario(
    FAutomationTestBase* Test, int32 TrackCount, bool bKeyframes = false)
{
    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDefinition Definition;
    Definition.From = 0.2f;
    Definition.To = 0.8f;
    Definition.DurationSeconds = 100.0;
    if (bKeyframes)
    {
        FRmlUiAnimationEasing EaseInOut;
        EaseInOut.Type = ERmlUiAnimationEasingType::CubicBezier;
        EaseInOut.X1 = 0.42f;
        EaseInOut.Y1 = 0.0f;
        EaseInOut.X2 = 0.58f;
        EaseInOut.Y2 = 1.0f;
        Definition.Keyframes = {
            {0.0f, 0.2f, EaseInOut}, {0.25f, 0.8f, {}}, {0.5f, 0.3f, {}},
            {0.75f, 0.7f, {}}, {1.0f, 0.4f, {}}};
    }
    const FRmlUiAnimationDefinitionHandle DefinitionHandle =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Definition);
    Test->TestTrue(TEXT("callback benchmark definition registered"), DefinitionHandle.IsValid());

    TArray<FRmlUiAnimationBindingHandle> Bindings;
    Bindings.Reserve(TrackCount);
    const uint64 BindStart = FPlatformTime::Cycles64();
    for (int32 Index = 0; Index < TrackCount; ++Index)
    {
        Bindings.Add(Runtime.BindCallback(DefinitionHandle));
    }
    const double BindMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - BindStart);

    TArray<FRmlUiAnimationHandle> Handles;
    const uint64 PlayStart = FPlatformTime::Cycles64();
    const int32 Played = Runtime.PlayBindings(Bindings, Handles);
    const double PlayMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - PlayStart);
    Test->TestEqual(TEXT("callback benchmark batch played every binding"), Played, TrackCount);

    for (int32 Frame = 0; Frame < WarmupFrames; ++Frame)
    {
        Runtime.Advance(FrameDeltaSeconds);
    }
    FRmlUiPerformance::Reset();
    TArray<double> FrameSamples;
    FrameSamples.Reserve(SampleFrames);
    const uint64 SampleStart = FPlatformTime::Cycles64();
    for (int32 Frame = 0; Frame < SampleFrames; ++Frame)
    {
        const uint64 FrameStart = FPlatformTime::Cycles64();
        Runtime.Advance(FrameDeltaSeconds);
        FrameSamples.Add(CyclesToMilliseconds(FPlatformTime::Cycles64() - FrameStart));
    }
    const double SampleMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - SampleStart);
    const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
    const uint64 ExpectedWork = static_cast<uint64>(TrackCount) * SampleFrames;
    Test->TestEqual(TEXT("callback benchmark evaluated expected tracks"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated), ExpectedWork);
    Test->TestEqual(TEXT("callback benchmark collected expected changes"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationChangedProperties), ExpectedWork);
    Test->TestEqual(TEXT("callback benchmark remains active"), Runtime.GetActiveAnimationCount(), TrackCount);

    Runtime.CancelAll();
    for (FRmlUiAnimationBindingHandle Binding : Bindings)
    {
        Runtime.ReleaseBinding(Binding);
    }
    Test->TestTrue(TEXT("callback benchmark definition released"), Runtime.ReleaseDefinition(DefinitionHandle));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("tracks"), TrackCount);
    Result->SetNumberField(TEXT("bind_total_ms"), BindMilliseconds);
    Result->SetNumberField(TEXT("batch_play_total_ms"), PlayMilliseconds);
    Result->SetNumberField(TEXT("sample_total_ms"), SampleMilliseconds);
    Result->SetNumberField(TEXT("sample_mean_frame_ms"), SampleMilliseconds / SampleFrames);
    AddFrameDistribution(Result, TEXT("sample_frame"), FrameSamples);
    Result->SetObjectField(TEXT("advance"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationAdvance));
    Result->SetObjectField(TEXT("evaluate"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("collect"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCollect));
    Result->SetObjectField(TEXT("commit"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommit));
    Result->SetObjectField(TEXT("finalize"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalize));
    Result->SetNumberField(TEXT("evaluated_tracks"), static_cast<double>(ExpectedWork));
    Result->SetNumberField(TEXT("changed_properties"), static_cast<double>(ExpectedWork));
    Result->SetStringField(TEXT("scope"), bKeyframes
        ? TEXT("MovieScene ECS five-keyframe sample with precomputed cubic-bezier LUT, segment cursor, compose and result collection; one immutable definition is shared by every callback binding; no value callbacks and no RmlUi property commit")
        : TEXT("MovieScene ECS two-endpoint sample/compose and result collection with shared callback bindings; no value callbacks and no RmlUi property commit"));
    return Result;
}

FString NativeMarkup(int32 TrackCount, bool bTransform, bool bOverlappingTransforms = false)
{
    FString Nodes;
    Nodes.Reserve(TrackCount * 24);
    const int32 FirstChild = bOverlappingTransforms ? 1 : 0;
    for (int32 Index = FirstChild; Index < TrackCount; ++Index)
    {
        Nodes += FString::Printf(TEXT("<div id='n%d'/>"), Index);
    }
    if (bOverlappingTransforms)
    {
        const TCHAR* Style = TEXT("body{margin:0;}#n0{position:absolute;left:0;top:0;width:1px;height:1px;transform-origin:0px 0px;transform:translate(0px,0px);}" \
            "#n0>div{position:absolute;left:0;top:0;width:1px;height:1px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}");
        return FString::Printf(TEXT("<rml><head><style>%s</style></head><body><div id='n0'>%s</div></body></rml>"), Style, *Nodes);
    }
    const TCHAR* Style = bTransform
        ? TEXT("body{margin:0;}div{position:absolute;left:0;top:0;width:1px;height:1px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}")
        : TEXT("body{margin:0;}div{display:block;opacity:1;width:1px;height:1px;background:#fff;}");
    return FString::Printf(TEXT("<rml><head><style>%s</style></head><body>%s</body></rml>"), Style, *Nodes);
}

TSharedPtr<FJsonObject> RunNativeScenario(
    FAutomationTestBase* Test, int32 TrackCount, bool bTransform, bool bOverlappingTransforms = false)
{
    RmlUE_View* View = RmlUE_CreateSlateView(512, 512, 1.0f);
    Test->TestNotNull(TEXT("native benchmark view created"), View);
    if (!View) return MakeShared<FJsonObject>();

    const FString Markup = NativeMarkup(TrackCount, bTransform, bOverlappingTransforms);
    Test->TestTrue(TEXT("native benchmark document loaded"),
        RmlUE_LoadDocumentFromMemory(View, TCHAR_TO_UTF8(*Markup), "animation-performance.rml") != 0);
    Test->TestTrue(TEXT("native benchmark initial update completed"), RmlUE_Update(View) != 0);

    TArray<uint32> Nodes;
    Nodes.Reserve(TrackCount);
    for (int32 Index = 0; Index < TrackCount; ++Index)
    {
        const FTCHARToUTF8 Id(*FString::Printf(TEXT("n%d"), Index));
        Nodes.Add(RmlUE_FindNode(View, Id.Get()));
    }

    RmlUE_NodeMetrics BaselineMetrics{};
    RmlUE_LayoutInfo BaselineLayout{};
    if (bTransform)
    {
        Test->TestTrue(TEXT("native transform benchmark baseline geometry readable"),
            RmlUE_MeasureNodes(View, Nodes.GetData(), 1, &BaselineMetrics, &BaselineLayout) != 0 &&
            BaselineMetrics.Valid != 0);
    }
    RmlUE_SlateFrame BaselineFrame{};
    Test->TestTrue(TEXT("native benchmark retained baseline recorded"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);

    FRmlUiAnimationRuntime Runtime;
    FRmlUiAnimationDefinitionHandle DefinitionHandle;
    float InitialValue = 0.2f;
    if (bTransform)
    {
        FRmlUiTransform2DAnimationDefinition Definition;
        Definition.To.TranslationX = 100.0f;
        Definition.To.TranslationY = 50.0f;
        Definition.DurationSeconds = 100.0;
        DefinitionHandle = Runtime.RegisterTransform2DDefinition(Definition);
        InitialValue = Definition.From.TranslationX;
    }
    else
    {
        FRmlUiFloatAnimationDefinition Definition;
        Definition.From = 0.2f;
        Definition.To = 0.8f;
        Definition.DurationSeconds = 100.0;
        DefinitionHandle = Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Definition);
    }
    Test->TestTrue(TEXT("native benchmark definition registered"), DefinitionHandle.IsValid());
    TArray<FRmlUiAnimationBindingHandle> Bindings;
    const uint64 BindStart = FPlatformTime::Cycles64();
    const int32 Bound = Runtime.BindNodes(DefinitionHandle, View, Nodes, Bindings);
    const double BindMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - BindStart);
    Test->TestEqual(TEXT("native benchmark bound every node"), Bound, TrackCount);

    TArray<FRmlUiAnimationHandle> Handles;
    const uint64 PlayStart = FPlatformTime::Cycles64();
    const int32 Played = Runtime.PlayBindings(Bindings, Handles);
    const double PlayMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - PlayStart);
    Test->TestEqual(TEXT("native benchmark played every binding"), Played, TrackCount);

    for (int32 Frame = 0; Frame < WarmupFrames; ++Frame)
    {
        Runtime.Advance(FrameDeltaSeconds);
        RmlUE_Update(View);
        RmlUE_SlateFrame SlateFrame{};
        RmlUE_RenderSlate(View, &SlateFrame);
    }
    FRmlUiPerformance::Reset();
    uint64 AdvanceCycles = 0;
    uint64 UpdateCycles = 0;
    uint64 RenderSlateCycles = 0;
    TArray<double> AdvanceSamples;
    TArray<double> UpdateSamples;
    TArray<double> RenderSlateSamples;
    AdvanceSamples.Reserve(SampleFrames);
    UpdateSamples.Reserve(SampleFrames);
    RenderSlateSamples.Reserve(SampleFrames);
    uint64 VisualDraws = 0;
    bool bAllSlateFramesRendered = true;
    for (int32 Frame = 0; Frame < SampleFrames; ++Frame)
    {
        uint64 Start = FPlatformTime::Cycles64();
        Runtime.Advance(FrameDeltaSeconds);
        const uint64 AdvanceFrameCycles = FPlatformTime::Cycles64() - Start;
        AdvanceCycles += AdvanceFrameCycles;
        AdvanceSamples.Add(CyclesToMilliseconds(AdvanceFrameCycles));
        Start = FPlatformTime::Cycles64();
        RmlUE_Update(View);
        const uint64 UpdateFrameCycles = FPlatformTime::Cycles64() - Start;
        UpdateCycles += UpdateFrameCycles;
        UpdateSamples.Add(CyclesToMilliseconds(UpdateFrameCycles));
        RmlUE_SlateFrame SlateFrame{};
        Start = FPlatformTime::Cycles64();
        const bool bRendered = RmlUE_RenderSlate(View, &SlateFrame) != 0;
        const uint64 RenderSlateFrameCycles = FPlatformTime::Cycles64() - Start;
        bAllSlateFramesRendered &= bRendered;
        RenderSlateCycles += RenderSlateFrameCycles;
        RenderSlateSamples.Add(CyclesToMilliseconds(RenderSlateFrameCycles));
        for (uint32 DrawIndex = 0; DrawIndex < SlateFrame.DrawCount; ++DrawIndex)
            VisualDraws += SlateFrame.Draws[DrawIndex].VisualNode != 0 ? 1 : 0;
    }
    const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
    const uint64 ExpectedWork = static_cast<uint64>(TrackCount) * SampleFrames;
    Test->TestEqual(TEXT("native benchmark committed expected properties"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCommittedProperties), ExpectedWork);

    if (bTransform)
    {
        RmlUE_NodeMetrics MovedMetrics{};
        RmlUE_LayoutInfo MovedLayout{};
        Test->TestTrue(TEXT("native transform benchmark moved geometry readable"),
            RmlUE_MeasureNodes(View, Nodes.GetData(), 1, &MovedMetrics, &MovedLayout) != 0 &&
            MovedMetrics.Valid != 0);
        Test->TestTrue(TEXT("native transform benchmark changes visual bounds only"),
            MovedMetrics.X > BaselineMetrics.X + InitialValue &&
            FMath::IsNearlyEqual(MovedMetrics.LayoutX, BaselineMetrics.LayoutX));
    }
    else
    {
        char Opacity[64]{};
        Test->TestTrue(TEXT("native benchmark computed opacity readable"),
            RmlUE_GetComputedProperty(View, Nodes[0], "opacity", Opacity, sizeof(Opacity)) != 0);
        Test->TestTrue(TEXT("native benchmark opacity changed"),
            FCString::Atof(UTF8_TO_TCHAR(Opacity)) > InitialValue);
    }
    Test->TestTrue(TEXT("native benchmark Slate command frames rendered"), bAllSlateFramesRendered);
    Test->TestTrue(TEXT("native benchmark emitted visual draw commands"), VisualDraws > 0);
    RmlUE_SlateReplayStats ReplayStats{};
    RmlUE_GetSlateReplayStats(View, &ReplayStats);
    Test->TestEqual(TEXT("native benchmark records topology once"), ReplayStats.FullRenderFrames, uint64(1));
    Test->TestTrue(TEXT("native benchmark replays every sampled visual-only frame"),
        ReplayStats.ReplayedFrames >= static_cast<uint64>(SampleFrames));

    Runtime.CancelViewAnimations(View);
    Test->TestTrue(TEXT("native benchmark definition released"), Runtime.ReleaseDefinition(DefinitionHandle));
    RmlUE_DestroyView(View);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("tracks"), TrackCount);
    Result->SetNumberField(TEXT("bind_total_ms"), BindMilliseconds);
    Result->SetNumberField(TEXT("batch_play_total_ms"), PlayMilliseconds);
    Result->SetNumberField(TEXT("advance_total_ms"), CyclesToMilliseconds(AdvanceCycles));
    Result->SetNumberField(TEXT("advance_mean_frame_ms"), CyclesToMilliseconds(AdvanceCycles) / SampleFrames);
    Result->SetNumberField(TEXT("rmlui_update_total_ms"), CyclesToMilliseconds(UpdateCycles));
    Result->SetNumberField(TEXT("rmlui_update_mean_frame_ms"), CyclesToMilliseconds(UpdateCycles) / SampleFrames);
    Result->SetNumberField(TEXT("render_slate_total_ms"), CyclesToMilliseconds(RenderSlateCycles));
    Result->SetNumberField(TEXT("render_slate_mean_frame_ms"), CyclesToMilliseconds(RenderSlateCycles) / SampleFrames);
    Result->SetNumberField(TEXT("visual_draws"), static_cast<double>(VisualDraws));
    Result->SetNumberField(TEXT("full_render_frames"), static_cast<double>(ReplayStats.FullRenderFrames));
    Result->SetNumberField(TEXT("replayed_frames"), static_cast<double>(ReplayStats.ReplayedFrames));
    AddFrameDistribution(Result, TEXT("advance_frame"), AdvanceSamples);
    AddFrameDistribution(Result, TEXT("rmlui_update_frame"), UpdateSamples);
    AddFrameDistribution(Result, TEXT("render_slate_frame"), RenderSlateSamples);
    Result->SetObjectField(TEXT("evaluate"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("collect"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCollect));
    Result->SetObjectField(TEXT("commit"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommit));
    Result->SetObjectField(TEXT("finalize"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalize));
    Result->SetNumberField(TEXT("committed_properties"), static_cast<double>(ExpectedWork));
    Result->SetStringField(TEXT("scope"), bOverlappingTransforms
        ? TEXT("MovieScene ECS plus one parent and child Transform2D targets in the same visual-property batch; total targets equal tracks, and property-tree synchronization and draw publication select topmost dirty roots; one explicit idle RmlUi context update and retained RmlUE_RenderSlate replay run per sampled frame; Slate widget decode/paint, render thread, GPU and present are excluded")
        : bTransform
        ? TEXT("MovieScene ECS plus a leaf Transform2D visual-property batch, one explicit idle RmlUi context update, and one retained RmlUE_RenderSlate replay per sampled frame; one baseline frame records topology before playback; Slate widget decode/paint, render thread, GPU and present are excluded")
        : TEXT("MovieScene ECS plus one opacity visual-property batch, one explicit idle RmlUi context update, and one retained RmlUE_RenderSlate replay per sampled frame; one baseline frame records topology before playback; Slate widget decode/paint, render thread, GPU and present are excluded"));
    return Result;
}

TSharedPtr<FJsonObject> RunLayeredContributionScenario(
    FAutomationTestBase* Test, int32 TargetCount, int32 LayerCount)
{
    const int32 EntityCount = TargetCount * LayerCount;
    SIZE_T MemoryBefore = 0;
    FPlatformProcess::GetApplicationMemoryUsage(
        FPlatformProcess::GetCurrentProcessId(), &MemoryBefore);

    RmlUE_View* View = RmlUE_CreateSlateView(512, 512, 1.0f);
    Test->TestNotNull(TEXT("layered benchmark view created"), View);
    if (!View) return MakeShared<FJsonObject>();
    const FString Markup = NativeMarkup(TargetCount, false);
    Test->TestTrue(TEXT("layered benchmark document loaded"),
        RmlUE_LoadDocumentFromMemory(View, TCHAR_TO_UTF8(*Markup), "animation-layered-performance.rml") != 0);
    Test->TestTrue(TEXT("layered benchmark initial update completed"), RmlUE_Update(View) != 0);
    RmlUE_SlateFrame BaselineFrame{};
    Test->TestTrue(TEXT("layered benchmark retained baseline recorded"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);

    TArray<uint32> Nodes;
    Nodes.Reserve(TargetCount);
    for (int32 TargetIndex = 0; TargetIndex < TargetCount; ++TargetIndex)
    {
        const FTCHARToUTF8 Id(*FString::Printf(TEXT("n%d"), TargetIndex));
        const uint32 Node = RmlUE_FindNode(View, Id.Get());
        Test->TestTrue(TEXT("layered benchmark target found"), Node != 0);
        Nodes.Add(Node);
    }

    FRmlUiAnimationRuntime Runtime;
    TArray<FRmlUiAnimationDefinitionHandle> Definitions;
    TArray<FRmlUiAnimationBindingHandle> Bindings;
    TArray<FRmlUiAnimationContributionSpec> Contributions;
    Definitions.Reserve(LayerCount);
    Bindings.Reserve(EntityCount);
    Contributions.Reserve(EntityCount);
    const uint64 BindStart = FPlatformTime::Cycles64();
    for (int32 LayerIndex = 0; LayerIndex < LayerCount; ++LayerIndex)
    {
        FRmlUiFloatAnimationDefinition Definition;
        Definition.From = 0.1f + 0.1f * LayerIndex;
        Definition.To = 0.5f + 0.1f * LayerIndex;
        Definition.DurationSeconds = 100.0;
        const FRmlUiAnimationDefinitionHandle DefinitionHandle =
            Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Definition);
        Test->TestTrue(TEXT("layered benchmark definition registered"), DefinitionHandle.IsValid());
        Definitions.Add(DefinitionHandle);
        TArray<FRmlUiAnimationBindingHandle> LayerBindings;
        Test->TestEqual(TEXT("layered benchmark bound every target"),
            Runtime.BindNodes(DefinitionHandle, View, Nodes, LayerBindings), TargetCount);
        for (FRmlUiAnimationBindingHandle Binding : LayerBindings)
        {
            Bindings.Add(Binding);
            Contributions.Add({LayerIndex, true, true});
        }
    }
    const double BindMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - BindStart);

    TArray<FRmlUiAnimationHandle> Handles;
    const uint64 PlayStart = FPlatformTime::Cycles64();
    const int32 Played = LayerCount == 1
        ? Runtime.PlayBindings(Bindings, Handles)
        : Runtime.PlayContributionBindings(Bindings, Contributions, Handles);
    const double PlayMilliseconds = CyclesToMilliseconds(FPlatformTime::Cycles64() - PlayStart);
    Test->TestEqual(TEXT("layered benchmark played every entity"), Played, EntityCount);
    SIZE_T MemoryAfterSetup = 0;
    FPlatformProcess::GetApplicationMemoryUsage(
        FPlatformProcess::GetCurrentProcessId(), &MemoryAfterSetup);

    for (int32 Frame = 0; Frame < WarmupFrames; ++Frame)
        Runtime.Advance(FrameDeltaSeconds);
    FRmlUiPerformance::Reset();
    TArray<double> FrameSamples;
    FrameSamples.Reserve(SampleFrames);
    for (int32 Frame = 0; Frame < SampleFrames; ++Frame)
    {
        const uint64 FrameStart = FPlatformTime::Cycles64();
        Runtime.Advance(FrameDeltaSeconds);
        FrameSamples.Add(CyclesToMilliseconds(FPlatformTime::Cycles64() - FrameStart));
    }
    SIZE_T MemoryAfterSamples = 0;
    FPlatformProcess::GetApplicationMemoryUsage(
        FPlatformProcess::GetCurrentProcessId(), &MemoryAfterSamples);
    const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
    const uint64 ExpectedEvaluated = static_cast<uint64>(
        LayerCount == 1 ? EntityCount : TargetCount) * SampleFrames;
    const uint64 ExpectedCommitted = static_cast<uint64>(TargetCount) * SampleFrames;
    Test->TestEqual(TEXT("layered benchmark evaluates only ordinary tracks or layered winners"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated), ExpectedEvaluated);
    Test->TestEqual(TEXT("layered benchmark committed only target winners"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCommittedProperties), ExpectedCommitted);
    Test->TestEqual(TEXT("layered benchmark keeps every entity active"),
        Runtime.GetActiveAnimationCount(), EntityCount);
    Test->TestEqual(TEXT("stable layered benchmark refreshes no group after warmup"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed), uint64{0});
    Test->TestEqual(TEXT("stable layered benchmark scans no layered record after warmup"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned), uint64{0});
    Test->TestEqual(TEXT("stable layered benchmark crosses no contribution boundary"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredBoundaryWakes), uint64{0});

    Runtime.CancelViewAnimations(View);
    for (FRmlUiAnimationDefinitionHandle Definition : Definitions)
        Test->TestTrue(TEXT("layered benchmark definition released"), Runtime.ReleaseDefinition(Definition));
    RmlUE_DestroyView(View);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("targets"), TargetCount);
    Result->SetNumberField(TEXT("layers"), LayerCount);
    Result->SetNumberField(TEXT("entities"), EntityCount);
    Result->SetStringField(TEXT("path"), LayerCount == 1 ? TEXT("ordinary-replace") : TEXT("layered-replace"));
    Result->SetNumberField(TEXT("bind_total_ms"), BindMilliseconds);
    Result->SetNumberField(TEXT("batch_play_total_ms"), PlayMilliseconds);
    AddFrameDistribution(Result, TEXT("advance_frame"), FrameSamples);
    Result->SetObjectField(TEXT("advance"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationAdvance));
    Result->SetObjectField(TEXT("evaluate"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("collect"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCollect));
    Result->SetObjectField(TEXT("commit"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommit));
    Result->SetObjectField(TEXT("finalize"), StageJson(Snapshot, ERmlUiPerformanceStage::AnimationFinalize));
    Result->SetNumberField(TEXT("evaluated_tracks"), static_cast<double>(ExpectedEvaluated));
    Result->SetNumberField(TEXT("committed_properties"), static_cast<double>(ExpectedCommitted));
    Result->SetNumberField(TEXT("layered_groups_refreshed"), static_cast<double>(
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed)));
    Result->SetNumberField(TEXT("layered_records_scanned"), static_cast<double>(
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned)));
    Result->SetNumberField(TEXT("layered_boundary_wakes"), static_cast<double>(
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredBoundaryWakes)));
    Result->SetNumberField(TEXT("process_memory_before_bytes"), static_cast<double>(MemoryBefore));
    Result->SetNumberField(TEXT("process_memory_after_setup_bytes"), static_cast<double>(MemoryAfterSetup));
    Result->SetNumberField(TEXT("process_memory_after_samples_bytes"), static_cast<double>(MemoryAfterSamples));
    Result->SetNumberField(TEXT("process_memory_setup_delta_bytes"),
        static_cast<double>(MemoryAfterSetup >= MemoryBefore ? MemoryAfterSetup - MemoryBefore : 0));
    Result->SetStringField(TEXT("scope"),
        TEXT("Editor NullRHI MovieScene ECS opacity benchmark with a real RmlUi Slate view and visual commit sink. One layer uses ordinary replace; two and four layers use same-target ordered contributions. Setup memory is process working-set delta and remains allocator/order sensitive. Slate widget paint, render thread, GPU and present are excluded."));
    return Result;
}

TSharedPtr<FJsonObject> RunLayeredBoundaryBurstScenario(
    FAutomationTestBase* Test, int32 TargetCount)
{
    constexpr double BoundaryDelaySeconds = 10.0;
    RmlUE_View* View = RmlUE_CreateSlateView(512, 512, 1.0f);
    Test->TestNotNull(TEXT("boundary burst benchmark view created"), View);
    if (!View) return MakeShared<FJsonObject>();

    const FString Markup = NativeMarkup(TargetCount, false);
    Test->TestTrue(TEXT("boundary burst benchmark document loaded"),
        RmlUE_LoadDocumentFromMemory(View, TCHAR_TO_UTF8(*Markup),
            "animation-layered-boundary-burst.rml") != 0);
    Test->TestTrue(TEXT("boundary burst benchmark initial update completed"),
        RmlUE_Update(View) != 0);
    RmlUE_SlateFrame BaselineFrame{};
    Test->TestTrue(TEXT("boundary burst benchmark retained baseline recorded"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);

    TArray<uint32> Nodes;
    Nodes.Reserve(TargetCount);
    for (int32 TargetIndex = 0; TargetIndex < TargetCount; ++TargetIndex)
    {
        const FTCHARToUTF8 Id(*FString::Printf(TEXT("n%d"), TargetIndex));
        const uint32 Node = RmlUE_FindNode(View, Id.Get());
        Test->TestTrue(TEXT("boundary burst benchmark target found"), Node != 0);
        Nodes.Add(Node);
    }

    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDefinition Lower;
    Lower.From = 0.1f;
    Lower.To = 0.5f;
    Lower.DurationSeconds = 100.0;
    FRmlUiFloatAnimationDefinition Upper;
    Upper.From = 0.5f;
    Upper.To = 0.9f;
    Upper.DelaySeconds = BoundaryDelaySeconds;
    Upper.DurationSeconds = 100.0;
    const FRmlUiAnimationDefinitionHandle LowerDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Lower);
    const FRmlUiAnimationDefinitionHandle UpperDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Upper);
    Test->TestTrue(TEXT("boundary burst benchmark definitions registered"),
        LowerDefinition.IsValid() && UpperDefinition.IsValid());

    TArray<FRmlUiAnimationBindingHandle> LowerBindings;
    TArray<FRmlUiAnimationBindingHandle> UpperBindings;
    Test->TestEqual(TEXT("boundary burst benchmark bound every lower target"),
        Runtime.BindNodes(LowerDefinition, View, Nodes, LowerBindings), TargetCount);
    Test->TestEqual(TEXT("boundary burst benchmark bound every upper target"),
        Runtime.BindNodes(UpperDefinition, View, Nodes, UpperBindings), TargetCount);
    TArray<FRmlUiAnimationBindingHandle> Bindings = LowerBindings;
    Bindings.Append(UpperBindings);
    TArray<FRmlUiAnimationContributionSpec> Contributions;
    Contributions.Reserve(TargetCount * 2);
    for (int32 Index = 0; Index < TargetCount; ++Index) Contributions.Add({0, true, true});
    for (int32 Index = 0; Index < TargetCount; ++Index) Contributions.Add({1, true, true});

    TArray<FRmlUiAnimationHandle> Handles;
    Test->TestEqual(TEXT("boundary burst benchmark played every contribution"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), TargetCount * 2);
    for (int32 Index = 0; Index < TargetCount; ++Index)
    {
        Test->TestTrue(TEXT("boundary burst benchmark paused lower contribution"),
            Runtime.Pause(Handles[Index]));
    }
    Runtime.Advance(0.0f);
    Test->TestFalse(TEXT("boundary burst benchmark sleeps before shared delay"),
        Runtime.HasActiveAnimations(View));

    FRmlUiPerformance::Reset();
    TArray<double> SleepFrameSamples;
    SleepFrameSamples.Reserve(SampleFrames);
    for (int32 Frame = 0; Frame < SampleFrames; ++Frame)
    {
        const uint64 FrameStart = FPlatformTime::Cycles64();
        Runtime.Advance(FrameDeltaSeconds);
        SleepFrameSamples.Add(CyclesToMilliseconds(FPlatformTime::Cycles64() - FrameStart));
    }
    const FRmlUiPerformanceSnapshot SleepSnapshot = FRmlUiPerformance::Snapshot();
    Test->TestEqual(TEXT("sleeping burst benchmark refreshes no group"),
        SleepSnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed), uint64{0});
    Test->TestEqual(TEXT("sleeping burst benchmark scans no record"),
        SleepSnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned), uint64{0});
    Test->TestEqual(TEXT("sleeping burst benchmark evaluates no track"),
        SleepSnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated), uint64{0});

    const double RemainingDelaySeconds = Runtime.GetNextWakeDelaySeconds(View);
    Test->TestTrue(TEXT("boundary burst benchmark exposes the shared remaining delay"),
        FMath::IsNearlyEqual(RemainingDelaySeconds,
            BoundaryDelaySeconds - SampleFrames * FrameDeltaSeconds, 0.002));
    FRmlUiPerformance::Reset();
    const uint64 BoundaryStart = FPlatformTime::Cycles64();
    Runtime.Advance(static_cast<float>(RemainingDelaySeconds + 0.001));
    const double BoundaryWallMilliseconds =
        CyclesToMilliseconds(FPlatformTime::Cycles64() - BoundaryStart);
    const FRmlUiPerformanceSnapshot BoundarySnapshot = FRmlUiPerformance::Snapshot();
    const uint64 ExpectedTargets = static_cast<uint64>(TargetCount);
    Test->TestEqual(TEXT("shared boundary wakes every layered group"),
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredBoundaryWakes), ExpectedTargets);
    Test->TestEqual(TEXT("shared boundary refreshes every layered group once"),
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed), ExpectedTargets);
    Test->TestEqual(TEXT("shared boundary scans both records in every group"),
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned), ExpectedTargets * 2);
    Test->TestEqual(TEXT("shared boundary batches occlusion tag migration"),
        BoundarySnapshot.CallCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationScheduleTagMutation), uint64{1});
    Test->TestEqual(TEXT("shared boundary evaluates every newly visible winner"),
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated), ExpectedTargets);
    Test->TestEqual(TEXT("shared boundary commits every newly visible winner"),
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCommittedProperties), ExpectedTargets);
    Test->TestTrue(TEXT("shared boundary makes the view frame-active"),
        Runtime.HasActiveAnimations(View));

    FRmlUiPerformance::Reset();
    const uint64 WarmActiveStart = FPlatformTime::Cycles64();
    Runtime.Advance(FrameDeltaSeconds);
    const double WarmActiveWallMilliseconds =
        CyclesToMilliseconds(FPlatformTime::Cycles64() - WarmActiveStart);
    const FRmlUiPerformanceSnapshot WarmActiveSnapshot = FRmlUiPerformance::Snapshot();
    Test->TestEqual(TEXT("warm active frame evaluates every visible winner"),
        WarmActiveSnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated), ExpectedTargets);
    Test->TestEqual(TEXT("warm active frame commits every visible winner"),
        WarmActiveSnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCommittedProperties), ExpectedTargets);

    Runtime.CancelViewAnimations(View);
    Test->TestTrue(TEXT("boundary burst lower definition released"),
        Runtime.ReleaseDefinition(LowerDefinition));
    Test->TestTrue(TEXT("boundary burst upper definition released"),
        Runtime.ReleaseDefinition(UpperDefinition));
    RmlUE_DestroyView(View);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("targets"), TargetCount);
    Result->SetNumberField(TEXT("entities"), TargetCount * 2);
    Result->SetNumberField(TEXT("shared_boundary_delay_seconds"), BoundaryDelaySeconds);
    Result->SetNumberField(TEXT("remaining_delay_before_crossing_seconds"),
        RemainingDelaySeconds);
    AddFrameDistribution(Result, TEXT("sleep_advance_frame"), SleepFrameSamples);
    Result->SetObjectField(TEXT("sleep_advance"),
        StageJson(SleepSnapshot, ERmlUiPerformanceStage::AnimationAdvance));
    AddScheduleBreakdown(Result, TEXT("sleep"), SleepSnapshot);
    Result->SetNumberField(TEXT("boundary_wall_ms"), BoundaryWallMilliseconds);
    Result->SetObjectField(TEXT("boundary_advance"),
        StageJson(BoundarySnapshot, ERmlUiPerformanceStage::AnimationAdvance));
    AddScheduleBreakdown(Result, TEXT("boundary"), BoundarySnapshot);
    Result->SetObjectField(TEXT("boundary_evaluate"),
        StageJson(BoundarySnapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("boundary_collect"),
        StageJson(BoundarySnapshot, ERmlUiPerformanceStage::AnimationCollect));
    AddCommitBreakdown(Result, TEXT("boundary"), BoundarySnapshot);
    Result->SetObjectField(TEXT("boundary_finalize"),
        StageJson(BoundarySnapshot, ERmlUiPerformanceStage::AnimationFinalize));
    Result->SetNumberField(TEXT("warm_active_wall_ms"), WarmActiveWallMilliseconds);
    Result->SetObjectField(TEXT("warm_active_advance"),
        StageJson(WarmActiveSnapshot, ERmlUiPerformanceStage::AnimationAdvance));
    AddScheduleBreakdown(Result, TEXT("warm_active"), WarmActiveSnapshot);
    Result->SetObjectField(TEXT("warm_active_evaluate"),
        StageJson(WarmActiveSnapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("warm_active_collect"),
        StageJson(WarmActiveSnapshot, ERmlUiPerformanceStage::AnimationCollect));
    AddCommitBreakdown(Result, TEXT("warm_active"), WarmActiveSnapshot);
    Result->SetNumberField(TEXT("layered_groups_refreshed"), static_cast<double>(
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed)));
    Result->SetNumberField(TEXT("layered_records_scanned"), static_cast<double>(
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned)));
    Result->SetNumberField(TEXT("layered_boundary_wakes"), static_cast<double>(
        BoundarySnapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredBoundaryWakes)));
    Result->SetStringField(TEXT("scope"),
        TEXT("Editor NullRHI same-frame layered delay burst. Every target has one paused lower contribution and one delayed upper contribution. Sleep samples cover Runtime::Advance and heap-top checks only; the boundary sample includes due-heap draining, group refresh, MovieScene ECS evaluation and one RmlUi visual commit per target. Slate widget paint, render thread, GPU and present are excluded."));
    return Result;
}

TSharedPtr<FJsonObject> RunCompletionBurstScenario(
    FAutomationTestBase* Test, int32 TrackCount, bool bWithCallbacks)
{
    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDefinition Definition;
    Definition.From = 0.2f;
    Definition.To = 0.8f;
    Definition.DurationSeconds = 1.0;
    const FRmlUiAnimationDefinitionHandle DefinitionHandle =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Definition);
    Test->TestTrue(TEXT("completion burst definition registered"), DefinitionHandle.IsValid());

    TArray<FRmlUiAnimationBindingHandle> Bindings;
    Bindings.Reserve(TrackCount);
    for (int32 Index = 0; Index < TrackCount; ++Index)
    {
        Bindings.Add(Runtime.BindCallback(DefinitionHandle));
    }

    uint64 CallbackCount = 0;
    TArray<FRmlUiAnimationCompletionCallback> CompletionCallbacks;
    if (bWithCallbacks)
    {
        CompletionCallbacks.Reserve(TrackCount);
        for (int32 Index = 0; Index < TrackCount; ++Index)
        {
            CompletionCallbacks.Add(
                [&CallbackCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
                {
                    if (Reason == ERmlUiAnimationCompletionReason::Completed)
                    {
                        ++CallbackCount;
                    }
                });
        }
    }

    TArray<FRmlUiAnimationHandle> Handles;
    const int32 Played = Runtime.PlayBindings(
        Bindings, Handles, MoveTemp(CompletionCallbacks));
    Test->TestEqual(TEXT("completion burst played every binding"), Played, TrackCount);
    Runtime.Advance(0.0f);

    FRmlUiPerformance::Reset();
    const uint64 BoundaryStart = FPlatformTime::Cycles64();
    Runtime.Advance(1.001f);
    const double BoundaryWallMilliseconds =
        CyclesToMilliseconds(FPlatformTime::Cycles64() - BoundaryStart);
    const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
    const uint64 ExpectedCount = static_cast<uint64>(TrackCount);
    Test->TestEqual(TEXT("completion burst retires every animation"),
        Runtime.GetActiveAnimationCount(), 0);
    Test->TestEqual(TEXT("completion burst records every completion"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletedInstances), ExpectedCount);
    Test->TestEqual(TEXT("completion burst dispatches expected callbacks"),
        CallbackCount, bWithCallbacks ? ExpectedCount : uint64{0});
    Test->TestEqual(TEXT("completion burst profiles expected callbacks"),
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletionCallbacks),
        bWithCallbacks ? ExpectedCount : uint64{0});

    for (FRmlUiAnimationBindingHandle Binding : Bindings)
    {
        Runtime.ReleaseBinding(Binding);
    }
    Test->TestTrue(TEXT("completion burst definition released"),
        Runtime.ReleaseDefinition(DefinitionHandle));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("tracks"), TrackCount);
    Result->SetBoolField(TEXT("completion_callbacks"), bWithCallbacks);
    Result->SetNumberField(TEXT("boundary_wall_ms"), BoundaryWallMilliseconds);
    Result->SetObjectField(TEXT("advance"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationAdvance));
    Result->SetObjectField(TEXT("evaluate"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationEvaluate));
    Result->SetObjectField(TEXT("collect"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCollect));
    Result->SetObjectField(TEXT("commit"),
        StageJson(Snapshot, ERmlUiPerformanceStage::AnimationCommit));
    AddFinalizeBreakdown(Result, TEXT("boundary"), Snapshot);
    Result->SetNumberField(TEXT("completed_instances"), static_cast<double>(
        Snapshot.WorkCount(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletedInstances)));
    Result->SetNumberField(TEXT("callbacks_dispatched"), static_cast<double>(CallbackCount));
    return Result;
}

FString OutputDirectory()
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(), TEXT("RmlUiAnimationPerfOutput="), Directory) || Directory.IsEmpty())
    {
        Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance/AnimationRuntime"));
    }
    return FPaths::ConvertRelativePathToFull(Directory);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationPerformanceBaselineTest,
    "RmlUi.Animation.MovieSceneRuntime.PerformanceBaseline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationPerformanceBaselineTest::RunTest(const FString& Parameters)
{
    using namespace RmlUiAnimationPerformanceTests;
    const bool bPreviouslyEnabled = FRmlUiPerformance::IsEnabled();
    FRmlUiPerformance::SetEnabled(true);

    TArray<TSharedPtr<FJsonValue>> DenseResults;
    TArray<TSharedPtr<FJsonValue>> MovieSceneResults;
    TArray<TSharedPtr<FJsonValue>> MovieSceneKeyframeResults;
    TArray<TSharedPtr<FJsonValue>> NativeResults;
    TArray<TSharedPtr<FJsonValue>> NativeTransformResults;
    TArray<TSharedPtr<FJsonValue>> NativeOverlappingTransformResults;
    for (int32 TrackCount : TrackCounts)
    {
        DenseResults.Add(MakeShared<FJsonValueObject>(RunDenseBaseline(TrackCount)));
        MovieSceneResults.Add(MakeShared<FJsonValueObject>(RunMovieSceneScenario(this, TrackCount)));
        MovieSceneKeyframeResults.Add(MakeShared<FJsonValueObject>(
            RunMovieSceneScenario(this, TrackCount, true)));
        NativeResults.Add(MakeShared<FJsonValueObject>(RunNativeScenario(this, TrackCount, false)));
        NativeTransformResults.Add(MakeShared<FJsonValueObject>(RunNativeScenario(this, TrackCount, true)));
        NativeOverlappingTransformResults.Add(MakeShared<FJsonValueObject>(RunNativeScenario(this, TrackCount, true, true)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), 4);
    Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
    Root->SetNumberField(TEXT("warmup_frames"), WarmupFrames);
    Root->SetNumberField(TEXT("sample_frames"), SampleFrames);
    Root->SetNumberField(TEXT("frame_delta_seconds"), FrameDeltaSeconds);
    Root->SetArrayField(TEXT("dense_linear_control"), DenseResults);
    Root->SetArrayField(TEXT("moviescene_ecs_no_sink"), MovieSceneResults);
    Root->SetArrayField(TEXT("moviescene_ecs_keyframes_no_sink"), MovieSceneKeyframeResults);
    Root->SetArrayField(TEXT("moviescene_ecs_rmlui_opacity"), NativeResults);
    Root->SetArrayField(TEXT("moviescene_ecs_rmlui_transform2d"), NativeTransformResults);
    Root->SetArrayField(TEXT("moviescene_ecs_rmlui_transform2d_overlapping"), NativeOverlappingTransformResults);
    Root->SetStringField(TEXT("limitations"),
        TEXT("Editor NullRHI microbenchmark. Dense control is an algorithmic lower bound and excludes entity lifecycle, dispatch and sink work. The keyframe callback scenario isolates a shared immutable five-keyframe definition, precomputed easing LUT and entity-local segment cursor; it excludes RmlUi commit. Native scenarios include opacity, eligible leaf Transform2D, or overlapping parent/child Transform2D visual-property commit, an explicit idle RmlUi Update probe, and retained RmlUE_RenderSlate replay after one baseline topology record, but exclude Slate widget decode/paint, render thread, GPU and present. The overlapping scenario measures one parent plus child targets with total targets equal to the reported track count; it is not a representative element-depth distribution. The animation runtime itself does not request RmlUi Update for accepted visual commits. Results are not a browser or UMG comparison."));

    FString Json;
    FJsonSerializer::Serialize(
        Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Directory = OutputDirectory();
    IFileManager::Get().MakeDirectory(*Directory, true);
    FString EngineLabel = FEngineVersion::Current().ToString();
    EngineLabel.ReplaceInline(TEXT("+"), TEXT("-"));
    const FString Path = FPaths::Combine(Directory, TEXT("AnimationRuntime-") + EngineLabel + TEXT(".json"));
    TestTrue(TEXT("animation performance report saved"), FFileHelper::SaveStringToFile(Json, *Path));
    AddInfo(FString::Printf(TEXT("Animation performance report: %s"), *Path));

    FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationLayeredContributionPerformanceTest,
    "RmlUi.Animation.MovieSceneRuntime.LayeredContributionPerformance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationLayeredContributionPerformanceTest::RunTest(const FString& Parameters)
{
    using namespace RmlUiAnimationPerformanceTests;
    const bool bPreviouslyEnabled = FRmlUiPerformance::IsEnabled();
    FRmlUiPerformance::SetEnabled(true);

    TArray<TSharedPtr<FJsonValue>> FixedTargetResults;
    for (int32 TargetCount : {1000, 10000})
    {
        for (int32 LayerCount : {1, 2, 4})
            FixedTargetResults.Add(MakeShared<FJsonValueObject>(
                RunLayeredContributionScenario(this, TargetCount, LayerCount)));
    }

    TArray<TSharedPtr<FJsonValue>> FixedEntityResults;
    for (int32 LayerCount : {1, 2, 4})
    {
        FixedEntityResults.Add(MakeShared<FJsonValueObject>(
            RunLayeredContributionScenario(this, 10000 / LayerCount, LayerCount)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), 1);
    Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
    Root->SetNumberField(TEXT("warmup_frames"), WarmupFrames);
    Root->SetNumberField(TEXT("sample_frames"), SampleFrames);
    Root->SetNumberField(TEXT("frame_delta_seconds"), FrameDeltaSeconds);
    Root->SetArrayField(TEXT("fixed_target_count"), FixedTargetResults);
    Root->SetArrayField(TEXT("fixed_entity_count_10000"), FixedEntityResults);
    Root->SetStringField(TEXT("limitations"),
        TEXT("Editor NullRHI microbenchmark. Timings include MovieScene evaluation, contribution arbitration and the RmlUi visual commit sink, but exclude Slate widget paint, render thread, GPU and present. Process memory deltas are order-sensitive working-set observations, not retained allocation accounting. Each automation run should be repeated before using small differences for an architectural decision."));

    FString Json;
    FJsonSerializer::Serialize(
        Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Directory = OutputDirectory();
    IFileManager::Get().MakeDirectory(*Directory, true);
    FString EngineLabel = FEngineVersion::Current().ToString();
    EngineLabel.ReplaceInline(TEXT("+"), TEXT("-"));
    const FString Path = FPaths::Combine(
        Directory, TEXT("LayeredContribution-") + EngineLabel + TEXT(".json"));
    TestTrue(TEXT("layered contribution performance report saved"),
        FFileHelper::SaveStringToFile(Json, *Path));
    AddInfo(FString::Printf(TEXT("Layered contribution performance report: %s"), *Path));

    FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationLayeredBoundaryBurstPerformanceTest,
    "RmlUi.Animation.MovieSceneRuntime.LayeredBoundaryBurstPerformance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationLayeredBoundaryBurstPerformanceTest::RunTest(const FString& Parameters)
{
    using namespace RmlUiAnimationPerformanceTests;
    const bool bPreviouslyEnabled = FRmlUiPerformance::IsEnabled();
    FRmlUiPerformance::SetEnabled(true);

    TArray<TSharedPtr<FJsonValue>> Results;
    for (int32 TargetCount : {1000, 5000, 10000})
    {
        Results.Add(MakeShared<FJsonValueObject>(
            RunLayeredBoundaryBurstScenario(this, TargetCount)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), 1);
    Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
    Root->SetNumberField(TEXT("sleep_sample_frames"), SampleFrames);
    Root->SetNumberField(TEXT("frame_delta_seconds"), FrameDeltaSeconds);
    Root->SetArrayField(TEXT("same_frame_delay_boundary"), Results);
    Root->SetStringField(TEXT("limitations"),
        TEXT("Editor NullRHI microbenchmark. The boundary is intentionally synchronized across all groups and represents a worst-case burst rather than typical staggered animation traffic. Timings include scheduler, MovieScene ECS evaluation and RmlUi visual commit, but exclude Slate widget paint, render thread, GPU and present. Repeat independent processes before using small differences for an architectural decision."));

    FString Json;
    FJsonSerializer::Serialize(
        Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Directory = OutputDirectory();
    IFileManager::Get().MakeDirectory(*Directory, true);
    FString EngineLabel = FEngineVersion::Current().ToString();
    EngineLabel.ReplaceInline(TEXT("+"), TEXT("-"));
    const FString Path = FPaths::Combine(
        Directory, TEXT("LayeredBoundaryBurst-") + EngineLabel + TEXT(".json"));
    TestTrue(TEXT("layered boundary burst performance report saved"),
        FFileHelper::SaveStringToFile(Json, *Path));
    AddInfo(FString::Printf(TEXT("Layered boundary burst performance report: %s"), *Path));

    FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationCompletionBurstPerformanceTest,
    "RmlUi.Animation.MovieSceneRuntime.CompletionBurstPerformance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationCompletionBurstPerformanceTest::RunTest(const FString& Parameters)
{
    using namespace RmlUiAnimationPerformanceTests;
    const bool bPreviouslyEnabled = FRmlUiPerformance::IsEnabled();
    FRmlUiPerformance::SetEnabled(true);

    TArray<TSharedPtr<FJsonValue>> WithoutCallbacks;
    TArray<TSharedPtr<FJsonValue>> WithCallbacks;
    for (int32 TrackCount : {1000, 5000, 10000})
    {
        WithoutCallbacks.Add(MakeShared<FJsonValueObject>(
            RunCompletionBurstScenario(this, TrackCount, false)));
        WithCallbacks.Add(MakeShared<FJsonValueObject>(
            RunCompletionBurstScenario(this, TrackCount, true)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), 1);
    Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
    Root->SetArrayField(TEXT("without_completion_callbacks"), WithoutCallbacks);
    Root->SetArrayField(TEXT("with_completion_callbacks"), WithCallbacks);
    Root->SetStringField(TEXT("limitations"),
        TEXT("Editor NullRHI callback-binding microbenchmark. Every track completes in one shared Runtime::Advance. It measures MovieScene evaluation, result collection, record/entity retirement and native TFunction completion dispatch, but excludes RmlUi property commit, Puerts/V8 event delivery, Slate paint, render thread, GPU and present."));

    FString Json;
    FJsonSerializer::Serialize(
        Root.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Directory = OutputDirectory();
    IFileManager::Get().MakeDirectory(*Directory, true);
    FString EngineLabel = FEngineVersion::Current().ToString();
    EngineLabel.ReplaceInline(TEXT("+"), TEXT("-"));
    const FString Path = FPaths::Combine(
        Directory, TEXT("CompletionBurst-") + EngineLabel + TEXT(".json"));
    TestTrue(TEXT("completion burst performance report saved"),
        FFileHelper::SaveStringToFile(Json, *Path));
    AddInfo(FString::Printf(TEXT("Completion burst performance report: %s"), *Path));

    FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
    return true;
}

#endif
