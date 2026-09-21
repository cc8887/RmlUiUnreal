#pragma once

#include "CoreMinimal.h"

struct RmlUE_View;

enum class ERmlUiAnimatedProperty : uint8
{
    None = 0,
    Opacity = 1,
    Transform2D = 2
};

enum class ERmlUiAnimationCompletionReason : uint8
{
    Completed,
    Cancelled,
    Replaced
};

enum class ERmlUiAnimationDirection : uint8
{
    Normal,
    Reverse,
    Alternate,
    AlternateReverse
};

struct FRmlUiAnimationHandle
{
    uint64 Value = 0;

    bool IsValid() const { return Value != 0; }

    friend bool operator==(FRmlUiAnimationHandle A, FRmlUiAnimationHandle B)
    {
        return A.Value == B.Value;
    }
};

struct FRmlUiAnimationDefinitionHandle
{
    uint64 Value = 0;

    bool IsValid() const { return Value != 0; }

    friend bool operator==(FRmlUiAnimationDefinitionHandle A, FRmlUiAnimationDefinitionHandle B)
    {
        return A.Value == B.Value;
    }
};

struct FRmlUiAnimationBindingHandle
{
    uint64 Value = 0;

    bool IsValid() const { return Value != 0; }

    friend bool operator==(FRmlUiAnimationBindingHandle A, FRmlUiAnimationBindingHandle B)
    {
        return A.Value == B.Value;
    }
};

struct FRmlUiAnimationContributionSpec
{
    // Higher orders replace lower orders while both contributions are active.
    int32 Order = 0;
    // Delayed contributions stay out of arbitration until their local time reaches zero.
    bool bSuppressBeforeStart = true;
    // False keeps the ordinary single-track replacement path for unrelated batch entries.
    bool bLayered = true;
};

FORCEINLINE uint32 GetTypeHash(FRmlUiAnimationHandle Handle)
{
    return GetTypeHash(Handle.Value);
}

FORCEINLINE uint32 GetTypeHash(FRmlUiAnimationDefinitionHandle Handle)
{
    return GetTypeHash(Handle.Value);
}

FORCEINLINE uint32 GetTypeHash(FRmlUiAnimationBindingHandle Handle)
{
    return GetTypeHash(Handle.Value);
}

struct FRmlUiFloatAnimationDesc
{
    float From = 0.0f;
    float To = 1.0f;
    double DurationSeconds = 0.2;
    double DelaySeconds = 0.0;
    int32 Iterations = 1;

    // A non-zero binding replaces the previous animation for the same property target.
    uint64 BindingId = 0;
    double PlaybackRate = 1.0;
    ERmlUiAnimationDirection Direction = ERmlUiAnimationDirection::Normal;
};

enum class ERmlUiAnimationEasingType : uint8
{
    Linear,
    CubicBezier
};

struct FRmlUiAnimationEasing
{
    ERmlUiAnimationEasingType Type = ERmlUiAnimationEasingType::Linear;
    float X1 = 0.0f;
    float Y1 = 0.0f;
    float X2 = 1.0f;
    float Y2 = 1.0f;
};

struct FRmlUiFloatAnimationKeyframe
{
    // Offsets are resolved by the adapter and must be non-decreasing from 0 to 1.
    // Adjacent equal offsets represent an instantaneous value change.
    float Offset = 0.0f;
    float Value = 0.0f;
    FRmlUiAnimationEasing EasingToNext;
};

struct FRmlUiFloatAnimationDefinition
{
    float From = 0.0f;
    float To = 1.0f;
    double DurationSeconds = 0.2;
    double DelaySeconds = 0.0;
    int32 Iterations = 1;
    double PlaybackRate = 1.0;
    ERmlUiAnimationDirection Direction = ERmlUiAnimationDirection::Normal;
    // When present, Keyframes replace From/To. Easing belongs to the segment starting here.
    TArray<FRmlUiFloatAnimationKeyframe> Keyframes;
};

struct FRmlUiTransform2D
{
    float TranslationX = 0.0f;
    float TranslationY = 0.0f;
    float ScaleX = 1.0f;
    float ScaleY = 1.0f;
    float RotationDegrees = 0.0f;
};

struct FRmlUiTransform2DAnimationDesc
{
    FRmlUiTransform2D From;
    FRmlUiTransform2D To;
    double DurationSeconds = 0.2;
    double DelaySeconds = 0.0;
    int32 Iterations = 1;
    uint64 BindingId = 0;
    double PlaybackRate = 1.0;
    ERmlUiAnimationDirection Direction = ERmlUiAnimationDirection::Normal;
};

struct FRmlUiTransform2DAnimationKeyframe
{
    float Offset = 0.0f;
    FRmlUiTransform2D Value;
    FRmlUiAnimationEasing EasingToNext;
};

struct FRmlUiTransform2DAnimationDefinition
{
    FRmlUiTransform2D From;
    FRmlUiTransform2D To;
    double DurationSeconds = 0.2;
    double DelaySeconds = 0.0;
    int32 Iterations = 1;
    double PlaybackRate = 1.0;
    ERmlUiAnimationDirection Direction = ERmlUiAnimationDirection::Normal;
    TArray<FRmlUiTransform2DAnimationKeyframe> Keyframes;
};

struct FRmlUiAnimationCommitUpdate
{
    RmlUE_View* View = nullptr;
    uint32 Node = 0;
    ERmlUiAnimatedProperty Property = ERmlUiAnimatedProperty::None;
    float Values[5] = {};
    bool bFinal = false;
    uint64 Target = 0;
};

