#include "RmlUiAnimationRuntime.h"

#include "RmlUiAnimationSystems.h"
#include "RmlUiPerformance.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformTime.h"
#include "EntitySystem/BuiltInComponentTypes.h"
#include "EntitySystem/MovieSceneComponentRegistry.h"
#include "EntitySystem/MovieSceneEntityBuilder.h"
#include "EntitySystem/MovieSceneEntityFactoryTemplates.h"
#include "EntitySystem/MovieSceneEntityMutations.h"
#include "EntitySystem/MovieSceneEntitySystemGraphs.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "EntitySystem/MovieSceneEntitySystemTask.h"
#include "RmlUiBridge.h"
#include "UObject/StrongObjectPtr.h"

namespace RmlUiAnimation
{
using namespace UE::MovieScene;

struct FComponentTypes
{
    TComponentTypeID<uint32> RecordIndex;
    TComponentTypeID<FTrack> Track;
    TComponentTypeID<FSample> Sample;
    TComponentTypeID<FOutput> Output;
    TComponentTypeID<FPreviousOutput> PreviousOutput;
    FComponentTypeID Paused;
    FComponentTypeID Occluded;
    TComponentTypeID<FTransform2DTrack> TransformTrack;
    TComponentTypeID<FTransform2DSample> TransformSample;
    TComponentTypeID<FTransform2DOutput> TransformOutput;
    TComponentTypeID<FPreviousTransform2DOutput> PreviousTransformOutput;

    static FComponentTypes& Get()
    {
        static FComponentTypes Types;
        return Types;
    }

    void EnsureRegistered()
    {
        if (!Track)
        {
            FComponentRegistry* Registry = UMovieSceneEntitySystemLinker::GetComponents();
            RecordIndex = Registry->NewComponentType<uint32>(TEXT("RmlUi animation record index"));
            Track = Registry->NewComponentType<FTrack>(TEXT("RmlUi float animation track"));
            Sample = Registry->NewComponentType<FSample>(TEXT("RmlUi animation sample"));
            Output = Registry->NewComponentType<FOutput>(TEXT("RmlUi animation output"));
            PreviousOutput = Registry->NewComponentType<FPreviousOutput>(TEXT("RmlUi previous animation output"));
            Paused = Registry->NewTag(TEXT("RmlUi paused animation"));
            Occluded = Registry->NewTag(TEXT("RmlUi occluded layered animation"));
            TransformTrack = Registry->NewComponentType<FTransform2DTrack>(TEXT("RmlUi Transform2D animation track"));
            TransformSample = Registry->NewComponentType<FTransform2DSample>(TEXT("RmlUi Transform2D animation sample"));
            TransformOutput = Registry->NewComponentType<FTransform2DOutput>(TEXT("RmlUi Transform2D animation output"));
            PreviousTransformOutput = Registry->NewComponentType<FPreviousTransform2DOutput>(TEXT("RmlUi previous Transform2D output"));
        }
    }
};

struct FTrackProgress
{
    float Normalized = 0.0f;
    bool bComplete = false;
    bool bContributes = true;
};

template <typename TrackType>
static FTrackProgress EvaluateProgress(const TrackType& Track, double TimeSeconds)
{
    const double LocalTime = Track.OriginLocalTimeSeconds +
        (TimeSeconds - Track.OriginTimeSeconds) * Track.PlaybackRate;
    const bool bReverseFirst = Track.Direction == static_cast<uint8>(ERmlUiAnimationDirection::Reverse) ||
        Track.Direction == static_cast<uint8>(ERmlUiAnimationDirection::AlternateReverse);
    if (LocalTime < 0.0)
    {
        return {bReverseFirst ? 1.0f : 0.0f, false, !Track.bSuppressBeforeStart};
    }
    if (Track.DurationSeconds == 0.0)
    {
        return {bReverseFirst ? 0.0f : 1.0f, true};
    }
    if (LocalTime == 0.0)
    {
        return {bReverseFirst ? 1.0f : 0.0f, false};
    }

    const double Duration = FMath::Max(Track.DurationSeconds, UE_DOUBLE_SMALL_NUMBER);
    const int32 Iterations = FMath::Max(Track.Iterations, 1);
    const double TotalDuration = Duration * Iterations;
    const bool bComplete = LocalTime >= TotalDuration;
    const int32 IterationIndex = bComplete
        ? Iterations - 1
        : FMath::Clamp(FMath::FloorToInt(LocalTime / Duration), 0, Iterations - 1);
    float Normalized = bComplete ? 1.0f : static_cast<float>(FMath::Fmod(LocalTime, Duration) / Duration);
    const ERmlUiAnimationDirection Direction = static_cast<ERmlUiAnimationDirection>(Track.Direction);
    const bool bReverse = Direction == ERmlUiAnimationDirection::Reverse ||
        (Direction == ERmlUiAnimationDirection::Alternate && (IterationIndex & 1) != 0) ||
        (Direction == ERmlUiAnimationDirection::AlternateReverse && (IterationIndex & 1) == 0);
    if (bReverse) Normalized = 1.0f - Normalized;
    return {Normalized, bComplete};
}

struct FEvaluateTrack
{
    explicit FEvaluateTrack(double InTimeSeconds)
        : TimeSeconds(InTimeSeconds)
    {
    }

    void ForEachEntity(const FTrack& Track, FSample& Sample) const
    {
        const FTrackProgress Progress = EvaluateProgress(Track, TimeSeconds);
        Sample.bComplete = Progress.bComplete;
        Sample.bContributes = Progress.bContributes;
        const float Normalized = Progress.Normalized;
        if (Track.KeyframeCount < 2)
        {
            Sample.Value = FMath::Lerp(Track.From, Track.To, Normalized);
            return;
        }
        if (Normalized >= Track.Keyframes[Track.KeyframeCount - 1].Offset)
        {
            Sample.SegmentIndex = Track.KeyframeCount - 2;
            Sample.Value = Track.Keyframes[Track.KeyframeCount - 1].Value;
            Sample.LastNormalized = Normalized;
            return;
        }
        if (Normalized < Sample.LastNormalized)
        {
            Sample.SegmentIndex = 0;
        }
        while (Sample.SegmentIndex + 1 < Track.KeyframeCount - 1 &&
            Normalized >= Track.Keyframes[Sample.SegmentIndex + 1].Offset)
        {
            ++Sample.SegmentIndex;
        }
        while (Sample.SegmentIndex > 0 && Normalized < Track.Keyframes[Sample.SegmentIndex].Offset)
        {
            --Sample.SegmentIndex;
        }
        const FFloatKeyframe& A = Track.Keyframes[Sample.SegmentIndex];
        const FFloatKeyframe& B = Track.Keyframes[Sample.SegmentIndex + 1];
        const float SegmentAlpha = FMath::Clamp(
            (Normalized - A.Offset) / (B.Offset - A.Offset), 0.0f, 1.0f);
        const float LutPosition = SegmentAlpha * EasingLutIntervals;
        const int32 LutIndex = FMath::Min(FMath::FloorToInt(LutPosition), EasingLutIntervals - 1);
        const float EasedAlpha = FMath::Lerp(
            A.EasingToNext.Values[LutIndex], A.EasingToNext.Values[LutIndex + 1],
            LutPosition - LutIndex);
        Sample.Value = FMath::Lerp(A.Value, B.Value, EasedAlpha);
        Sample.LastNormalized = Normalized;
    }

    double TimeSeconds;
};

struct FComposeTrack
{
    static void ForEachEntity(const FSample& Sample, FPreviousOutput& Previous, FOutput& Output)
    {
        Output.Value = Sample.Value;
        Output.bComplete = Sample.bComplete;
        Output.bContributes = Sample.bContributes;
        Output.bChanged = Sample.bContributes &&
            (!Previous.bValid || !FMath::IsNearlyEqual(Previous.Value, Sample.Value));
        Previous.Value = Sample.Value;
        Previous.bValid = Sample.bContributes;
    }
};

static FTransform2DValue LerpTransform(const FTransform2DValue& A, const FTransform2DValue& B, float Alpha)
{
    return {
        FMath::Lerp(A.TranslationX, B.TranslationX, Alpha),
        FMath::Lerp(A.TranslationY, B.TranslationY, Alpha),
        FMath::Lerp(A.ScaleX, B.ScaleX, Alpha),
        FMath::Lerp(A.ScaleY, B.ScaleY, Alpha),
        FMath::Lerp(A.RotationDegrees, B.RotationDegrees, Alpha)};
}

static bool NearlyEqual(const FTransform2DValue& A, const FTransform2DValue& B)
{
    return FMath::IsNearlyEqual(A.TranslationX, B.TranslationX) &&
        FMath::IsNearlyEqual(A.TranslationY, B.TranslationY) &&
        FMath::IsNearlyEqual(A.ScaleX, B.ScaleX) && FMath::IsNearlyEqual(A.ScaleY, B.ScaleY) &&
        FMath::IsNearlyEqual(A.RotationDegrees, B.RotationDegrees);
}

static bool IsFiniteTransformValue(const FRmlUiTransform2D& Value)
{
    return FMath::IsFinite(Value.TranslationX) && FMath::IsFinite(Value.TranslationY) &&
        FMath::IsFinite(Value.ScaleX) && FMath::IsFinite(Value.ScaleY) &&
        FMath::IsFinite(Value.RotationDegrees);
}

static FTransform2DValue ToInternalTransform(const FRmlUiTransform2D& Value)
{
    return {Value.TranslationX, Value.TranslationY, Value.ScaleX, Value.ScaleY,
        Value.RotationDegrees};
}

static bool IsValidEasing(const FRmlUiAnimationEasing& Easing)
{
    if (Easing.Type == ERmlUiAnimationEasingType::Linear)
    {
        return true;
    }
    return Easing.Type == ERmlUiAnimationEasingType::CubicBezier &&
        FMath::IsFinite(Easing.X1) && FMath::IsFinite(Easing.Y1) &&
        FMath::IsFinite(Easing.X2) && FMath::IsFinite(Easing.Y2) &&
        Easing.X1 >= 0.0f && Easing.X1 <= 1.0f &&
        Easing.X2 >= 0.0f && Easing.X2 <= 1.0f;
}

static bool IsValidDirection(ERmlUiAnimationDirection Direction)
{
    return Direction >= ERmlUiAnimationDirection::Normal &&
        Direction <= ERmlUiAnimationDirection::AlternateReverse;
}

static float CubicBezierCoordinate(float T, float P1, float P2)
{
    const float OneMinusT = 1.0f - T;
    return 3.0f * OneMinusT * OneMinusT * T * P1 +
        3.0f * OneMinusT * T * T * P2 + T * T * T;
}

static FEasingLut BuildEasingLut(const FRmlUiAnimationEasing& Easing)
{
    FEasingLut Result;
    for (int32 Index = 0; Index <= EasingLutIntervals; ++Index)
    {
        const float X = static_cast<float>(Index) / EasingLutIntervals;
        if (Easing.Type == ERmlUiAnimationEasingType::Linear || Index == 0 ||
            Index == EasingLutIntervals)
        {
            Result.Values[Index] = X;
            continue;
        }
        float Low = 0.0f;
        float High = 1.0f;
        for (int32 Iteration = 0; Iteration < 20; ++Iteration)
        {
            const float Mid = (Low + High) * 0.5f;
            if (CubicBezierCoordinate(Mid, Easing.X1, Easing.X2) < X)
            {
                Low = Mid;
            }
            else
            {
                High = Mid;
            }
        }
        Result.Values[Index] = CubicBezierCoordinate(
            (Low + High) * 0.5f, Easing.Y1, Easing.Y2);
    }
    return Result;
}

static bool BuildFloatKeyframes(
    const TArray<FRmlUiFloatAnimationKeyframe>& Source,
    ERmlUiAnimatedProperty Property,
    TArray<FFloatKeyframe>& OutKeyframes)
{
    OutKeyframes.Reset();
    if (Source.Num() < 2 || Source.Num() > 65536 ||
        !FMath::IsNearlyZero(Source[0].Offset) ||
        !FMath::IsNearlyEqual(Source.Last().Offset, 1.0f))
    {
        return false;
    }
    OutKeyframes.Reserve(Source.Num());
    float PreviousOffset = -1.0f;
    for (int32 Index = 0; Index < Source.Num(); ++Index)
    {
        const FRmlUiFloatAnimationKeyframe& Keyframe = Source[Index];
        if (!FMath::IsFinite(Keyframe.Offset) || !FMath::IsFinite(Keyframe.Value) ||
            Keyframe.Offset < 0.0f || Keyframe.Offset > 1.0f ||
            Keyframe.Offset < PreviousOffset ||
            (Property == ERmlUiAnimatedProperty::Opacity &&
                (Keyframe.Value < 0.0f || Keyframe.Value > 1.0f)) ||
            (Index + 1 < Source.Num() && !IsValidEasing(Keyframe.EasingToNext)))
        {
            OutKeyframes.Reset();
            return false;
        }
        OutKeyframes.Add({Keyframe.Offset, Keyframe.Value, BuildEasingLut(Keyframe.EasingToNext)});
        PreviousOffset = Keyframe.Offset;
    }
    OutKeyframes[0].Offset = 0.0f;
    OutKeyframes.Last().Offset = 1.0f;
    return true;
}

static bool BuildTransformKeyframes(
    const TArray<FRmlUiTransform2DAnimationKeyframe>& Source,
    TArray<FTransform2DKeyframe>& OutKeyframes)
{
    OutKeyframes.Reset();
    if (Source.Num() < 2 || Source.Num() > 65536 ||
        !FMath::IsNearlyZero(Source[0].Offset) ||
        !FMath::IsNearlyEqual(Source.Last().Offset, 1.0f))
    {
        return false;
    }
    OutKeyframes.Reserve(Source.Num());
    float PreviousOffset = -1.0f;
    for (int32 Index = 0; Index < Source.Num(); ++Index)
    {
        const FRmlUiTransform2DAnimationKeyframe& Keyframe = Source[Index];
        if (!FMath::IsFinite(Keyframe.Offset) || !IsFiniteTransformValue(Keyframe.Value) ||
            Keyframe.Offset < 0.0f || Keyframe.Offset > 1.0f ||
            Keyframe.Offset < PreviousOffset ||
            (Index + 1 < Source.Num() && !IsValidEasing(Keyframe.EasingToNext)))
        {
            OutKeyframes.Reset();
            return false;
        }
        OutKeyframes.Add({Keyframe.Offset, ToInternalTransform(Keyframe.Value),
            BuildEasingLut(Keyframe.EasingToNext)});
        PreviousOffset = Keyframe.Offset;
    }
    OutKeyframes[0].Offset = 0.0f;
    OutKeyframes.Last().Offset = 1.0f;
    return true;
}

struct FEvaluateTransform2DTrack
{
    explicit FEvaluateTransform2DTrack(double InTimeSeconds) : TimeSeconds(InTimeSeconds) {}
    void ForEachEntity(const FTransform2DTrack& Track, FTransform2DSample& Sample) const
    {
        const FTrackProgress Progress = EvaluateProgress(Track, TimeSeconds);
        Sample.bComplete = Progress.bComplete;
        Sample.bContributes = Progress.bContributes;
        const float Normalized = Progress.Normalized;
        if (Track.KeyframeCount < 2)
        {
            Sample.Value = LerpTransform(Track.From, Track.To, Normalized);
            return;
        }
        if (Normalized >= Track.Keyframes[Track.KeyframeCount - 1].Offset)
        {
            Sample.SegmentIndex = Track.KeyframeCount - 2;
            Sample.Value = Track.Keyframes[Track.KeyframeCount - 1].Value;
            Sample.LastNormalized = Normalized;
            return;
        }
        if (Normalized < Sample.LastNormalized)
        {
            Sample.SegmentIndex = 0;
        }
        while (Sample.SegmentIndex + 1 < Track.KeyframeCount - 1 &&
            Normalized >= Track.Keyframes[Sample.SegmentIndex + 1].Offset)
        {
            ++Sample.SegmentIndex;
        }
        while (Sample.SegmentIndex > 0 && Normalized < Track.Keyframes[Sample.SegmentIndex].Offset)
        {
            --Sample.SegmentIndex;
        }
        const FTransform2DKeyframe& A = Track.Keyframes[Sample.SegmentIndex];
        const FTransform2DKeyframe& B = Track.Keyframes[Sample.SegmentIndex + 1];
        const float SegmentAlpha = FMath::Clamp(
            (Normalized - A.Offset) / (B.Offset - A.Offset), 0.0f, 1.0f);
        const float LutPosition = SegmentAlpha * EasingLutIntervals;
        const int32 LutIndex = FMath::Min(FMath::FloorToInt(LutPosition), EasingLutIntervals - 1);
        const float EasedAlpha = FMath::Lerp(
            A.EasingToNext.Values[LutIndex], A.EasingToNext.Values[LutIndex + 1],
            LutPosition - LutIndex);
        Sample.Value = LerpTransform(A.Value, B.Value, EasedAlpha);
        Sample.LastNormalized = Normalized;
    }
    double TimeSeconds;
};

struct FComposeTransform2DTrack
{
    static void ForEachEntity(const FTransform2DSample& Sample, FPreviousTransform2DOutput& Previous, FTransform2DOutput& Output)
    {
        Output.Value = Sample.Value;
        Output.bComplete = Sample.bComplete;
        Output.bContributes = Sample.bContributes;
        Output.bChanged = Sample.bContributes &&
            (!Previous.bValid || !NearlyEqual(Previous.Value, Sample.Value));
        Previous.Value = Sample.Value;
        Previous.bValid = Sample.bContributes;
    }
};
}

