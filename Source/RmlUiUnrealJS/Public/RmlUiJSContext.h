#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ArrayBuffer.h"
#include "RmlUiJSContext.generated.h"

namespace puerts { class FJsEnv; }
class FRmlUiAnimationRuntime;
struct RmlUE_View;
struct RmlUE_NodeEvent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlJsMessage, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlJsFrame, float, DeltaSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlJsLayout, int64, Revision);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRmlJsHostRequest, int32, RequestId, const FString&, Method, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FRmlJsAnimationEventBatch, const FArrayBuffer&, Payload);

UCLASS(BlueprintType)
class RMLUIUNREALJS_API URmlUiJSContext : public UObject
{
    GENERATED_BODY()
public:
    URmlUiJSContext();
    virtual ~URmlUiJSContext() override;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnNativeEvent;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsFrame OnFrame;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsLayout OnAfterLayout;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnLifecycle;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnHostResponse;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnHostEvent;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnAnimationEvent;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnAnimationEventBatch;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsAnimationEventBatch OnAnimationEventBatchPacked;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsHostRequest OnHostRequest;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation") bool bUsePackedAnimationEvents = true;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation") bool bUseCompiledAnimationPlans = true;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation") bool bShareAnimationDefinitionsInBatch = true;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation", meta=(ClampMin="1"))
    int32 CompiledAnimationPlanCacheMaxEntries = 1024;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation", meta=(ClampMin="1024"))
    int64 CompiledAnimationPlanCacheMaxBytes = 16 * 1024 * 1024;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|JavaScript|Animation", meta=(ClampMin="1024"))
    int64 CompiledAnimationPlanResidentLimitBytes = 64 * 1024 * 1024;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString StateJson = TEXT("{}");
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString Version;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString LastError;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString DebugStateJson = TEXT("{}");
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") bool bReady = false;

    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 RootNode();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 FindNode(const FString& Id);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 CreateNode(int32 Kind, const FString& TagOrText);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool IsNodeValid(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool InsertNode(int32 Node, int32 Parent, int32 Before);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool RemoveNode(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 ParentNode(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 NextNode(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool SetText(int32 Node, const FString& Text);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") FString GetText(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool SetAttribute(int32 Node, const FString& Name, const FString& Value, bool bRemove);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") FString GetAttribute(int32 Node, const FString& Name);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool SetProperty(int32 Node, const FString& Name, const FString& Value, bool bRemove);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool SetInnerRml(int32 Node, const FString& Rml);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") bool Listen(int32 Node, const FString& Type, int32 Listener, bool bCapture);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") void Unlisten(int32 Listener);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") void SetEventResult(int32 Result);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|State") void SaveState(const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Lifecycle") void ReportReady();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Lifecycle") void ReportError(const FString& Error);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|State") void ReportDebugState(const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Scheduling") void SetWakeSchedule(
        float TimerDelayMilliseconds, bool bHasAnimationFrame);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Host") void RequestHost(int32 RequestId, const FString& Method, const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Host") void ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool ScrollNode(int32 Node, float Top);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") float ScrollRemaining(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool FocusNode(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 QueryNode(int32 Root, const FString& Selector);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") FString QueryNodes(int32 Root, const FString& Selector);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") FString ChildNodes(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool ContainsNode(int32 Parent, int32 Child);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") int32 ActiveNode();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool BlurNode(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool SetNodeClass(int32 Node, const FString& Name, bool bEnabled);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") bool RestartCssAnimation(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") FString GetComputedProperty(int32 Node, const FString& Name);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Layout") FString MeasureNodes(const FString& HandlesJson);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString ResolveAnimationHostSnapshot(
        const FString& RequestJson);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Input") bool SetModalRoot(int32 Root, int32 InitialFocus);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Input") bool CaptureNode(int32 Node, int32 PointerId);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Input") bool ReleaseCaptureNode(int32 Node, int32 PointerId);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") bool AnimateNode(int32 Node, const FString& Property,
        const FString& From, const FString& To, float Duration, int32 Iterations);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") bool AnimateNodeKeyframes(
        int32 Node, const FString& Property, const FString& KeyframesJson,
        float Duration, int32 Iterations);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString StartNodeKeyframeAnimation(
        int32 Node, const FString& Property, const FString& KeyframesJson,
        const FString& OptionsJson);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString StartNodeKeyframeAnimationBatch(
        const FString& RequestsJson);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString RegisterAnimationPlansPacked(
        const FArrayBuffer& Payload);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString StartCompiledAnimationBatchPacked(
        const FArrayBuffer& Payload);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString ReleaseAnimationPlansPacked(
        const FArrayBuffer& Payload);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString GetAnimationPlanCacheStats() const;
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString GetAnimationRuntimeStats() const;
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString ControlAnimation(
        const FString& Handle, const FString& Command, double Value);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") FString ApplyNodePropertyBatch(
        const FString& UpdatesJson);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Animation") bool CancelAnimation(int32 Node, const FString& Property);
    void QueueHostEvent(const FString& Json);
    void QueueAnimationEvent(uint64 Handle, uint8 Reason);

    bool Initialize(RmlUE_View* InView, const FString& Directory, const FString& Entry, const FString& InVersion,
        const FString& State, int32 DebugPort, const TArray<TPair<FString, UObject*>>& Services);
    void Advance(float DeltaSeconds);
    bool NeedsAdvance(double NowSeconds) const;
    double GetNextWakeTimeSeconds(double NowSeconds) const;
    bool HasTimerWake(double NowSeconds) const;
    bool HasAnimationFrameWake() const { return bAnimationFramePending; }
    bool HasDispatchWake() const;
    void SetWakeCallback(TFunction<void()> Callback) { WakeCallback = MoveTemp(Callback); }
    void SetCssAnimationRestartCallback(TFunction<bool(int32)> Callback) { CssAnimationRestartCallback = MoveTemp(Callback); }
    void ActivateHostRequests();
    void Dispose();
    FString CaptureState();
    RmlUE_View* GetView() const { return View; }
    virtual void BeginDestroy() override;
