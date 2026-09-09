#include "RmlUiResourceRegistry.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "RmlUiBridge.h"
#include "Trace/Trace.h"

UE_TRACE_CHANNEL_DEFINE(RmlUiResourcesChannel, "RmlUi resource lifetime events");

UE_TRACE_EVENT_BEGIN(RmlUiResources, Lifetime)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, Id)
    UE_TRACE_EVENT_FIELD(uint64, OwnerId)
    UE_TRACE_EVENT_FIELD(uint64, EstimatedBytes)
    UE_TRACE_EVENT_FIELD(uint64, CreatedSequence)
    UE_TRACE_EVENT_FIELD(uint8, Action)
    UE_TRACE_EVENT_FIELD(uint8, Domain)
    UE_TRACE_EVENT_FIELD(int32, Type)
    UE_TRACE_EVENT_FIELD(int32, Backend)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, Name)
UE_TRACE_EVENT_END()

FRmlUiResourceRegistry& FRmlUiResourceRegistry::Get()
{
    static FRmlUiResourceRegistry Registry;
    return Registry;
}

uint64 FRmlUiResourceRegistry::RegisterUnreal(ERmlUiResourceType Type, ERmlUiResourceBackend Backend, uint64 OwnerId,
    uint64 EstimatedBytes, FString Name, UObject* Object)
{
    FRmlUiResourceInfo Info;
    {
        FScopeLock Lock(&Mutex);
        Info.Id = ++NextUnrealId;
        Info.OwnerId = OwnerId;
        Info.EstimatedBytes = EstimatedBytes;
        Info.CreatedSequence = ++NextUnrealSequence;
        Info.Domain = ERmlUiResourceDomain::Unreal;
        Info.Type = Type;
        Info.Backend = Backend;
        Info.Name = MoveTemp(Name);
        Info.Object = Object;
        Resources.Add(Info.Id, Info);
    }
    Trace(ERmlUiResourceAction::Created, Info);
    return Info.Id;
}

void FRmlUiResourceRegistry::UpdateUnreal(uint64 Id, uint64 EstimatedBytes)
{
    FRmlUiResourceInfo Info;
    {
        FScopeLock Lock(&Mutex);
        FRmlUiResourceInfo* Existing = Resources.Find(Id);
        if (!Existing || Existing->Domain != ERmlUiResourceDomain::Unreal) return;
        Existing->EstimatedBytes = EstimatedBytes;
        Info = *Existing;
    }
    Trace(ERmlUiResourceAction::Updated, Info);
}

void FRmlUiResourceRegistry::UnregisterUnreal(uint64 Id)
{
    FRmlUiResourceInfo Info;
    {
        FScopeLock Lock(&Mutex);
        FRmlUiResourceInfo* Existing = Resources.Find(Id);
        if (!Existing || Existing->Domain != ERmlUiResourceDomain::Unreal) return;
        Info = *Existing;
        Resources.Remove(Id);
    }
    Trace(ERmlUiResourceAction::Destroyed, Info);
}

void FRmlUiResourceRegistry::ApplyNativeEvent(int32 Action, const RmlUE_ResourceRecord& Record)
{
    FRmlUiResourceInfo Info;
    Info.Id = Record.Id;
    Info.OwnerId = Record.OwnerId;
    Info.EstimatedBytes = Record.EstimatedBytes;
    Info.CreatedSequence = Record.CreatedSequence;
    Info.Domain = ERmlUiResourceDomain::Native;
    Info.Type = static_cast<ERmlUiResourceType>(Record.Type);
    Info.Backend = static_cast<ERmlUiResourceBackend>(Record.Backend);
    Info.Name = UTF8_TO_TCHAR(Record.Name);

    const ERmlUiResourceAction TypedAction = static_cast<ERmlUiResourceAction>(Action);
    {
        FScopeLock Lock(&Mutex);
        if (TypedAction == ERmlUiResourceAction::Destroyed) Resources.Remove(Info.Id);
        else Resources.Add(Info.Id, Info);
    }
    Trace(TypedAction, Info);
}

TArray<FRmlUiResourceInfo> FRmlUiResourceRegistry::Snapshot() const
{
    TArray<FRmlUiResourceInfo> Result;
    {
        FScopeLock Lock(&Mutex);
        Resources.GenerateValueArray(Result);
    }
    Result.Sort([](const FRmlUiResourceInfo& A, const FRmlUiResourceInfo& B) { return A.Id < B.Id; });
    return Result;
}

void FRmlUiResourceRegistry::Visit(TFunctionRef<void(const FRmlUiResourceInfo&)> Visitor) const
{
    const TArray<FRmlUiResourceInfo> Copy = Snapshot();
    for (const FRmlUiResourceInfo& Info : Copy) Visitor(Info);
}

void FRmlUiResourceRegistry::Trace(ERmlUiResourceAction Action, const FRmlUiResourceInfo& Info) const
{
    UE_TRACE_LOG(RmlUiResources, Lifetime, RmlUiResourcesChannel)
        << Lifetime.Timestamp(FPlatformTime::Cycles64())
        << Lifetime.Id(Info.Id)
        << Lifetime.OwnerId(Info.OwnerId)
        << Lifetime.EstimatedBytes(Info.EstimatedBytes)
        << Lifetime.CreatedSequence(Info.CreatedSequence)
        << Lifetime.Action(static_cast<uint8>(Action))
        << Lifetime.Domain(static_cast<uint8>(Info.Domain))
        << Lifetime.Type(static_cast<int32>(Info.Type))
        << Lifetime.Backend(static_cast<int32>(Info.Backend))
        << Lifetime.Name(*Info.Name);
}