URmlUiAnimationSampleSystem::URmlUiAnimationSampleSystem(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Phase = UE::MovieScene::ESystemPhase::Evaluation;
}

void URmlUiAnimationSampleSystem::OnRun(
    FSystemTaskPrerequisites& Prerequisites,
    FSystemSubsequentTasks& Subsequents)
{
    using namespace UE::MovieScene;
    const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
    FEntityTaskBuilder()
        .Read(Components.Track)
        .Write(Components.Sample)
        .FilterNone({Components.Paused, Components.Occluded})
        .Dispatch_PerEntity<RmlUiAnimation::FEvaluateTrack>(
            &Linker->EntityManager, Prerequisites, &Subsequents, EvaluationTimeSeconds);
    FEntityTaskBuilder()
        .Read(Components.TransformTrack)
        .Write(Components.TransformSample)
        .FilterNone({Components.Paused, Components.Occluded})
        .Dispatch_PerEntity<RmlUiAnimation::FEvaluateTransform2DTrack>(
            &Linker->EntityManager, Prerequisites, &Subsequents, EvaluationTimeSeconds);
}

URmlUiAnimationComposeSystem::URmlUiAnimationComposeSystem(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Phase = UE::MovieScene::ESystemPhase::Evaluation;
}

void URmlUiAnimationComposeSystem::OnRun(
    FSystemTaskPrerequisites& Prerequisites,
    FSystemSubsequentTasks& Subsequents)
{
    using namespace UE::MovieScene;
    const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
    FEntityTaskBuilder()
        .Read(Components.Sample)
        .Write(Components.PreviousOutput)
        .Write(Components.Output)
        .FilterNone({Components.Paused, Components.Occluded})
        .Dispatch_PerEntity<RmlUiAnimation::FComposeTrack>(
            &Linker->EntityManager, Prerequisites, &Subsequents);
    FEntityTaskBuilder()
        .Read(Components.TransformSample)
        .Write(Components.PreviousTransformOutput)
        .Write(Components.TransformOutput)
        .FilterNone({Components.Paused, Components.Occluded})
        .Dispatch_PerEntity<RmlUiAnimation::FComposeTransform2DTrack>(
            &Linker->EntityManager, Prerequisites, &Subsequents);
}

namespace
{
RmlUE_AnimatedPropertyUpdate ToBridgeUpdate(const FRmlUiAnimationCommitUpdate& Update)
{
    return {Update.View, Update.Node, static_cast<uint32>(Update.Property),
        {Update.Values[0], Update.Values[1], Update.Values[2], Update.Values[3], Update.Values[4]}, Update.Target};
}

uint64 BridgeNanosecondsToCycles(uint64 Nanoseconds)
{
    return static_cast<uint64>((static_cast<double>(Nanoseconds) / 1000000000.0) /
        FPlatformTime::GetSecondsPerCycle64());
}

class FRmlUiSlateVisualCommitSink final : public IRmlUiAnimationCommitSink
{
public:
    void Commit(TConstArrayView<FRmlUiAnimationCommitUpdate> Updates,
        TArray<ERmlUiAnimationCommitResult>& OutResults) override
    {
        const uint64 PrepareStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        OutResults.Init(ERmlUiAnimationCommitResult::Unsupported, Updates.Num());
        TArray<RmlUE_AnimatedPropertyUpdate, TInlineAllocator<64>> Candidates;
        TArray<int32, TInlineAllocator<64>> CandidateIndices;
        for (int32 Index = 0; Index < Updates.Num(); ++Index)
        {
            const FRmlUiAnimationCommitUpdate& Update = Updates[Index];
            if (!Update.bFinal && (Update.Property == ERmlUiAnimatedProperty::Opacity ||
                Update.Property == ERmlUiAnimatedProperty::Transform2D))
            {
                Candidates.Add(ToBridgeUpdate(Update));
                CandidateIndices.Add(Index);
            }
        }
        if (PrepareStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitPrepare,
                FPlatformTime::Cycles64() - PrepareStart);
        }
        if (Candidates.IsEmpty()) return;

        TArray<uint8, TInlineAllocator<64>> Accepted;
        Accepted.SetNumZeroed(Candidates.Num());
        RmlUE_AnimatedVisualCommitStats BridgeStats{};
        const bool bProfile = FRmlUiPerformance::IsEnabled();
        const int32 Applied = bProfile
            ? RmlUE_ApplyAnimatedVisualPropertiesProfiled(
                Candidates.GetData(), Candidates.Num(), Accepted.GetData(), &BridgeStats)
            : RmlUE_ApplyAnimatedVisualProperties(
                Candidates.GetData(), Candidates.Num(), Accepted.GetData());
        if (bProfile)
        {
            FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitValidate,
                BridgeNanosecondsToCycles(BridgeStats.ValidateNanoseconds));
            FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitTransformPrepare,
                BridgeNanosecondsToCycles(BridgeStats.PrepareNanoseconds));
            FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitApply,
                BridgeNanosecondsToCycles(BridgeStats.ApplyNanoseconds));
            FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitSynchronize,
                BridgeNanosecondsToCycles(BridgeStats.SynchronizeNanoseconds));
            FRmlUiPerformance::AddCycles(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitPublish,
                BridgeNanosecondsToCycles(BridgeStats.PublishNanoseconds));
        }
        if (Applied < 0)
        {
            for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
            {
                const RmlUE_AnimatedPropertyUpdate& Candidate = Candidates[CandidateIndex];
                if (!(Candidate.Target
                    ? RmlUE_IsAnimationTargetValid(Candidate.View, Candidate.Target)
                    : RmlUE_IsNodeValid(Candidate.View, Candidate.Node)))
                {
                    OutResults[CandidateIndices[CandidateIndex]] = ERmlUiAnimationCommitResult::InvalidTarget;
                }
            }
            return;
        }
        for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
        {
            if (Accepted[CandidateIndex])
            {
                OutResults[CandidateIndices[CandidateIndex]] = ERmlUiAnimationCommitResult::Committed;
            }
        }
    }
};

class FRmlUiPropertyCommitSink final : public IRmlUiAnimationCommitSink
{
public:
    void Commit(TConstArrayView<FRmlUiAnimationCommitUpdate> Updates,
        TArray<ERmlUiAnimationCommitResult>& OutResults) override
    {
        OutResults.Init(ERmlUiAnimationCommitResult::Unsupported, Updates.Num());
        if (Updates.IsEmpty()) return;
        TArray<RmlUE_AnimatedPropertyUpdate, TInlineAllocator<64>> BridgeUpdates;
        BridgeUpdates.Reserve(Updates.Num());
        for (const FRmlUiAnimationCommitUpdate& Update : Updates) BridgeUpdates.Add(ToBridgeUpdate(Update));
        if (RmlUE_ApplyAnimatedProperties(BridgeUpdates.GetData(), BridgeUpdates.Num()) == BridgeUpdates.Num())
        {
            for (ERmlUiAnimationCommitResult& Result : OutResults) Result = ERmlUiAnimationCommitResult::Committed;
            return;
        }

        TArray<RmlUE_AnimatedPropertyUpdate, TInlineAllocator<64>> ValidUpdates;
        TArray<int32, TInlineAllocator<64>> ValidIndices;
        for (int32 Index = 0; Index < BridgeUpdates.Num(); ++Index)
        {
            const RmlUE_AnimatedPropertyUpdate& Update = BridgeUpdates[Index];
            if (Update.Target
                ? RmlUE_IsAnimationTargetValid(Update.View, Update.Target)
                : RmlUE_IsNodeValid(Update.View, Update.Node))
            {
                ValidUpdates.Add(Update);
                ValidIndices.Add(Index);
            }
            else
            {
                OutResults[Index] = ERmlUiAnimationCommitResult::InvalidTarget;
            }
        }
        if (!ValidUpdates.IsEmpty() &&
            RmlUE_ApplyAnimatedProperties(ValidUpdates.GetData(), ValidUpdates.Num()) == ValidUpdates.Num())
        {
            for (int32 Index : ValidIndices) OutResults[Index] = ERmlUiAnimationCommitResult::Committed;
        }
    }
};
}

class FRmlUiAnimationRuntime::FImpl
{
public:
    enum class EDefinitionType : uint8
    {
        Float,
        Transform2D
    };

    struct FNativeTargetKey
    {
        RmlUE_View* View = nullptr;
        uint32 Node = 0;
        ERmlUiAnimatedProperty Property = ERmlUiAnimatedProperty::Opacity;
        uint64 Target = 0;

        friend bool operator==(const FNativeTargetKey& A, const FNativeTargetKey& B)
        {
            return A.View == B.View && A.Node == B.Node && A.Property == B.Property;
        }
        friend uint32 GetTypeHash(const FNativeTargetKey& Key)
        {
            return HashCombineFast(HashCombineFast(PointerHash(Key.View), GetTypeHash(Key.Node)), GetTypeHash(static_cast<uint8>(Key.Property)));
        }
    };

    FImpl()
    {
        using namespace UE::MovieScene;
        RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        Components.EnsureRegistered();

        static const bool bDependenciesDefined = [&Components]()
        {
            UMovieSceneEntitySystem::DefineComponentProducer(
                URmlUiAnimationSampleSystem::StaticClass(), Components.Sample);
            UMovieSceneEntitySystem::DefineComponentConsumer(
                URmlUiAnimationComposeSystem::StaticClass(), Components.Sample);
            UMovieSceneEntitySystem::DefineComponentProducer(
                URmlUiAnimationSampleSystem::StaticClass(), Components.TransformSample);
            UMovieSceneEntitySystem::DefineComponentConsumer(
                URmlUiAnimationComposeSystem::StaticClass(), Components.TransformSample);
            return true;
        }();
        check(bDependenciesDefined);

        static const EEntitySystemLinkerRole LinkerRole = RegisterCustomEntitySystemLinkerRole();
        Linker.Reset(UMovieSceneEntitySystemLinker::CreateLinker(GetTransientPackage(), LinkerRole));
        check(Linker.IsValid());
        Linker->LinkSystem<URmlUiAnimationComposeSystem>();
        SampleSystem = Linker->LinkSystem<URmlUiAnimationSampleSystem>();
        VisualCommitSink = MakeUnique<FRmlUiSlateVisualCommitSink>();
        PropertyCommitSink = MakeUnique<FRmlUiPropertyCommitSink>();
    }

    ~FImpl()
    {
        RemoveAll(false);
        for (uint32 Index = 0; Index < static_cast<uint32>(Bindings.Num()); ++Index)
        {
            const FBindingRecord& Binding = Bindings[Index];
            if (Binding.bActive)
            {
                ReleaseBinding({PackHandle(Index, Binding.Generation)});
            }
        }
        if (Linker)
        {
            Linker->Reset();
        }
    }

    struct FRecord
    {
        UE::MovieScene::FMovieSceneEntityID Entity;
        FRmlUiAnimationHandle Handle;
        FRmlUiAnimationBindingHandle ResolvedBinding;
        uint64 BindingId = 0;
        FNativeTargetKey NativeTarget;
        bool bHasNativeTarget = false;
        float LastValues[5] = {};
        bool bHasLastValue = false;
        FRmlUiAnimationValueCallback OnValue;
        FRmlUiAnimationCompletionCallback OnComplete;
        double OriginTimeSeconds = 0.0;
        double OriginLocalTimeSeconds = 0.0;
        double PlaybackRate = 1.0;
        double DurationSeconds = 0.0;
        int32 Iterations = 1;
        bool bPaused = false;
        bool bScheduledPausedSample = false;
        bool bSuppressBeforeStart = false;
        bool bOccluded = false;
        bool bLayeredContribution = false;
        int32 LayeredGroupIndex = INDEX_NONE;
        int32 ContributionOrder = 0;
        bool bActive = false;
    };

    struct FDefinitionRecord
    {
        uint32 Generation = 1;
        int32 BindingCount = 0;
        EDefinitionType Type = EDefinitionType::Float;
        ERmlUiAnimatedProperty Property = ERmlUiAnimatedProperty::None;
        FRmlUiFloatAnimationDefinition Float;
        FRmlUiTransform2DAnimationDefinition Transform2D;
        TArray<RmlUiAnimation::FFloatKeyframe> FloatKeyframes;
        TArray<RmlUiAnimation::FTransform2DKeyframe> TransformKeyframes;
        bool bActive = false;
    };

    struct FBindingRecord
    {
        uint32 Generation = 1;
        FRmlUiAnimationDefinitionHandle Definition;
        FNativeTargetKey NativeTarget;
        uint64 ReplacementId = 0;
        bool bHasNativeTarget = false;
        bool bActive = false;
    };

    struct FLayeredGroup
    {
        FNativeTargetKey NativeTarget;
        TArray<uint32> RecordIndices;
        uint64 PreviousWinner = 0;
        uint64 ScheduleSerial = 0;
        double NextBoundaryTimeSeconds = TNumericLimits<double>::Max();
        bool bDirty = false;
        bool bActive = false;
    };

    struct FLayeredBoundary
    {
        double TimeSeconds = 0.0;
        int32 GroupIndex = INDEX_NONE;
        uint64 ScheduleSerial = 0;
    };

    struct FDispatch
    {
        uint32 RecordIndex = 0;
        float Value = 0.0f;
        bool bChanged = false;
        bool bComplete = false;
    };

    struct FTransformDispatch
    {
        uint32 RecordIndex = 0;
        RmlUiAnimation::FTransform2DValue Value;
        bool bChanged = false;
        bool bComplete = false;
    };

    struct FValueNotification
    {
        FRmlUiAnimationHandle Handle;
        float Value = 0.0f;
        FRmlUiAnimationValueCallback Callback;
    };

    struct FCompletionNotification
    {
        FRmlUiAnimationHandle Handle;
        ERmlUiAnimationCompletionReason Reason = ERmlUiAnimationCompletionReason::Completed;
        FRmlUiAnimationCompletionCallback Callback;
    };

    static uint64 PackHandle(uint32 Index, uint32 Generation)
    {
        return (static_cast<uint64>(Generation) << 32) | (static_cast<uint64>(Index) + 1);
    }

    static bool UnpackHandle(uint64 Value, uint32& OutIndex, uint32& OutGeneration)
    {
        const uint32 PackedIndex = static_cast<uint32>(Value);
        OutGeneration = static_cast<uint32>(Value >> 32);
        if (PackedIndex == 0 || OutGeneration == 0)
        {
            return false;
        }
        OutIndex = PackedIndex - 1;
        return true;
    }

    static uint32 NextGeneration(uint32 Generation)
    {
        ++Generation;
        return Generation == 0 ? 1 : Generation;
    }

    FDefinitionRecord* FindDefinition(FRmlUiAnimationDefinitionHandle Handle)
    {
        uint32 Index = 0;
        uint32 Generation = 0;
        if (!UnpackHandle(Handle.Value, Index, Generation) || !Definitions.IsValidIndex(Index))
        {
            return nullptr;
        }
        FDefinitionRecord& Record = Definitions[Index];
        return Record.bActive && Record.Generation == Generation ? &Record : nullptr;
    }

