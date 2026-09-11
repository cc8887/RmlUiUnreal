#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiJSContext.generated.h"

namespace puerts { class FJsEnv; }
struct RmlUE_View;
struct RmlUE_NodeEvent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlJsMessage, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlJsFrame, float, DeltaSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRmlJsHostRequest, int32, RequestId, const FString&, Method, const FString&, Json);

UCLASS(BlueprintType)
class RMLUIUNREALJS_API URmlUiJSContext : public UObject
{
    GENERATED_BODY()
public:
    URmlUiJSContext();
    virtual ~URmlUiJSContext() override;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnNativeEvent;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsFrame OnFrame;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnLifecycle;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnHostResponse;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnHostEvent;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsHostRequest OnHostRequest;
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
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") bool Listen(int32 Node, const FString& Type, int32 Listener, bool bCapture);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") void Unlisten(int32 Listener);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Events") void SetEventResult(int32 Result);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|State") void SaveState(const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Lifecycle") void ReportReady();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Lifecycle") void ReportError(const FString& Error);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|State") void ReportDebugState(const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Host") void RequestHost(int32 RequestId, const FString& Method, const FString& Json);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Host") void ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool ScrollNode(int32 Node, float Top);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") float ScrollRemaining(int32 Node);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Nodes") bool FocusNode(int32 Node);
    void QueueHostEvent(const FString& Json);

    bool Initialize(RmlUE_View* InView, const FString& Directory, const FString& Entry, const FString& InVersion,
        const FString& State, int32 DebugPort, const TArray<TPair<FString, UObject*>>& Services);
    void Advance(float DeltaSeconds);
    void ActivateHostRequests();
    void Dispose();
    FString CaptureState();
    RmlUE_View* GetView() const { return View; }
    virtual void BeginDestroy() override;
private:
    static int NativeEvent(void* User, uint32 Listener, const RmlUE_NodeEvent* Event);
    bool Result(int Value);
    RmlUE_View* View = nullptr;
    TSharedPtr<puerts::FJsEnv> Environment;
    TArray<int32> EventResults;
    TArray<FString> PendingResponses;
    TArray<FString> PendingHostEvents;
    struct FPendingHostRequest { int32 Id; FString Method, Json; };
    TArray<FPendingHostRequest> PendingHostRequests;
    bool bHostActive = false;
    bool bDisposed = false;
};
