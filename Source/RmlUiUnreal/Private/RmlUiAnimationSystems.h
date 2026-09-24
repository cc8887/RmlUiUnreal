#pragma once

#include "EntitySystem/MovieSceneEntitySystem.h"

#include "RmlUiAnimationSystems.generated.h"

namespace RmlUiAnimation
{
inline constexpr int32 EasingLutIntervals = 32;

struct FEasingLut
{
    float Values[EasingLutIntervals + 1] = {};
    uint16 StepCount = 1;
    uint8 Type = 0;
    uint8 StepPosition = 0;
};

struct FFloatKeyframe
{
    float Offset = 0.0f;
    float Value = 0.0f;
    FEasingLut EasingToNext;
};

struct FTrack
{
    float From = 0.0f;
    float To = 0.0f;
    double OriginTimeSeconds = 0.0;
    double OriginLocalTimeSeconds = 0.0;
    double PlaybackRate = 1.0;
    double DurationSeconds = 0.0;
    int32 Iterations = 1;
    uint8 Direction = 0;
    const FFloatKeyframe* Keyframes = nullptr;
    int32 KeyframeCount = 0;
    bool bSuppressBeforeStart = false;
    bool bDiscrete = false;
};

struct FSample
{
    float Value = 0.0f;
    bool bComplete = false;
    bool bContributes = true;
    int32 SegmentIndex = 0;
    float LastNormalized = 0.0f;
};

struct FOutput
{
    float Value = 0.0f;
    bool bChanged = false;
    bool bComplete = false;
    bool bContributes = true;
};

struct FPreviousOutput
{
    float Value = 0.0f;
    bool bValid = false;
};

struct FTransform2DValue
{
    float TranslationX = 0.0f;
    float TranslationY = 0.0f;
    float ScaleX = 1.0f;
    float ScaleY = 1.0f;
    float RotationDegrees = 0.0f;
    float SkewXDegrees = 0.0f;
    float SkewYDegrees = 0.0f;
};

struct FTransform2DTrack
{
    FTransform2DValue From;
    FTransform2DValue To;
    double OriginTimeSeconds = 0.0;
    double OriginLocalTimeSeconds = 0.0;
    double PlaybackRate = 1.0;
    double DurationSeconds = 0.0;
    int32 Iterations = 1;
    uint8 Direction = 0;
    struct FTransform2DKeyframe const* Keyframes = nullptr;
    int32 KeyframeCount = 0;
    bool bSuppressBeforeStart = false;
};

struct FTransform2DKeyframe
{
    float Offset = 0.0f;
    FTransform2DValue Value;
    FEasingLut EasingToNext;
};

struct FTransform2DSample
{
    FTransform2DValue Value;
    bool bComplete = false;
    bool bContributes = true;
    int32 SegmentIndex = 0;
    float LastNormalized = 0.0f;
};

struct FTransform2DOutput
{
    FTransform2DValue Value;
    bool bChanged = false;
    bool bComplete = false;
    bool bContributes = true;
};

struct FPreviousTransform2DOutput
{
    FTransform2DValue Value;
    bool bValid = false;
};
}

UCLASS()
class URmlUiAnimationSampleSystem final : public UMovieSceneEntitySystem
{
    GENERATED_BODY()

public:
    URmlUiAnimationSampleSystem(const FObjectInitializer& ObjectInitializer);
    void SetEvaluationTime(double InTimeSeconds) { EvaluationTimeSeconds = InTimeSeconds; }

private:
    virtual void OnRun(FSystemTaskPrerequisites& Prerequisites, FSystemSubsequentTasks& Subsequents) override;

    double EvaluationTimeSeconds = 0.0;
};

UCLASS()
class URmlUiAnimationComposeSystem final : public UMovieSceneEntitySystem
{
    GENERATED_BODY()

public:
    URmlUiAnimationComposeSystem(const FObjectInitializer& ObjectInitializer);

private:
    virtual void OnRun(FSystemTaskPrerequisites& Prerequisites, FSystemSubsequentTasks& Subsequents) override;
};