    const FDefinitionRecord* FindDefinition(FRmlUiAnimationDefinitionHandle Handle) const
    {
        return const_cast<FImpl*>(this)->FindDefinition(Handle);
    }

    FBindingRecord* FindBinding(FRmlUiAnimationBindingHandle Handle)
    {
        uint32 Index = 0;
        uint32 Generation = 0;
        if (!UnpackHandle(Handle.Value, Index, Generation) || !Bindings.IsValidIndex(Index))
        {
            return nullptr;
        }
        FBindingRecord& Record = Bindings[Index];
        return Record.bActive && Record.Generation == Generation ? &Record : nullptr;
    }

    const FBindingRecord* FindBinding(FRmlUiAnimationBindingHandle Handle) const
    {
        return const_cast<FImpl*>(this)->FindBinding(Handle);
    }

    FRmlUiAnimationDefinitionHandle RegisterFloatDefinition(
        ERmlUiAnimatedProperty Property,
        const FRmlUiFloatAnimationDefinition& InDefinition)
    {
        check(IsInGameThread());
        TArray<RmlUiAnimation::FFloatKeyframe> BuiltKeyframes;
        if ((Property != ERmlUiAnimatedProperty::None && Property != ERmlUiAnimatedProperty::Opacity) ||
            (!InDefinition.Keyframes.IsEmpty() &&
                !RmlUiAnimation::BuildFloatKeyframes(InDefinition.Keyframes, Property, BuiltKeyframes)) ||
            (InDefinition.Keyframes.IsEmpty() &&
                (!FMath::IsFinite(InDefinition.From) || !FMath::IsFinite(InDefinition.To))) ||
            InDefinition.DurationSeconds < 0.0 || InDefinition.Iterations <= 0 ||
            !FMath::IsFinite(InDefinition.PlaybackRate) || InDefinition.PlaybackRate <= 0.0 ||
            !RmlUiAnimation::IsValidDirection(InDefinition.Direction) ||
            (InDefinition.Keyframes.IsEmpty() && Property == ERmlUiAnimatedProperty::Opacity &&
                (InDefinition.From < 0.0f || InDefinition.From > 1.0f ||
                    InDefinition.To < 0.0f || InDefinition.To > 1.0f)))
        {
            return {};
        }

        const uint32 Index = AllocateDefinitionIndex();
        FDefinitionRecord& Record = Definitions[Index];
        Record.BindingCount = 0;
        Record.Type = EDefinitionType::Float;
        Record.Property = Property;
        Record.Float = InDefinition;
        Record.Float.Keyframes.Reset();
        Record.FloatKeyframes = MoveTemp(BuiltKeyframes);
        Record.TransformKeyframes.Reset();
        Record.Float.DelaySeconds = FMath::Max(Record.Float.DelaySeconds, 0.0);
        Record.bActive = true;
        ++DefinitionCount;
        return {PackHandle(Index, Record.Generation)};
    }

    FRmlUiAnimationDefinitionHandle RegisterTransform2DDefinition(
        const FRmlUiTransform2DAnimationDefinition& InDefinition)
    {
        check(IsInGameThread());
        TArray<RmlUiAnimation::FTransform2DKeyframe> BuiltKeyframes;
        if ((!InDefinition.Keyframes.IsEmpty() &&
                !RmlUiAnimation::BuildTransformKeyframes(InDefinition.Keyframes, BuiltKeyframes)) ||
            (InDefinition.Keyframes.IsEmpty() &&
                (!RmlUiAnimation::IsFiniteTransformValue(InDefinition.From) ||
                    !RmlUiAnimation::IsFiniteTransformValue(InDefinition.To))) ||
            InDefinition.DurationSeconds < 0.0 || InDefinition.Iterations <= 0 ||
            !FMath::IsFinite(InDefinition.PlaybackRate) || InDefinition.PlaybackRate <= 0.0 ||
            !RmlUiAnimation::IsValidDirection(InDefinition.Direction))
        {
            return {};
        }

        const uint32 Index = AllocateDefinitionIndex();
        FDefinitionRecord& Record = Definitions[Index];
        Record.BindingCount = 0;
        Record.Type = EDefinitionType::Transform2D;
        Record.Property = ERmlUiAnimatedProperty::Transform2D;
        Record.Transform2D = InDefinition;
        Record.Transform2D.Keyframes.Reset();
        Record.TransformKeyframes = MoveTemp(BuiltKeyframes);
        Record.FloatKeyframes.Reset();
        Record.Transform2D.DelaySeconds = FMath::Max(Record.Transform2D.DelaySeconds, 0.0);
        Record.bActive = true;
        ++DefinitionCount;
        return {PackHandle(Index, Record.Generation)};
    }

    bool ReleaseDefinition(FRmlUiAnimationDefinitionHandle Handle)
    {
        check(IsInGameThread());
        FDefinitionRecord* Record = FindDefinition(Handle);
        if (!Record || Record->BindingCount != 0)
        {
            return false;
        }
        uint32 Index = 0;
        uint32 Generation = 0;
        UnpackHandle(Handle.Value, Index, Generation);
        Record->FloatKeyframes.Empty();
        Record->TransformKeyframes.Empty();
        Record->bActive = false;
        Record->Generation = NextGeneration(Record->Generation);
        FreeDefinitionIndices.Add(Index);
        --DefinitionCount;
        return true;
    }

    uint64 GetDefinitionAllocatedBytes(FRmlUiAnimationDefinitionHandle Handle) const
    {
        const FDefinitionRecord* Record = FindDefinition(Handle);
        if (!Record) return 0;
        return static_cast<uint64>(Record->FloatKeyframes.GetAllocatedSize()) +
            static_cast<uint64>(Record->TransformKeyframes.GetAllocatedSize());
    }

    FRmlUiAnimationBindingHandle BindCallback(
        FRmlUiAnimationDefinitionHandle Definition,
        uint64 ReplacementId)
    {
        check(IsInGameThread());
        FDefinitionRecord* DefinitionRecord = FindDefinition(Definition);
        if (!DefinitionRecord || DefinitionRecord->Type != EDefinitionType::Float ||
            DefinitionRecord->Property != ERmlUiAnimatedProperty::None)
        {
            return {};
        }
        return AllocateBinding(Definition, nullptr, ReplacementId);
    }

    FRmlUiAnimationBindingHandle BindNode(
        FRmlUiAnimationDefinitionHandle Definition,
        RmlUE_View* View,
        uint32 Node,
        uint64 ReplacementId)
    {
        check(IsInGameThread());
        FDefinitionRecord* DefinitionRecord = FindDefinition(Definition);
        if (!DefinitionRecord || DefinitionRecord->Property == ERmlUiAnimatedProperty::None ||
            !View || !Node || CancellingViews.Contains(View))
        {
            return {};
        }
        const uint64 ResolvedTarget = RmlUE_ResolveAnimationTarget(View, Node);
        if (!ResolvedTarget) return {};
        if (!RmlUE_PrepareAnimationTargetProperty(
            View, ResolvedTarget, static_cast<uint32>(DefinitionRecord->Property)))
        {
            RmlUE_ReleaseAnimationTarget(View, ResolvedTarget);
            return {};
        }
        const FNativeTargetKey Target{View, Node, DefinitionRecord->Property, ResolvedTarget};
        const FRmlUiAnimationBindingHandle Binding = AllocateBinding(Definition, &Target, ReplacementId);
        if (!Binding.IsValid()) RmlUE_ReleaseAnimationTarget(View, ResolvedTarget);
        return Binding;
    }

    int32 BindNodes(
        FRmlUiAnimationDefinitionHandle Definition,
        RmlUE_View* View,
        const TArray<uint32>& Nodes,
        TArray<FRmlUiAnimationBindingHandle>& OutBindings)
    {
        check(IsInGameThread());
        OutBindings.Reset();
        const FDefinitionRecord* DefinitionRecord = FindDefinition(Definition);
        if (!DefinitionRecord || DefinitionRecord->Property == ERmlUiAnimatedProperty::None ||
            !View || CancellingViews.Contains(View))
        {
            return 0;
        }
        TArray<FNativeTargetKey, TInlineAllocator<64>> Targets;
        Targets.Reserve(Nodes.Num());
        for (uint32 Node : Nodes)
        {
            const uint64 Target = Node ? RmlUE_ResolveAnimationTarget(View, Node) : 0;
            if (!Target || !RmlUE_PrepareAnimationTargetProperty(
                View, Target, static_cast<uint32>(DefinitionRecord->Property)))
            {
                if (Target) RmlUE_ReleaseAnimationTarget(View, Target);
                for (const FNativeTargetKey& Created : Targets)
                {
                    RmlUE_ReleaseAnimationTarget(Created.View, Created.Target);
                }
                return 0;
            }
            Targets.Add({View, Node, DefinitionRecord->Property, Target});
        }
        OutBindings.Reserve(Nodes.Num());
        for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
        {
            const FNativeTargetKey& Target = Targets[TargetIndex];
            const FRmlUiAnimationBindingHandle Binding = AllocateBinding(Definition, &Target, 0);
            if (!Binding.IsValid())
            {
                for (FRmlUiAnimationBindingHandle Created : OutBindings)
                {
                    ReleaseBinding(Created);
                }
                for (int32 Index = TargetIndex; Index < Targets.Num(); ++Index)
                {
                    RmlUE_ReleaseAnimationTarget(Targets[Index].View, Targets[Index].Target);
                }
                OutBindings.Reset();
                return 0;
            }
            OutBindings.Add(Binding);
        }
        return OutBindings.Num();
    }

    bool ReleaseBinding(FRmlUiAnimationBindingHandle Handle)
    {
        check(IsInGameThread());
        FBindingRecord* Record = FindBinding(Handle);
        if (!Record)
        {
            return false;
        }
        if (const uint64* Animation = ResolvedBindingToAnimation.Find(Handle.Value))
        {
            Cancel(FRmlUiAnimationHandle{*Animation}, ERmlUiAnimationCompletionReason::Cancelled);
            Record = FindBinding(Handle);
            if (!Record)
            {
                return true;
            }
        }

        if (FDefinitionRecord* Definition = FindDefinition(Record->Definition))
        {
            check(Definition->BindingCount > 0);
            --Definition->BindingCount;
        }
        if (Record->bHasNativeTarget)
        {
            RmlUE_ReleaseAnimationTarget(Record->NativeTarget.View, Record->NativeTarget.Target);
        }
        uint32 Index = 0;
        uint32 Generation = 0;
        UnpackHandle(Handle.Value, Index, Generation);
        Record->bActive = false;
        Record->Generation = NextGeneration(Record->Generation);
        FreeBindingIndices.Add(Index);
        --BindingCount;
        return true;
    }

    FRmlUiAnimationHandle PlayBinding(
        FRmlUiAnimationBindingHandle Binding,
        FRmlUiAnimationValueCallback OnValue,
        FRmlUiAnimationCompletionCallback OnComplete,
        const FRmlUiAnimationContributionSpec* Contribution = nullptr)
    {
        check(IsInGameThread());
        FScopedRmlUiPerformanceTimer BindTimer(
            ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceStage::AnimationBind);
        const FBindingRecord* BindingRecord = FindBinding(Binding);
        if (!BindingRecord)
        {
            return {};
        }
        const FDefinitionRecord* Definition = FindDefinition(BindingRecord->Definition);
        if (!Definition || (Definition->Type == EDefinitionType::Transform2D && OnValue))
        {
            return {};
        }
        if (const uint64* Existing = ResolvedBindingToAnimation.Find(Binding.Value))
        {
            Cancel(FRmlUiAnimationHandle{*Existing}, ERmlUiAnimationCompletionReason::Replaced);
            BindingRecord = FindBinding(Binding);
            if (!BindingRecord)
            {
                return {};
            }
            Definition = FindDefinition(BindingRecord->Definition);
            if (!Definition)
            {
                return {};
            }
        }

        if (Definition->Type == EDefinitionType::Float)
        {
            const FRmlUiFloatAnimationDefinition& Source = Definition->Float;
            FRmlUiFloatAnimationDesc Desc;
            Desc.From = Source.From;
            Desc.To = Source.To;
            Desc.DurationSeconds = Source.DurationSeconds;
            Desc.DelaySeconds = Source.DelaySeconds;
            Desc.Iterations = Source.Iterations;
            Desc.BindingId = BindingRecord->ReplacementId;
            Desc.PlaybackRate = Source.PlaybackRate;
            Desc.Direction = Source.Direction;
            const RmlUiAnimation::FFloatKeyframe* Keyframes =
                Definition->FloatKeyframes.IsEmpty() ? nullptr : Definition->FloatKeyframes.GetData();
            const int32 KeyframeCount = Definition->FloatKeyframes.Num();
            return BindingRecord->bHasNativeTarget
                ? PlayNode(BindingRecord->NativeTarget.View, BindingRecord->NativeTarget.Node,
                    Definition->Property, Desc, MoveTemp(OnComplete), Binding,
                    &BindingRecord->NativeTarget, Keyframes, KeyframeCount, Contribution)
                : Play(Desc, MoveTemp(OnValue), MoveTemp(OnComplete), nullptr, Binding,
                    Keyframes, KeyframeCount, Contribution);
        }

        const FRmlUiTransform2DAnimationDefinition& Source = Definition->Transform2D;
        FRmlUiTransform2DAnimationDesc Desc;
        Desc.From = Source.From;
        Desc.To = Source.To;
        Desc.DurationSeconds = Source.DurationSeconds;
        Desc.DelaySeconds = Source.DelaySeconds;
        Desc.Iterations = Source.Iterations;
        Desc.BindingId = BindingRecord->ReplacementId;
        Desc.PlaybackRate = Source.PlaybackRate;
        Desc.Direction = Source.Direction;
        return PlayNodeTransform2D(
            BindingRecord->NativeTarget.View, BindingRecord->NativeTarget.Node,
            Desc, MoveTemp(OnComplete), Binding, &BindingRecord->NativeTarget,
            Definition->TransformKeyframes.IsEmpty() ? nullptr : Definition->TransformKeyframes.GetData(),
            Definition->TransformKeyframes.Num(), Contribution);
    }

    int32 PlayBindings(
        const TArray<FRmlUiAnimationBindingHandle>& InBindings,
        TArray<FRmlUiAnimationHandle>& OutHandles,
        TArray<FRmlUiAnimationCompletionCallback> OnCompletes)
    {
        check(IsInGameThread());
        OutHandles.Reset();
        if (!OnCompletes.IsEmpty() && OnCompletes.Num() != InBindings.Num())
        {
            return 0;
        }
        for (FRmlUiAnimationBindingHandle Binding : InBindings)
        {
            const FBindingRecord* BindingRecord = FindBinding(Binding);
            if (!BindingRecord || !FindDefinition(BindingRecord->Definition))
            {
                return 0;
            }
        }

        OutHandles.Reserve(InBindings.Num());
        for (int32 Index = 0; Index < InBindings.Num(); ++Index)
        {
            FRmlUiAnimationCompletionCallback OnComplete = OnCompletes.IsEmpty()
                ? FRmlUiAnimationCompletionCallback{}
                : MoveTemp(OnCompletes[Index]);
            const FRmlUiAnimationHandle Handle = PlayBinding(
                InBindings[Index], {}, MoveTemp(OnComplete));
            if (!Handle.IsValid())
            {
                for (FRmlUiAnimationHandle Created : OutHandles)
                {
                    Cancel(Created, ERmlUiAnimationCompletionReason::Cancelled);
                }
                OutHandles.Reset();
                return 0;
            }
            OutHandles.Add(Handle);
        }
        return OutHandles.Num();
    }

