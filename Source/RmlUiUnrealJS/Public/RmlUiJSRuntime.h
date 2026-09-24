#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiJSContext.h"
#include "RmlUiJSRuntime.generated.h"

class URmlUiWidget;
class SRmlUiWidget;
class IHttpRequest;
class IRmlUiCssAnimationSession;

UCLASS(BlueprintType)
class RMLUIUNREALJS_API URmlUiJSRuntime : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString LastError;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString ActiveVersion;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") FString ActiveManifest;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") int32 ReloadCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") int32 JsonHostRequestCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") int32 AdvanceCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") int32 AdvanceSkipCount = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|JavaScript|State") bool bDownloading = false;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsHostRequest OnHostRequest;
    UPROPERTY(BlueprintAssignable, Category="RmlUi|JavaScript|Events") FRmlJsMessage OnStatus;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RmlUi|JavaScript") int32 DebugPort = -1;

    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") bool Start(URmlUiWidget* Widget, const FString& ManifestPath, bool bWatch);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") void Stop();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Updates") void Reload();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Updates") void LoadVersion(const FString& ManifestPath);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Updates") void Rollback();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Updates") void FetchUpdate(const FString& ManifestUrl);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") URmlUiJSContext* GetContext() const { return Active; }
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Diagnostics") FString GetCssAnimationDiagnostics() const;
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") bool RegisterService(const FString& Name, UObject* Service);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") bool UnregisterService(const FString& Name);
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript") void ClearServices();
    UFUNCTION(BlueprintCallable, Category="RmlUi|JavaScript|Host") void ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess);
    static FString DefaultManifestPath();
    virtual void BeginDestroy() override;
private:
    void Advance(float DeltaSeconds);
    void RefreshWidgetWake();
    void QueueManifest(const FString& Path);
    bool Activate(const FString& Path);
    bool Fail(const FString& Message);
    UFUNCTION() void ForwardHostRequest(int32 RequestId, const FString& Method, const FString& Json);
    UPROPERTY(Transient) TObjectPtr<URmlUiJSContext> Active;
    UPROPERTY(Transient) TObjectPtr<URmlUiJSContext> Candidate;
    UPROPERTY(Transient) TObjectPtr<URmlUiWidget> Target;
    UPROPERTY(Transient) TMap<FString, TObjectPtr<UObject>> Services;
    TWeakPtr<SRmlUiWidget> SlateWidget;
    FString WatchedManifest;
    FString WatchedText;
    FString PendingManifest;
    FString PreviousManifest;
    FDelegateHandle FrameHandle;
    FDelegateHandle ShutdownHandle;
    double NextWatchTime = 0;
    double LastAdvanceTime = 0;
    bool bWatchFiles = false;
    bool bAdvancing = false;
    TArray<TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>> Requests;
    uint32 DownloadGeneration = 0;
    IRmlUiCssAnimationSession* CssAnimationSession = nullptr;
    struct FHostRequest { TWeakObjectPtr<URmlUiJSContext> Context; int32 LocalId; };
    TMap<int32, FHostRequest> HostRequests;
    int32 NextHostRequest = 0;
};
