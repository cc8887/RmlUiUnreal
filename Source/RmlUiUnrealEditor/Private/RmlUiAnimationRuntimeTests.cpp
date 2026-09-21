#include "RmlUiAnimationRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RmlUiPerformance.h"
#include "RmlUiBridge.h"
#include "RmlUiUnrealModule.h"
#include "SRmlUiWidget.h"
#include "Widgets/SInvalidationPanel.h"
#include "Widgets/SWindow.h"

namespace
{
bool FindVisualOpacity(const RmlUE_SlateFrame& Frame, uint32 Node, float& OutOpacity)
{
    for (uint32 Index = 0; Index < Frame.DrawCount; ++Index)
    {
        if (Frame.Draws[Index].VisualNode == Node)
        {
            OutOpacity = Frame.Draws[Index].VisualOpacity;
            return true;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeLifecycleTest,
    "RmlUi.Animation.MovieSceneRuntime.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeLifecycleTest::RunTest(const FString& Parameters)
{
    FRmlUiAnimationRuntime Runtime;
    TArray<float> Values;
    int32 CompletedCount = 0;

    FRmlUiFloatAnimationDesc Desc;
    Desc.From = 0.0f;
    Desc.To = 10.0f;
    Desc.DelaySeconds = 0.25;
    Desc.DurationSeconds = 0.5;
    Desc.Iterations = 2;
    Desc.BindingId = 7;
    Runtime.PlayFloat(
        Desc,
        [&Values](FRmlUiAnimationHandle, float Value) { Values.Add(Value); },
        [&CompletedCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
        {
            if (Reason == ERmlUiAnimationCompletionReason::Completed)
            {
                ++CompletedCount;
            }
        });

    Runtime.Advance(0.0f);
    Runtime.Advance(0.5f);
    Runtime.Advance(0.75f);
    TestEqual(TEXT("initial, repeated midpoint and terminal values emitted"), Values.Num(), 3);
    if (Values.Num() == 3)
    {
        TestEqual(TEXT("initial value"), Values[0], 0.0f);
        TestTrue(TEXT("second iteration midpoint"), FMath::IsNearlyEqual(Values[1], 5.0f));
        TestEqual(TEXT("terminal value"), Values[2], 10.0f);
    }
    TestEqual(TEXT("completion emitted once"), CompletedCount, 1);
    TestEqual(TEXT("completed entity removed"), Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("legacy definition released after completion"), Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("legacy binding released after completion"), Runtime.GetBindingCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeKeyframesTest,
    "RmlUi.Animation.MovieSceneRuntime.KeyframesAndSegmentEasing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeKeyframesTest::RunTest(const FString& Parameters)
{
    FRmlUiAnimationRuntime Runtime;

    FRmlUiFloatAnimationDefinition InvalidOffsets;
    InvalidOffsets.Keyframes = {
        {0.0f, 0.0f, {}}, {0.75f, 0.5f, {}}, {0.5f, 1.0f, {}}, {1.0f, 0.0f, {}}};
    TestFalse(TEXT("non-monotonic keyframe offsets rejected"),
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, InvalidOffsets).IsValid());

    FRmlUiFloatAnimationDefinition InvalidEasing;
    FRmlUiAnimationEasing BadBezier;
    BadBezier.Type = ERmlUiAnimationEasingType::CubicBezier;
    BadBezier.X1 = -0.1f;
    InvalidEasing.Keyframes = {{0.0f, 0.0f, BadBezier}, {1.0f, 1.0f, {}}};
    TestFalse(TEXT("cubic-bezier x control outside unit interval rejected"),
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, InvalidEasing).IsValid());

    FRmlUiFloatAnimationDefinition MultiSegment;
    MultiSegment.DurationSeconds = 1.0;
    MultiSegment.Keyframes = {
        {0.0f, 0.0f, {}}, {0.25f, 1.0f, {}}, {1.0f, 0.0f, {}}};
    const FRmlUiAnimationDefinitionHandle MultiDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, MultiSegment);
    const FRmlUiAnimationBindingHandle MultiBinding = Runtime.BindCallback(MultiDefinition);
    TArray<float> MultiValues;
    int32 MultiCompleted = 0;
    TestTrue(TEXT("multi-segment float definition registered"), MultiDefinition.IsValid());
    TestTrue(TEXT("registered definition reports expanded keyframe allocation"),
        Runtime.GetDefinitionAllocatedBytes(MultiDefinition) > 0);
    TestTrue(TEXT("multi-segment callback binding created"), MultiBinding.IsValid());
    TestTrue(TEXT("multi-segment callback binding played"),
        Runtime.PlayBinding(MultiBinding,
            [&MultiValues](FRmlUiAnimationHandle, float Value) { MultiValues.Add(Value); },
            [&MultiCompleted](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
            {
                MultiCompleted += Reason == ERmlUiAnimationCompletionReason::Completed ? 1 : 0;
            }).IsValid());
    Runtime.Advance(0.0f);
    Runtime.Advance(0.125f);
    Runtime.Advance(0.125f);
    Runtime.Advance(0.375f);
    Runtime.Advance(0.375f);
    TestEqual(TEXT("multi-segment track emits each sampled value"), MultiValues.Num(), 5);
    if (MultiValues.Num() == 5)
    {
        TestTrue(TEXT("multi-segment initial value"), FMath::IsNearlyEqual(MultiValues[0], 0.0f));
        TestTrue(TEXT("multi-segment first midpoint"), FMath::IsNearlyEqual(MultiValues[1], 0.5f));
        TestTrue(TEXT("multi-segment interior keyframe"), FMath::IsNearlyEqual(MultiValues[2], 1.0f));
        TestTrue(TEXT("multi-segment second midpoint"), FMath::IsNearlyEqual(MultiValues[3], 0.5f));
        TestTrue(TEXT("multi-segment terminal value"), FMath::IsNearlyEqual(MultiValues[4], 0.0f));
    }
    TestEqual(TEXT("multi-segment completion emitted once"), MultiCompleted, 1);
    TestTrue(TEXT("multi-segment binding released"), Runtime.ReleaseBinding(MultiBinding));
    TestTrue(TEXT("multi-segment definition released"), Runtime.ReleaseDefinition(MultiDefinition));

    FRmlUiFloatAnimationDefinition Discontinuous;
    Discontinuous.DurationSeconds = 1.0;
    Discontinuous.Keyframes = {
        {0.0f, 0.0f, {}}, {0.5f, 0.0f, {}}, {0.5f, 1.0f, {}}, {1.0f, 1.0f, {}}};
    const FRmlUiAnimationDefinitionHandle DiscontinuousDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Discontinuous);
    const FRmlUiAnimationBindingHandle DiscontinuousBinding = Runtime.BindCallback(DiscontinuousDefinition);
    float DiscontinuousValue = -1.0f;
    TestTrue(TEXT("equal-offset discontinuity definition registered"), DiscontinuousDefinition.IsValid());
    TestTrue(TEXT("equal-offset discontinuity binding played"), Runtime.PlayBinding(
        DiscontinuousBinding,
        [&DiscontinuousValue](FRmlUiAnimationHandle, float Value) { DiscontinuousValue = Value; }).IsValid());
    Runtime.Advance(0.49f);
    TestTrue(TEXT("discontinuity retains the old value before its boundary"),
        FMath::IsNearlyEqual(DiscontinuousValue, 0.0f));
    Runtime.Advance(0.01f);
    TestTrue(TEXT("discontinuity selects the new value at its exact boundary"),
        FMath::IsNearlyEqual(DiscontinuousValue, 1.0f));
    Runtime.CancelAll();
    TestTrue(TEXT("discontinuity binding released"), Runtime.ReleaseBinding(DiscontinuousBinding));
    TestTrue(TEXT("discontinuity definition released"), Runtime.ReleaseDefinition(DiscontinuousDefinition));

    FRmlUiFloatAnimationDefinition Instant;
    Instant.DurationSeconds = 0.0;
    Instant.DelaySeconds = 0.1;
    Instant.Keyframes = {{0.0f, 0.0f, {}}, {1.0f, 2.0f, {}}};
    const FRmlUiAnimationDefinitionHandle InstantDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Instant);
    const FRmlUiAnimationBindingHandle InstantBinding = Runtime.BindCallback(InstantDefinition);
    float InstantValue = -1.0f;
    int32 InstantCompleted = 0;
    TestTrue(TEXT("zero-duration definition registered"), InstantDefinition.IsValid());
    TestTrue(TEXT("zero-duration binding played"), Runtime.PlayBinding(
        InstantBinding,
        [&InstantValue](FRmlUiAnimationHandle, float Value) { InstantValue = Value; },
        [&InstantCompleted](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
        {
            InstantCompleted += Reason == ERmlUiAnimationCompletionReason::Completed ? 1 : 0;
        }).IsValid());
    Runtime.Advance(0.05f);
    TestTrue(TEXT("zero-duration track retains its start value before delay"),
        FMath::IsNearlyEqual(InstantValue, 0.0f));
    TestEqual(TEXT("zero-duration track remains active before delay"), Runtime.GetActiveAnimationCount(), 1);
    Runtime.Advance(0.05f);
    TestTrue(TEXT("zero-duration track commits its terminal value at delay"),
        FMath::IsNearlyEqual(InstantValue, 2.0f));
    TestEqual(TEXT("zero-duration completion emitted once"), InstantCompleted, 1);
    TestEqual(TEXT("zero-duration entity retires in the commit tick"), Runtime.GetActiveAnimationCount(), 0);
    TestTrue(TEXT("zero-duration binding released"), Runtime.ReleaseBinding(InstantBinding));
    TestTrue(TEXT("zero-duration definition released"), Runtime.ReleaseDefinition(InstantDefinition));

    FRmlUiAnimationEasing EaseIn;
    EaseIn.Type = ERmlUiAnimationEasingType::CubicBezier;
    EaseIn.X1 = 0.42f;
    EaseIn.Y1 = 0.0f;
    EaseIn.X2 = 1.0f;
    EaseIn.Y2 = 1.0f;
    FRmlUiFloatAnimationDefinition Eased;
    Eased.DurationSeconds = 1.0;
    Eased.Keyframes = {{0.0f, 0.0f, EaseIn}, {1.0f, 1.0f, {}}};
    const FRmlUiAnimationDefinitionHandle EasedDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Eased);
    const FRmlUiAnimationBindingHandle EasedBinding = Runtime.BindCallback(EasedDefinition);
    float EasedMidpoint = -1.0f;
    TestTrue(TEXT("cubic-bezier definition registered"), EasedDefinition.IsValid());
    TestTrue(TEXT("cubic-bezier binding played"), Runtime.PlayBinding(EasedBinding,
        [&EasedMidpoint](FRmlUiAnimationHandle, float Value) { EasedMidpoint = Value; }).IsValid());
    Runtime.Advance(0.5f);
    TestTrue(TEXT("cubic-bezier LUT applies ease-in at segment midpoint"),
        EasedMidpoint > 0.30f && EasedMidpoint < 0.33f);
    Runtime.CancelAll();
    TestTrue(TEXT("cubic-bezier binding released"), Runtime.ReleaseBinding(EasedBinding));
    TestTrue(TEXT("cubic-bezier definition released"), Runtime.ReleaseDefinition(EasedDefinition));

    RmlUE_View* View = RmlUE_CreateSlateView(160, 100, 1.0f);
    TestNotNull(TEXT("keyframe Transform2D view created"), View);
    if (!View) return false;
    const char* Markup = "<rml><head><style>body{margin:0;}#target{position:absolute;left:20px;top:10px;width:10px;height:10px;background:#fff;transform-origin:0 0;transform:translate(0px,0px);}</style></head><body><div id='target'/></body></rml>";
    TestTrue(TEXT("keyframe Transform2D document loaded"),
        RmlUE_LoadDocumentFromMemory(View, Markup, "animation-keyframes-test.rml") != 0);
    TestTrue(TEXT("keyframe Transform2D layout completed"), RmlUE_Update(View) != 0);
    const RmlUE_Node Node = RmlUE_FindNode(View, "target");
    RmlUE_SlateFrame BaselineFrame{};
    TestTrue(TEXT("keyframe Transform2D retained baseline recorded"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);
    FRmlUiTransform2DAnimationDefinition Transform;
    Transform.DurationSeconds = 1.0;
    FRmlUiTransform2D Start;
    FRmlUiTransform2D Peak;
    Peak.TranslationX = 20.0f;
    FRmlUiTransform2D End;
    End.TranslationX = 10.0f;
    Transform.Keyframes = {{0.0f, Start, {}}, {0.5f, Peak, {}}, {1.0f, End, {}}};
    const FRmlUiAnimationDefinitionHandle TransformDefinition =
        Runtime.RegisterTransform2DDefinition(Transform);
    const FRmlUiAnimationBindingHandle TransformBinding =
        Runtime.BindNode(TransformDefinition, View, Node);
    TestTrue(TEXT("Transform2D keyframe definition registered"), TransformDefinition.IsValid());
    TestTrue(TEXT("Transform2D keyframe binding created"), TransformBinding.IsValid());
    TestTrue(TEXT("Transform2D keyframe binding played"), Runtime.PlayBinding(TransformBinding).IsValid());
    Runtime.Advance(0.25f);
    RmlUE_NodeMetrics Metrics{};
    RmlUE_LayoutInfo Layout{};
    TestTrue(TEXT("Transform2D first segment measured"),
        RmlUE_MeasureNodes(View, &Node, 1, &Metrics, &Layout) == 1);
    TestTrue(TEXT("Transform2D first segment reaches visual geometry"),
        FMath::IsNearlyEqual(Metrics.X, 30.0f));
    Runtime.Advance(0.5f);
    TestTrue(TEXT("Transform2D second segment measured"),
        RmlUE_MeasureNodes(View, &Node, 1, &Metrics, &Layout) == 1);
    TestTrue(TEXT("Transform2D second segment uses interior keyframe"),
        FMath::IsNearlyEqual(Metrics.X, 35.0f));
    Runtime.Advance(0.25f);
    TestEqual(TEXT("Transform2D keyframe entity completes"), Runtime.GetActiveAnimationCount(), 0);
    TestTrue(TEXT("Transform2D keyframe binding released"), Runtime.ReleaseBinding(TransformBinding));
    TestTrue(TEXT("Transform2D keyframe definition released"), Runtime.ReleaseDefinition(TransformDefinition));
    TestEqual(TEXT("keyframe test leaves no definitions"), Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("released definition no longer reports allocated bytes"),
        Runtime.GetDefinitionAllocatedBytes(MultiDefinition), static_cast<uint64>(0));
    TestEqual(TEXT("keyframe test leaves no bindings"), Runtime.GetBindingCount(), 0);
    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimePlaybackControlsTest,
    "RmlUi.Animation.MovieSceneRuntime.PlaybackControlsAndDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimePlaybackControlsTest::RunTest(const FString& Parameters)
{
    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDesc Reverse;
    Reverse.DurationSeconds = 1.0;
    Reverse.Direction = ERmlUiAnimationDirection::Reverse;
    float Value = -1.0f;
    const FRmlUiAnimationHandle Handle = Runtime.PlayFloat(
        Reverse, [&Value](FRmlUiAnimationHandle, float Sample) { Value = Sample; });
    TestTrue(TEXT("reverse playback handle is valid"), Handle.IsValid());
    Runtime.Advance(0.0f);
    TestTrue(TEXT("reverse playback starts at the terminal value"),
        FMath::IsNearlyEqual(Value, 1.0f));
    Runtime.Advance(0.25f);
    TestTrue(TEXT("reverse playback moves backward"), FMath::IsNearlyEqual(Value, 0.75f));

    TestTrue(TEXT("pause accepts an active handle"), Runtime.Pause(Handle));
    TestTrue(TEXT("paused handle reports paused"), Runtime.IsPaused(Handle));
    Runtime.Advance(0.25f);
    TestTrue(TEXT("paused playback keeps its local time"), FMath::IsNearlyEqual(Value, 0.75f));
    TestTrue(TEXT("seek accepts local animation time"), Runtime.Seek(Handle, 0.5));
    Runtime.Advance(0.0f);
    TestTrue(TEXT("seek updates a paused animation deterministically"),
        FMath::IsNearlyEqual(Value, 0.5f));
    TestTrue(TEXT("playback rate can change while paused"),
        Runtime.SetPlaybackRate(Handle, 2.0));
    TestTrue(TEXT("resume accepts a paused handle"), Runtime.Resume(Handle));
    TestFalse(TEXT("resumed handle no longer reports paused"), Runtime.IsPaused(Handle));
    Runtime.Advance(0.125f);
    TestTrue(TEXT("resumed animation uses the updated playback rate"),
        FMath::IsNearlyEqual(Value, 0.25f));
    TestTrue(TEXT("controlled handle remains active"), Runtime.IsActive(Handle));
    TestTrue(TEXT("controlled handle can be cancelled"), Runtime.Cancel(Handle));
    TestFalse(TEXT("cancelled handle becomes stale"), Runtime.IsActive(Handle));

    FRmlUiFloatAnimationDesc Alternate;
    Alternate.DurationSeconds = 1.0;
    Alternate.Iterations = 2;
    Alternate.Direction = ERmlUiAnimationDirection::Alternate;
    float AlternateValue = -1.0f;
    const FRmlUiAnimationHandle AlternateHandle = Runtime.PlayFloat(
        Alternate,
        [&AlternateValue](FRmlUiAnimationHandle, float Sample) { AlternateValue = Sample; });
    Runtime.Advance(2.0f);
    TestTrue(TEXT("alternate two-iteration animation ends at its initial value"),
        FMath::IsNearlyEqual(AlternateValue, 0.0f));
    TestFalse(TEXT("completed alternate handle becomes stale"), Runtime.IsActive(AlternateHandle));
    TestEqual(TEXT("playback control test releases temporary definitions"),
        Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("playback control test releases temporary bindings"),
        Runtime.GetBindingCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeReplacementTest,
    "RmlUi.Animation.MovieSceneRuntime.ReplacementAndReentrancy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeReplacementTest::RunTest(const FString& Parameters)
{
    FRmlUiAnimationRuntime Runtime;
    int32 ReplacedCount = 0;
    int32 CancelledCount = 0;
    int32 ReentrantValueCount = 0;

    FRmlUiFloatAnimationDesc Desc;
    Desc.DurationSeconds = 1.0;
    Desc.BindingId = 42;
    Runtime.PlayFloat(
        Desc,
        {},
        [&ReplacedCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
        {
            ReplacedCount += Reason == ERmlUiAnimationCompletionReason::Replaced ? 1 : 0;
        });

    const FRmlUiAnimationHandle Replacement = Runtime.PlayFloat(
        Desc,
        [&Runtime, &ReentrantValueCount](FRmlUiAnimationHandle Handle, float)
        {
            ++ReentrantValueCount;
            Runtime.Cancel(Handle);
        },
        [&CancelledCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
        {
            CancelledCount += Reason == ERmlUiAnimationCompletionReason::Cancelled ? 1 : 0;
        });

    TestTrue(TEXT("replacement handle valid"), Replacement.IsValid());
    TestEqual(TEXT("prior binding replaced once"), ReplacedCount, 1);
    Runtime.Advance(0.25f);
    TestEqual(TEXT("value callback executed once"), ReentrantValueCount, 1);
    TestEqual(TEXT("callback cancellation emitted once"), CancelledCount, 1);
    TestEqual(TEXT("reentrant cancellation removed entity"), Runtime.GetActiveAnimationCount(), 0);

    FRmlUiFloatAnimationDesc CancelAllDesc;
    CancelAllDesc.DurationSeconds = 1.0;
    Runtime.PlayFloat(
        CancelAllDesc,
        {},
        [&Runtime, &CancelAllDesc](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
        {
            if (Reason == ERmlUiAnimationCompletionReason::Cancelled)
            {
                Runtime.PlayFloat(CancelAllDesc, {});
            }
        });
    Runtime.CancelAll();
    TestEqual(TEXT("animation created by cancellation callback remains managed"),
        Runtime.GetActiveAnimationCount(), 1);
    Runtime.CancelAll();
    TestEqual(TEXT("follow-up cancel all removes reentrant animation"), Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("legacy definitions released after cancellation"), Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("legacy bindings released after cancellation"), Runtime.GetBindingCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeSharedDefinitionTest,
    "RmlUi.Animation.MovieSceneRuntime.SharedDefinitionBindings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeSharedDefinitionTest::RunTest(const FString& Parameters)
{
    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    TestNotNull(TEXT("shared-definition Slate view created"), View);
    if (!View) return false;

    const char* Markup = "<rml><head><style>#a,#b,#c{display:block;opacity:1;width:10px;height:10px;background:#fff;}</style></head><body><div id='a'/><div id='b'/><div id='c'/></body></rml>";
    TestTrue(TEXT("shared-definition document loaded"),
        RmlUE_LoadDocumentFromMemory(View, Markup, "animation-shared-definition-test.rml") != 0);
    TestTrue(TEXT("shared-definition layout completed"), RmlUE_Update(View) != 0);
    const TArray<uint32> Nodes{
        RmlUE_FindNode(View, "a"), RmlUE_FindNode(View, "b"), RmlUE_FindNode(View, "c")};

    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDefinition Definition;
    Definition.From = 1.0f;
    Definition.To = 0.0f;
    Definition.DurationSeconds = 1.0;
    const FRmlUiAnimationDefinitionHandle DefinitionHandle =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Definition);
    TestTrue(TEXT("shared definition registered"), DefinitionHandle.IsValid());

    TArray<FRmlUiAnimationBindingHandle> Bindings;
    TestEqual(TEXT("all native nodes bound atomically"),
        Runtime.BindNodes(DefinitionHandle, View, Nodes, Bindings), 3);
    TestEqual(TEXT("one definition shared by three bindings"), Runtime.GetDefinitionCount(), 1);
    TestEqual(TEXT("three bindings retained"), Runtime.GetBindingCount(), 3);
    TestFalse(TEXT("definition with live bindings cannot be released"),
        Runtime.ReleaseDefinition(DefinitionHandle));

    TArray<FRmlUiAnimationHandle> BatchHandles;
    TArray<FRmlUiAnimationCompletionCallback> InvalidCallbacks;
    InvalidCallbacks.Add([](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason) {});
    TestEqual(TEXT("batch rejects a mismatched completion callback array"),
        Runtime.PlayBindings(Bindings, BatchHandles, MoveTemp(InvalidCallbacks)), 0);
    TestTrue(TEXT("callback mismatch starts no animation"), BatchHandles.IsEmpty());
    int32 BatchCompletionCount = 0;
    TArray<FRmlUiAnimationCompletionCallback> BatchCallbacks;
    for (int32 Index = 0; Index < Bindings.Num(); ++Index)
    {
        BatchCallbacks.Add([&BatchCompletionCount](
            FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason)
        {
            ++BatchCompletionCount;
        });
    }
    TestEqual(TEXT("shared bindings played as one control batch"),
        Runtime.PlayBindings(Bindings, BatchHandles, MoveTemp(BatchCallbacks)), 3);
    TestEqual(TEXT("batch returned every animation handle"), BatchHandles.Num(), 3);
    TestEqual(TEXT("three ECS instances created"), Runtime.GetActiveAnimationCount(), 3);
    Runtime.Advance(0.5f);
    RmlUE_SlateFrame MidpointFrame{};
    TestTrue(TEXT("shared-definition midpoint rendered"), RmlUE_RenderSlate(View, &MidpointFrame) != 0);
    for (uint32 Node : Nodes)
    {
        char Value[64]{};
        TestTrue(TEXT("shared-definition opacity readable"),
            RmlUE_GetComputedProperty(View, Node, "opacity", Value, sizeof(Value)) != 0);
        TestTrue(TEXT("active visual animation leaves computed opacity unchanged"),
            FMath::IsNearlyEqual(FCString::Atof(UTF8_TO_TCHAR(Value)), 1.0f));
        float VisualOpacity = 0.0f;
        TestTrue(TEXT("shared-definition midpoint reaches draw command"),
            FindVisualOpacity(MidpointFrame, Node, VisualOpacity) && FMath::IsNearlyEqual(VisualOpacity, 0.5f));
    }

    const FRmlUiAnimationBindingHandle StaleBinding = Bindings[0];
    TestTrue(TEXT("releasing binding cancels its instance"), Runtime.ReleaseBinding(StaleBinding));
    TestEqual(TEXT("binding cancellation invokes its batch completion callback"),
        BatchCompletionCount, 1);
    TestEqual(TEXT("two instances remain after binding release"), Runtime.GetActiveAnimationCount(), 2);
    TestFalse(TEXT("released generation cannot be replayed"), Runtime.PlayBinding(StaleBinding).IsValid());
    TArray<FRmlUiAnimationBindingHandle> InvalidBatch{StaleBinding, Bindings[1]};
    TArray<FRmlUiAnimationHandle> InvalidBatchHandles;
    TestEqual(TEXT("batch rejects stale binding before starting work"),
        Runtime.PlayBindings(InvalidBatch, InvalidBatchHandles), 0);
    TestTrue(TEXT("rejected batch returns no handles"), InvalidBatchHandles.IsEmpty());
    TestEqual(TEXT("rejected batch leaves existing instances unchanged"),
        Runtime.GetActiveAnimationCount(), 2);
    Runtime.Advance(0.5f);
    TestEqual(TEXT("remaining instances complete"), Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("every batch completion callback runs exactly once"),
        BatchCompletionCount, 3);
    TestEqual(TEXT("completed explicit bindings remain replayable"), Runtime.GetBindingCount(), 2);
    for (int32 Index = 1; Index < Nodes.Num(); ++Index)
    {
        char Value[64]{};
        TestTrue(TEXT("completed opacity readable"),
            RmlUE_GetComputedProperty(View, Nodes[Index], "opacity", Value, sizeof(Value)) != 0);
        TestTrue(TEXT("completed visual animation synchronizes final property"),
            FMath::IsNearlyEqual(FCString::Atof(UTF8_TO_TCHAR(Value)), 0.0f));
    }

    TestTrue(TEXT("completed binding can play again"), Runtime.PlayBinding(Bindings[1]).IsValid());
    TestEqual(TEXT("replayed binding creates one instance"), Runtime.GetActiveAnimationCount(), 1);
    TestTrue(TEXT("second binding released"), Runtime.ReleaseBinding(Bindings[1]));
    TestEqual(TEXT("replayed instance cancelled by binding release"), Runtime.GetActiveAnimationCount(), 0);
    TestTrue(TEXT("third binding released"), Runtime.ReleaseBinding(Bindings[2]));
    TestTrue(TEXT("unbound definition released"), Runtime.ReleaseDefinition(DefinitionHandle));
    TestEqual(TEXT("definition store empty"), Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("binding store empty"), Runtime.GetBindingCount(), 0);

    const FRmlUiAnimationDefinitionHandle ReusedSlot =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Definition);
    TestTrue(TEXT("definition slot can be reused"), ReusedSlot.IsValid());
    TestTrue(TEXT("reused definition has a new generation"), ReusedSlot.Value != DefinitionHandle.Value);
    TestFalse(TEXT("released definition handle remains stale"),
        Runtime.BindNode(DefinitionHandle, View, Nodes[0]).IsValid());
    const FRmlUiAnimationBindingHandle ViewBinding = Runtime.BindNode(ReusedSlot, View, Nodes[0]);
    TestTrue(TEXT("reused definition binds to view"), ViewBinding.IsValid());
    TestTrue(TEXT("view binding plays"), Runtime.PlayBinding(ViewBinding).IsValid());
    Runtime.CancelViewAnimations(View);
    TestEqual(TEXT("view teardown cancels explicit instance"), Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("view teardown releases explicit binding"), Runtime.GetBindingCount(), 0);
    TestFalse(TEXT("view teardown invalidates binding generation"), Runtime.PlayBinding(ViewBinding).IsValid());
    TestTrue(TEXT("reused definition released after view teardown"), Runtime.ReleaseDefinition(ReusedSlot));

    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeLayeredContributionTest,
    "RmlUi.Animation.MovieSceneRuntime.LayeredContributions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeLayeredContributionTest::RunTest(const FString& Parameters)
{
    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    TestNotNull(TEXT("layered-contribution Slate view created"), View);
    if (!View) return false;

    const char* Markup = "<rml><head><style>#target{display:block;opacity:1;width:10px;height:10px;background:#fff;}</style></head><body><div id='target'/></body></rml>";
    TestTrue(TEXT("layered-contribution document loaded"),
        RmlUE_LoadDocumentFromMemory(View, Markup, "animation-layered-contribution-test.rml") != 0);
    TestTrue(TEXT("layered-contribution layout completed"), RmlUE_Update(View) != 0);
    const uint32 Node = RmlUE_FindNode(View, "target");
    TestTrue(TEXT("layered-contribution node found"), Node != 0);

    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDefinition Lower;
    Lower.From = 0.0f;
    Lower.DurationSeconds = 1.0;
    Lower.Keyframes = {{0.0f, 0.0f, {}}, {0.5f, 0.5f, {}}, {1.0f, 0.5f, {}}};
    FRmlUiFloatAnimationDefinition Upper;
    Upper.From = 0.5f;
    Upper.To = 1.0f;
    Upper.DelaySeconds = 0.5;
    Upper.DurationSeconds = 0.25;
    const FRmlUiAnimationDefinitionHandle LowerDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Lower);
    const FRmlUiAnimationDefinitionHandle UpperDefinition =
        Runtime.RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Upper);
    const FRmlUiAnimationBindingHandle LowerBinding = Runtime.BindNode(LowerDefinition, View, Node);
    const FRmlUiAnimationBindingHandle UpperBinding = Runtime.BindNode(UpperDefinition, View, Node);
    const TArray<FRmlUiAnimationBindingHandle> Bindings{LowerBinding, UpperBinding};

    TArray<FRmlUiAnimationHandle> Handles;
    const TArray<FRmlUiAnimationContributionSpec> DuplicateOrders{{0, true}, {0, true}};
    TestEqual(TEXT("duplicate contribution order is rejected atomically"),
        Runtime.PlayContributionBindings(Bindings, DuplicateOrders, Handles), 0);
    TestTrue(TEXT("rejected contribution batch creates no entity"), Handles.IsEmpty());

    int32 CompletedCount = 0;
    TArray<FRmlUiAnimationCompletionCallback> Callbacks;
    Callbacks.Add([&CompletedCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
    {
        CompletedCount += Reason == ERmlUiAnimationCompletionReason::Completed ? 1 : 0;
    });
    Callbacks.Add([&CompletedCount](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
    {
        CompletedCount += Reason == ERmlUiAnimationCompletionReason::Completed ? 1 : 0;
    });
    const TArray<FRmlUiAnimationContributionSpec> Contributions{{0, true}, {1, true}};
    TestEqual(TEXT("two same-target contributions start together"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles, MoveTemp(Callbacks)), 2);
    TestEqual(TEXT("two contribution entities are active"), Runtime.GetActiveAnimationCount(), 2);

    const auto TestVisualOpacity = [this, View, Node](const TCHAR* Label, float Expected)
    {
        RmlUE_SlateFrame Frame{};
        float VisualMultiplier = -1.0f;
        char ComputedValue[64]{};
        const bool bRendered = RmlUE_RenderSlate(View, &Frame) != 0 &&
            FindVisualOpacity(Frame, Node, VisualMultiplier) &&
            RmlUE_GetComputedProperty(View, Node, "opacity", ComputedValue, sizeof(ComputedValue)) != 0;
        const float EffectiveOpacity = VisualMultiplier *
            FCString::Atof(UTF8_TO_TCHAR(ComputedValue));
        TestTrue(FString::Printf(TEXT("%s (actual %.4f, expected %.4f)"),
            Label, EffectiveOpacity, Expected),
            bRendered && FMath::IsNearlyEqual(EffectiveOpacity, Expected, 0.002f));
    };

    Runtime.Advance(0.25f);
    TestVisualOpacity(TEXT("delayed upper contribution does not mask lower contribution"), 0.25f);
    Runtime.Advance(0.375f);
    TestVisualOpacity(TEXT("active upper contribution wins by order"), 0.75f);
    Runtime.Advance(0.125f);
    TestVisualOpacity(TEXT("upper contribution commits its terminal value on completion frame"), 1.0f);
    TestEqual(TEXT("completed upper contribution retires after commit"), Runtime.GetActiveAnimationCount(), 1);
    TestEqual(TEXT("upper completion callback fires once"), CompletedCount, 1);
    Runtime.Advance(0.001f);
    TestVisualOpacity(TEXT("unchanged lower hold resumes on the next tick"), 0.5f);
    Runtime.Advance(0.251f);
    TestEqual(TEXT("both contributions complete"), CompletedCount, 2);
    TestEqual(TEXT("all contribution entities retire"), Runtime.GetActiveAnimationCount(), 0);

    TestEqual(TEXT("completed contribution group can be reused"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), 2);
    Runtime.Advance(0.25f);
    TestEqual(TEXT("reused contribution group evaluates both new handles"),
        Runtime.GetActiveAnimationCount(), 2);
    TestTrue(TEXT("first reused contribution cancels"), Runtime.Cancel(Handles[0]));
    TestTrue(TEXT("second reused contribution cancels"), Runtime.Cancel(Handles[1]));
    TestEqual(TEXT("reused contribution group is fully released"), Runtime.GetActiveAnimationCount(), 0);

    TestEqual(TEXT("pause/resume contribution scenario starts both layers"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), 2);
    Runtime.Advance(0.625f);
    TestVisualOpacity(TEXT("upper contribution wins before pause"), 0.75f);
    TestTrue(TEXT("upper contribution pauses"), Runtime.Pause(Handles[1]));
    Runtime.Advance(0.001f);
    TestVisualOpacity(TEXT("lower contribution takes over while upper is paused"), 0.5f);
    TestTrue(TEXT("upper contribution resumes"), Runtime.Resume(Handles[1]));
    Runtime.Advance(0.001f);
    TestVisualOpacity(TEXT("resumed upper contribution retakes winner"), 0.752f);
    TestTrue(TEXT("lower pause/resume contribution cancels"), Runtime.Cancel(Handles[0]));
    TestTrue(TEXT("upper pause/resume contribution cancels"), Runtime.Cancel(Handles[1]));
    TestEqual(TEXT("pause/resume contribution group is fully released"),
        Runtime.GetActiveAnimationCount(), 0);

    TestEqual(TEXT("playback-rate boundary scenario starts both layers"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), 2);
    Runtime.Advance(0.25f);
    TestVisualOpacity(TEXT("lower contribution leads before accelerated boundary"), 0.25f);
    TestTrue(TEXT("hidden upper contribution accepts playback-rate change"),
        Runtime.SetPlaybackRate(Handles[1], 2.0));
    Runtime.Advance(0.124f);
    TestVisualOpacity(TEXT("accelerated upper contribution remains hidden before boundary"), 0.374f);
    Runtime.Advance(0.0011f);
    TestVisualOpacity(TEXT("accelerated upper contribution activates at recomputed boundary"), 0.5004f);
    Runtime.Advance(0.0625f);
    TestVisualOpacity(TEXT("accelerated upper contribution uses updated playback rate"), 0.7504f);
    TestTrue(TEXT("lower playback-rate contribution cancels"), Runtime.Cancel(Handles[0]));
    TestTrue(TEXT("upper playback-rate contribution cancels"), Runtime.Cancel(Handles[1]));
    TestEqual(TEXT("playback-rate contribution group is fully released"),
        Runtime.GetActiveAnimationCount(), 0);

    TestEqual(TEXT("stale-boundary compaction scenario starts both layers"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), 2);
    Runtime.Advance(0.1f);
    for (int32 Iteration = 0; Iteration < 80; ++Iteration)
    {
        TestTrue(TEXT("hidden contribution rate rebase succeeds during boundary churn"),
            Runtime.SetPlaybackRate(Handles[1], 1.0));
        Runtime.Advance(0.0001f);
    }
    TestVisualOpacity(TEXT("boundary churn keeps delayed upper contribution hidden"), 0.108f);
    TestTrue(TEXT("hidden contribution rate changes after boundary compaction"),
        Runtime.SetPlaybackRate(Handles[1], 2.0));
    Runtime.Advance(0.1961f);
    TestVisualOpacity(TEXT("compacted boundary heap preserves recomputed activation"), 0.5004f);
    TestTrue(TEXT("lower boundary-compaction contribution cancels"), Runtime.Cancel(Handles[0]));
    TestTrue(TEXT("upper boundary-compaction contribution cancels"), Runtime.Cancel(Handles[1]));
    TestEqual(TEXT("boundary-compaction contribution group is fully released"),
        Runtime.GetActiveAnimationCount(), 0);

    TestEqual(TEXT("delayed-view scheduling scenario starts both layers"),
        Runtime.PlayContributionBindings(Bindings, Contributions, Handles), 2);
    TestTrue(TEXT("lower contribution pauses before first sample"), Runtime.Pause(Handles[0]));
    Runtime.Advance(0.0f);
    TestFalse(TEXT("fully delayed or paused view has no per-frame animation work"),
        Runtime.HasActiveAnimations(View));
    TestTrue(TEXT("fully delayed view exposes its next activation deadline"),
        FMath::IsNearlyEqual(Runtime.GetNextWakeDelaySeconds(View), 0.5, 0.0001));
    Runtime.Advance(0.499f);
    TestFalse(TEXT("view remains asleep before contribution boundary"),
        Runtime.HasActiveAnimations(View));
    TestTrue(TEXT("remaining wake delay tracks runtime time"),
        FMath::IsNearlyEqual(Runtime.GetNextWakeDelaySeconds(View), 0.001, 0.0001));
    Runtime.Advance(0.0011f);
    TestTrue(TEXT("view becomes frame-active after contribution boundary"),
        Runtime.HasActiveAnimations(View));
    TestTrue(TEXT("paused lower scheduling contribution cancels"), Runtime.Cancel(Handles[0]));
    TestTrue(TEXT("active upper scheduling contribution cancels"), Runtime.Cancel(Handles[1]));
    TestEqual(TEXT("delayed-view scheduling group is fully released"),
        Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("released view has no animation wake deadline"),
        Runtime.GetNextWakeDelaySeconds(View), TNumericLimits<double>::Max());

    int32 HiddenCompletionCount = 0;
    TArray<FRmlUiAnimationCompletionCallback> HiddenCallbacks;
    HiddenCallbacks.Add([&HiddenCompletionCount](FRmlUiAnimationHandle,
        ERmlUiAnimationCompletionReason Reason)
    {
        HiddenCompletionCount += Reason == ERmlUiAnimationCompletionReason::Completed ? 1 : 0;
    });
    HiddenCallbacks.Add([](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason) {});
    const TArray<FRmlUiAnimationBindingHandle> HiddenCompletionBindings{UpperBinding, LowerBinding};
    TestEqual(TEXT("hidden completion scenario starts both contributions"),
        Runtime.PlayContributionBindings(HiddenCompletionBindings, Contributions, Handles,
            MoveTemp(HiddenCallbacks)), 2);
    Runtime.Advance(0.75f);
    TestEqual(TEXT("occluded lower-order contribution completes on schedule"), HiddenCompletionCount, 1);
    TestEqual(TEXT("only the higher-order contribution remains after hidden completion"),
        Runtime.GetActiveAnimationCount(), 1);
    TestTrue(TEXT("remaining higher-order contribution cancels"), Runtime.Cancel(Handles[1]));

    TestTrue(TEXT("lower contribution binding released"), Runtime.ReleaseBinding(LowerBinding));
    TestTrue(TEXT("upper contribution binding released"), Runtime.ReleaseBinding(UpperBinding));
    TestTrue(TEXT("lower contribution definition released"), Runtime.ReleaseDefinition(LowerDefinition));
    TestTrue(TEXT("upper contribution definition released"), Runtime.ReleaseDefinition(UpperDefinition));
    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationRuntimeNativeOpacityTest,
    "RmlUi.Animation.MovieSceneRuntime.NativeOpacityBatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationRuntimeNativeOpacityTest::RunTest(const FString& Parameters)
{
    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    TestNotNull(TEXT("native Slate view created"), View);
    if (!View) return false;

    const char* Markup = "<rml><head><style>body{margin:0;}#a,#b{display:block;opacity:1;width:10px;height:10px;background:#fff;}#c{position:absolute;left:20px;top:30px;width:10px;height:10px;background:#fff;transform-origin:0 0;transform:translate(0px,0px);}</style></head><body><div id='a'/><div id='b'/><div id='c'/></body></rml>";
    TestTrue(TEXT("document loaded"), RmlUE_LoadDocumentFromMemory(View, Markup, "animation-runtime-test.rml") != 0);
    TestTrue(TEXT("initial layout completed"), RmlUE_Update(View) != 0);
    const RmlUE_Node NodeA = RmlUE_FindNode(View, "a");
    const RmlUE_Node NodeB = RmlUE_FindNode(View, "b");
    const RmlUE_Node NodeC = RmlUE_FindNode(View, "c");
    TestTrue(TEXT("animation nodes found"), NodeA != 0 && NodeB != 0 && NodeC != 0);

    FRmlUiAnimationRuntime Runtime;
    FRmlUiFloatAnimationDesc Desc;
    Desc.From = 1.0f;
    Desc.To = 0.0f;
    Desc.DurationSeconds = 1.0;
    const FRmlUiAnimationHandle Handle = Runtime.PlayNodeFloat(
        View, NodeA, ERmlUiAnimatedProperty::Opacity, Desc);
    TestTrue(TEXT("native opacity track accepted"), Handle.IsValid());
    Runtime.Advance(0.5f);

    char Value[64]{};
    TestTrue(TEXT("computed opacity readable"), RmlUE_GetComputedProperty(View, NodeA, "opacity", Value, sizeof(Value)) != 0);
    TestTrue(TEXT("visual midpoint avoids computed-style mutation"), FMath::IsNearlyEqual(FCString::Atof(UTF8_TO_TCHAR(Value)), 1.0f));
    RmlUE_SlateFrame VisualFrame{};
    TestTrue(TEXT("visual midpoint frame rendered"), RmlUE_RenderSlate(View, &VisualFrame) != 0);
    float VisualOpacity = 0.0f;
    TestTrue(TEXT("ECS midpoint committed through Slate visual sink"),
        FindVisualOpacity(VisualFrame, NodeA, VisualOpacity) && FMath::IsNearlyEqual(VisualOpacity, 0.5f));

    TestTrue(TEXT("animated node removed"), RmlUE_RemoveNode(View, NodeA) != 0);
    Runtime.Advance(0.1f);
    TestEqual(TEXT("stale node track retired after failed batch validation"), Runtime.GetActiveAnimationCount(), 0);
    RmlUE_SlateFrame TransformBaseline{};
    TestTrue(TEXT("Transform2D baseline snapshot refreshed after node removal"),
        RmlUE_RenderSlate(View, &TransformBaseline) != 0 && TransformBaseline.Replayed == 0);

    FRmlUiTransform2DAnimationDesc TransformDesc;
    TransformDesc.To.TranslationX = 20.0f;
    TransformDesc.To.TranslationY = 10.0f;
    TransformDesc.DurationSeconds = 1.0;
    TestTrue(TEXT("native Transform2D track accepted"), Runtime.PlayNodeTransform2D(View, NodeC, TransformDesc).IsValid());
    Runtime.Advance(0.5f);
    RmlUE_NodeMetrics Metrics{};
    RmlUE_LayoutInfo Layout{};
    TestTrue(TEXT("transformed geometry measured"), RmlUE_MeasureNodes(View, &NodeC, 1, &Metrics, &Layout) == 1);
    TestTrue(TEXT("ECS Transform2D midpoint updates geometry without Context update"),
        FMath::IsNearlyEqual(Metrics.X, 30.0f) && FMath::IsNearlyEqual(Metrics.Y, 35.0f));
    RmlUE_SlateFrame TransformFrame{};
    TestTrue(TEXT("ECS Transform2D midpoint replays through Slate visual sink"),
        RmlUE_RenderSlate(View, &TransformFrame) != 0 && TransformFrame.Replayed == 1 &&
        TransformFrame.VisualDeltaCount == 1 && TransformFrame.VisualDeltas &&
        TransformFrame.VisualDeltas[0].Node == NodeC && TransformFrame.VisualDeltas[0].TransformChanged);
    Runtime.CancelNodeAnimation(View, NodeC, ERmlUiAnimatedProperty::Transform2D);
    TestEqual(TEXT("Transform2D cancellation removes entity"), Runtime.GetActiveAnimationCount(), 0);

    TestTrue(TEXT("cancellable visual opacity track accepted"), Runtime.PlayNodeFloat(
        View, NodeB, ERmlUiAnimatedProperty::Opacity, Desc).IsValid());
    Runtime.Advance(0.25f);
    TestTrue(TEXT("visual opacity cancellation accepted"),
        Runtime.CancelNodeAnimation(View, NodeB, ERmlUiAnimatedProperty::Opacity));
    Value[0] = '\0';
    TestTrue(TEXT("cancelled visual opacity readable"),
        RmlUE_GetComputedProperty(View, NodeB, "opacity", Value, sizeof(Value)) != 0);
    TestTrue(TEXT("cancellation promotes last visual opacity into property tree"),
        FMath::IsNearlyEqual(FCString::Atof(UTF8_TO_TCHAR(Value)), 0.75f));
    RmlUE_SlateFrame CancelledFrame{};
    TestTrue(TEXT("cancelled visual opacity frame rendered"), RmlUE_RenderSlate(View, &CancelledFrame) != 0);
    VisualOpacity = 0.0f;
    TestTrue(TEXT("cancellation clears visual override to neutral multiplier"),
        FindVisualOpacity(CancelledFrame, NodeB, VisualOpacity) && FMath::IsNearlyEqual(VisualOpacity, 1.0f));

    bool bTeardownReentryRejected = false;
    TestTrue(TEXT("second native opacity track accepted"), Runtime.PlayNodeFloat(
        View, NodeB, ERmlUiAnimatedProperty::Opacity, Desc,
        [&Runtime, View, NodeB, Desc, &bTeardownReentryRejected](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason)
        {
            bTeardownReentryRejected = !Runtime.PlayNodeFloat(
                View, NodeB, ERmlUiAnimatedProperty::Opacity, Desc).IsValid();
        }).IsValid());
    Runtime.CancelViewAnimations(View);
    TestTrue(TEXT("view teardown rejects animation created by cancellation callback"), bTeardownReentryRejected);
    TestEqual(TEXT("view teardown cancels every native target"), Runtime.GetActiveAnimationCount(), 0);
    TestEqual(TEXT("view teardown releases legacy definitions"), Runtime.GetDefinitionCount(), 0);
    TestEqual(TEXT("view teardown releases legacy bindings"), Runtime.GetBindingCount(), 0);
    RmlUE_DestroyView(View);
    return true;
}

class FRmlUiAnimationVisualOpacityCapture final : public IAutomationLatentCommand
{
public:
    explicit FRmlUiAnimationVisualOpacityCapture(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            bPerformanceWasEnabled = FRmlUiPerformance::IsEnabled();
            FRmlUiPerformance::SetEnabled(true);
            FRmlUiPerformance::Reset();
            const FString Document = TEXT(R"RML(
<rml><head><style>
body { width:200px; height:170px; margin:0; background:#000; }
#box { display:block; position:absolute; left:20px; top:20px; width:80px; height:80px; background:#fff; opacity:1; }
#clip { display:block; position:absolute; left:110px; top:20px; width:30px; height:30px; overflow:hidden; border-radius:8px; transform-origin:0 0; transform:translate(0px,0px); }
#clip-child { display:block; width:60px; height:30px; background:#0f0; }
#nested-root { display:block; position:absolute; left:20px; top:125px; width:60px; height:35px; transform-origin:0 0; transform:translate(0px,0px); }
#nested-outer { display:block; width:50px; height:30px; overflow:hidden; border-radius:8px; }
#nested-inner { display:block; position:relative; left:8px; top:5px; width:30px; height:20px; overflow:hidden; border-radius:5px; transform-origin:0 0; transform:translate(0px,0px); }
#nested-fill { display:block; width:60px; height:30px; background:#00f; }
</style></head><body><div id="box"/><div id="clip"><div id="clip-child"/></div><div id="nested-root"><div id="nested-outer"><div id="nested-inner"><div id="nested-fill"/></div></div></div></body></rml>
)RML");
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(true).InlineDocument(Document)
                .SourcePath(TEXT("/animation-visual-opacity.rml")).DesiredSize(FVector2D(200, 170));
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi animation visual opacity verification")))
                .ClientSize(FVector2D(200, 170)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)
                [Widget.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            StartSeconds = FPlatformTime::Seconds();
            return false;
        }

        if (!Runtime.IsValid())
        {
            RmlUE_SlateScheduleState Schedule{};
            const bool bReplayReady = RmlUE_GetSlateScheduleState(Widget->GetNativeView(), &Schedule) && Schedule.CanReplay;
            if ((!bReplayReady || Widget->GetReadySlateRhiGeometryCount() == 0) &&
                FPlatformTime::Seconds() - StartSeconds < 5.0)
            {
                return false;
            }
            Test->TestTrue(TEXT("visual opacity fixture reaches retained replay state"), bReplayReady);
            const uint64 ContentRevisionBeforeAnimation = Schedule.ContentRevision;
            const uint64 VisualRevisionBeforeAnimation = Schedule.VisualRevision;
            const uint32 Node = RmlUE_FindNode(Widget->GetNativeView(), "box");
            const uint32 ClipNode = RmlUE_FindNode(Widget->GetNativeView(), "clip");
            const uint32 NestedRootNode = RmlUE_FindNode(Widget->GetNativeView(), "nested-root");
            const uint32 NestedInnerNode = RmlUE_FindNode(Widget->GetNativeView(), "nested-inner");
            Test->TestTrue(TEXT("visual opacity pixel fixture node found"), Node != 0);
            Test->TestTrue(TEXT("dynamic clip-mask pixel fixture node found"), ClipNode != 0);
            Test->TestTrue(TEXT("nested clip-mask pixel fixture nodes found"),
                NestedRootNode != 0 && NestedInnerNode != 0);
            Runtime = MakeUnique<FRmlUiAnimationRuntime>();
            FRmlUiFloatAnimationDesc Desc;
            Desc.From = 1.0f;
            Desc.To = 0.0f;
            Desc.DurationSeconds = 1.0;
            Test->TestTrue(TEXT("visual opacity pixel animation accepted"), Runtime->PlayNodeFloat(
                Widget->GetNativeView(), Node, ERmlUiAnimatedProperty::Opacity, Desc).IsValid());
            FRmlUiTransform2DAnimationDesc ClipTransform;
            ClipTransform.To.TranslationX = 40.0f;
            ClipTransform.DurationSeconds = 1.0;
            Test->TestTrue(TEXT("dynamic clip-mask pixel animation accepted"), Runtime->PlayNodeTransform2D(
                Widget->GetNativeView(), ClipNode, ClipTransform).IsValid());
            FRmlUiTransform2DAnimationDesc NestedRootTransform;
            NestedRootTransform.To.TranslationX = 40.0f;
            NestedRootTransform.DurationSeconds = 1.0;
            Test->TestTrue(TEXT("nested clip-mask parent animation accepted"), Runtime->PlayNodeTransform2D(
                Widget->GetNativeView(), NestedRootNode, NestedRootTransform).IsValid());
            FRmlUiTransform2DAnimationDesc NestedInnerTransform;
            NestedInnerTransform.To.TranslationX = 10.0f;
            NestedInnerTransform.DurationSeconds = 1.0;
            Test->TestTrue(TEXT("nested clip-mask owner animation accepted"), Runtime->PlayNodeTransform2D(
                Widget->GetNativeView(), NestedInnerNode, NestedInnerTransform).IsValid());
            RmlUE_SlateScheduleState PlayedSchedule{};
            RmlUE_GetSlateScheduleState(Widget->GetNativeView(), &PlayedSchedule);
            Test->TestEqual(TEXT("starting MovieScene animation preserves retained content revision"),
                PlayedSchedule.ContentRevision, ContentRevisionBeforeAnimation);
            Runtime->Advance(0.5f);
            RmlUE_SlateScheduleState AnimatedSchedule{};
            Test->TestTrue(TEXT("visual opacity update preserves content revision"),
                RmlUE_GetSlateScheduleState(Widget->GetNativeView(), &AnimatedSchedule) &&
                AnimatedSchedule.ContentRevision == ContentRevisionBeforeAnimation);
            Test->TestTrue(TEXT("visual opacity update advances visual revision"),
                AnimatedSchedule.VisualRevision != VisualRevisionBeforeAnimation);
            Test->TestTrue(TEXT("visual opacity midpoint command frame built"), Widget->RenderFrame(200, 170));
            const FRmlUiPerformanceSnapshot MidpointPerformance = FRmlUiPerformance::Snapshot();
            Test->TestTrue(TEXT("initial frame decodes complete Slate draw records"),
                MidpointPerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::DrawRecordsDecoded) > 0);
            Test->TestTrue(TEXT("visual-only frame reuses retained Slate draw records"),
                MidpointPerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::DrawRecordsReused) > 0);
            Test->TestTrue(TEXT("visual-only frame applies a targeted Slate visual delta"),
                MidpointPerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::VisualDeltaUpdates) > 0);
            FlushRenderingCommands();

            TArray<FColor> Pixels;
            FIntVector Size = FIntVector::ZeroValue;
            const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size);
            Test->TestTrue(TEXT("visual opacity midpoint screenshot captured"),
                bCaptured && Size.X == 200 && Size.Y == 170 && Pixels.Num() == Size.X * Size.Y);
            if (bCaptured && Pixels.Num() == Size.X * Size.Y)
            {
                const FColor Midpoint = Pixels[40 * Size.X + 40];
                // The shader applies opacity in linear space before the SDR target encodes to sRGB.
                const uint8 ExpectedSrgbHalf = FLinearColor(0.5f, 0.5f, 0.5f).ToFColorSRGB().R;
                Test->TestTrue(TEXT("RHI visual opacity produces linear half-white pixel"),
                    FMath::Abs(int32(Midpoint.R) - int32(ExpectedSrgbHalf)) <= 8 &&
                    FMath::Abs(int32(Midpoint.G) - int32(ExpectedSrgbHalf)) <= 8 &&
                    FMath::Abs(int32(Midpoint.B) - int32(ExpectedSrgbHalf)) <= 8);
                const FColor OldClipCenter = Pixels[35 * Size.X + 115];
                const FColor MovedClipCenter = Pixels[35 * Size.X + 145];
                const FColor MovedRoundedCorner = Pixels[21 * Size.X + 131];
                Test->TestTrue(TEXT("retained dynamic mask removes content from its old position"),
                    OldClipCenter.R <= 12 && OldClipCenter.G <= 12 && OldClipCenter.B <= 12);
                Test->TestTrue(TEXT("retained dynamic mask and content reach their new position"),
                    MovedClipCenter.G >= 220 && MovedClipCenter.R <= 20 && MovedClipCenter.B <= 20);
                Test->TestTrue(TEXT("retained dynamic mask preserves rounded clipping at the new position"),
                    MovedRoundedCorner.R <= 20 && MovedRoundedCorner.G <= 20 && MovedRoundedCorner.B <= 20);
                const FColor OldNestedCenter = Pixels[140 * Size.X + 35];
                const FColor MovedNestedCenter = Pixels[140 * Size.X + 65];
                const FColor MovedNestedCorner = Pixels[130 * Size.X + 53];
                Test->TestTrue(TEXT("retained nested masks remove content from old coordinates"),
                    OldNestedCenter.R <= 20 && OldNestedCenter.G <= 20 && OldNestedCenter.B <= 20);
                Test->TestTrue(TEXT("retained nested mask owners compose into the new position"),
                    MovedNestedCenter.B >= 220 && MovedNestedCenter.R <= 20 && MovedNestedCenter.G <= 20);
                Test->TestTrue(TEXT("retained nested masks preserve the inner rounded corner"),
                    MovedNestedCorner.R <= 20 && MovedNestedCorner.G <= 20 && MovedNestedCorner.B <= 20);
                const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiTests"));
                IFileManager::Get().MakeDirectory(*Directory, true);
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
                    TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                Test->TestTrue(TEXT("visual opacity midpoint screenshot saved"), FFileHelper::SaveArrayToFile(
                    Png, *FPaths::Combine(Directory, TEXT("animation-visual-opacity-midpoint.png"))));
            }

            Runtime->Advance(0.5f);
            Test->TestEqual(TEXT("visual opacity and dynamic clip-mask animations complete"), Runtime->GetActiveAnimationCount(), 0);
            Test->TestTrue(TEXT("final property-backed command frame built"), Widget->RenderFrame(200, 170));
            FlushRenderingCommands();
            Pixels.Reset();
            Size = FIntVector::ZeroValue;
            const bool bFinalCaptured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size);
            Test->TestTrue(TEXT("visual opacity final screenshot captured"),
                bFinalCaptured && Size.X == 200 && Size.Y == 170 && Pixels.Num() == Size.X * Size.Y);
            if (bFinalCaptured && Pixels.Num() == Size.X * Size.Y)
            {
                const FColor Final = Pixels[40 * Size.X + 40];
                Test->TestTrue(TEXT("final opacity property produces background pixel"),
                    Final.R <= 12 && Final.G <= 12 && Final.B <= 12);
                const FColor FinalClipCenter = Pixels[35 * Size.X + 155];
                Test->TestTrue(TEXT("final clip-mask property frame preserves moved clipped content"),
                    FinalClipCenter.G >= 220 && FinalClipCenter.R <= 20 && FinalClipCenter.B <= 20);
                const FColor FinalNestedCenter = Pixels[140 * Size.X + 90];
                Test->TestTrue(TEXT("final nested mask property frame preserves composed clipped content"),
                    FinalNestedCenter.B >= 220 && FinalNestedCenter.R <= 20 && FinalNestedCenter.G <= 20);
            }

            Widget->ShutdownNative();
            FlushRenderingCommands();
            FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
            Widget.Reset();
            Window.Reset();
            Runtime.Reset();
            FRmlUiPerformance::SetEnabled(bPerformanceWasEnabled);
            return true;
        }
        return false;
    }

private:
    FAutomationTestBase* Test = nullptr;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SRmlUiWidget> Widget;
    TUniquePtr<FRmlUiAnimationRuntime> Runtime;
    double StartSeconds = 0.0;
    bool bPerformanceWasEnabled = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiAnimationVisualOpacityPixelsTest,
    "RmlUi.Animation.VisualSink.SlateRhiOpacityPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiAnimationVisualOpacityPixelsTest::RunTest(const FString& Parameters)
{
    AddCommand(new FRmlUiAnimationVisualOpacityCapture(this));
    return true;
}

class FRmlUiSlateScheduledTickCapture final : public IAutomationLatentCommand
{
public:
    explicit FRmlUiSlateScheduledTickCapture(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (!Window.IsValid())
        {
            bPerformanceWasEnabled = FRmlUiPerformance::IsEnabled();
            FRmlUiPerformance::SetEnabled(true);
            FRmlUiPerformance::Reset();
            const FString Document = TEXT(R"RML(
<rml><head><style>
body { width:160px; height:120px; margin:0; background:#000; }
#box { display:block; width:80px; height:80px; background:#fff; opacity:1; }
@keyframes schedule-pulse { from { opacity:1; } to { opacity:0.2; } }
</style></head><body><div id="box"/></body></rml>
)RML");
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(true).UsePaintCache(true).InlineDocument(Document)
                .SourcePath(TEXT("/scheduled-tick.rml")).DesiredSize(FVector2D(160, 120));
            Widget->OnBeforeRender.AddLambda([this](float) { ++BeforeRenderCalls; });
            InvalidationPanel = SNew(SInvalidationPanel)
                .DebugName(TEXT("RmlUi scheduled tick paint cache"))
                [
                    Widget.ToSharedRef()
                ];
            Test->TestTrue(TEXT("RmlUi paint invalidation root can cache"), InvalidationPanel->GetCanCache());
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi scheduled tick verification")))
                .ClientSize(FVector2D(160, 120)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)
                [InvalidationPanel.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            StartSeconds = Now;
            return false;
        }

        if (Phase == 0)
        {
            if ((Widget->GetFrameNumber() < 1 || Widget->GetReadySlateRhiGeometryCount() == 0) &&
                Now - StartSeconds < 5.0) return false;
            Test->TestTrue(TEXT("scheduled widget produced its initial retained frame"), Widget->GetFrameNumber() >= 1);
            if (ObservedFrame != Widget->GetFrameNumber())
            {
                ObservedFrame = Widget->GetFrameNumber();
                StableSince = Now;
                return false;
            }
            const uint64 PaintCalls = FRmlUiPerformance::Snapshot().CallCount(
                ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceStage::OnPaint);
            if (ObservedPaintCalls != PaintCalls)
            {
                ObservedPaintCalls = PaintCalls;
                StableSince = Now;
                if (Now - StartSeconds < 5.0) return false;
            }
            if ((!Widget->IsSlatePaintCacheReady() || Widget->GetCanTick() || Now - StableSince < 0.2) &&
                Now - StartSeconds < 5.0) return false;
            Test->TestTrue(TEXT("idle measurement starts from an active Slate paint cache"),
                Widget->IsSlatePaintCacheReady());
            if (Widget->GetCanTick())
            {
                Test->AddInfo(FString::Printf(TEXT("Slate idle timeout: schedule=%d cache_eligible=%d cache_ready=%d ready_rhi=%d"),
                    Widget->HasSlateSchedule(), Widget->IsSlatePaintCacheEligible(),
                    Widget->IsSlatePaintCacheReady(), Widget->GetReadySlateRhiGeometryCount()));
            }
            IdleFrame = Widget->GetFrameNumber();
            IdleBeforeRenderCalls = BeforeRenderCalls;
            const FRmlUiPerformanceSnapshot IdleStart = FRmlUiPerformance::Snapshot();
            IdleTickCalls = IdleStart.CallCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceStage::Tick);
            IdlePaintCalls = IdleStart.CallCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceStage::OnPaint);
            IdleRhiDraws = IdleStart.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::RhiDraws);
            IdleFallbackDraws = IdleStart.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::FallbackDraws);
            Test->TestTrue(TEXT("simple retained frame has no paint-cache rejection"),
                Widget->GetSlatePaintCacheRejectReason() == ERmlUiPaintCacheRejectReason::None);
            Test->TestTrue(TEXT("paint-cache eligibility evaluation is attributed"),
                IdleStart.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::PaintCacheEvaluations) > 0);
            Test->TestTrue(TEXT("eligible paint-cache frames are attributed"),
                IdleStart.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::PaintCacheEligibleFrames) > 0);
            Test->TestTrue(TEXT("paint-cache activation is attributed"),
                IdleStart.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::PaintCacheActivations) > 0);
            StartSeconds = Now;
            Phase = 1;
            return false;
        }

        if (Phase == 1)
        {
            if (Now - StartSeconds < 0.1) return false;
            const FRmlUiPerformanceSnapshot IdleEnd = FRmlUiPerformance::Snapshot();
            Test->TestEqual(TEXT("idle Slate scheduling skips bridge frames"), Widget->GetFrameNumber(), IdleFrame);
            Test->TestEqual(TEXT("idle Slate scheduling suspends before-render services"),
                BeforeRenderCalls, IdleBeforeRenderCalls);
            Test->TestFalse(TEXT("idle Slate widget leaves the per-frame Tick list"), Widget->GetCanTick());
            Test->TestEqual(TEXT("idle Slate widget performs no additional Tick"),
                IdleEnd.CallCount(ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceStage::Tick), IdleTickCalls);
            Test->TestEqual(TEXT("invalidation root reuses cached Slate paint elements while idle"),
                IdleEnd.CallCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceStage::OnPaint), IdlePaintCalls);
            Test->TestEqual(TEXT("cached Slate paint suppresses repeated render-thread draws"),
                IdleEnd.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::RhiDraws), IdleRhiDraws);

            const uint32 Node = RmlUE_FindNode(Widget->GetNativeView(), "box");
            RmlUE_AnimatedPropertyUpdate Update{Widget->GetNativeView(), Node,
                RMLUE_ANIMATED_PROPERTY_OPACITY, {0.5f, 0.f, 0.f, 0.f, 0.f}};
            uint8 Accepted = 0;
            Test->TestTrue(TEXT("scheduled visual update accepted"), Node != 0 &&
                RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1);
            Widget->RequestScheduledRender();
            WakeFrame = Widget->GetFrameNumber();
            StartSeconds = Now;
            Phase = 2;
            return false;
        }

        if (Phase == 2)
        {
            const FRmlUiPerformanceSnapshot WakePerformance = FRmlUiPerformance::Snapshot();
            const bool bFrameAdvanced = Widget->GetFrameNumber() > WakeFrame;
            const bool bPaintInvalidated = WakePerformance.CallCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceStage::OnPaint) > IdlePaintCalls;
            const bool bNativePaintRebuilt = WakePerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::FallbackDraws) > IdleFallbackDraws;
            if ((!bFrameAdvanced || !bPaintInvalidated || !bNativePaintRebuilt) && Now - StartSeconds < 2.0) return false;
            Test->TestTrue(TEXT("visual revision wakes a retained Slate frame"), Widget->GetFrameNumber() > WakeFrame);
            Test->TestTrue(TEXT("visual revision invalidates the cached Slate paint elements"),
                bPaintInvalidated);
            Test->TestTrue(TEXT("visual revision rebuilds cached native Slate draw elements"), bNativePaintRebuilt);
            Test->TestTrue(TEXT("visual scheduling wake is attributed"),
                WakePerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::ScheduledVisualWakes) > 0);
            Test->TestTrue(TEXT("visual scheduling is driven by a Slate active timer"),
                WakePerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::ScheduledActiveTimerWakes) > 0);
            DeadlineWakeCount = WakePerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::ScheduledDeadlineWakes);
            Test->TestTrue(TEXT("RCSS animation property accepted"), Widget->SetElementProperty(
                TEXT("box"), TEXT("animation"), TEXT("0.2s linear schedule-pulse infinite alternate")));
            WakeFrame = Widget->GetFrameNumber();
            StartSeconds = Now;
            Phase = 3;
            return false;
        }

        if (Now - StartSeconds < 0.1) return false;
        const FRmlUiPerformanceSnapshot DeadlinePerformance = FRmlUiPerformance::Snapshot();
        Test->TestTrue(TEXT("RCSS animation advances scheduled Slate frames"),
            Widget->GetFrameNumber() > WakeFrame + 1);
        Test->TestTrue(TEXT("RCSS animation is driven by RmlUi deadlines"),
            DeadlinePerformance.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::ScheduledDeadlineWakes) > DeadlineWakeCount);
        Cleanup();
        return true;
    }

