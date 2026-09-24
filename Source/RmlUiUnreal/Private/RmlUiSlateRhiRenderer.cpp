#include "RmlUiSlateRhiRenderer.h"

#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RenderDeferredCleanup.h"
#include "RenderGraphBuilder.h"
#include "RenderResource.h"
#include "RHIStaticStates.h"
#include "RHIUtilities.h"
#include "RmlUiBridge.h"
#include "RmlUiPerformance.h"
#include "RmlUiResourceRegistry.h"
#include "ShaderParameterStruct.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT_NAMED(RmlUiSlateRhi, TEXT("RmlUi Slate RHI"));
DECLARE_GPU_STAT_NAMED(RmlUiSlateRhiMask, TEXT("RmlUi Slate RHI Mask"));
DECLARE_GPU_STAT_NAMED(RmlUiSlateRhiContent, TEXT("RmlUi Slate RHI Content"));

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
        SHADER_PARAMETER(float, VisualOpacity)
        SHADER_PARAMETER(float, VisualColorEnabled)
        SHADER_PARAMETER(FVector4f, VisualColor)
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
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_GeometryInitRHI);
        FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceStage::GeometryInit);
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
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_GeometryReleaseRHI);
        FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Slate, ERmlUiPerformanceStage::GeometryRelease);
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

namespace
{
bool EqualSlateRect(const FSlateRect& A, const FSlateRect& B)
{
    return A.Left == B.Left && A.Top == B.Top && A.Right == B.Right && A.Bottom == B.Bottom;
}

bool EqualClipChain(const FRmlUiSlateRhiDrawDesc& A, const FRmlUiSlateRhiDrawDesc& B)
{
    if (A.ClipMasks.Num() != B.ClipMasks.Num()) return false;
    for (int32 Index = 0; Index < A.ClipMasks.Num(); ++Index)
    {
        const FRmlUiSlateRhiMaskDesc& Left = A.ClipMasks[Index];
        const FRmlUiSlateRhiMaskDesc& Right = B.ClipMasks[Index];
        if (Left.Geometry != Right.Geometry || Left.GeometryId != Right.GeometryId ||
            Left.Operation != Right.Operation || Left.Origin != Right.Origin ||
            Left.AxisX != Right.AxisX || Left.AxisY != Right.AxisY ||
            !EqualSlateRect(Left.ScissorRect, Right.ScissorRect))
        {
            return false;
        }
    }
    return true;
}

struct FRmlUiSlateRhiDrawGroup
{
    int32 StartIndex = 0;
    int32 EndIndex = 0;
};

struct FRmlUiSlateRhiSubmissionSnapshot
{
    explicit FRmlUiSlateRhiSubmissionSnapshot(TArray<FRmlUiSlateRhiDrawDesc> InDraws)
        : Draws(MoveTemp(InDraws))
    {
        Groups.Reserve(Draws.Num());
        for (int32 GroupStart = 0; GroupStart < Draws.Num();)
        {
            int32 GroupEnd = GroupStart + 1;
            while (GroupEnd < Draws.Num() && EqualClipChain(Draws[GroupStart], Draws[GroupEnd]))
                ++GroupEnd;
            Groups.Add({GroupStart, GroupEnd});
            GroupStart = GroupEnd;
        }
        FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Slate,
            ERmlUiPerformanceWork::RhiGroupsCompiled, Groups.Num());
    }

    TArray<FRmlUiSlateRhiDrawDesc> Draws;
    TArray<FRmlUiSlateRhiDrawGroup> Groups;
};
}

class FRmlUiSlateRhiSubmissionImpl final : public FRmlUiSlateRhiSubmission, public FDeferredCleanupInterface
{
public:
    explicit FRmlUiSlateRhiSubmissionImpl(TArray<FRmlUiSlateRhiDrawDesc> InDraws)
        : DrawSnapshot(MakeShared<FRmlUiSlateRhiSubmissionSnapshot, ESPMode::ThreadSafe>(MoveTemp(InDraws))) {}