private:
    static int NativeEvent(void* User, uint32 Listener, const RmlUE_NodeEvent* Event);
    static void LayoutCompleted(void* User, uint64 Revision);
    bool Result(int Value);
    void NotifyWake();
    void FlushAnimationEvents();
    void ReleaseAllAnimationPlans();
    struct FPendingAnimationEvent { uint64 Handle = 0; uint8 Reason = 0; };
    RmlUE_View* View = nullptr;
    FRmlUiAnimationRuntime* AnimationRuntime = nullptr;
    TSharedPtr<puerts::FJsEnv> Environment;
    TArray<int32> EventResults;
    TArray<FString> PendingResponses;
    TArray<FString> PendingHostEvents;
    TArray<FPendingAnimationEvent> PendingAnimationEvents;
    struct FCompiledAnimationPlan
    {
        uint64 Definition = 0;
        uint32 Generation = 1;
        uint32 UseCount = 0;
        uint64 AllocatedBytes = 0;
        uint8 Property = 0;
        uint8 CostClass = 0;
        uint8 Fill = 3;
        bool bActive = false;
    };
    TArray<FCompiledAnimationPlan> CompiledAnimationPlans;
    TArray<uint32> FreeCompiledAnimationPlanSlots;
    uint64 CompiledAnimationPlanAllocatedBytes = 0;
    struct FPendingHostRequest { int32 Id; FString Method, Json; };
    TArray<FPendingHostRequest> PendingHostRequests;
    bool bHostActive = false;
    bool bDisposed = false;
    bool bAnimationFramePending = false;
    double NextTimerWakeTime = TNumericLimits<double>::Max();
    TFunction<void()> WakeCallback;
    TFunction<bool(int32)> CssAnimationRestartCallback;
    FDelegateHandle AnimationPostAdvanceHandle;
};
