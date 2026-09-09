#include "RmlUiSlateRhiRenderer.h"

#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RenderDeferredCleanup.h"
#include "RenderGraphBuilder.h"
#include "RenderResource.h"
#include "RHIStaticStates.h"
#include "RHIUtilities.h"
#include "RmlUiResourceRegistry.h"
#include "ShaderParameterStruct.h"
#include "Containers/Queue.h"
#include "Misc/ScopeLock.h"

namespace
{
struct FRmlUiSlateRhiVertex
{
    FVector2f Position;
    FVector2f TexCoord;
    FColor Color;
};

class FRmlUiSlateVertexDeclaration : public FRenderResource
{
public:
    FVertexDeclarationRHIRef VertexDeclarationRHI;

    virtual void InitRHI(FRHICommandListBase&) override
    {
        FVertexDeclarationElementList Elements;
        Elements.Add(FVertexElement(0, STRUCT_OFFSET(FRmlUiSlateRhiVertex, Position), VET_Float2, 0,
            sizeof(FRmlUiSlateRhiVertex)));
        Elements.Add(FVertexElement(0, STRUCT_OFFSET(FRmlUiSlateRhiVertex, TexCoord), VET_Float2, 1,
            sizeof(FRmlUiSlateRhiVertex)));
        Elements.Add(FVertexElement(0, STRUCT_OFFSET(FRmlUiSlateRhiVertex, Color), VET_Color, 2,
            sizeof(FRmlUiSlateRhiVertex)));
        VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
    }

    virtual void ReleaseRHI() override
    {
        VertexDeclarationRHI.SafeRelease();
    }
};

TGlobalResource<FRmlUiSlateVertexDeclaration> GRmlUiSlateVertexDeclaration;

class FRmlUiSlateVertexShader final : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FRmlUiSlateVertexShader);
    SHADER_USE_PARAMETER_STRUCT(FRmlUiSlateVertexShader, FGlobalShader);

public:
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FMatrix44f, ViewProjection)
        SHADER_PARAMETER(FVector2f, Origin)
        SHADER_PARAMETER(FVector2f, AxisX)
        SHADER_PARAMETER(FVector2f, AxisY)
    END_SHADER_PARAMETER_STRUCT()
};

class FRmlUiSlatePixelShader final : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FRmlUiSlatePixelShader);
    SHADER_USE_PARAMETER_STRUCT(FRmlUiSlatePixelShader, FGlobalShader);

public:
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_TEXTURE(Texture2D, InTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, TextureSampler)
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FRmlUiSlateVertexShader, "/Plugin/RmlUiUnreal/Private/RmlUiSlate.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FRmlUiSlatePixelShader, "/Plugin/RmlUiUnreal/Private/RmlUiSlate.usf", "MainPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FRmlUiSlateRenderPassParameters, )
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
}

class FRmlUiSlateRhiGeometry final : public FRenderResource, public FDeferredCleanupInterface
{
public:
    FRmlUiSlateRhiGeometry(TArray<FRmlUiSlateRhiVertex> InVertices, TArray<uint32> InIndices,
        uint64 InCacheRegistryId, uint64 InVertexBufferRegistryId, uint64 InIndexBufferRegistryId)
        : SourceVertices(MoveTemp(InVertices)), SourceIndices(MoveTemp(InIndices)),
          CacheRegistryId(InCacheRegistryId), VertexBufferRegistryId(InVertexBufferRegistryId),
          IndexBufferRegistryId(InIndexBufferRegistryId)
    {
    }

    void MarkPendingDestroy()
    {
        if (bPendingDestroy.Exchange(true)) return;
        FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
        Registry.SetUnrealState(CacheRegistryId, ERmlUiResourceState::PendingDestroy);
        Registry.SetUnrealState(VertexBufferRegistryId, ERmlUiResourceState::PendingDestroy);
        Registry.SetUnrealState(IndexBufferRegistryId, ERmlUiResourceState::PendingDestroy);
    }

    bool IsReady() const
    {
        return bReady.Load();
    }

    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        if (SourceVertices.IsEmpty() || SourceIndices.IsEmpty()) return;

        const FRHIBufferCreateDesc VertexDesc = FRHIBufferCreateDesc::CreateVertex<FRmlUiSlateRhiVertex>(
            TEXT("RmlUi.Slate.VertexBuffer"), SourceVertices.Num())
            .AddUsage(EBufferUsageFlags::Static)
            .SetInitialState(ERHIAccess::VertexOrIndexBuffer)
            .SetInitActionInitializer();
        TRHIBufferInitializer<FRmlUiSlateRhiVertex> VertexInitializer = RHICmdList.CreateBufferInitializer(VertexDesc);
        FMemory::Memcpy(VertexInitializer.GetWritableData(), SourceVertices.GetData(), SourceVertices.Num() * sizeof(FRmlUiSlateRhiVertex));
        VertexBufferRHI = VertexInitializer.Finalize();