    int32 PlayContributionBindings(
        const TArray<FRmlUiAnimationBindingHandle>& InBindings,
        TConstArrayView<FRmlUiAnimationContributionSpec> Contributions,
        TArray<FRmlUiAnimationHandle>& OutHandles,
        TArray<FRmlUiAnimationCompletionCallback> OnCompletes)
    {
        check(IsInGameThread());
        OutHandles.Reset();
        if (Contributions.Num() != InBindings.Num() ||
            (!OnCompletes.IsEmpty() && OnCompletes.Num() != InBindings.Num()))
        {
            return 0;
        }

        TMap<FNativeTargetKey, TSet<int32>> OrdersByTarget;
        TMap<FNativeTargetKey, bool> LayeredModeByTarget;
        TSet<FNativeTargetKey> Targets;
        for (int32 Index = 0; Index < InBindings.Num(); ++Index)
        {
            const FBindingRecord* BindingRecord = FindBinding(InBindings[Index]);
            if (!BindingRecord || !BindingRecord->bHasNativeTarget ||
                !FindDefinition(BindingRecord->Definition))
            {
                return 0;
            }
            if (const bool* ExistingMode = LayeredModeByTarget.Find(BindingRecord->NativeTarget))
            {
                if (*ExistingMode != Contributions[Index].bLayered || !Contributions[Index].bLayered)
                    return 0;
            }
            else
            {
                LayeredModeByTarget.Add(BindingRecord->NativeTarget, Contributions[Index].bLayered);
            }
            if (Contributions[Index].bLayered)
            {
                TSet<int32>& Orders = OrdersByTarget.FindOrAdd(BindingRecord->NativeTarget);
                if (Orders.Contains(Contributions[Index].Order)) return 0;
                Orders.Add(Contributions[Index].Order);
            }
            Targets.Add(BindingRecord->NativeTarget);
        }

        for (const FNativeTargetKey& Target : Targets)
        {
            CancelNativeTarget(Target, ERmlUiAnimationCompletionReason::Replaced);
        }

        OutHandles.Reserve(InBindings.Num());
        for (int32 Index = 0; Index < InBindings.Num(); ++Index)
        {
            FRmlUiAnimationCompletionCallback OnComplete = OnCompletes.IsEmpty()
                ? FRmlUiAnimationCompletionCallback{}
                : MoveTemp(OnCompletes[Index]);
            const FRmlUiAnimationHandle Handle = PlayBinding(
                InBindings[Index], {}, MoveTemp(OnComplete),
                Contributions[Index].bLayered ? &Contributions[Index] : nullptr);
            if (!Handle.IsValid())
            {
                for (FRmlUiAnimationHandle Created : OutHandles)
                {
                    Cancel(Created, ERmlUiAnimationCompletionReason::Cancelled);
                }
                OutHandles.Reset();
                return 0;
            }
            OutHandles.Add(Handle);
        }
        return OutHandles.Num();
    }

    FRmlUiAnimationHandle Play(
        const FRmlUiFloatAnimationDesc& InDesc,
        FRmlUiAnimationValueCallback OnValue,
        FRmlUiAnimationCompletionCallback OnComplete,
        const FNativeTargetKey* NativeTarget = nullptr,
        FRmlUiAnimationBindingHandle ResolvedBinding = {},
        const RmlUiAnimation::FFloatKeyframe* Keyframes = nullptr,
        int32 KeyframeCount = 0,
        const FRmlUiAnimationContributionSpec* Contribution = nullptr)
    {
        check(IsInGameThread());
        FRmlUiFloatAnimationDesc Desc = InDesc;
        Desc.DurationSeconds = FMath::Max(Desc.DurationSeconds, 0.0);
        Desc.DelaySeconds = FMath::Max(Desc.DelaySeconds, 0.0);
        Desc.Iterations = FMath::Max(Desc.Iterations, 1);
        if (!FMath::IsFinite(Desc.PlaybackRate) || Desc.PlaybackRate <= 0.0 ||
            !RmlUiAnimation::IsValidDirection(Desc.Direction)) return {};

        if (Desc.BindingId != 0 && !Contribution)
        {
            CancelBinding(Desc.BindingId, ERmlUiAnimationCompletionReason::Replaced);
        }
        if (NativeTarget && !Contribution)
        {
            CancelNativeTarget(*NativeTarget, ERmlUiAnimationCompletionReason::Replaced);
        }
        if (ResolvedBinding.IsValid() && !FindBinding(ResolvedBinding))
        {
            return {};
        }

        FRmlUiAnimationHandle Handle{NextHandle++};
        if (!Handle.IsValid())
        {
            Handle.Value = NextHandle++;
        }

        const uint32 RecordIndex = AllocateRecordIndex();
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        const RmlUiAnimation::FTrack Track{
            Desc.From,
            Desc.To,
            CurrentTimeSeconds,
            -Desc.DelaySeconds,
            Desc.PlaybackRate,
            Desc.DurationSeconds,
            Desc.Iterations,
            static_cast<uint8>(Desc.Direction),
            Keyframes,
            KeyframeCount,
            Contribution && Contribution->bSuppressBeforeStart};
        const UE::MovieScene::FMovieSceneEntityID Entity = UE::MovieScene::FEntityBuilder()
            .Add(Components.RecordIndex, RecordIndex)
            .Add(Components.Track, Track)
            .Add(Components.Sample, RmlUiAnimation::FSample{})
            .Add(Components.Output, RmlUiAnimation::FOutput{})
            .Add(Components.PreviousOutput, RmlUiAnimation::FPreviousOutput{})
            .CreateEntity(&Linker->EntityManager);

        FRecord& Record = Records[RecordIndex];
        Record.Entity = Entity;
        Record.Handle = Handle;
        Record.ResolvedBinding = ResolvedBinding;
        Record.BindingId = Desc.BindingId;
        Record.OnValue = MoveTemp(OnValue);
        Record.OnComplete = MoveTemp(OnComplete);
        Record.OriginTimeSeconds = CurrentTimeSeconds;
        Record.OriginLocalTimeSeconds = -Desc.DelaySeconds;
        Record.PlaybackRate = Desc.PlaybackRate;
        Record.DurationSeconds = Desc.DurationSeconds;
        Record.Iterations = Desc.Iterations;
        Record.bPaused = false;
        Record.bSuppressBeforeStart = Contribution && Contribution->bSuppressBeforeStart;
        Record.bLayeredContribution = Contribution != nullptr;
        Record.LayeredGroupIndex = Contribution && NativeTarget
            ? AddLayeredRecord(*NativeTarget, RecordIndex) : INDEX_NONE;
        Record.ContributionOrder = Contribution ? Contribution->Order : 0;
        Record.bActive = true;
        if (NativeTarget)
        {
            Record.NativeTarget = *NativeTarget;
            Record.bHasNativeTarget = true;
            if (!Contribution)
            {
                NativeTargetToHandle.Add(*NativeTarget, Handle.Value);
            }
        }
        HandleToRecord.Add(Handle.Value, RecordIndex);
        ++ActiveRecordCount;
        ++RunningRecordCount;
        AdjustEvaluatingRecordCount(Record, 1);
        if (Desc.BindingId != 0 && !Contribution)
        {
            BindingToHandle.Add(Desc.BindingId, Handle.Value);
        }
        if (ResolvedBinding.IsValid())
        {
            ResolvedBindingToAnimation.Add(ResolvedBinding.Value, Handle.Value);
        }
        if (NativeTarget && WakeCallback)
        {
            WakeCallback(NativeTarget->View);
        }
        return Handle;
    }

    FRmlUiAnimationHandle PlayNode(
        RmlUE_View* View,
        uint32 Node,
        ERmlUiAnimatedProperty Property,
        const FRmlUiFloatAnimationDesc& Desc,
        FRmlUiAnimationCompletionCallback OnComplete,
        FRmlUiAnimationBindingHandle ResolvedBinding = {},
        const FNativeTargetKey* ResolvedTarget = nullptr,
        const RmlUiAnimation::FFloatKeyframe* Keyframes = nullptr,
        int32 KeyframeCount = 0,
        const FRmlUiAnimationContributionSpec* Contribution = nullptr)
    {
        check(IsInGameThread());
        if (!View || !Node || Property != ERmlUiAnimatedProperty::Opacity ||
            CancellingViews.Contains(View) ||
            !FMath::IsFinite(Desc.From) || !FMath::IsFinite(Desc.To) ||
            Desc.From < 0.0f || Desc.From > 1.0f || Desc.To < 0.0f || Desc.To > 1.0f ||
            Desc.DurationSeconds < 0.0 || Desc.Iterations <= 0 ||
            !(ResolvedTarget ? RmlUE_IsAnimationTargetValid(View, ResolvedTarget->Target) : RmlUE_IsNodeValid(View, Node)))
        {
            return {};
        }
        RmlUE_CancelAnimation(View, Node, "opacity");
        const FNativeTargetKey Target = ResolvedTarget
            ? *ResolvedTarget : FNativeTargetKey{View, Node, Property, 0};
        return Play(Desc, {}, MoveTemp(OnComplete), &Target, ResolvedBinding,
            Keyframes, KeyframeCount, Contribution);
    }

    FRmlUiAnimationHandle PlayNodeTransform2D(
        RmlUE_View* View,
        uint32 Node,
        const FRmlUiTransform2DAnimationDesc& InDesc,
        FRmlUiAnimationCompletionCallback OnComplete,
        FRmlUiAnimationBindingHandle ResolvedBinding = {},
        const FNativeTargetKey* ResolvedTarget = nullptr,
        const RmlUiAnimation::FTransform2DKeyframe* Keyframes = nullptr,
        int32 KeyframeCount = 0,
        const FRmlUiAnimationContributionSpec* Contribution = nullptr)
    {
        check(IsInGameThread());
        const auto IsFinite = [](const FRmlUiTransform2D& Value)
        {
            return FMath::IsFinite(Value.TranslationX) && FMath::IsFinite(Value.TranslationY) &&
                FMath::IsFinite(Value.ScaleX) && FMath::IsFinite(Value.ScaleY) &&
                FMath::IsFinite(Value.RotationDegrees);
        };
        if (!View || !Node || CancellingViews.Contains(View) || !IsFinite(InDesc.From) || !IsFinite(InDesc.To) ||
            InDesc.DurationSeconds < 0.0 || InDesc.Iterations <= 0 ||
            !FMath::IsFinite(InDesc.PlaybackRate) || InDesc.PlaybackRate <= 0.0 ||
            !RmlUiAnimation::IsValidDirection(InDesc.Direction) ||
            !(ResolvedTarget ? RmlUE_IsAnimationTargetValid(View, ResolvedTarget->Target) : RmlUE_IsNodeValid(View, Node)))
        {
            return {};
        }

        FRmlUiTransform2DAnimationDesc Desc = InDesc;
        Desc.DelaySeconds = FMath::Max(Desc.DelaySeconds, 0.0);
        const FNativeTargetKey Target = ResolvedTarget
            ? *ResolvedTarget : FNativeTargetKey{View, Node, ERmlUiAnimatedProperty::Transform2D, 0};
        if (Desc.BindingId != 0 && !Contribution)
            CancelBinding(Desc.BindingId, ERmlUiAnimationCompletionReason::Replaced);
        if (!Contribution) CancelNativeTarget(Target, ERmlUiAnimationCompletionReason::Replaced);
        if (ResolvedBinding.IsValid() && !FindBinding(ResolvedBinding)) return {};
        RmlUE_CancelAnimation(View, Node, "transform");

        FRmlUiAnimationHandle Handle{NextHandle++};
        if (!Handle.IsValid()) Handle.Value = NextHandle++;
        const uint32 RecordIndex = AllocateRecordIndex();
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        const RmlUiAnimation::FTransform2DTrack Track{
            RmlUiAnimation::ToInternalTransform(Desc.From),
            RmlUiAnimation::ToInternalTransform(Desc.To),
            CurrentTimeSeconds, -Desc.DelaySeconds, Desc.PlaybackRate,
            Desc.DurationSeconds, Desc.Iterations, static_cast<uint8>(Desc.Direction),
            Keyframes, KeyframeCount, Contribution && Contribution->bSuppressBeforeStart};
        const UE::MovieScene::FMovieSceneEntityID Entity = UE::MovieScene::FEntityBuilder()
            .Add(Components.RecordIndex, RecordIndex)
            .Add(Components.TransformTrack, Track)
            .Add(Components.TransformSample, RmlUiAnimation::FTransform2DSample{})
            .Add(Components.TransformOutput, RmlUiAnimation::FTransform2DOutput{})
            .Add(Components.PreviousTransformOutput, RmlUiAnimation::FPreviousTransform2DOutput{})
            .CreateEntity(&Linker->EntityManager);

        FRecord& Record = Records[RecordIndex];
        Record.Entity = Entity;
        Record.Handle = Handle;
        Record.ResolvedBinding = ResolvedBinding;
        Record.BindingId = Desc.BindingId;
        Record.NativeTarget = Target;
        Record.bHasNativeTarget = true;
        Record.OnComplete = MoveTemp(OnComplete);
        Record.OriginTimeSeconds = CurrentTimeSeconds;
        Record.OriginLocalTimeSeconds = -Desc.DelaySeconds;
        Record.PlaybackRate = Desc.PlaybackRate;
        Record.DurationSeconds = Desc.DurationSeconds;
        Record.Iterations = Desc.Iterations;
        Record.bPaused = false;
        Record.bSuppressBeforeStart = Contribution && Contribution->bSuppressBeforeStart;
        Record.bLayeredContribution = Contribution != nullptr;
        Record.LayeredGroupIndex = Contribution
            ? AddLayeredRecord(Target, RecordIndex) : INDEX_NONE;
        Record.ContributionOrder = Contribution ? Contribution->Order : 0;
        Record.bActive = true;
        HandleToRecord.Add(Handle.Value, RecordIndex);
        if (!Contribution)
            NativeTargetToHandle.Add(Target, Handle.Value);
        if (Desc.BindingId != 0 && !Contribution) BindingToHandle.Add(Desc.BindingId, Handle.Value);
        if (ResolvedBinding.IsValid()) ResolvedBindingToAnimation.Add(ResolvedBinding.Value, Handle.Value);
        ++ActiveRecordCount;
        ++RunningRecordCount;
        AdjustEvaluatingRecordCount(Record, 1);
        if (WakeCallback) WakeCallback(View);
        return Handle;
    }

    bool Cancel(FRmlUiAnimationHandle Handle, ERmlUiAnimationCompletionReason Reason)
    {
        check(IsInGameThread());
        FRecord Record;
        if (!TakeRecord(Handle, Record))
        {
            return false;
        }
        if (!Record.bLayeredContribution)
        {
            CommitLastVisualValue(Record);
        }
        ReleaseEntity(Record.Entity);
        if (Record.OnComplete)
        {
            Record.OnComplete(Handle, Reason);
        }
        return true;
    }

    FRecord* FindRecord(FRmlUiAnimationHandle Handle)
    {
        const uint32* RecordIndex = HandleToRecord.Find(Handle.Value);
        if (!RecordIndex || !Records.IsValidIndex(*RecordIndex) || !Records[*RecordIndex].bActive)
        {
            return nullptr;
        }
        return &Records[*RecordIndex];
    }

    const FRecord* FindRecord(FRmlUiAnimationHandle Handle) const
    {
        return const_cast<FImpl*>(this)->FindRecord(Handle);
    }

    template <typename TrackType>
    bool RebaseTrack(
        FRecord& Record,
        UE::MovieScene::TComponentTypeID<TrackType> Component,
        TOptional<double> NewLocalTime,
        double NewRate)
    {
        auto Track = Linker->EntityManager.WriteComponent(Record.Entity, Component);
        if (!Track) return false;
        const double CurrentLocalTime = Track->OriginLocalTimeSeconds +
            (CurrentTimeSeconds - Track->OriginTimeSeconds) * Track->PlaybackRate;
        const double TotalDuration = Track->DurationSeconds * FMath::Max(Track->Iterations, 1);
        Track->OriginLocalTimeSeconds = NewLocalTime.IsSet()
            ? FMath::Clamp(NewLocalTime.GetValue(), 0.0, TotalDuration)
            : CurrentLocalTime;
        Track->OriginTimeSeconds = CurrentTimeSeconds;
        Track->PlaybackRate = NewRate;
        Record.OriginLocalTimeSeconds = Track->OriginLocalTimeSeconds;
        Record.OriginTimeSeconds = Track->OriginTimeSeconds;
        return true;
    }

    bool RebaseRecord(FRecord& Record, TOptional<double> NewLocalTime, double NewRate)
    {
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        if (RebaseTrack(Record, Components.Track, NewLocalTime, NewRate)) return true;
        return RebaseTrack(Record, Components.TransformTrack, NewLocalTime, NewRate);
    }

