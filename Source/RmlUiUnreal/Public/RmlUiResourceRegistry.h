#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtr.h"

struct RmlUE_ResourceRecord;

enum class ERmlUiResourceDomain : uint8
{
    Native,
    Unreal
};

enum class ERmlUiResourceAction : uint8
{
    Created = 1,
    Updated = 2,
    Destroyed = 3
};

enum class ERmlUiResourceType : int32
{
    View = 1,
    StyleSheet = 2,
    Geometry = 3,
    Texture = 4,
    FrameBuffer = 5,
    MaterialBinding = 6,
    UnrealTexture = 100,
    SlateMaterialBrush = 101,
    SlateGeometryCache = 102
};

enum class ERmlUiResourceBackend : int32
{
    Shared = 0,
    DX11 = 1,
    Slate = 2
};

struct RMLUIUNREAL_API FRmlUiResourceInfo
{
    uint64 Id = 0;
    uint64 OwnerId = 0;
    uint64 EstimatedBytes = 0;
    uint64 CreatedSequence = 0;
    ERmlUiResourceDomain Domain = ERmlUiResourceDomain::Native;
    ERmlUiResourceType Type = ERmlUiResourceType::View;
    ERmlUiResourceBackend Backend = ERmlUiResourceBackend::Shared;
    FString Name;
    TWeakObjectPtr<UObject> Object;
};

class RMLUIUNREAL_API FRmlUiResourceRegistry
{
public:
    static FRmlUiResourceRegistry& Get();

    uint64 RegisterUnreal(ERmlUiResourceType Type, ERmlUiResourceBackend Backend, uint64 OwnerId,
        uint64 EstimatedBytes, FString Name, UObject* Object = nullptr);
    void UpdateUnreal(uint64 Id, uint64 EstimatedBytes);
    void UnregisterUnreal(uint64 Id);
    void ApplyNativeEvent(int32 Action, const RmlUE_ResourceRecord& Record);

    TArray<FRmlUiResourceInfo> Snapshot() const;
    void Visit(TFunctionRef<void(const FRmlUiResourceInfo&)> Visitor) const;

private:
    void Trace(ERmlUiResourceAction Action, const FRmlUiResourceInfo& Info) const;

    mutable FCriticalSection Mutex;
    TMap<uint64, FRmlUiResourceInfo> Resources;
    uint64 NextUnrealId = 0x8000000000000000ull;
    uint64 NextUnrealSequence = 0;
};
