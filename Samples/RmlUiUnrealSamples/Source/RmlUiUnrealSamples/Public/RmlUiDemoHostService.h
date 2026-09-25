#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiDemoHostService.generated.h"

UCLASS()
class RMLUIUNREALSAMPLES_API URmlUiDemoProbeObject : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable) FString GetIdentity();
    UFUNCTION(BlueprintCallable) int32 Add(int32 Left, int32 Right);
    int32 IdentityCalls = 0;
    int32 AddCalls = 0;
    int32 LastLeft = 0;
    int32 LastRight = 0;
};

UCLASS()
class RMLUIUNREALSAMPLES_API URmlUiDemoHostService : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable) FString GetHostName();
    UFUNCTION(BlueprintCallable) URmlUiDemoProbeObject* GetProbeObject();
    UFUNCTION(BlueprintCallable) FString SaveSession(const FString& Name, int32 ProjectCount, bool bEnabled);
    UPROPERTY(Transient) TObjectPtr<URmlUiDemoProbeObject> Probe;
    int32 HostNameCalls = 0;
    int32 ProbeReturnCalls = 0;
    int32 SaveCalls = 0;
};