    void WakeRecord(const FRecord& Record)
    {
        if (Record.bHasNativeTarget && WakeCallback) WakeCallback(Record.NativeTarget.View);
    }

    void AdjustEvaluatingRecordCount(const FRecord& Record, int32 Delta)
    {
        EvaluatingRecordCount += Delta;
        check(EvaluatingRecordCount >= 0);
        if (!Record.bHasNativeTarget)
        {
            return;
        }
        int32& ViewCount = ViewEvaluatingRecordCounts.FindOrAdd(Record.NativeTarget.View);
        ViewCount += Delta;
        check(ViewCount >= 0);
        if (ViewCount == 0)
        {
            ViewEvaluatingRecordCounts.Remove(Record.NativeTarget.View);
        }
    }

    bool HasEvaluatingRecords(RmlUE_View* View) const
    {
        return View && ViewEvaluatingRecordCounts.FindRef(View) > 0;
    }

    static bool IsEarlierBoundary(const FLayeredBoundary& A, const FLayeredBoundary& B)
    {
        return A.TimeSeconds < B.TimeSeconds ||
            (A.TimeSeconds == B.TimeSeconds && A.GroupIndex < B.GroupIndex);
    }

    void PushLayeredBoundary(FLayeredBoundary Boundary)
    {
        int32 ChildIndex = LayeredBoundaryHeap.Add(Boundary);
        while (ChildIndex > 0)
        {
            const int32 ParentIndex = (ChildIndex - 1) / 2;
            if (!IsEarlierBoundary(LayeredBoundaryHeap[ChildIndex],
                LayeredBoundaryHeap[ParentIndex]))
            {
                break;
            }
            Swap(LayeredBoundaryHeap[ChildIndex], LayeredBoundaryHeap[ParentIndex]);
            ChildIndex = ParentIndex;
        }
    }

    void SiftDownLayeredBoundary(int32 ParentIndex)
    {
        for (;;)
        {
            const int32 LeftIndex = ParentIndex * 2 + 1;
            if (!LayeredBoundaryHeap.IsValidIndex(LeftIndex))
            {
                break;
            }
            const int32 RightIndex = LeftIndex + 1;
            int32 EarlierChild = LeftIndex;
            if (LayeredBoundaryHeap.IsValidIndex(RightIndex) &&
                IsEarlierBoundary(LayeredBoundaryHeap[RightIndex],
                    LayeredBoundaryHeap[LeftIndex]))
            {
                EarlierChild = RightIndex;
            }
            if (!IsEarlierBoundary(LayeredBoundaryHeap[EarlierChild],
                LayeredBoundaryHeap[ParentIndex]))
            {
                break;
            }
            Swap(LayeredBoundaryHeap[ParentIndex], LayeredBoundaryHeap[EarlierChild]);
            ParentIndex = EarlierChild;
        }
    }

    void HeapifyLayeredBoundaries()
    {
        for (int32 ParentIndex = LayeredBoundaryHeap.Num() / 2 - 1;
            ParentIndex >= 0; --ParentIndex)
        {
            SiftDownLayeredBoundary(ParentIndex);
        }
    }

    FLayeredBoundary PopLayeredBoundary()
    {
        check(!LayeredBoundaryHeap.IsEmpty());
        const FLayeredBoundary Result = LayeredBoundaryHeap[0];
        const FLayeredBoundary Last = LayeredBoundaryHeap.Pop(EAllowShrinking::No);
        if (LayeredBoundaryHeap.IsEmpty())
        {
            return Result;
        }
        LayeredBoundaryHeap[0] = Last;
        SiftDownLayeredBoundary(0);
        return Result;
    }

    void WakeLayeredBoundary(const FLayeredBoundary& Boundary)
    {
        if (!LayeredGroups.IsValidIndex(Boundary.GroupIndex))
        {
            return;
        }
        const FLayeredGroup& Group = LayeredGroups[Boundary.GroupIndex];
        if (!Group.bActive || Group.ScheduleSerial != Boundary.ScheduleSerial)
        {
            return;
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredBoundaryWakes);
        MarkLayeredGroupDirty(Boundary.GroupIndex);
    }

    void MarkLayeredGroupDirty(int32 GroupIndex)
    {
        if (!LayeredGroups.IsValidIndex(GroupIndex) || !LayeredGroups[GroupIndex].bActive)
        {
            return;
        }
        FLayeredGroup& Group = LayeredGroups[GroupIndex];
        Group.ScheduleSerial = NextLayeredScheduleSerial++;
        Group.NextBoundaryTimeSeconds = CurrentTimeSeconds;
        if (NextLayeredScheduleSerial == 0)
        {
            NextLayeredScheduleSerial = 1;
        }
        if (!Group.bDirty)
        {
            Group.bDirty = true;
            DirtyLayeredGroupIndices.Add(GroupIndex);
        }
    }

    void RebuildLayeredBoundaryHeapIfNeeded()
    {
        const int32 MaximumLazyEntries = LayeredNativeTargetToGroup.Num() * 2 + 64;
        if (LayeredBoundaryHeap.Num() <= MaximumLazyEntries)
        {
            return;
        }
        TArray<FLayeredBoundary> OldHeap = MoveTemp(LayeredBoundaryHeap);
        LayeredBoundaryHeap.Reset();
        LayeredBoundaryHeap.Reserve(LayeredNativeTargetToGroup.Num());
        for (const FLayeredBoundary& Boundary : OldHeap)
        {
            if (!LayeredGroups.IsValidIndex(Boundary.GroupIndex))
            {
                continue;
            }
            const FLayeredGroup& Group = LayeredGroups[Boundary.GroupIndex];
            if (Group.bActive && !Group.bDirty &&
                Group.ScheduleSerial == Boundary.ScheduleSerial)
            {
                LayeredBoundaryHeap.Add(Boundary);
            }
        }
        HeapifyLayeredBoundaries();
    }

    void SetRecordOccluded(FRecord& Record, bool bOccluded)
    {
        if (!Record.bLayeredContribution || Record.bOccluded == bOccluded)
        {
            return;
        }
        if (!Record.bPaused || Record.bScheduledPausedSample)
        {
            AdjustEvaluatingRecordCount(Record, bOccluded ? -1 : 1);
        }
        Record.bOccluded = bOccluded;
        ++PendingOcclusionChanges;
    }

    void ApplyPendingOcclusionChanges()
    {
        if (PendingOcclusionChanges == 0)
        {
            return;
        }

        struct FToggleOccludedMutation final : UE::MovieScene::IMovieSceneConditionalEntityMutation
        {
            TArrayView<const FRecord> Records;
            UE::MovieScene::TComponentTypeID<uint32> RecordIndexComponent;
            UE::MovieScene::FComponentTypeID OccludedComponent;

            FToggleOccludedMutation(TArrayView<const FRecord> InRecords,
                UE::MovieScene::TComponentTypeID<uint32> InRecordIndexComponent,
                UE::MovieScene::FComponentTypeID InOccludedComponent)
                : Records(InRecords)
                , RecordIndexComponent(InRecordIndexComponent)
                , OccludedComponent(InOccludedComponent)
            {
            }

            void MarkAllocation(UE::MovieScene::FEntityAllocation* Allocation,
                TBitArray<>& OutEntitiesToMutate) const override
            {
                const bool bCurrentlyOccluded = Allocation->HasComponent(OccludedComponent);
                const UE::MovieScene::TComponentReader<uint32> RecordIndices =
                    Allocation->ReadComponents(RecordIndexComponent);
                const int32 EntityCount = Allocation->Num();
                for (int32 EntityIndex = 0; EntityIndex < EntityCount; ++EntityIndex)
                {
                    const uint32 RecordIndex = RecordIndices[EntityIndex];
                    if (Records.IsValidIndex(RecordIndex))
                    {
                        const FRecord& Record = Records[RecordIndex];
                        if (Record.bActive && Record.bLayeredContribution &&
                            Record.bOccluded != bCurrentlyOccluded)
                        {
                            OutEntitiesToMutate.PadToNum(EntityIndex + 1, false);
                            OutEntitiesToMutate[EntityIndex] = true;
                        }
                    }
                }
            }

            void CreateMutation(UE::MovieScene::FEntityManager*,
                UE::MovieScene::FComponentMask* InOutEntityComponentTypes) const override
            {
                if (InOutEntityComponentTypes->Contains(OccludedComponent))
                {
                    InOutEntityComponentTypes->Remove(OccludedComponent);
                }
                else
                {
                    InOutEntityComponentTypes->Set(OccludedComponent);
                }
            }
        };

        FScopedRmlUiPerformanceTimer TagMutationTimer(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationScheduleTagMutation);
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        FToggleOccludedMutation Mutation(
            MakeArrayView(Records), Components.RecordIndex, Components.Occluded);
        UE::MovieScene::FEntityComponentFilter Filter;
        Filter.All({Components.RecordIndex});
        const int32 MutatedEntities = Linker->EntityManager.MutateConditional(Filter, Mutation);
        check(MutatedEntities == PendingOcclusionChanges);
        PendingOcclusionChanges = 0;
    }

    void RefreshLayeredGroup(int32 GroupIndex)
    {
        if (!LayeredGroups.IsValidIndex(GroupIndex))
        {
            return;
        }
        FLayeredGroup& Group = LayeredGroups[GroupIndex];
        if (!Group.bActive)
        {
            return;
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredGroupsRefreshed);
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationLayeredRecordsScanned,
            Group.RecordIndices.Num());

        uint32 WinnerRecordIndex = MAX_uint32;
        int32 WinnerOrder = MIN_int32;
        for (uint32 RecordIndex : Group.RecordIndices)
        {
            if (!Records.IsValidIndex(RecordIndex))
            {
                continue;
            }
            const FRecord& Record = Records[RecordIndex];
            if (!Record.bActive || Record.bPaused)
            {
                continue;
            }
            const double LocalTime = Record.OriginLocalTimeSeconds +
                (CurrentTimeSeconds - Record.OriginTimeSeconds) * Record.PlaybackRate;
            const bool bContributes = LocalTime >= 0.0 || !Record.bSuppressBeforeStart;
            if (bContributes && (WinnerRecordIndex == MAX_uint32 ||
                Record.ContributionOrder > WinnerOrder))
            {
                WinnerRecordIndex = RecordIndex;
                WinnerOrder = Record.ContributionOrder;
            }
        }