private:
    void Cleanup()
    {
        if (Widget) Widget->ShutdownNative();
        FlushRenderingCommands();
        if (Window) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        InvalidationPanel.Reset();
        Widget.Reset();
        Window.Reset();
        FRmlUiPerformance::SetEnabled(bPerformanceWasEnabled);
    }

    FAutomationTestBase* Test = nullptr;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SInvalidationPanel> InvalidationPanel;
    TSharedPtr<SRmlUiWidget> Widget;
    double StartSeconds = 0.0;
    uint64 IdleFrame = 0;
    uint64 WakeFrame = 0;
    uint64 BeforeRenderCalls = 0;
    uint64 IdleBeforeRenderCalls = 0;
    uint64 DeadlineWakeCount = 0;
    uint64 IdleTickCalls = 0;
    uint64 IdlePaintCalls = 0;
    uint64 IdleRhiDraws = 0;
    uint64 IdleFallbackDraws = 0;
    uint64 ObservedFrame = 0;
    uint64 ObservedPaintCalls = 0;
    double StableSince = 0.0;
    int32 Phase = 0;
    bool bPerformanceWasEnabled = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiSlateScheduledTickTest,
    "RmlUi.Animation.Scheduling.IdleAndVisualWake",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiSlateScheduledTickTest::RunTest(const FString& Parameters)
{
    AddCommand(new FRmlUiSlateScheduledTickCapture(this));
    return true;
}