        const FRHIBufferCreateDesc IndexDesc = FRHIBufferCreateDesc::CreateIndex<uint32>(
            TEXT("RmlUi.Slate.IndexBuffer"), SourceIndices.Num())
            .AddUsage(EBufferUsageFlags::Static)
            .SetInitialState(ERHIAccess::VertexOrIndexBuffer)
            .SetInitActionInitializer();
        TRHIBufferInitializer<uint32> IndexInitializer = RHICmdList.CreateBufferInitializer(IndexDesc);
        FMemory::Memcpy(IndexInitializer.GetWritableData(), SourceIndices.GetData(), SourceIndices.Num() * sizeof(uint32));
        IndexBufferRHI = IndexInitializer.Finalize();

        bReady.Store(VertexBufferRHI.IsValid() && IndexBufferRHI.IsValid());
        if (bReady.Load() && !bPendingDestroy.Load())
        {
            FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
            Registry.SetUnrealState(CacheRegistryId, ERmlUiResourceState::Live);
            Registry.SetUnrealState(VertexBufferRegistryId, ERmlUiResourceState::Live);
            Registry.SetUnrealState(IndexBufferRegistryId, ERmlUiResourceState::Live);
        }
    }

    virtual void ReleaseRHI() override
    {
        bReady.Store(false);
        VertexBufferRHI.SafeRelease();
        IndexBufferRHI.SafeRelease();
        FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
        if (bPendingDestroy.Load())
        {
            Registry.UnregisterUnreal(VertexBufferRegistryId);
            Registry.UnregisterUnreal(IndexBufferRegistryId);
            Registry.UnregisterUnreal(CacheRegistryId);
        }
        else
        {
            Registry.SetUnrealState(CacheRegistryId, ERmlUiResourceState::PendingCreate);
            Registry.SetUnrealState(VertexBufferRegistryId, ERmlUiResourceState::PendingCreate);
            Registry.SetUnrealState(IndexBufferRegistryId, ERmlUiResourceState::PendingCreate);
        }
    }

    FBufferRHIRef VertexBufferRHI;
    FBufferRHIRef IndexBufferRHI;
    uint32 VertexCount = 0;
    uint32 IndexCount = 0;

private:
    TArray<FRmlUiSlateRhiVertex> SourceVertices;
    TArray<uint32> SourceIndices;
    uint64 CacheRegistryId = 0;
    uint64 VertexBufferRegistryId = 0;
    uint64 IndexBufferRegistryId = 0;
    TAtomic<bool> bReady = false;
    TAtomic<bool> bPendingDestroy = false;
};

class FRmlUiSlateRhiDrawImpl final : public FRmlUiSlateRhiDraw, public FDeferredCleanupInterface
{
public:
    explicit FRmlUiSlateRhiDrawImpl(FRmlUiSlateRhiDrawDesc InDesc) : Desc(MoveTemp(InDesc)) {}

    virtual void UpdateDesc(FRmlUiSlateRhiDrawDesc InDesc) override
    {
        FScopeLock Lock(&DescMutex);
        Desc = MoveTemp(InDesc);
    }

    virtual void ResetDesc() override
    {
        UpdateDesc(FRmlUiSlateRhiDrawDesc());
    }

    virtual void PostCustomElementAdded(FSlateElementBatcher&) const override
    {
        FScopeLock Lock(&DescMutex);
        PendingDraws.Enqueue(Desc);
    }

    virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs) override
    {
        check(IsInRenderingThread());
        FRmlUiSlateRhiDrawDesc Draw;
        if (!PendingDraws.Dequeue(Draw) || !Draw.Geometry.IsValid() || !Draw.Geometry->IsReady() ||
            !Draw.Texture.IsValid() || !Inputs.OutputTexture)
        {
            return;
        }

        FRmlUiSlateRenderPassParameters* PassParameters = GraphBuilder.AllocParameters<FRmlUiSlateRenderPassParameters>();
        PassParameters->RenderTargets[0] = FRenderTargetBinding(Inputs.OutputTexture, ERenderTargetLoadAction::ELoad);
        const FIntPoint Extent = Inputs.OutputTexture->Desc.Extent;
        const FVector2f ElementsOffset = Inputs.ElementsOffset;
        const FMatrix44f ViewProjection = Inputs.ElementsMatrix;

        GraphBuilder.AddPass(RDG_EVENT_NAME("RmlUiSlateRhi Geometry=%llu", Draw.GeometryId), PassParameters,
            ERDGPassFlags::Raster, [Draw, Extent, ElementsOffset, ViewProjection](FRHICommandList& RHICmdList)
            {
                if (!Draw.Geometry->IsReady()) return;
                RHICmdList.SetViewport(0, 0, 0.0f, Extent.X, Extent.Y, 1.0f);
                const int32 Left = FMath::Clamp(FMath::FloorToInt(Draw.ScissorRect.Left + ElementsOffset.X), 0, Extent.X);
                const int32 Top = FMath::Clamp(FMath::FloorToInt(Draw.ScissorRect.Top + ElementsOffset.Y), 0, Extent.Y);
                const int32 Right = FMath::Clamp(FMath::CeilToInt(Draw.ScissorRect.Right + ElementsOffset.X), Left, Extent.X);
                const int32 Bottom = FMath::Clamp(FMath::CeilToInt(Draw.ScissorRect.Bottom + ElementsOffset.Y), Top, Extent.Y);
                if (Right <= Left || Bottom <= Top) return;
                RHICmdList.SetScissorRect(true, Left, Top, Right, Bottom);

                TShaderMapRef<FRmlUiSlateVertexShader> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                TShaderMapRef<FRmlUiSlatePixelShader> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                FGraphicsPipelineStateInitializer GraphicsPSOInit;
                RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
                GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha,
                    BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
                GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
                GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GRmlUiSlateVertexDeclaration.VertexDeclarationRHI;
                GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
                GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
                GraphicsPSOInit.PrimitiveType = PT_TriangleList;
                SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

                FRmlUiSlateVertexShader::FParameters VertexParameters;
                VertexParameters.ViewProjection = ViewProjection;
                VertexParameters.Origin = Draw.Origin;
                VertexParameters.AxisX = Draw.AxisX;
                VertexParameters.AxisY = Draw.AxisY;
                SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VertexParameters);

                FRmlUiSlatePixelShader::FParameters PixelParameters;
                PixelParameters.InTexture = Draw.Texture;
                PixelParameters.TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PixelParameters);

                RHICmdList.SetStreamSource(0, Draw.Geometry->VertexBufferRHI, 0);
                RHICmdList.DrawIndexedPrimitive(Draw.Geometry->IndexBufferRHI, 0, 0,
                    Draw.Geometry->VertexCount, 0, Draw.Geometry->IndexCount / 3, 1);
                RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
            });
    }