        double NextBoundarySeconds = TNumericLimits<double>::Max();
        for (uint32 RecordIndex : Group.RecordIndices)
        {
            if (!Records.IsValidIndex(RecordIndex))
            {
                continue;
            }
            FRecord& Record = Records[RecordIndex];
            if (!Record.bActive)
            {
                continue;
            }
            const double LocalTime = Record.OriginLocalTimeSeconds +
                (CurrentTimeSeconds - Record.OriginTimeSeconds) * Record.PlaybackRate;
            const double TotalDuration = Record.DurationSeconds * FMath::Max(Record.Iterations, 1);
            const bool bComplete = !Record.bPaused && LocalTime >= 0.0 &&
                (Record.DurationSeconds == 0.0 || LocalTime >= TotalDuration);
            SetRecordOccluded(Record,
                Record.bPaused || (RecordIndex != WinnerRecordIndex && !bComplete));

            if (Record.bPaused)
            {
                continue;
            }
            if (Record.bSuppressBeforeStart && LocalTime < 0.0 &&
                (WinnerRecordIndex == MAX_uint32 || Record.ContributionOrder > WinnerOrder))
            {
                NextBoundarySeconds = FMath::Min(NextBoundarySeconds,
                    CurrentTimeSeconds - LocalTime / Record.PlaybackRate);
            }
            if (RecordIndex != WinnerRecordIndex && !bComplete && LocalTime < TotalDuration)
            {
                NextBoundarySeconds = FMath::Min(NextBoundarySeconds,
                    CurrentTimeSeconds + (TotalDuration - LocalTime) / Record.PlaybackRate);
            }
        }
        if (NextBoundarySeconds < TNumericLimits<double>::Max())
        {
            PushLayeredBoundary({NextBoundarySeconds, GroupIndex, Group.ScheduleSerial});
        }
        Group.NextBoundaryTimeSeconds = NextBoundarySeconds;
    }

    void RefreshLayeredOcclusion()
    {
        if (LayeredNativeTargetToGroup.IsEmpty())
        {
            DirtyLayeredGroupIndices.Reset();
            LayeredBoundaryHeap.Reset();
            return;
        }
        constexpr double BoundaryToleranceSeconds = 1e-9;
        const double DueTimeSeconds = CurrentTimeSeconds + BoundaryToleranceSeconds;
        {
            FScopedRmlUiPerformanceTimer HeapDrainTimer(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationScheduleHeapDrain);
            while (!LayeredBoundaryHeap.IsEmpty() &&
                LayeredBoundaryHeap[0].TimeSeconds <= DueTimeSeconds)
            {
                WakeLayeredBoundary(PopLayeredBoundary());
            }
        }

        const int32 DirtyCount = DirtyLayeredGroupIndices.Num();
        {
            FScopedRmlUiPerformanceTimer GroupRefreshTimer(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationScheduleGroupRefresh);
            for (int32 DirtyIndex = 0; DirtyIndex < DirtyCount; ++DirtyIndex)
            {
                const int32 GroupIndex = DirtyLayeredGroupIndices[DirtyIndex];
                if (!LayeredGroups.IsValidIndex(GroupIndex) ||
                    !LayeredGroups[GroupIndex].bActive || !LayeredGroups[GroupIndex].bDirty)
                {
                    continue;
                }
                LayeredGroups[GroupIndex].bDirty = false;
                RefreshLayeredGroup(GroupIndex);
            }
        }
        ApplyPendingOcclusionChanges();
        {
            FScopedRmlUiPerformanceTimer MaintenanceTimer(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationScheduleMaintenance);
            DirtyLayeredGroupIndices.RemoveAt(0, DirtyCount, EAllowShrinking::No);
            RebuildLayeredBoundaryHeapIfNeeded();
        }
    }

    bool Pause(FRmlUiAnimationHandle Handle)
    {
        check(IsInGameThread());
        FRecord* Record = FindRecord(Handle);
        if (!Record) return false;
        if (Record->bPaused) return true;
        if (!RebaseRecord(*Record, {}, 0.0)) return false;
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        Linker->EntityManager.AddComponent(Record->Entity, Components.Paused);
        Record->bPaused = true;
        MarkLayeredGroupDirty(Record->LayeredGroupIndex);
        --RunningRecordCount;
        if (!Record->bOccluded)
        {
            AdjustEvaluatingRecordCount(*Record, -1);
        }
        return true;
    }

    bool Resume(FRmlUiAnimationHandle Handle)
    {
        check(IsInGameThread());
        FRecord* Record = FindRecord(Handle);
        if (!Record) return false;
        if (!Record->bPaused) return true;
        if (!RebaseRecord(*Record, {}, Record->PlaybackRate)) return false;
        if (Record->bScheduledPausedSample)
        {
            Record->bScheduledPausedSample = false;
        }
        else
        {
            const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
            Linker->EntityManager.RemoveComponent(Record->Entity, Components.Paused);
            ++RunningRecordCount;
            if (!Record->bOccluded)
            {
                AdjustEvaluatingRecordCount(*Record, 1);
            }
        }
        Record->bPaused = false;
        MarkLayeredGroupDirty(Record->LayeredGroupIndex);
        WakeRecord(*Record);
        return true;
    }

    bool Seek(FRmlUiAnimationHandle Handle, double LocalTimeSeconds)
    {
        check(IsInGameThread());
        if (!FMath::IsFinite(LocalTimeSeconds) || LocalTimeSeconds < 0.0) return false;
        FRecord* Record = FindRecord(Handle);
        if (!Record || Record->bLayeredContribution) return false;
        if (!RebaseRecord(*Record, LocalTimeSeconds, Record->bPaused ? 0.0 : Record->PlaybackRate))
            return false;
        if (Record->bPaused && !Record->bScheduledPausedSample)
        {
            const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
            Linker->EntityManager.RemoveComponent(Record->Entity, Components.Paused);
            Record->bScheduledPausedSample = true;
            PendingResuspendIndices.Add(HandleToRecord.FindChecked(Handle.Value));
            ++RunningRecordCount;
            if (!Record->bOccluded)
            {
                AdjustEvaluatingRecordCount(*Record, 1);
            }
        }
        WakeRecord(*Record);
        return true;
    }

    bool SetPlaybackRate(FRmlUiAnimationHandle Handle, double PlaybackRate)
    {
        check(IsInGameThread());
        if (!FMath::IsFinite(PlaybackRate) || PlaybackRate <= 0.0) return false;
        FRecord* Record = FindRecord(Handle);
        if (!Record) return false;
        if (!RebaseRecord(*Record, {}, Record->bPaused ? 0.0 : PlaybackRate)) return false;
        Record->PlaybackRate = PlaybackRate;
        MarkLayeredGroupDirty(Record->LayeredGroupIndex);
        if (!Record->bPaused) WakeRecord(*Record);
        return true;
    }

    void CommitLastVisualValue(const FRecord& Record)
    {
        if (!Record.bHasNativeTarget || !Record.bHasLastValue ||
            Record.NativeTarget.Property != ERmlUiAnimatedProperty::Opacity ||
            !(Record.NativeTarget.Target
                ? RmlUE_IsAnimationTargetValid(Record.NativeTarget.View, Record.NativeTarget.Target)
                : RmlUE_IsNodeValid(Record.NativeTarget.View, Record.NativeTarget.Node)))
        {
            return;
        }
        FRmlUiAnimationCommitUpdate Update{Record.NativeTarget.View, Record.NativeTarget.Node,
            Record.NativeTarget.Property,
            {Record.LastValues[0], Record.LastValues[1], Record.LastValues[2], Record.LastValues[3], Record.LastValues[4]},
            true, Record.NativeTarget.Target};
        TArray<ERmlUiAnimationCommitResult> Results;
        PropertyCommitSink->Commit(MakeArrayView(&Update, 1), Results);
        if (Results.Num() == 1 && Results[0] == ERmlUiAnimationCommitResult::Committed)
        {
            const RmlUE_AnimatedPropertyUpdate BridgeUpdate = ToBridgeUpdate(Update);
            RmlUE_ClearAnimatedVisualProperties(&BridgeUpdate, 1);
        }
    }

    bool CancelBinding(uint64 BindingId, ERmlUiAnimationCompletionReason Reason)
    {
        if (const uint64* Handle = BindingToHandle.Find(BindingId))
        {
            return Cancel(FRmlUiAnimationHandle{*Handle}, Reason);
        }
        return false;
    }

    bool CancelNativeTarget(const FNativeTargetKey& Target, ERmlUiAnimationCompletionReason Reason)
    {
        bool bCancelled = false;
        if (const uint64* Handle = NativeTargetToHandle.Find(Target))
        {
            const uint64 HandleValue = *Handle;
            bCancelled |= Cancel(FRmlUiAnimationHandle{HandleValue}, Reason);
        }
        if (const int32* GroupIndex = LayeredNativeTargetToGroup.Find(Target);
            GroupIndex && LayeredGroups.IsValidIndex(*GroupIndex) && LayeredGroups[*GroupIndex].bActive)
        {
            TArray<FRmlUiAnimationHandle> Handles;
            Handles.Reserve(LayeredGroups[*GroupIndex].RecordIndices.Num());
            for (uint32 RecordIndex : LayeredGroups[*GroupIndex].RecordIndices)
            {
                if (Records.IsValidIndex(RecordIndex) && Records[RecordIndex].bActive)
                {
                    Handles.Add(Records[RecordIndex].Handle);
                }
            }
            for (FRmlUiAnimationHandle Handle : Handles)
            {
                bCancelled |= Cancel(Handle, Reason);
            }
        }
        return bCancelled;
    }

    void CancelView(RmlUE_View* View)
    {
        check(IsInGameThread());
        if (!View) return;
        CancellingViews.Add(View);
        TArray<FRmlUiAnimationHandle> Handles;
        Handles.Reserve(ActiveRecordCount);
        for (const FRecord& Record : Records)
        {
            if (Record.bActive && Record.bHasNativeTarget && Record.NativeTarget.View == View)
            {
                Handles.Add(Record.Handle);
            }
        }
        for (FRmlUiAnimationHandle Handle : Handles)
        {
            Cancel(Handle, ERmlUiAnimationCompletionReason::Cancelled);
        }
        TArray<FRmlUiAnimationBindingHandle> ViewBindings;
        ViewBindings.Reserve(BindingCount);
        for (uint32 Index = 0; Index < static_cast<uint32>(Bindings.Num()); ++Index)
        {
            const FBindingRecord& Binding = Bindings[Index];
            if (Binding.bActive && Binding.bHasNativeTarget && Binding.NativeTarget.View == View)
            {
                ViewBindings.Add({PackHandle(Index, Binding.Generation)});
            }
        }
        for (FRmlUiAnimationBindingHandle Binding : ViewBindings)
        {
            ReleaseBinding(Binding);
        }
        CancellingViews.Remove(View);
    }

    void RemoveAll(bool bNotify)
    {
        TArray<uint64> Handles;
        HandleToRecord.GenerateKeyArray(Handles);
        for (uint64 Handle : Handles)
        {
            if (bNotify)
            {
                Cancel(FRmlUiAnimationHandle{Handle}, ERmlUiAnimationCompletionReason::Cancelled);
            }
            else
            {
                RemoveWithoutNotification(FRmlUiAnimationHandle{Handle});
            }
        }
    }

    void Advance(float DeltaSeconds)
    {
        check(IsInGameThread());
        FScopedRmlUiPerformanceTimer AdvanceTimer(
            ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceStage::AnimationAdvance);
        if (bAdvancing)
        {
            PendingDeltaSeconds += FMath::Max(DeltaSeconds, 0.0f);
            return;
        }
        bAdvancing = true;
        CurrentTimeSeconds += FMath::Max(DeltaSeconds, 0.0f);
        {
            FScopedRmlUiPerformanceTimer ScheduleTimer(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationSchedule);
            RefreshLayeredOcclusion();
        }

        if (EvaluatingRecordCount != 0)
        {
            Evaluate();
            DispatchResults();
        }
        PostAdvanceCallbacks.Broadcast();

        bAdvancing = false;
        if (PendingDeltaSeconds > 0.0f)
        {
            const float DeferredDelta = PendingDeltaSeconds;
            PendingDeltaSeconds = 0.0f;
            Advance(DeferredDelta);
        }
    }

    void Evaluate()
    {
        FScopedRmlUiPerformanceTimer EvaluateTimer(
            ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceStage::AnimationEvaluate);
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationTracksEvaluated,
            EvaluatingRecordCount);
        using namespace UE::MovieScene;
        SampleSystem->SetEvaluationTime(CurrentTimeSeconds);
        Linker->EntityManager.UpdateThreadingModel();
        Linker->EntityManager.LockDown();
        FGraphEventArray Tasks;
        Linker->SystemGraph.ExecutePhase(ESystemPhase::Evaluation, Linker.Get(), Tasks);
        if (!Tasks.IsEmpty())
        {
            FTaskGraphInterface::Get().WaitUntilTasksComplete(Tasks, ENamedThreads::GameThread);
        }
        Linker->EntityManager.ReleaseLockDown();
    }

    void RemoveRecordsWithoutNotificationBatch(
        const TArray<FRmlUiAnimationHandle>& Handles,
        const TArray<uint32>& RecordIndices)
    {
        check(!Handles.IsEmpty());
        check(RecordIndices.Num() == Handles.Num());

        TBitArray<>& RecordBits = RemovalRecordBitScratch;
        RecordBits.Init(false, Records.Num());
        for (uint32 RecordIndex : RecordIndices)
        {
            RecordBits[RecordIndex] = true;
        }

        const RmlUiAnimation::FComponentTypes& Components =
            RmlUiAnimation::FComponentTypes::Get();
        const UE::MovieScene::FComponentTypeID NeedsUnlink =
            UE::MovieScene::FBuiltInComponentTypes::Get()->Tags.NeedsUnlink;

        struct FMarkRemovalMutation final : UE::MovieScene::IMovieSceneConditionalEntityMutation
        {
            const TBitArray<>& RecordBits;
            UE::MovieScene::TComponentTypeID<uint32> RecordIndexComponent;
            UE::MovieScene::FComponentTypeID NeedsUnlinkComponent;

            FMarkRemovalMutation(const TBitArray<>& InRecordBits,
                UE::MovieScene::TComponentTypeID<uint32> InRecordIndexComponent,
                UE::MovieScene::FComponentTypeID InNeedsUnlinkComponent)
                : RecordBits(InRecordBits)
                , RecordIndexComponent(InRecordIndexComponent)
                , NeedsUnlinkComponent(InNeedsUnlinkComponent)
            {
            }

            void MarkAllocation(UE::MovieScene::FEntityAllocation* Allocation,
                TBitArray<>& OutEntitiesToMutate) const override
            {
                const UE::MovieScene::TComponentReader<uint32> EntityRecordIndices =
                    Allocation->ReadComponents(RecordIndexComponent);
                const int32 EntityCount = Allocation->Num();
                for (int32 EntityIndex = 0; EntityIndex < EntityCount; ++EntityIndex)
                {
                    const uint32 RecordIndex = EntityRecordIndices[EntityIndex];
                    if (RecordBits.IsValidIndex(RecordIndex) && RecordBits[RecordIndex])
                    {
                        OutEntitiesToMutate.PadToNum(EntityIndex + 1, false);
                        OutEntitiesToMutate[EntityIndex] = true;
                    }
                }
            }

            void CreateMutation(UE::MovieScene::FEntityManager*,
                UE::MovieScene::FComponentMask* InOutEntityComponentTypes) const override
            {
                InOutEntityComponentTypes->Set(NeedsUnlinkComponent);
            }
        };

        const uint64 EntityReleaseStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        const uint64 EntityMarkStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        FMarkRemovalMutation MarkRemoval(RecordBits, Components.RecordIndex, NeedsUnlink);
        UE::MovieScene::FEntityComponentFilter MarkFilter;
        MarkFilter.All({Components.RecordIndex});
        MarkFilter.None({NeedsUnlink});
        const int32 MarkedEntityCount =
            Linker->EntityManager.MutateConditional(MarkFilter, MarkRemoval);
        check(MarkedEntityCount == Handles.Num());
        if (EntityMarkStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeEntityMark,
                FPlatformTime::Cycles64() - EntityMarkStart);
        }

        const uint64 DetachStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        int32 DetachedRecordCount = 0;
        for (FRmlUiAnimationHandle Handle : Handles)
        {
            FRecord Record;
            if (TakeRecord(Handle, Record))
            {
                ++DetachedRecordCount;
            }
        }
        check(DetachedRecordCount == Handles.Num());
        if (DetachStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeDetach,
                FPlatformTime::Cycles64() - DetachStart);
        }

        const uint64 EntityFreeStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        UE::MovieScene::FEntityComponentFilter FreeFilter;
        FreeFilter.All({Components.RecordIndex, NeedsUnlink});
        const int32 FreedEntityCount = Linker->EntityManager.FreeEntities(FreeFilter);
        check(FreedEntityCount == Handles.Num());
        if (EntityFreeStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeEntityFree,
                FPlatformTime::Cycles64() - EntityFreeStart);
        }
        if (EntityReleaseStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeEntityRelease,
                FPlatformTime::Cycles64() - EntityReleaseStart);
        }
    }

    void DispatchResults()
    {
        const uint64 CollectStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        using namespace UE::MovieScene;
        const RmlUiAnimation::FComponentTypes& Components = RmlUiAnimation::FComponentTypes::Get();
        TArray<FDispatch>& Dispatches = DispatchScratch;
        TArray<FTransformDispatch>& TransformDispatches = TransformDispatchScratch;
        Dispatches.Reset();
        TransformDispatches.Reset();
        Dispatches.Reserve(ActiveRecordCount);
        TransformDispatches.Reserve(ActiveRecordCount);
        TArray<int32>& LayeredWinners = LayeredWinnerScratch;
        LayeredWinners.Init(INDEX_NONE, LayeredGroups.Num());
        FEntityTaskBuilder()
            .Read(Components.RecordIndex)
            .Read(Components.Output)
            .FilterNone({Components.Paused, Components.Occluded})
            .Iterate_PerEntity(&Linker->EntityManager,
                [this, &Dispatches, &LayeredWinners]
                (uint32 RecordIndex, const RmlUiAnimation::FOutput& Output)
                {
                    if (Records.IsValidIndex(RecordIndex) && Records[RecordIndex].bActive)
                    {
                        FRecord& Record = Records[RecordIndex];
                        if (Record.bHasNativeTarget)
                        {
                            Record.LastValues[0] = Output.Value;
                            Record.bHasLastValue = true;
                        }
                        const int32 DispatchIndex = Dispatches.Add(FDispatch{
                            RecordIndex,
                            Output.Value,
                            Output.bChanged,
                            Output.bComplete && !Record.bPaused});
                        if (Record.bLayeredContribution && Output.bContributes &&
                            LayeredWinners.IsValidIndex(Record.LayeredGroupIndex))
                        {
                            int32& WinnerIndex = LayeredWinners[Record.LayeredGroupIndex];
                            if (WinnerIndex == INDEX_NONE ||
                                Records[Dispatches[WinnerIndex].RecordIndex].ContributionOrder <
                                    Record.ContributionOrder)
                            {
                                WinnerIndex = DispatchIndex;
                            }
                        }
                    }
                });

        FEntityTaskBuilder()
            .Read(Components.RecordIndex)
            .Read(Components.TransformOutput)
            .FilterNone({Components.Paused, Components.Occluded})
            .Iterate_PerEntity(&Linker->EntityManager,
                [this, &TransformDispatches, &LayeredWinners]
                (uint32 RecordIndex, const RmlUiAnimation::FTransform2DOutput& Output)
                {
                    if (Records.IsValidIndex(RecordIndex) && Records[RecordIndex].bActive)
                    {
                        FRecord& Record = Records[RecordIndex];
                        Record.LastValues[0] = Output.Value.TranslationX;
                        Record.LastValues[1] = Output.Value.TranslationY;
                        Record.LastValues[2] = Output.Value.ScaleX;
                        Record.LastValues[3] = Output.Value.ScaleY;
                        Record.LastValues[4] = Output.Value.RotationDegrees;
                        Record.bHasLastValue = true;
                        const int32 DispatchIndex = TransformDispatches.Add(FTransformDispatch{
                            RecordIndex, Output.Value, Output.bChanged,
                            Output.bComplete && !Record.bPaused});
                        if (Record.bLayeredContribution && Output.bContributes &&
                            LayeredWinners.IsValidIndex(Record.LayeredGroupIndex))
                        {
                            int32& WinnerIndex = LayeredWinners[Record.LayeredGroupIndex];
                            if (WinnerIndex == INDEX_NONE ||
                                Records[TransformDispatches[WinnerIndex].RecordIndex].ContributionOrder <
                                    Record.ContributionOrder)
                            {
                                WinnerIndex = DispatchIndex;
                            }
                        }
                    }
                });

        const RmlUiAnimation::FComponentTypes& MutableComponents =
            RmlUiAnimation::FComponentTypes::Get();
        for (uint32 RecordIndex : PendingResuspendIndices)
        {
            if (Records.IsValidIndex(RecordIndex))
            {
                FRecord& Record = Records[RecordIndex];
                if (Record.bActive && Record.bPaused && Record.bScheduledPausedSample)
                {
                    Linker->EntityManager.AddComponent(Record.Entity, MutableComponents.Paused);
                    Record.bScheduledPausedSample = false;
                    --RunningRecordCount;
                    if (!Record.bOccluded)
                    {
                        AdjustEvaluatingRecordCount(Record, -1);
                    }
                }
            }
        }
        PendingResuspendIndices.Reset();

        TArray<FRmlUiAnimationCommitUpdate>& CommitUpdates = CommitUpdateScratch;
        TArray<uint64>& CommitHandles = CommitHandleScratch;
        CommitUpdates.Reset();
        CommitHandles.Reset();
        CommitUpdates.Reserve(ActiveRecordCount);
        CommitHandles.Reserve(ActiveRecordCount);

        for (int32 Index = 0; Index < Dispatches.Num(); ++Index)
        {
            const FDispatch& Dispatch = Dispatches[Index];
            FRecord& Record = Records[Dispatch.RecordIndex];
            FLayeredGroup* LayeredGroup = nullptr;
            if (Record.bLayeredContribution)
            {
                if (!LayeredGroups.IsValidIndex(Record.LayeredGroupIndex) ||
                    LayeredWinners[Record.LayeredGroupIndex] != Index) continue;
                LayeredGroup = &LayeredGroups[Record.LayeredGroupIndex];
            }
            const bool bWinnerChanged = Record.bLayeredContribution &&
                LayeredGroup->PreviousWinner != Record.Handle.Value;
            if ((Dispatch.bChanged || Dispatch.bComplete || bWinnerChanged) && Record.bHasNativeTarget)
            {
                CommitUpdates.Add({Record.NativeTarget.View, Record.NativeTarget.Node,
                    Record.NativeTarget.Property, {Dispatch.Value, 0.f, 0.f, 0.f, 0.f}, Dispatch.bComplete,
                    Record.NativeTarget.Target});
                CommitHandles.Add(Record.Handle.Value);
            }
            if (LayeredGroup) LayeredGroup->PreviousWinner = Record.Handle.Value;
        }
        for (int32 Index = 0; Index < TransformDispatches.Num(); ++Index)
        {
            const FTransformDispatch& Dispatch = TransformDispatches[Index];
            FRecord& Record = Records[Dispatch.RecordIndex];
            FLayeredGroup* LayeredGroup = nullptr;
            if (Record.bLayeredContribution)
            {
                if (!LayeredGroups.IsValidIndex(Record.LayeredGroupIndex) ||
                    LayeredWinners[Record.LayeredGroupIndex] != Index) continue;
                LayeredGroup = &LayeredGroups[Record.LayeredGroupIndex];
            }
            const bool bWinnerChanged = Record.bLayeredContribution &&
                LayeredGroup->PreviousWinner != Record.Handle.Value;
            if (Dispatch.bChanged || Dispatch.bComplete || bWinnerChanged)
            {
                const auto& V = Dispatch.Value;
                CommitUpdates.Add({Record.NativeTarget.View, Record.NativeTarget.Node,
                    Record.NativeTarget.Property,
                    {V.TranslationX, V.TranslationY, V.ScaleX, V.ScaleY, V.RotationDegrees}, Dispatch.bComplete,
                    Record.NativeTarget.Target});
                CommitHandles.Add(Record.Handle.Value);
            }
            if (LayeredGroup) LayeredGroup->PreviousWinner = Record.Handle.Value;
        }

        uint64 ChangedPropertyCount = 0;
        uint64 CompletedInstanceCount = 0;
        for (const FDispatch& Dispatch : Dispatches)
        {
            ChangedPropertyCount += Dispatch.bChanged ? 1 : 0;
            CompletedInstanceCount += Dispatch.bComplete ? 1 : 0;
        }
        for (const FTransformDispatch& Dispatch : TransformDispatches)
        {
            ChangedPropertyCount += Dispatch.bChanged ? 1 : 0;
            CompletedInstanceCount += Dispatch.bComplete ? 1 : 0;
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationChangedProperties,
            ChangedPropertyCount);
        if (CollectStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCollect,
                FPlatformTime::Cycles64() - CollectStart);
        }

        TSet<uint64>& InvalidNativeHandles = InvalidNativeHandleScratch;
        InvalidNativeHandles.Reset();
        const uint64 CommitStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        TArray<ERmlUiAnimationCommitResult>& CommitResults = CommitResultScratch;
        VisualCommitSink->Commit(CommitUpdates, CommitResults);

        const uint64 FallbackStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        TArray<FRmlUiAnimationCommitUpdate>& FallbackUpdates = FallbackUpdateScratch;
        TArray<int32>& FallbackIndices = FallbackIndexScratch;
        FallbackUpdates.Reset();
        FallbackIndices.Reset();
        for (int32 Index = 0; Index < CommitResults.Num(); ++Index)
        {
            if (CommitResults[Index] == ERmlUiAnimationCommitResult::Unsupported)
            {
                FallbackUpdates.Add(CommitUpdates[Index]);
                FallbackIndices.Add(Index);
            }
            else if (CommitResults[Index] == ERmlUiAnimationCommitResult::InvalidTarget)
            {
                InvalidNativeHandles.Add(CommitHandles[Index]);
            }
        }
        TArray<ERmlUiAnimationCommitResult>& FallbackResults = FallbackResultScratch;
        PropertyCommitSink->Commit(FallbackUpdates, FallbackResults);
        for (int32 FallbackIndex = 0; FallbackIndex < FallbackResults.Num(); ++FallbackIndex)
        {
            const int32 CommitIndex = FallbackIndices[FallbackIndex];
            CommitResults[CommitIndex] = FallbackResults[FallbackIndex];
            if (FallbackResults[FallbackIndex] == ERmlUiAnimationCommitResult::InvalidTarget)
            {
                InvalidNativeHandles.Add(CommitHandles[CommitIndex]);
            }
        }
        if (FallbackStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitFallback,
                FPlatformTime::Cycles64() - FallbackStart);
        }

        const uint64 PostprocessStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        TArray<RmlUE_AnimatedPropertyUpdate, TInlineAllocator<64>> ClearVisualUpdates;
        uint64 CommittedPropertyCount = 0;
        for (int32 Index = 0; Index < CommitResults.Num(); ++Index)
        {
            if (CommitResults[Index] != ERmlUiAnimationCommitResult::Committed) continue;
            ++CommittedPropertyCount;
            const FRmlUiAnimationCommitUpdate& Update = CommitUpdates[Index];
            if (Update.bFinal && (Update.Property == ERmlUiAnimatedProperty::Opacity ||
                Update.Property == ERmlUiAnimatedProperty::Transform2D))
            {
                ClearVisualUpdates.Add(ToBridgeUpdate(Update));
            }
        }
        if (!ClearVisualUpdates.IsEmpty())
        {
            RmlUE_ClearAnimatedVisualProperties(ClearVisualUpdates.GetData(), ClearVisualUpdates.Num());
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCommittedProperties,
            CommittedPropertyCount);
        if (PostprocessStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommitPostprocess,
                FPlatformTime::Cycles64() - PostprocessStart);
        }
        if (CommitStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCommit,
                FPlatformTime::Cycles64() - CommitStart);
        }

        const uint64 FinalizeStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        TArray<FValueNotification>& ValueNotifications = ValueNotificationScratch;
        TArray<FCompletionNotification>& CompletionNotifications = CompletionNotificationScratch;
        TArray<FRmlUiAnimationHandle>& RemovalHandles = RemovalHandleScratch;
        TArray<uint32>& RemovalRecordIndices = RemovalRecordIndexScratch;
        ValueNotifications.Reset();
        CompletionNotifications.Reset();
        RemovalHandles.Reset();
        RemovalRecordIndices.Reset();
        const uint64 FinalizePrepareStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        for (const FDispatch& Dispatch : Dispatches)
        {
            FRecord& Record = Records[Dispatch.RecordIndex];
            const FRmlUiAnimationHandle Handle = Record.Handle;
            const bool bInvalidNativeTarget = InvalidNativeHandles.Contains(Handle.Value);
            if (!bInvalidNativeTarget && Dispatch.bChanged && Record.OnValue)
            {
                ValueNotifications.Add({Handle, Dispatch.Value, Record.OnValue});
            }
            if ((Dispatch.bComplete || bInvalidNativeTarget) && Record.OnComplete)
            {
                CompletionNotifications.Add({Handle,
                    bInvalidNativeTarget ? ERmlUiAnimationCompletionReason::Cancelled
                                         : ERmlUiAnimationCompletionReason::Completed,
                    Record.OnComplete});
            }
            if (Dispatch.bComplete || bInvalidNativeTarget)
            {
                RemovalHandles.Add(Handle);
                RemovalRecordIndices.Add(Dispatch.RecordIndex);
            }
        }
        for (const FTransformDispatch& Dispatch : TransformDispatches)
        {
            FRecord& Record = Records[Dispatch.RecordIndex];
            const FRmlUiAnimationHandle Handle = Record.Handle;
            const bool bInvalidNativeTarget = InvalidNativeHandles.Contains(Handle.Value);
            if ((Dispatch.bComplete || bInvalidNativeTarget) && Record.OnComplete)
            {
                CompletionNotifications.Add({Handle,
                    bInvalidNativeTarget ? ERmlUiAnimationCompletionReason::Cancelled
                                         : ERmlUiAnimationCompletionReason::Completed,
                    Record.OnComplete});
            }
            if (Dispatch.bComplete || bInvalidNativeTarget)
            {
                RemovalHandles.Add(Handle);
                RemovalRecordIndices.Add(Dispatch.RecordIndex);
            }
        }
        if (FinalizePrepareStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizePrepare,
                FPlatformTime::Cycles64() - FinalizePrepareStart);
        }

        const uint64 FinalizeRemoveStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        if (!RemovalHandles.IsEmpty())
        {
            RemoveRecordsWithoutNotificationBatch(RemovalHandles, RemovalRecordIndices);
        }
        if (FinalizeRemoveStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeRemove,
                FPlatformTime::Cycles64() - FinalizeRemoveStart);
        }

        const uint64 FinalizeCallbacksStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        for (const FValueNotification& Notification : ValueNotifications)
        {
            Notification.Callback(Notification.Handle, Notification.Value);
        }
        for (const FCompletionNotification& Notification : CompletionNotifications)
        {
            Notification.Callback(Notification.Handle, Notification.Reason);
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletionCallbacks,
            CompletionNotifications.Num());
        if (FinalizeCallbacksStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalizeCallbacks,
                FPlatformTime::Cycles64() - FinalizeCallbacksStart);
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletedInstances,
            CompletedInstanceCount + InvalidNativeHandles.Num());
        if (FinalizeStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationFinalize,
                FPlatformTime::Cycles64() - FinalizeStart);
        }
    }

    void RemoveWithoutNotification(FRmlUiAnimationHandle Handle)
    {
        FRecord Record;
        if (TakeRecord(Handle, Record))
        {
            ReleaseEntity(Record.Entity);
        }
    }

    uint32 AllocateRecordIndex()
    {
        if (!FreeRecordIndices.IsEmpty())
        {
            return FreeRecordIndices.Pop(EAllowShrinking::No);
        }
        return Records.AddDefaulted();
    }

    uint32 AllocateDefinitionIndex()
    {
        if (!FreeDefinitionIndices.IsEmpty())
        {
            return FreeDefinitionIndices.Pop(EAllowShrinking::No);
        }
        return Definitions.AddDefaulted();
    }

    uint32 AllocateBindingIndex()
    {
        if (!FreeBindingIndices.IsEmpty())
        {
            return FreeBindingIndices.Pop(EAllowShrinking::No);
        }
        return Bindings.AddDefaulted();
    }

    int32 AddLayeredRecord(const FNativeTargetKey& Target, uint32 RecordIndex)
    {
        int32 GroupIndex = INDEX_NONE;
        if (const int32* ExistingIndex = LayeredNativeTargetToGroup.Find(Target))
        {
            GroupIndex = *ExistingIndex;
        }
        else
        {
            GroupIndex = FreeLayeredGroupIndices.IsEmpty()
                ? LayeredGroups.AddDefaulted()
                : FreeLayeredGroupIndices.Pop(EAllowShrinking::No);
            FLayeredGroup& NewGroup = LayeredGroups[GroupIndex];
            NewGroup = FLayeredGroup{};
            NewGroup.NativeTarget = Target;
            NewGroup.bActive = true;
            LayeredNativeTargetToGroup.Add(Target, GroupIndex);
        }
        check(LayeredGroups.IsValidIndex(GroupIndex) && LayeredGroups[GroupIndex].bActive);
        LayeredGroups[GroupIndex].RecordIndices.Add(RecordIndex);
        MarkLayeredGroupDirty(GroupIndex);
        return GroupIndex;
    }

    FRmlUiAnimationBindingHandle AllocateBinding(
        FRmlUiAnimationDefinitionHandle Definition,
        const FNativeTargetKey* NativeTarget,
        uint64 ReplacementId)
    {
        FDefinitionRecord* DefinitionRecord = FindDefinition(Definition);
        if (!DefinitionRecord)
        {
            return {};
        }
        const uint32 Index = AllocateBindingIndex();
        FBindingRecord& Record = Bindings[Index];
        Record.Definition = Definition;
        Record.ReplacementId = ReplacementId;
        Record.bHasNativeTarget = NativeTarget != nullptr;
        Record.NativeTarget = NativeTarget ? *NativeTarget : FNativeTargetKey{};
        Record.bActive = true;
        ++DefinitionRecord->BindingCount;
        ++BindingCount;
        return {PackHandle(Index, Record.Generation)};
    }

    bool TakeRecord(FRmlUiAnimationHandle Handle, FRecord& OutRecord)
    {
        uint32 RecordIndex = 0;
        if (!HandleToRecord.RemoveAndCopyValue(Handle.Value, RecordIndex) ||
            !Records.IsValidIndex(RecordIndex) || !Records[RecordIndex].bActive)
        {
            return false;
        }

        OutRecord = MoveTemp(Records[RecordIndex]);
        Records[RecordIndex] = FRecord{};
        FreeRecordIndices.Add(RecordIndex);
        --ActiveRecordCount;
        if (!OutRecord.bPaused || OutRecord.bScheduledPausedSample)
        {
            --RunningRecordCount;
            if (!OutRecord.bOccluded)
            {
                AdjustEvaluatingRecordCount(OutRecord, -1);
            }
        }
        if (OutRecord.BindingId != 0)
        {
            const uint64* CurrentHandle = BindingToHandle.Find(OutRecord.BindingId);
            if (CurrentHandle && *CurrentHandle == Handle.Value)
            {
                BindingToHandle.Remove(OutRecord.BindingId);
            }
        }
        if (OutRecord.bHasNativeTarget)
        {
            if (OutRecord.bLayeredContribution)
            {
                if (LayeredGroups.IsValidIndex(OutRecord.LayeredGroupIndex))
                {
                    FLayeredGroup& Group = LayeredGroups[OutRecord.LayeredGroupIndex];
                    Group.RecordIndices.RemoveSingleSwap(RecordIndex, EAllowShrinking::No);
                    if (Group.RecordIndices.IsEmpty())
                    {
                        LayeredNativeTargetToGroup.Remove(Group.NativeTarget);
                        DirtyLayeredGroupIndices.RemoveSingleSwap(
                            OutRecord.LayeredGroupIndex, EAllowShrinking::No);
                        Group = FLayeredGroup{};
                        FreeLayeredGroupIndices.Add(OutRecord.LayeredGroupIndex);
                        if (LayeredNativeTargetToGroup.IsEmpty())
                        {
                            LayeredBoundaryHeap.Reset();
                        }
                    }
                    else
                    {
                        MarkLayeredGroupDirty(OutRecord.LayeredGroupIndex);
                    }
                }
            }
            else if (const uint64* CurrentHandle = NativeTargetToHandle.Find(OutRecord.NativeTarget);
                CurrentHandle && *CurrentHandle == Handle.Value)
            {
                NativeTargetToHandle.Remove(OutRecord.NativeTarget);
            }
        }
        if (OutRecord.ResolvedBinding.IsValid())
        {
            const uint64* CurrentHandle = ResolvedBindingToAnimation.Find(OutRecord.ResolvedBinding.Value);
            if (CurrentHandle && *CurrentHandle == Handle.Value)
            {
                ResolvedBindingToAnimation.Remove(OutRecord.ResolvedBinding.Value);
            }
        }
        return true;
    }

    void ReleaseEntity(UE::MovieScene::FMovieSceneEntityID Entity)
    {
        if (Linker->EntityManager.IsAllocated(Entity))
        {
            Linker->EntityManager.AddComponent(
                Entity, UE::MovieScene::FBuiltInComponentTypes::Get()->Tags.NeedsUnlink);
            Linker->EntityManager.FreeEntity(Entity);
        }
    }

    TStrongObjectPtr<UMovieSceneEntitySystemLinker> Linker;
    TObjectPtr<URmlUiAnimationSampleSystem> SampleSystem = nullptr;
    TArray<FRecord> Records;
    TArray<uint32> FreeRecordIndices;
    TArray<FDispatch> DispatchScratch;
    TArray<FTransformDispatch> TransformDispatchScratch;
    TArray<int32> LayeredWinnerScratch;
    TArray<FValueNotification> ValueNotificationScratch;
    TArray<FCompletionNotification> CompletionNotificationScratch;
    TArray<FRmlUiAnimationHandle> RemovalHandleScratch;
    TArray<uint32> RemovalRecordIndexScratch;
    TBitArray<> RemovalRecordBitScratch;
    TArray<uint32> PendingResuspendIndices;
    TArray<FRmlUiAnimationCommitUpdate> CommitUpdateScratch;
    TArray<uint64> CommitHandleScratch;
    TArray<ERmlUiAnimationCommitResult> CommitResultScratch;
    TArray<FRmlUiAnimationCommitUpdate> FallbackUpdateScratch;
    TArray<int32> FallbackIndexScratch;
    TArray<ERmlUiAnimationCommitResult> FallbackResultScratch;
    TSet<uint64> InvalidNativeHandleScratch;
    TArray<FDefinitionRecord> Definitions;
    TArray<uint32> FreeDefinitionIndices;
    TArray<FBindingRecord> Bindings;
    TArray<uint32> FreeBindingIndices;
    TMap<uint64, uint32> HandleToRecord;
    TMap<uint64, uint64> BindingToHandle;
    TMap<uint64, uint64> ResolvedBindingToAnimation;
    TMap<FNativeTargetKey, uint64> NativeTargetToHandle;
    TArray<FLayeredGroup> LayeredGroups;
    TArray<int32> FreeLayeredGroupIndices;
    TMap<FNativeTargetKey, int32> LayeredNativeTargetToGroup;
    TArray<int32> DirtyLayeredGroupIndices;
    TArray<FLayeredBoundary> LayeredBoundaryHeap;
    TSet<RmlUE_View*> CancellingViews;
    TUniquePtr<IRmlUiAnimationCommitSink> VisualCommitSink;
    TUniquePtr<IRmlUiAnimationCommitSink> PropertyCommitSink;
    TFunction<void(RmlUE_View*)> WakeCallback;
    FSimpleMulticastDelegate PostAdvanceCallbacks;
    TMap<RmlUE_View*, int32> ViewEvaluatingRecordCounts;
    int32 ActiveRecordCount = 0;
    int32 RunningRecordCount = 0;
    int32 EvaluatingRecordCount = 0;
    int32 PendingOcclusionChanges = 0;
    int32 DefinitionCount = 0;
    int32 BindingCount = 0;
    uint64 NextHandle = 1;
    uint64 NextLayeredScheduleSerial = 1;
    double CurrentTimeSeconds = 0.0;
    float PendingDeltaSeconds = 0.0f;
    bool bAdvancing = false;
};

