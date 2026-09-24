#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiWebCompatJSBridge.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRmlUiWebCompatCompileRequest,
    const FString&, Markup, const FString&, SourcePath);

UCLASS()
class URmlUiWebCompatJSBridge : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable, Category="RmlUi|WebCompat")
    FRmlUiWebCompatCompileRequest OnCompileRequest;

    UPROPERTY(BlueprintReadOnly, Category="RmlUi|WebCompat")
    FString CapabilityProfile = TEXT("legacy");

    UPROPERTY(BlueprintReadOnly, Category="RmlUi|WebCompat")
    FString CapabilityMode = TEXT("strict");

    UPROPERTY(BlueprintReadOnly, Category="RmlUi|WebCompat")
    FString AllowedDegradationsJson = TEXT("[]");

    UFUNCTION(BlueprintCallable, Category="RmlUi|WebCompat")
    void Complete(bool bSuccess, const FString& Markup, const FString& Diagnostics, const FString& MotionManifest);

    UFUNCTION(BlueprintCallable, Category="RmlUi|WebCompat")
    void ReportReady();

    bool Invoke(const FString& Markup, const FString& SourcePath, FString& OutMarkup, FString& OutDiagnostics,
        FString& OutMotionManifest);
    bool IsReady() const { return bReady; }
    void SetRuntimeError(const FString& Error) { RuntimeError = Error; }

private:
    bool bReady = false;
    bool bInvoking = false;
    bool bCompleted = false;
    bool bCompileSuccess = false;
    FString CompiledMarkup;
    FString CompileDiagnostics;
    FString CompiledMotionManifest;
    FString RuntimeError;
};
