#pragma once

#include "CoreMinimal.h"
#include "Rendering/RenderingCommon.h"
#include "RHIResources.h"

class FRmlUiSlateRhiGeometry;

struct FRmlUiSlateRhiMaskDesc
{
    TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> Geometry;
    FVector2f Origin = FVector2f::ZeroVector;
    FVector2f AxisX = FVector2f(1.0f, 0.0f);
    FVector2f AxisY = FVector2f(0.0f, 1.0f);
    FSlateRect ScissorRect;
    uint64 GeometryId = 0;
    int32 Operation = 0;
};

struct FRmlUiSlateRhiDrawDesc
{
    uint64 ViewId = 0;
    uint64 FrameId = 0;
    TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> Geometry;
    FTextureRHIRef Texture;
    FVector2f Origin = FVector2f::ZeroVector;
    FVector2f AxisX = FVector2f(1.0f, 0.0f);
    FVector2f AxisY = FVector2f(0.0f, 1.0f);
    FSlateRect ScissorRect;
    uint64 GeometryId = 0;
    float VisualOpacity = 1.0f;
    TArray<FRmlUiSlateRhiMaskDesc> ClipMasks;
};

class FRmlUiSlateRhiSubmission : public ICustomSlateElement
{
public:
    virtual void UpdateDraws(TArray<FRmlUiSlateRhiDrawDesc> Draws) = 0;
    virtual void ResetDraws() = 0;
};

TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> CreateRmlUiSlateRhiGeometry(
    TConstArrayView<float> Vertices, TConstArrayView<uint32> Indices,
    uint64 CacheRegistryId, uint64 VertexBufferRegistryId, uint64 IndexBufferRegistryId);

void MarkRmlUiSlateRhiGeometryPendingDestroy(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry);

bool IsRmlUiSlateRhiGeometryReady(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry);

TSharedRef<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe> CreateRmlUiSlateRhiSubmission(
    TArray<FRmlUiSlateRhiDrawDesc> Draws);
void UpdateRmlUiSlateRhiSubmission(
    const TSharedPtr<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe>& Submission,
    TArray<FRmlUiSlateRhiDrawDesc> Draws);
void ResetRmlUiSlateRhiSubmission(
    const TSharedPtr<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe>& Submission);