FRmlUiAnimationRuntime::FRmlUiAnimationRuntime()
    : Impl(MakeUnique<FImpl>())
{
}

FRmlUiAnimationRuntime::~FRmlUiAnimationRuntime() = default;

FRmlUiAnimationDefinitionHandle FRmlUiAnimationRuntime::RegisterFloatDefinition(
    ERmlUiAnimatedProperty Property,
    const FRmlUiFloatAnimationDefinition& Definition)
{
    return Impl->RegisterFloatDefinition(Property, Definition);
}

FRmlUiAnimationDefinitionHandle FRmlUiAnimationRuntime::RegisterTransform2DDefinition(
    const FRmlUiTransform2DAnimationDefinition& Definition)
{
    return Impl->RegisterTransform2DDefinition(Definition);
}

bool FRmlUiAnimationRuntime::ReleaseDefinition(FRmlUiAnimationDefinitionHandle Handle)
{
    return Impl->ReleaseDefinition(Handle);
}

FRmlUiAnimationBindingHandle FRmlUiAnimationRuntime::BindCallback(
    FRmlUiAnimationDefinitionHandle Definition,
    uint64 ReplacementId)
{
    return Impl->BindCallback(Definition, ReplacementId);
}

FRmlUiAnimationBindingHandle FRmlUiAnimationRuntime::BindNode(
    FRmlUiAnimationDefinitionHandle Definition,
    RmlUE_View* View,
    uint32 Node,
    uint64 ReplacementId)
{
    return Impl->BindNode(Definition, View, Node, ReplacementId);
}

