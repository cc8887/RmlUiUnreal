#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Containers/Ticker.h"
#include "RmlUiChatTransport.generated.h"

class URmlUiJSRuntime;
class URmlUiJSContext;
class IHttpRequest;
struct FRmlChatStream;

UCLASS(BlueprintType)
class RMLUIUNREALSAMPLES_API URmlUiChatTransport : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="RmlUi|Chat") void Attach(URmlUiJSRuntime* Runtime, const FString& Endpoint, const FString& Model, const FString& Protocol);
    UFUNCTION(BlueprintCallable, Category="RmlUi|Chat") void Stop();
    UFUNCTION(BlueprintCallable, Category="RmlUi|Chat") void SetApiKey(const FString& Key) { ApiKey = Key; }
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|Chat") bool bStreaming = false;
    UPROPERTY(BlueprintReadWrite, Category="RmlUi|Chat") bool bPersistenceEnabled = true;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|Chat|Diagnostics") int32 StreamStarts = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|Chat|Diagnostics") int32 StreamCancels = 0;
    UPROPERTY(BlueprintReadOnly, Category="RmlUi|Chat|Diagnostics") int32 StreamDeltas = 0;
    virtual void BeginDestroy() override;
private:
    UFUNCTION() void HandleRequest(int32 Id, const FString& Method, const FString& Json);
    bool Tick(float DeltaSeconds);
    void Cancel();
    void Emit(const FString& Delta, const FString& Status, const FString& Error = TEXT(""));
    bool StartStream(const TSharedPtr<class FJsonObject>& Json, FString& Error);
    bool ParseEvent(const FString& Data, FString& Delta, FString& Error);
    UPROPERTY() TObjectPtr<URmlUiJSRuntime> Runtime;
    TWeakObjectPtr<URmlUiJSContext> StreamContext;
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
    TSharedPtr<FRmlChatStream, ESPMode::ThreadSafe> Stream;
    FTSTicker::FDelegateHandle TickHandle;
    FString Endpoint, Model, Protocol, ApiKey, StreamId, EventData;
    TArray<uint8> PendingBytes;
    bool bTerminal = false;
};
