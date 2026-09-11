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

    UFUNCTION(BlueprintCallable, Category="RmlUi|WebCompat")
    void Complete(bool bSuccess, const FString& Markup, const FString& Diagnostics);

    UFUNCTION(BlueprintCallable, Category="RmlUi|WebCompat")
    void ReportReady();

    bool Invoke(const FString& Markup, const FString& SourcePath, FString& OutMarkup, FString& OutDiagnostics);
    bool IsReady() const { return bReady; }
    void SetRuntimeError(const FString& Error) { RuntimeError = Error; }

private:
    bool bReady = false;
    bool bInvoking = false;
    bool bCompleted = false;
    bool bCompileSuccess = false;
    FString CompiledMarkup;
    FString CompileDiagnostics;
    FString RuntimeError;
};
