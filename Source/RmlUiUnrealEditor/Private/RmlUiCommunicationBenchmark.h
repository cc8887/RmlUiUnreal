#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiCommunicationBenchmark.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRmlUiBenchmarkRun, const FString&, Configuration);

// Editor-only fixture: both JS runtimes read the same native object and float values.
UCLASS()
class URmlUiCommunicationValue : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadOnly, Category="Benchmark") float Value = 0.0f;
};

UCLASS()
class URmlUiCommunicationBenchmark : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY() TObjectPtr<URmlUiCommunicationValue> Object;
    UPROPERTY() FRmlUiBenchmarkRun OnRun;

    UFUNCTION() float GetFloat();
    UFUNCTION() float GetObjectValue();
    UFUNCTION() URmlUiCommunicationValue* GetObject();
    UFUNCTION() void Request(const FString& Json);
    UFUNCTION() void Ready();
    UFUNCTION() void Report(const FString& Json);
    UFUNCTION() void BeginBatch();
    UFUNCTION() double EndBatch();

    void ResetBatch(float Value);
    float Scalar = 0.0f;
    int64 ScalarReads = 0, PropertyReads = 0, ObjectResolves = 0, Requests = 0;
    int64 RequestBytes = 0, ResponseBytes = 0;
    uint64 ParseCycles = 0, ReadCycles = 0, SerializeCycles = 0, EnqueueCycles = 0;
    uint64 BatchStart = 0;
    bool bReady = false;
    FString LastReport, Error;
    TFunction<void(const FString&)> SendJavascript;
};