enum class ERmlUiAnimationCommitResult : uint8
{
    Unsupported,
    Committed,
    InvalidTarget
};

class RMLUIUNREAL_API IRmlUiAnimationCommitSink
{
public:
    virtual ~IRmlUiAnimationCommitSink() = default;
    virtual void Commit(
        TConstArrayView<FRmlUiAnimationCommitUpdate> Updates,
        TArray<ERmlUiAnimationCommitResult>& OutResults) = 0;
};

using FRmlUiAnimationValueCallback = TFunction<void(FRmlUiAnimationHandle, float)>;
using FRmlUiAnimationCompletionCallback =
    TFunction<void(FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason)>;

class RMLUIUNREAL_API FRmlUiAnimationRuntime
{
public:
    FRmlUiAnimationRuntime();
    ~FRmlUiAnimationRuntime();

    FRmlUiAnimationRuntime(const FRmlUiAnimationRuntime&) = delete;
    FRmlUiAnimationRuntime& operator=(const FRmlUiAnimationRuntime&) = delete;

    FRmlUiAnimationDefinitionHandle RegisterFloatDefinition(
        ERmlUiAnimatedProperty Property,
        const FRmlUiFloatAnimationDefinition& Definition);
    FRmlUiAnimationDefinitionHandle RegisterTransform2DDefinition(
        const FRmlUiTransform2DAnimationDefinition& Definition);
    bool ReleaseDefinition(FRmlUiAnimationDefinitionHandle Handle);

    FRmlUiAnimationBindingHandle BindCallback(
        FRmlUiAnimationDefinitionHandle Definition,
        uint64 ReplacementId = 0);
    FRmlUiAnimationBindingHandle BindNode(
        FRmlUiAnimationDefinitionHandle Definition,
        RmlUE_View* View,
        uint32 Node,
        uint64 ReplacementId = 0);
    int32 BindNodes(
        FRmlUiAnimationDefinitionHandle Definition,
        RmlUE_View* View,
        const TArray<uint32>& Nodes,
        TArray<FRmlUiAnimationBindingHandle>& OutBindings);
    bool ReleaseBinding(FRmlUiAnimationBindingHandle Handle);

    FRmlUiAnimationHandle PlayBinding(
        FRmlUiAnimationBindingHandle Binding,
        FRmlUiAnimationValueCallback OnValue = {},
        FRmlUiAnimationCompletionCallback OnComplete = {});
    int32 PlayBindings(
        const TArray<FRmlUiAnimationBindingHandle>& Bindings,
        TArray<FRmlUiAnimationHandle>& OutHandles,
        TArray<FRmlUiAnimationCompletionCallback> OnCompletes = {});
    int32 PlayContributionBindings(
        const TArray<FRmlUiAnimationBindingHandle>& Bindings,
        TConstArrayView<FRmlUiAnimationContributionSpec> Contributions,
        TArray<FRmlUiAnimationHandle>& OutHandles,
        TArray<FRmlUiAnimationCompletionCallback> OnCompletes = {});

    FRmlUiAnimationHandle PlayFloat(
        const FRmlUiFloatAnimationDesc& Desc,
        FRmlUiAnimationValueCallback OnValue,
        FRmlUiAnimationCompletionCallback OnComplete = {});
    FRmlUiAnimationHandle PlayNodeFloat(
        RmlUE_View* View,
        uint32 Node,
        ERmlUiAnimatedProperty Property,
        const FRmlUiFloatAnimationDesc& Desc,
        FRmlUiAnimationCompletionCallback OnComplete = {});
    FRmlUiAnimationHandle PlayNodeTransform2D(
        RmlUE_View* View,
        uint32 Node,
        const FRmlUiTransform2DAnimationDesc& Desc,
        FRmlUiAnimationCompletionCallback OnComplete = {});
    bool Cancel(FRmlUiAnimationHandle Handle);
    bool Pause(FRmlUiAnimationHandle Handle);
    bool Resume(FRmlUiAnimationHandle Handle);
    bool Seek(FRmlUiAnimationHandle Handle, double LocalTimeSeconds);
    bool SetPlaybackRate(FRmlUiAnimationHandle Handle, double PlaybackRate);
    bool IsActive(FRmlUiAnimationHandle Handle) const;
    bool IsPaused(FRmlUiAnimationHandle Handle) const;
    bool CancelBinding(uint64 BindingId);
    bool CancelNodeAnimation(RmlUE_View* View, uint32 Node, ERmlUiAnimatedProperty Property);
    void CancelViewAnimations(RmlUE_View* View);
    void CancelAll();

    // Called once by the module from Slate OnPreTick. Tests and headless hosts may drive it directly.
    void Advance(float DeltaSeconds);
    FDelegateHandle AddPostAdvanceCallback(FSimpleDelegate Callback);
    void RemovePostAdvanceCallback(FDelegateHandle Handle);
    bool IsAdvancing() const;

    int32 GetActiveAnimationCount() const;
    bool HasActiveAnimations(RmlUE_View* View) const;
    double GetNextWakeDelaySeconds(RmlUE_View* View) const;
    int32 GetDefinitionCount() const;
    uint64 GetDefinitionAllocatedBytes(FRmlUiAnimationDefinitionHandle Handle) const;
    int32 GetBindingCount() const;
    double GetCurrentTimeSeconds() const;
    void SetWakeCallback(TFunction<void(RmlUE_View*)> Callback);

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
