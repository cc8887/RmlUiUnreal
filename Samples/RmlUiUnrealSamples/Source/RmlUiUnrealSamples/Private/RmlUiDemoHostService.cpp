#include "RmlUiDemoHostService.h"

#include "Misc/EngineVersion.h"

FString URmlUiDemoProbeObject::GetIdentity()
{
    ++IdentityCalls;
    return TEXT("RmlUiDemoProbe");
}

int32 URmlUiDemoProbeObject::Add(int32 Left, int32 Right)
{
    ++AddCalls;
    LastLeft = Left;
    LastRight = Right;
    UE_LOG(LogTemp, Display, TEXT("RmlUiJS returned UObject call: object=%p left=%d right=%d"), this, Left, Right);
    return Left + Right;
}

FString URmlUiDemoHostService::GetHostName()
{
    ++HostNameCalls;
    return FString::Printf(TEXT("Unreal Engine %d.%d"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
}

URmlUiDemoProbeObject* URmlUiDemoHostService::GetProbeObject()
{
    ++ProbeReturnCalls;
    return Probe;
}

FString URmlUiDemoHostService::SaveSession(const FString& Name, int32 ProjectCount, bool bEnabled)
{
    ++SaveCalls;
    UE_LOG(LogTemp, Display, TEXT("RmlUiJS direct save: name=%s projects=%d enabled=%s"),
        *Name, ProjectCount, bEnabled ? TEXT("true") : TEXT("false"));
    return TEXT("Saved in Unreal");
}