    virtual void UpdateDraws(TArray<FRmlUiSlateRhiDrawDesc> InDraws) override
    {
        TSharedPtr<const FRmlUiSlateRhiSubmissionSnapshot, ESPMode::ThreadSafe> NewSnapshot =
            MakeShared<FRmlUiSlateRhiSubmissionSnapshot, ESPMode::ThreadSafe>(MoveTemp(InDraws));
        FScopeLock Lock(&SnapshotMutex);
        DrawSnapshot = MoveTemp(NewSnapshot);
    }

    virtual void ResetDraws() override
    {
        FScopeLock Lock(&SnapshotMutex);
        DrawSnapshot.Reset();
    }

    virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs) override
    {
        check(IsInRenderingThread());
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Draw_RenderThread_Setup);
        TSharedPtr<const FRmlUiSlateRhiSubmissionSnapshot, ESPMode::ThreadSafe> LocalSnapshot;
        {
            FScopeLock Lock(&SnapshotMutex);
            LocalSnapshot = DrawSnapshot;
        }
        if (!LocalSnapshot.IsValid()) return;
        if (!Inputs.OutputTexture)
        {
            for (const FRmlUiSlateRhiDrawDesc& Draw : LocalSnapshot->Draws)
                FRmlUiPerformance::RecordRenderThread(Draw.ViewId, Draw.FrameId, Draw.GeometryId, 0, 0, 0, 0, true);
            return;
        }

        const FIntPoint Extent = Inputs.OutputTexture->Desc.Extent;
        const FVector2f ElementsOffset = Inputs.ElementsOffset;
        const FMatrix44f ViewProjection = Inputs.ElementsMatrix;
        RDG_EVENT_SCOPE_STAT(GraphBuilder, RmlUiSlateRhi, "RmlUiSlateRhi");
        FRDGTextureRef SharedStencilTexture = nullptr;
        for (const FRmlUiSlateRhiDrawGroup& Group : LocalSnapshot->Groups)
        {
            const int32 GroupStart = Group.StartIndex;
            const int32 GroupEnd = Group.EndIndex;
            const FRmlUiSlateRhiDrawDesc& Draw = LocalSnapshot->Draws[GroupStart];
            bool bHasReadyContent = false;
            for (int32 Index = GroupStart; Index < GroupEnd; ++Index)
            {
                const FRmlUiSlateRhiDrawDesc& ContentDraw = LocalSnapshot->Draws[Index];
                bHasReadyContent |= ContentDraw.Geometry.IsValid() && ContentDraw.Geometry->IsReady() &&
                    ContentDraw.Texture.IsValid();
            }
            if (!bHasReadyContent)
            {
                for (int32 Index = GroupStart; Index < GroupEnd; ++Index)
                {
                    const FRmlUiSlateRhiDrawDesc& SkippedDraw = LocalSnapshot->Draws[Index];
                    FRmlUiPerformance::RecordRenderThread(SkippedDraw.ViewId, SkippedDraw.FrameId,
                        SkippedDraw.GeometryId, 0, 0, 0, 0, true);
                }
                continue;
            }

            bool bMasksReady = true;
            for (const FRmlUiSlateRhiMaskDesc& Mask : Draw.ClipMasks)
                bMasksReady &= Mask.Geometry.IsValid() && Mask.Geometry->IsReady();
            if (!bMasksReady)
            {
                for (int32 Index = GroupStart; Index < GroupEnd; ++Index)
                {
                    const FRmlUiSlateRhiDrawDesc& SkippedDraw = LocalSnapshot->Draws[Index];
                    FRmlUiPerformance::RecordRenderThread(SkippedDraw.ViewId, SkippedDraw.FrameId,
                        SkippedDraw.GeometryId, 0, 0, 0, 0, true);
                }
                continue;
            }

            FRmlUiSlateRenderPassParameters* PassParameters =
                GraphBuilder.AllocParameters<FRmlUiSlateRenderPassParameters>();
            PassParameters->RenderTargets[0] =
                FRenderTargetBinding(Inputs.OutputTexture, ERenderTargetLoadAction::ELoad);
            if (!Draw.ClipMasks.IsEmpty())
            {
                if (!SharedStencilTexture)
                {
                    const FRDGTextureDesc StencilDesc = FRDGTextureDesc::Create2D(Extent, PF_DepthStencil,
                        FClearValueBinding(0.0f, 0), TexCreate_DepthStencilTargetable);
                    SharedStencilTexture =
                        GraphBuilder.CreateTexture(StencilDesc, TEXT("RmlUiSlate.ClipStencilScratch"));
                    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Slate,
                        ERmlUiPerformanceWork::StencilTextures);
                }
                PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SharedStencilTexture,
                    ERenderTargetLoadAction::ENoAction, ERenderTargetLoadAction::EClear,
                    FExclusiveDepthStencil::DepthNop_StencilWrite);
            }

            GraphBuilder.AddPass(RDG_EVENT_NAME("RmlUiSlateRhi Geometry=%llu Draws=%d",
                Draw.GeometryId, GroupEnd - GroupStart), PassParameters, ERDGPassFlags::Raster,
                [LocalSnapshot, GroupStart, GroupEnd, Extent, ElementsOffset, ViewProjection](FRHICommandList& RHICmdList)
                {
                TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Draw_RenderThread);
                RHI_BREADCRUMB_EVENT_STAT(RHICmdList, RmlUiSlateRhi, "RmlUiSlateRhiGroup");
                FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceStage::RenderThreadDraw);
                const FRmlUiSlateRhiDrawDesc& ClipOwner = LocalSnapshot->Draws[GroupStart];
                FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Slate,
                    ERmlUiPerformanceWork::RhiRasterPasses);
                if (!ClipOwner.ClipMasks.IsEmpty())
                    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Slate,
                        ERmlUiPerformanceWork::RhiClipBuilds);
                bool bGroupReady = true;
                for (const FRmlUiSlateRhiMaskDesc& Mask : ClipOwner.ClipMasks)
                    bGroupReady &= Mask.Geometry.IsValid() && Mask.Geometry->IsReady();
                if (!bGroupReady)
                {
                    for (int32 Index = GroupStart; Index < GroupEnd; ++Index)
                    {
                        const FRmlUiSlateRhiDrawDesc& SkippedDraw = LocalSnapshot->Draws[Index];
                        FRmlUiPerformance::RecordRenderThread(SkippedDraw.ViewId, SkippedDraw.FrameId,
                            SkippedDraw.GeometryId, 0, 0, 0, 0, true);
                    }
                    return;
                }
                RHICmdList.SetViewport(0, 0, 0.0f, Extent.X, Extent.Y, 1.0f);
                TShaderMapRef<FRmlUiSlateVertexShader> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                TShaderMapRef<FRmlUiSlatePixelShader> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                FRmlUiSlatePixelShader::FParameters PixelParameters;
                PixelParameters.InTexture = ClipOwner.Texture;
                PixelParameters.TextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                PixelParameters.VisualOpacity = ClipOwner.VisualOpacity;
                PixelParameters.VisualColorEnabled = 0.0f;
                PixelParameters.VisualColor = FVector4f::Zero();

                const auto SetScissor = [&](const FSlateRect& Rect)
                {
                    const int32 Left = FMath::Clamp(FMath::FloorToInt(Rect.Left + ElementsOffset.X), 0, Extent.X);
                    const int32 Top = FMath::Clamp(FMath::FloorToInt(Rect.Top + ElementsOffset.Y), 0, Extent.Y);
                    const int32 Right = FMath::Clamp(FMath::CeilToInt(Rect.Right + ElementsOffset.X), Left, Extent.X);
                    const int32 Bottom = FMath::Clamp(FMath::CeilToInt(Rect.Bottom + ElementsOffset.Y), Top, Extent.Y);
                    RHICmdList.SetScissorRect(true, Left, Top, Right, Bottom);
                    return Right > Left && Bottom > Top;
                };
                const auto SetPipeline = [&](FRHIBlendState* BlendState, FRHIDepthStencilState* DepthStencilState)
                {
                    FGraphicsPipelineStateInitializer GraphicsPSOInit;
                    RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
                    GraphicsPSOInit.BlendState = BlendState;
                    GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
                    GraphicsPSOInit.DepthStencilState = DepthStencilState;
                    GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GRmlUiSlateVertexDeclaration.VertexDeclarationRHI;
                    GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
                    GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
                    GraphicsPSOInit.PrimitiveType = PT_TriangleList;
                    SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
                    SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PixelParameters);
                };
                const auto DrawGeometry = [&](const TSharedPtr<FRmlUiSlateRhiGeometry, ESPMode::ThreadSafe>& Geometry,
                    FVector2f Origin, FVector2f AxisX, FVector2f AxisY)
                {
                    FRmlUiSlateVertexShader::FParameters VertexParameters;
                    VertexParameters.ViewProjection = ViewProjection;
                    VertexParameters.Origin = Origin;
                    VertexParameters.AxisX = AxisX;
                    VertexParameters.AxisY = AxisY;
                    SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VertexParameters);
                    RHICmdList.SetStreamSource(0, Geometry->VertexBufferRHI, 0);
                    RHICmdList.DrawIndexedPrimitive(Geometry->IndexBufferRHI, 0, 0,
                        Geometry->VertexCount, 0, Geometry->IndexCount / 3, 1);
                };

                uint32 StencilReference = 0;
                uint32 MaskTriangles = 0;
                if (!ClipOwner.ClipMasks.IsEmpty())
                {
                    RHI_BREADCRUMB_EVENT_STAT(RHICmdList, RmlUiSlateRhiMask, "RmlUiSlateRhiMask");
                    FRHIBlendState* NoColor = TStaticBlendState<CW_NONE>::GetRHI();
                    FRHIDepthStencilState* ReplaceStencil = TStaticDepthStencilState<false, CF_Always,
                        true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
                        true, CF_Always, SO_Keep, SO_Keep, SO_Replace>::GetRHI();
                    FRHIDepthStencilState* IncrementMatchingStencil = TStaticDepthStencilState<false, CF_Always,
                        true, CF_Equal, SO_Keep, SO_Keep, SO_SaturatedIncrement,
                        true, CF_Equal, SO_Keep, SO_Keep, SO_SaturatedIncrement>::GetRHI();
                    for (const FRmlUiSlateRhiMaskDesc& Mask : ClipOwner.ClipMasks)
                    {
                        MaskTriangles += Mask.Geometry->IndexCount / 3;
                        if (Mask.Operation == RMLUE_CLIP_MASK_SET || Mask.Operation == RMLUE_CLIP_MASK_SET_INVERSE)
                        {
                            StencilReference = Mask.Operation == RMLUE_CLIP_MASK_SET ? 1u : 0u;
                            SetPipeline(NoColor, ReplaceStencil);
                            RHICmdList.SetStencilRef(
                                Mask.Operation == RMLUE_CLIP_MASK_SET ? 1u : 255u);
                        }
                        else
                        {
                            SetPipeline(NoColor, IncrementMatchingStencil);
                            RHICmdList.SetStencilRef(StencilReference);
                            StencilReference = FMath::Min(StencilReference + 1u, 255u);
                        }
                        if (SetScissor(Mask.ScissorRect))
                            DrawGeometry(Mask.Geometry, Mask.Origin, Mask.AxisX, Mask.AxisY);
                    }
                }

                FRHIDepthStencilState* ContentDepthStencil = ClipOwner.ClipMasks.IsEmpty()
                    ? TStaticDepthStencilState<false, CF_Always>::GetRHI()
                    : TStaticDepthStencilState<false, CF_Always,
                        true, CF_Equal, SO_Keep, SO_Keep, SO_Keep,
                        true, CF_Equal, SO_Keep, SO_Keep, SO_Keep, 0xff, 0x00>::GetRHI();
                bool bFirstContentDraw = true;
                {
                    RHI_BREADCRUMB_EVENT_STAT(RHICmdList, RmlUiSlateRhiContent, "RmlUiSlateRhiContent");
                    for (int32 Index = GroupStart; Index < GroupEnd; ++Index)
                    {
                        const FRmlUiSlateRhiDrawDesc& ContentDraw = LocalSnapshot->Draws[Index];
                        if (!ContentDraw.Geometry.IsValid() || !ContentDraw.Geometry->IsReady() ||
                            !ContentDraw.Texture.IsValid())
                        {
                            FRmlUiPerformance::RecordRenderThread(ContentDraw.ViewId, ContentDraw.FrameId,
                                ContentDraw.GeometryId, 0, 0, 0, 0, true);
                            continue;
                        }
                        PixelParameters.InTexture = ContentDraw.Texture;
                        PixelParameters.VisualOpacity = ContentDraw.VisualOpacity;
                        PixelParameters.VisualColorEnabled = ContentDraw.bVisualColor ? 1.0f : 0.0f;
                        PixelParameters.VisualColor = ContentDraw.VisualColor;
                        SetPipeline(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha,
                            BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI(), ContentDepthStencil);
                        RHICmdList.SetStencilRef(StencilReference);
                        if (SetScissor(ContentDraw.ScissorRect))
                            DrawGeometry(ContentDraw.Geometry, ContentDraw.Origin, ContentDraw.AxisX, ContentDraw.AxisY);
                        FRmlUiPerformance::RecordRenderThread(ContentDraw.ViewId, ContentDraw.FrameId,
                            ContentDraw.GeometryId, 1,
                            bFirstContentDraw ? ClipOwner.ClipMasks.Num() : 0,
                            ContentDraw.Geometry->IndexCount / 3 + (bFirstContentDraw ? MaskTriangles : 0),
                            bFirstContentDraw && !ClipOwner.ClipMasks.IsEmpty()
                                ? static_cast<uint32>(Extent.X * Extent.Y) : 0,
                            false);
                        bFirstContentDraw = false;
                    }
                }
                RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
                });
        }
    }

private:
    mutable FCriticalSection SnapshotMutex;
    TSharedPtr<const FRmlUiSlateRhiSubmissionSnapshot, ESPMode::ThreadSafe> DrawSnapshot;
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

TSharedRef<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe> CreateRmlUiSlateRhiSubmission(
    TArray<FRmlUiSlateRhiDrawDesc> Draws)
{
    return MakeShareable<FRmlUiSlateRhiSubmission>(new FRmlUiSlateRhiSubmissionImpl(MoveTemp(Draws)),
        [](FRmlUiSlateRhiSubmission* Submission)
        {
            BeginCleanup(static_cast<FRmlUiSlateRhiSubmissionImpl*>(Submission));
        });
}

void UpdateRmlUiSlateRhiSubmission(
    const TSharedPtr<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe>& Submission,
    TArray<FRmlUiSlateRhiDrawDesc> Draws)
{
    if (Submission.IsValid()) Submission->UpdateDraws(MoveTemp(Draws));
}

void ResetRmlUiSlateRhiSubmission(
    const TSharedPtr<FRmlUiSlateRhiSubmission, ESPMode::ThreadSafe>& Submission)
{
    if (Submission.IsValid()) Submission->ResetDraws();
}
