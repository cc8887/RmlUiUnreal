#pragma once

#include "CoreMinimal.h"

struct RmlUE_View;

class RMLUIUNREALWEBCOMPAT_API IRmlUiCssAnimationSession
{
public:
    virtual ~IRmlUiCssAnimationSession() = default;
    virtual void Reset() = 0;
    virtual void SetWakeCallback(TFunction<void()> Callback) = 0;
    virtual void SetLifecycleEventBatchCallback(TFunction<void(const FString&)> Callback) = 0;
    virtual bool Install(const FString& Json, RmlUE_View* View, FString& OutError) = 0;
    virtual bool FlushActivationChanges(FString& OutError) = 0;
    virtual bool RequestRestart(uint32 Node) = 0;
    virtual bool ValidateInitialBindings(FString& OutError) = 0;
    virtual FString GetActivationDiagnostics() const = 0;
    virtual void FlushLifecycleEvents() = 0;
};

RMLUIUNREALWEBCOMPAT_API IRmlUiCssAnimationSession* CreateRmlUiCssAnimationSession();
