#include "RmlUiWebCompatJSBridge.h"

void URmlUiWebCompatJSBridge::ReportReady()
{
    bReady = true;
    RuntimeError.Reset();
}

void URmlUiWebCompatJSBridge::Complete(bool bSuccess, const FString& Markup, const FString& Diagnostics,
    const FString& MotionManifest)
{
    if (!bInvoking || bCompleted) return;
    bCompleted = true;
    bCompileSuccess = bSuccess;
    CompiledMarkup = Markup;
    CompileDiagnostics = Diagnostics;
    CompiledMotionManifest = MotionManifest;
}

bool URmlUiWebCompatJSBridge::Invoke(const FString& Markup, const FString& SourcePath,
    FString& OutMarkup, FString& OutDiagnostics, FString& OutMotionManifest)
{
    check(IsInGameThread());
    OutMarkup.Reset();
    OutDiagnostics.Reset();
    OutMotionManifest.Reset();
    if (!bReady)
    {
        OutDiagnostics = RuntimeError.IsEmpty() ? TEXT("The Puerts WebCompat compiler is not ready.") : RuntimeError;
        return false;
    }
    if (bInvoking)
    {
        OutDiagnostics = TEXT("Reentrant WebCompat compilation is not supported.");
        return false;
    }

    TGuardValue<bool> InvocationGuard(bInvoking, true);
    bCompleted = false;
    bCompileSuccess = false;
    CompiledMarkup.Reset();
    CompileDiagnostics.Reset();
    CompiledMotionManifest.Reset();
    RuntimeError.Reset();
    OnCompileRequest.Broadcast(Markup, SourcePath);
    if (!bCompleted)
    {
        OutDiagnostics = RuntimeError.IsEmpty()
            ? TEXT("The Puerts WebCompat compiler did not complete synchronously.")
            : RuntimeError;
        return false;
    }
    OutMarkup = MoveTemp(CompiledMarkup);
    OutDiagnostics = MoveTemp(CompileDiagnostics);
    OutMotionManifest = MoveTemp(CompiledMotionManifest);
    return bCompileSuccess;
}