class FRmlUiSlateLayeredBoundaryWakeCapture final : public IAutomationLatentCommand
{
public:
    explicit FRmlUiSlateLayeredBoundaryWakeCapture(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (!Window.IsValid())
        {
            bPerformanceWasEnabled = FRmlUiPerformance::IsEnabled();
            FRmlUiPerformance::SetEnabled(true);
            FRmlUiPerformance::Reset();
            const FString Document = TEXT(R"RML(
<rml><head><style>
body { width:160px; height:120px; margin:0; background:#000; }
#box { display:block; width:80px; height:80px; background:#fff; opacity:1; }
</style></head><body><div id="box"/></body></rml>
)RML");
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(true).UsePaintCache(true).InlineDocument(Document)
                .SourcePath(TEXT("/layered-boundary-wake.rml")).DesiredSize(FVector2D(160, 120));
            InvalidationPanel = SNew(SInvalidationPanel)
                .DebugName(TEXT("RmlUi layered animation boundary wake"))
                [
                    Widget.ToSharedRef()
                ];
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi layered boundary wake verification")))
                .ClientSize(FVector2D(160, 120)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)
                [InvalidationPanel.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            StartSeconds = Now;
            return false;
        }

        if (Phase == 0)
        {
            const bool bIdle = Widget->GetFrameNumber() >= 1 && !Widget->GetCanTick() &&
                !Widget->HasScheduledActiveTimerForTesting();
            if (!bIdle)
            {
                StableSince = 0.0;
                if (Now - StartSeconds >= 5.0)
                {
                    Test->AddError(TEXT("layered boundary widget did not reach its idle scheduled state"));
                    Cleanup();
                    return true;
                }
                return false;
            }
            if (StableSince == 0.0)
            {
                StableSince = Now;
                return false;
            }
            if (Now - StableSince < 0.15)
            {
                return false;
            }

            Runtime = &FRmlUiUnrealModule::Get().GetAnimationRuntime();
            Node = RmlUE_FindNode(Widget->GetNativeView(), "box");
            FRmlUiFloatAnimationDefinition Lower;
            Lower.From = 0.0f;
            Lower.To = 0.5f;
            Lower.DurationSeconds = 1.5;
            FRmlUiFloatAnimationDefinition Upper;
            Upper.From = 0.5f;
            Upper.To = 1.0f;
            Upper.DelaySeconds = 2.0;
            Upper.DurationSeconds = 0.25;
            LowerDefinition = Runtime->RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Lower);
            UpperDefinition = Runtime->RegisterFloatDefinition(ERmlUiAnimatedProperty::Opacity, Upper);
            LowerBinding = Runtime->BindNode(LowerDefinition, Widget->GetNativeView(), Node);
            UpperBinding = Runtime->BindNode(UpperDefinition, Widget->GetNativeView(), Node);
            const TArray<FRmlUiAnimationBindingHandle> Bindings{LowerBinding, UpperBinding};
            const TArray<FRmlUiAnimationContributionSpec> Contributions{{0, true}, {1, true}};
            Test->TestTrue(TEXT("layered boundary test node and resources are valid"), Node != 0 &&
                LowerDefinition.IsValid() && UpperDefinition.IsValid() &&
                LowerBinding.IsValid() && UpperBinding.IsValid());
            Test->TestEqual(TEXT("layered boundary contributions start together"),
                Runtime->PlayContributionBindings(Bindings, Contributions, Handles), 2);
            Test->TestTrue(TEXT("lower layered contribution pauses before its first sample"),
                Handles.Num() == 2 && Runtime->Pause(Handles[0]));
            Runtime->Advance(0.0f);
            Test->TestFalse(TEXT("delayed layered view does not require per-frame evaluation"),
                Runtime->HasActiveAnimations(Widget->GetNativeView()));
            Test->TestTrue(TEXT("delayed layered view publishes its Slate wake deadline"),
                FMath::IsNearlyEqual(Runtime->GetNextWakeDelaySeconds(Widget->GetNativeView()), 2.0, 0.002));
            StartFrame = Widget->GetFrameNumber();
            ObservedFrame = StartFrame;
            StableSince = 0.0;
            StartSeconds = Now;
            Phase = 1;
            return false;
        }

        if (Phase == 1)
        {
            const uint64 CurrentFrame = Widget->GetFrameNumber();
            if (Widget->GetCanTick() || CurrentFrame != ObservedFrame)
            {
                ObservedFrame = CurrentFrame;
                StableSince = 0.0;
                if (Now - StartSeconds >= 1.0)
                {
                    Test->AddError(TEXT("layered boundary widget did not settle after its initial commit"));
                    Cleanup();
                    return true;
                }
                return false;
            }
            if (StableSince == 0.0)
            {
                StableSince = Now;
                return false;
            }
            if (Now - StableSince < 0.15)
            {
                return false;
            }
            SleepFrame = Widget->GetFrameNumber();
            const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
            SleepTickCalls = Snapshot.CallCount(
                ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceStage::Tick);
            SleepWakeCount = Snapshot.WorkCount(
                ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceWork::ScheduledActiveTimerWakes);
            Test->TestTrue(TEXT("delayed layered boundary is armed as a Slate active timer"),
                Widget->HasScheduledActiveTimerForTesting());
            StartSeconds = Now;
            Phase = 2;
            return false;
        }

        if (Phase == 2)
        {
            if (Now - StartSeconds < 0.15)
            {
                return false;
            }
            const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
            Test->TestFalse(TEXT("delayed layered widget remains outside the per-frame Tick list"),
                Widget->GetCanTick());
            Test->TestEqual(TEXT("delayed layered widget emits no bridge frame before its boundary"),
                Widget->GetFrameNumber(), SleepFrame);
            Test->TestEqual(TEXT("delayed layered widget performs no Slate Tick before its boundary"),
                Snapshot.CallCount(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceStage::Tick), SleepTickCalls);
            Phase = 3;
            StartSeconds = Now;
            return false;
        }

        if (Phase == 3)
        {
            const FRmlUiPerformanceSnapshot Snapshot = FRmlUiPerformance::Snapshot();
            const bool bBoundaryReached = Runtime->HasActiveAnimations(Widget->GetNativeView());
            const bool bFrameAdvanced = Widget->GetFrameNumber() > SleepFrame;
            const bool bTimerWoke = Snapshot.WorkCount(ERmlUiPerformanceBackend::Slate,
                ERmlUiPerformanceWork::ScheduledActiveTimerWakes) > SleepWakeCount;
            if ((!bBoundaryReached || !bFrameAdvanced || !bTimerWoke) && Now - StartSeconds < 3.0)
            {
                return false;
            }
            Test->TestTrue(TEXT("layered contribution becomes evaluable at its scheduled boundary"),
                bBoundaryReached);
            Test->TestTrue(TEXT("layered animation boundary resumes retained Slate rendering"),
                bFrameAdvanced);
            Test->TestTrue(TEXT("layered animation boundary is delivered by a Slate active timer"),
                bTimerWoke);
            ReleaseAnimationResources();
            Widget->RequestScheduledRender();
            StartSeconds = Now;
            Phase = 4;
            return false;
        }

        if (Phase == 4)
        {
            const bool bTimerReleased = !Widget->GetCanTick() &&
                !Widget->HasScheduledActiveTimerForTesting();
            if (!bTimerReleased && Now - StartSeconds < 1.0)
            {
                return false;
            }
            Test->TestEqual(TEXT("released layered view has no animation deadline"),
                Runtime->GetNextWakeDelaySeconds(Widget->GetNativeView()),
                TNumericLimits<double>::Max());
            Test->TestTrue(TEXT("released layered deadline unregisters its Slate active timer"),
                bTimerReleased);
            Cleanup();
            return true;
        }

        Cleanup();
        return true;
    }

private:
    void ReleaseAnimationResources()
    {
        if (Runtime)
        {
            for (FRmlUiAnimationHandle Handle : Handles)
            {
                Runtime->Cancel(Handle);
            }
            if (LowerBinding.IsValid()) Runtime->ReleaseBinding(LowerBinding);
            if (UpperBinding.IsValid()) Runtime->ReleaseBinding(UpperBinding);
            if (LowerDefinition.IsValid()) Runtime->ReleaseDefinition(LowerDefinition);
            if (UpperDefinition.IsValid()) Runtime->ReleaseDefinition(UpperDefinition);
        }
        Handles.Reset();
        LowerBinding = {};
        UpperBinding = {};
        LowerDefinition = {};
        UpperDefinition = {};
    }

    void Cleanup()
    {
        ReleaseAnimationResources();
        if (Widget) Widget->ShutdownNative();
        FlushRenderingCommands();
        if (Window) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        InvalidationPanel.Reset();
        Widget.Reset();
        Window.Reset();
        Runtime = nullptr;
        FRmlUiPerformance::SetEnabled(bPerformanceWasEnabled);
    }

    FAutomationTestBase* Test = nullptr;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SInvalidationPanel> InvalidationPanel;
    TSharedPtr<SRmlUiWidget> Widget;
    FRmlUiAnimationRuntime* Runtime = nullptr;
    FRmlUiAnimationDefinitionHandle LowerDefinition;
    FRmlUiAnimationDefinitionHandle UpperDefinition;
    FRmlUiAnimationBindingHandle LowerBinding;
    FRmlUiAnimationBindingHandle UpperBinding;
    TArray<FRmlUiAnimationHandle> Handles;
    uint32 Node = 0;
    double StartSeconds = 0.0;
    uint64 StartFrame = 0;
    uint64 ObservedFrame = 0;
    uint64 SleepFrame = 0;
    uint64 SleepTickCalls = 0;
    uint64 SleepWakeCount = 0;
    double StableSince = 0.0;
    int32 Phase = 0;
    bool bPerformanceWasEnabled = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiSlateLayeredBoundaryWakeTest,
    "RmlUi.Animation.Scheduling.LayeredBoundaryWake",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiSlateLayeredBoundaryWakeTest::RunTest(const FString& Parameters)
{
    AddCommand(new FRmlUiSlateLayeredBoundaryWakeCapture(this));
    return true;
}

#endif