private:
    mutable FCriticalSection DescMutex;
    FRmlUiSlateRhiDrawDesc Desc;
    mutable TQueue<FRmlUiSlateRhiDrawDesc, EQueueMode::Spsc> PendingDraws;
};

TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> CreateRmlUiSlateRhiGeometry(
    TConstArrayView<float> Vertices, TConstArrayView<uint32> Indices,
    uint64 CacheRegistryId, uint64 VertexBufferRegistryId, uint64 IndexBufferRegistryId)
{
    if (Vertices.IsEmpty() || Vertices.Num() % 8 != 0 || Indices.IsEmpty() || Indices.Num() % 3 != 0)
    {
        return nullptr;
    }
    TArray<FRmlUiSlateRhiVertex> RhiVertices;
    RhiVertices.Reserve(Vertices.Num() / 8);
    for (int32 Offset = 0; Offset < Vertices.Num(); Offset += 8)
    {
        RhiVertices.Add({FVector2f(Vertices[Offset], Vertices[Offset + 1]),
            FVector2f(Vertices[Offset + 2], Vertices[Offset + 3]),
            FColor(static_cast<uint8>(Vertices[Offset + 4]), static_cast<uint8>(Vertices[Offset + 5]),
                static_cast<uint8>(Vertices[Offset + 6]), static_cast<uint8>(Vertices[Offset + 7]))});
    }
    for (uint32 Index : Indices)
    {
        if (Index >= static_cast<uint32>(RhiVertices.Num())) return nullptr;
    }

    TArray<uint32> RhiIndices;
    RhiIndices.Append(Indices.GetData(), Indices.Num());
    FRmlUiSlateRhiGeometry* Resource = new FRmlUiSlateRhiGeometry(MoveTemp(RhiVertices), MoveTemp(RhiIndices),
        CacheRegistryId, VertexBufferRegistryId, IndexBufferRegistryId);
    Resource->VertexCount = Vertices.Num() / 8;
    Resource->IndexCount = Indices.Num();
    TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe> Result = MakeShareable(Resource,
        [](FRmlUiSlateRhiGeometry* Geometry)
        {
            Geometry->MarkPendingDestroy();
            BeginReleaseResource(Geometry);
            BeginCleanup(Geometry);
        });
    BeginInitResource(FName(TEXT("RmlUiSlateGeometry")), Resource);
    return Result;
}

void MarkRmlUiSlateRhiGeometryPendingDestroy(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry)
{
    if (Geometry.IsValid()) Geometry->MarkPendingDestroy();
}

bool IsRmlUiSlateRhiGeometryReady(
    const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry)
{
    return Geometry.IsValid() && Geometry->IsReady();
}

TSharedRef<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe> CreateRmlUiSlateRhiDraw(FRmlUiSlateRhiDrawDesc Desc)
{
    return MakeShareable<FRmlUiSlateRhiDraw>(new FRmlUiSlateRhiDrawImpl(MoveTemp(Desc)),
        [](FRmlUiSlateRhiDraw* Draw) { BeginCleanup(static_cast<FRmlUiSlateRhiDrawImpl*>(Draw)); });
}

void UpdateRmlUiSlateRhiDraw(const TSharedPtr<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe>& Draw,
    FRmlUiSlateRhiDrawDesc Desc)
{
    if (Draw.IsValid()) Draw->UpdateDesc(MoveTemp(Desc));
}

void ResetRmlUiSlateRhiDraw(const TSharedPtr<FRmlUiSlateRhiDraw, ESPMode::ThreadSafe>& Draw)
{
    if (Draw.IsValid()) Draw->ResetDesc();
}