int32 FRmlUiAnimationRuntime::BindNodes(
    FRmlUiAnimationDefinitionHandle Definition,
    RmlUE_View* View,
    const TArray<uint32>& Nodes,
    TArray<FRmlUiAnimationBindingHandle>& OutBindings)
{
    return Impl->BindNodes(Definition, View, Nodes, OutBindings);
}

bool FRmlUiAnimationRuntime::ReleaseBinding(FRmlUiAnimationBindingHandle Handle)
{
    return Impl->ReleaseBinding(Handle);
}

FRmlUiAnimationHandle FRmlUiAnimationRuntime::PlayBinding(
    FRmlUiAnimationBindingHandle Binding,
    FRmlUiAnimationValueCallback OnValue,
    FRmlUiAnimationCompletionCallback OnComplete)
{
    return Impl->PlayBinding(Binding, MoveTemp(OnValue), MoveTemp(OnComplete));
}

int32 FRmlUiAnimationRuntime::PlayBindings(
    const TArray<FRmlUiAnimationBindingHandle>& Bindings,
    TArray<FRmlUiAnimationHandle>& OutHandles,
    TArray<FRmlUiAnimationCompletionCallback> OnCompletes)
{
    return Impl->PlayBindings(Bindings, OutHandles, MoveTemp(OnCompletes));
}

int32 FRmlUiAnimationRuntime::PlayContributionBindings(
    const TArray<FRmlUiAnimationBindingHandle>& Bindings,
    TConstArrayView<FRmlUiAnimationContributionSpec> Contributions,
    TArray<FRmlUiAnimationHandle>& OutHandles,
    TArray<FRmlUiAnimationCompletionCallback> OnCompletes)
{
    return Impl->PlayContributionBindings(
        Bindings, Contributions, OutHandles, MoveTemp(OnCompletes));
}

FRmlUiAnimationHandle FRmlUiAnimationRuntime::PlayFloat(
    const FRmlUiFloatAnimationDesc& Desc,
    FRmlUiAnimationValueCallback OnValue,
    FRmlUiAnimationCompletionCallback OnComplete)
{
    FRmlUiFloatAnimationDefinition Definition;
    Definition.From = Desc.From;
    Definition.To = Desc.To;
    Definition.DurationSeconds = Desc.DurationSeconds;
    Definition.DelaySeconds = Desc.DelaySeconds;
    Definition.Iterations = Desc.Iterations;
    Definition.PlaybackRate = Desc.PlaybackRate;
    Definition.Direction = Desc.Direction;
    const FRmlUiAnimationDefinitionHandle DefinitionHandle =
        RegisterFloatDefinition(ERmlUiAnimatedProperty::None, Definition);
    const FRmlUiAnimationBindingHandle Binding = BindCallback(DefinitionHandle, Desc.BindingId);
    if (!DefinitionHandle.IsValid() || !Binding.IsValid())
    {
        if (Binding.IsValid()) ReleaseBinding(Binding);
        if (DefinitionHandle.IsValid()) ReleaseDefinition(DefinitionHandle);
        return {};
    }
    const FRmlUiAnimationHandle Handle = PlayBinding(
        Binding,
        MoveTemp(OnValue),
        [this, Binding, DefinitionHandle, Completion = MoveTemp(OnComplete)](
            FRmlUiAnimationHandle CompletedHandle, ERmlUiAnimationCompletionReason Reason) mutable
        {
            if (Completion) Completion(CompletedHandle, Reason);
            ReleaseBinding(Binding);
            ReleaseDefinition(DefinitionHandle);
        });
    if (!Handle.IsValid())
    {
        ReleaseBinding(Binding);
        ReleaseDefinition(DefinitionHandle);
    }
    return Handle;
}

FRmlUiAnimationHandle FRmlUiAnimationRuntime::PlayNodeFloat(
    RmlUE_View* View,
    uint32 Node,
    ERmlUiAnimatedProperty Property,
    const FRmlUiFloatAnimationDesc& Desc,
    FRmlUiAnimationCompletionCallback OnComplete)
{
    FRmlUiFloatAnimationDefinition Definition;
    Definition.From = Desc.From;
    Definition.To = Desc.To;
    Definition.DurationSeconds = Desc.DurationSeconds;
    Definition.DelaySeconds = Desc.DelaySeconds;
    Definition.Iterations = Desc.Iterations;
    Definition.PlaybackRate = Desc.PlaybackRate;
    Definition.Direction = Desc.Direction;
    const FRmlUiAnimationDefinitionHandle DefinitionHandle = RegisterFloatDefinition(Property, Definition);
    const FRmlUiAnimationBindingHandle Binding = BindNode(DefinitionHandle, View, Node, Desc.BindingId);
    if (!DefinitionHandle.IsValid() || !Binding.IsValid())
    {
        if (Binding.IsValid()) ReleaseBinding(Binding);
        if (DefinitionHandle.IsValid()) ReleaseDefinition(DefinitionHandle);
        return {};
    }
    const FRmlUiAnimationHandle Handle = PlayBinding(
        Binding,
        {},
        [this, Binding, DefinitionHandle, Completion = MoveTemp(OnComplete)](
            FRmlUiAnimationHandle CompletedHandle, ERmlUiAnimationCompletionReason Reason) mutable
        {
            if (Completion) Completion(CompletedHandle, Reason);
            ReleaseBinding(Binding);
            ReleaseDefinition(DefinitionHandle);
        });
    if (!Handle.IsValid())
    {
        ReleaseBinding(Binding);
        ReleaseDefinition(DefinitionHandle);
    }
    return Handle;
}

FRmlUiAnimationHandle FRmlUiAnimationRuntime::PlayNodeTransform2D(
    RmlUE_View* View,
    uint32 Node,
    const FRmlUiTransform2DAnimationDesc& Desc,
    FRmlUiAnimationCompletionCallback OnComplete)
{
    FRmlUiTransform2DAnimationDefinition Definition;
    Definition.From = Desc.From;
    Definition.To = Desc.To;
    Definition.DurationSeconds = Desc.DurationSeconds;
    Definition.DelaySeconds = Desc.DelaySeconds;
    Definition.Iterations = Desc.Iterations;
    Definition.PlaybackRate = Desc.PlaybackRate;
    Definition.Direction = Desc.Direction;
    const FRmlUiAnimationDefinitionHandle DefinitionHandle = RegisterTransform2DDefinition(Definition);
    const FRmlUiAnimationBindingHandle Binding = BindNode(DefinitionHandle, View, Node, Desc.BindingId);
    if (!DefinitionHandle.IsValid() || !Binding.IsValid())
    {
        if (Binding.IsValid()) ReleaseBinding(Binding);
        if (DefinitionHandle.IsValid()) ReleaseDefinition(DefinitionHandle);
        return {};
    }
    const FRmlUiAnimationHandle Handle = PlayBinding(
        Binding,
        {},
        [this, Binding, DefinitionHandle, Completion = MoveTemp(OnComplete)](
            FRmlUiAnimationHandle CompletedHandle, ERmlUiAnimationCompletionReason Reason) mutable
        {
            if (Completion) Completion(CompletedHandle, Reason);
            ReleaseBinding(Binding);
            ReleaseDefinition(DefinitionHandle);
        });
    if (!Handle.IsValid())
    {
        ReleaseBinding(Binding);
        ReleaseDefinition(DefinitionHandle);
    }
    return Handle;
}

bool FRmlUiAnimationRuntime::Cancel(FRmlUiAnimationHandle Handle)
{
    return Impl->Cancel(Handle, ERmlUiAnimationCompletionReason::Cancelled);
}

bool FRmlUiAnimationRuntime::Pause(FRmlUiAnimationHandle Handle)
{
    return Impl->Pause(Handle);
}

bool FRmlUiAnimationRuntime::Resume(FRmlUiAnimationHandle Handle)
{
    return Impl->Resume(Handle);
}

bool FRmlUiAnimationRuntime::Seek(FRmlUiAnimationHandle Handle, double LocalTimeSeconds)
{
    return Impl->Seek(Handle, LocalTimeSeconds);
}

bool FRmlUiAnimationRuntime::SetPlaybackRate(
    FRmlUiAnimationHandle Handle, double PlaybackRate)
{
    return Impl->SetPlaybackRate(Handle, PlaybackRate);
}

bool FRmlUiAnimationRuntime::IsActive(FRmlUiAnimationHandle Handle) const
{
    return Impl->FindRecord(Handle) != nullptr;
}

bool FRmlUiAnimationRuntime::IsPaused(FRmlUiAnimationHandle Handle) const
{
    const FImpl::FRecord* Record = Impl->FindRecord(Handle);
    return Record && Record->bPaused;
}

bool FRmlUiAnimationRuntime::CancelBinding(uint64 BindingId)
{
    return Impl->CancelBinding(BindingId, ERmlUiAnimationCompletionReason::Cancelled);
}

bool FRmlUiAnimationRuntime::CancelNodeAnimation(RmlUE_View* View, uint32 Node, ERmlUiAnimatedProperty Property)
{
    return Impl->CancelNativeTarget({View, Node, Property}, ERmlUiAnimationCompletionReason::Cancelled);
}

void FRmlUiAnimationRuntime::CancelViewAnimations(RmlUE_View* View)
{
    Impl->CancelView(View);
}

void FRmlUiAnimationRuntime::CancelAll()
{
    Impl->RemoveAll(true);
}

void FRmlUiAnimationRuntime::Advance(float DeltaSeconds)
{
    Impl->Advance(DeltaSeconds);
}

FDelegateHandle FRmlUiAnimationRuntime::AddPostAdvanceCallback(FSimpleDelegate Callback)
{
    return Impl->PostAdvanceCallbacks.Add(MoveTemp(Callback));
}

void FRmlUiAnimationRuntime::RemovePostAdvanceCallback(FDelegateHandle Handle)
{
    Impl->PostAdvanceCallbacks.Remove(Handle);
}

bool FRmlUiAnimationRuntime::IsAdvancing() const
{
    return Impl->bAdvancing;
}

int32 FRmlUiAnimationRuntime::GetActiveAnimationCount() const
{
    return Impl->ActiveRecordCount;
}

bool FRmlUiAnimationRuntime::HasActiveAnimations(RmlUE_View* View) const
{
    return Impl->HasEvaluatingRecords(View);
}

double FRmlUiAnimationRuntime::GetNextWakeDelaySeconds(RmlUE_View* View) const
{
    if (!View) return TNumericLimits<double>::Max();
    double NextBoundaryTimeSeconds = TNumericLimits<double>::Max();
    for (const FImpl::FLayeredGroup& Group : Impl->LayeredGroups)
    {
        if (!Group.bActive || Group.NativeTarget.View != View)
        {
            continue;
        }
        if (Group.bDirty)
        {
            return 0.0;
        }
        NextBoundaryTimeSeconds = FMath::Min(
            NextBoundaryTimeSeconds, Group.NextBoundaryTimeSeconds);
    }
    return NextBoundaryTimeSeconds < TNumericLimits<double>::Max()
        ? FMath::Max(0.0, NextBoundaryTimeSeconds - Impl->CurrentTimeSeconds)
        : TNumericLimits<double>::Max();
}

int32 FRmlUiAnimationRuntime::GetDefinitionCount() const
{
    return Impl->DefinitionCount;
}

uint64 FRmlUiAnimationRuntime::GetDefinitionAllocatedBytes(
    FRmlUiAnimationDefinitionHandle Handle) const
{
    return Impl->GetDefinitionAllocatedBytes(Handle);
}

int32 FRmlUiAnimationRuntime::GetBindingCount() const
{
    return Impl->BindingCount;
}

double FRmlUiAnimationRuntime::GetCurrentTimeSeconds() const
{
    return Impl->CurrentTimeSeconds;
}

void FRmlUiAnimationRuntime::SetWakeCallback(TFunction<void(RmlUE_View*)> Callback)
{
    Impl->WakeCallback = MoveTemp(Callback);
}
