#pragma once

#include "CoreMinimal.h"
#include "Rendering/RenderingCommon.h"
#include "RHIResources.h"

class FRmlUiSlateRhiGeometry;

struct FRmlUiSlateRhiDrawDesc
{
    TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> Geometry;
    FTextureRHIRef Texture;
    FVector2f Origin = FVector2f::ZeroVector;
    FVector2f AxisX = FVector2f(1.0f, 0.0f);
    FVector2f AxisY = FVector2f(0.0f, 1.0f);
    FSlateRect ScissorRect;
    uint64 GeometryId = 0;
};

class FRmlUiSlateRhiDraw : public ICustomSlateElement
{
public:
    virtual void UpdateDesc(FRmlUiSlateRhiDrawDesc Desc) = 0;
    virtual void ResetDesc() = 0;
};

TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> CreateRmlUiSlateRhiGeometry(
    TConstArrayView<float> Vertices, TConstArrayView<uint32> Indices,
    uint64 CacheRegistryId, uint64 VertexBufferRegistryId, uint64 IndexBufferRegistryId);

void MarkRmlUiSlateRhiGeometryPendingDestroy(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry);

bool IsRmlUiSlateRhiGeometryReady(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry);

TSharedRef<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe> CreateRmlUiSlateRhiDraw(FRmlUiSlateRhiDrawDesc Desc);
void UpdateRmlUiSlateRhiDraw(const TSharedPtr<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe>& Draw,
    FRmlUiSlateRhiDrawDesc Desc);
void ResetRmlUiSlateRhiDraw(const TSharedPtr<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe>& Draw);
