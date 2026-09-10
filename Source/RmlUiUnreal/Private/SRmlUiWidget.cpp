#include "SRmlUiWidget.h"

#include "Engine/Texture2D.h"
#include "GlobalRenderResources.h"
#include "RHITypes.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "RmlUiBridge.h"
#include "RmlUiResourceRegistry.h"
#include "RmlUiSlateRhiRenderer.h"
#include "RmlUiUnrealModule.h"
#include "Slate/DeferredCleanupSlateBrush.h"
#include "SlateMaterialBrush.h"
#include "Styling/CoreStyle.h"
#include "TextureResource.h"

namespace
{
int GetModifiers(const FInputEvent& Event)
{
    return (Event.IsShiftDown() ? 1 : 0) | (Event.IsControlDown() ? 2 : 0) |
        (Event.IsAltDown() ? 4 : 0) | (Event.AreCapsLocked() ? 8 : 0) | (Event.IsCommandDown() ? 32 : 0);
}

int GetMouseButton(const FKey& Key)
{
    if (Key == EKeys::LeftMouseButton) return 0;
    if (Key == EKeys::RightMouseButton) return 1;
    if (Key == EKeys::MiddleMouseButton) return 2;
    if (Key == EKeys::ThumbMouseButton) return 3;
    if (Key == EKeys::ThumbMouseButton2) return 4;
    return -1;
}

int GetVirtualKey(const FKeyEvent& Event)
{
    if (Event.GetKeyCode()) return static_cast<int>(Event.GetKeyCode());
    const FKey Key = Event.GetKey();
    if (Key == EKeys::BackSpace) return 0x08;
    if (Key == EKeys::Tab) return 0x09;
    if (Key == EKeys::Enter) return 0x0d;
    if (Key == EKeys::Escape) return 0x1b;
    if (Key == EKeys::SpaceBar) return 0x20;
    if (Key == EKeys::PageUp) return 0x21;
    if (Key == EKeys::PageDown) return 0x22;
    if (Key == EKeys::End) return 0x23;
    if (Key == EKeys::Home) return 0x24;
    if (Key == EKeys::Left) return 0x25;
    if (Key == EKeys::Up) return 0x26;
    if (Key == EKeys::Right) return 0x27;
    if (Key == EKeys::Down) return 0x28;
    if (Key == EKeys::Insert) return 0x2d;
    if (Key == EKeys::Delete) return 0x2e;
    const FString Name = Key.GetFName().ToString();
    return Name.Len() == 1 ? FChar::ToUpper(Name[0]) : 0;
}
}

void SRmlUiWidget::Construct(const FArguments& InArgs)
{
    DesiredSize = InArgs._DesiredSize;
    MaxTextureDimension = FMath::Clamp(InArgs._MaxTextureDimension, 64, 4096);
    bUseSlateRenderer = InArgs._UseSlateRenderer;
    OnDocumentEvent = InArgs._OnDocumentEvent;
    SetBaseStyleSheet(InArgs._BaseStyleSheet);
    SetCanTick(true);
    ForceVolatile(true);
    SetClipping(EWidgetClipping::ClipToBounds);
    FRmlUiUnrealModule::Get().RegisterWidget(SharedThis(this));
    if (!InArgs._InlineDocument.IsEmpty())
    {
        LoadDocumentFromString(InArgs._InlineDocument, InArgs._SourcePath);
    }
    else
    {
        LoadDocument(InArgs._DocumentPath);
    }
}

SRmlUiWidget::~SRmlUiWidget()
{
    ShutdownNative();
}

uint64 SRmlUiWidget::GetResolvedMaterialDrawCount(int32 MaterialSlot) const
{
    return MaterialSlot >= 0 && MaterialSlot < UE_ARRAY_COUNT(ResolvedMaterialDrawCounts)
        ? ResolvedMaterialDrawCounts[MaterialSlot]
        : 0;
}

int32 SRmlUiWidget::GetReadySlateRhiGeometryCount() const
{
    int32 Count = 0;
    for (const auto& Pair : NativeGeometries)
    {
        if (IsRmlUiSlateRhiGeometryReady(Pair.Value.RhiGeometry)) ++Count;
    }
    return Count;
}

void SRmlUiWidget::ShutdownNative()
{
    if (NativeView)
    {
        OnNativeShutdown.Broadcast();
    }
    ReleaseUnrealRenderResources(true);
    if (NativeView)
    {
        RmlUE_DestroyView(NativeView);
        NativeView = nullptr;
    }
    if (BaseStyleSheet)
    {
        RmlUE_ReleaseStyleSheet(BaseStyleSheet);
        BaseStyleSheet = nullptr;
    }
    bNativeShutdown = true;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    NativeDraws.Reset();
    NativeMaterialResources.Reset();
}

void SRmlUiWidget::ReleaseUnrealRenderResources(bool bIncludeMaterials)
{
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    Registry.UnregisterUnreal(TextureRegistryId);
    TextureRegistryId = 0;
    Texture = nullptr;
    TextureBrush.Reset();
    TextureSize = FIntPoint::ZeroValue;
    for (const auto& DrawElement : SlateRhiDrawElements) ResetRmlUiSlateRhiDraw(DrawElement);
    SlateRhiDrawElements.Reset();
    for (const auto& Pair : NativeGeometries) MarkRmlUiSlateRhiGeometryPendingDestroy(Pair.Value.RhiGeometry);
    NativeGeometries.Reset();
    for (const auto& Pair : NativeTextures) Registry.UnregisterUnreal(Pair.Value.RegistryId);
    NativeTextures.Reset();
    if (bIncludeMaterials)
    {
        for (const auto& Pair : Materials) Registry.UnregisterUnreal(Pair.Value.RegistryId);
        Materials.Reset();
    }
}

RmlUE_View* SRmlUiWidget::ExchangeNativeView(RmlUE_View* Replacement)
{
    check(IsInGameThread() && Replacement);
    if (BaseStyleSheet && !RmlUE_SetBaseStyleSheet(Replacement, BaseStyleSheet))
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("Could not attach the base style sheet to a replacement view: %s"), UTF8_TO_TCHAR(RmlUE_GetLastError()));
    }
    RmlUE_View* Previous = NativeView;
    ReleaseUnrealRenderResources(false);
    NativeDraws.Reset();
    NativeMaterialResources.Reset();
    NativeView = Replacement;
    bNativeShutdown = false;
    DocumentError.Reset();
    LastError.Reset();
    FrameNumber = 0;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    return Previous;
}

bool SRmlUiWidget::EnsureNativeView()
{
    if (NativeView) return true;
    if (bNativeShutdown) return false;
    FRmlUiUnrealModule& Module = FRmlUiUnrealModule::Get();
    if (!Module.IsInitialized())
    {
        LastError = Module.GetInitializationError();
        return false;
    }
    const int32 Width = FMath::Clamp(FMath::RoundToInt(DesiredSize.X), 1, MaxTextureDimension);
    const int32 Height = FMath::Clamp(FMath::RoundToInt(DesiredSize.Y), 1, MaxTextureDimension);
    NativeView = bUseSlateRenderer ? RmlUE_CreateSlateView(Width, Height, 1.0f) : RmlUE_CreateView(Width, Height, 1.0f);
    if (!CheckResult(NativeView != nullptr)) return false;
    return !BaseStyleSheet || CheckResult(RmlUE_SetBaseStyleSheet(NativeView, BaseStyleSheet));
}

bool SRmlUiWidget::CheckResult(int Result)
{
    if (Result)
    {
        LastError.Reset();
        return true;
    }
    const FString Error = UTF8_TO_TCHAR(RmlUE_GetLastError());
    if (Error != LastError)
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("%s"), *Error);
    }
    LastError = Error;
    return false;
}

bool SRmlUiWidget::LoadDocument(const FString& Path)
{
    DocumentPath = FRmlUiUnrealModule::Get().ResolveDocumentPath(Path);
    InlineDocument.Reset();
    SourcePath.Reset();
    const bool bLoaded = EnsureNativeView() && CheckResult(RmlUE_LoadDocument(NativeView, TCHAR_TO_UTF8(*DocumentPath)));
    DocumentError = bLoaded ? FString() : LastError;
    return bLoaded;
}

bool SRmlUiWidget::LoadDocumentFromString(const FString& Markup, const FString& InSourcePath)
{
    InlineDocument = Markup;
    SourcePath = FRmlUiUnrealModule::Get().ResolveDocumentPath(InSourcePath);
    DocumentPath.Reset();
    const bool bLoaded = EnsureNativeView() && CheckResult(RmlUE_LoadDocumentFromMemory(NativeView,
        TCHAR_TO_UTF8(*InlineDocument), TCHAR_TO_UTF8(*SourcePath)));
    DocumentError = bLoaded ? FString() : LastError;
    return bLoaded;
}

bool SRmlUiWidget::ReloadDocument()
{
    return InlineDocument.IsEmpty() ? LoadDocument(DocumentPath) : LoadDocumentFromString(InlineDocument, SourcePath);
}

bool SRmlUiWidget::SetElementInnerRml(const FString& Id, const FString& Rml)
{
    return NativeView && CheckResult(RmlUE_SetInnerRml(NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Rml)));
}

bool SRmlUiWidget::SetElementText(const FString& Id, const FString& Text)
{
    FString Escaped = Text.Replace(TEXT("&"), TEXT("&amp;"));
    Escaped.ReplaceInline(TEXT("<"), TEXT("&lt;"));
    Escaped.ReplaceInline(TEXT(">"), TEXT("&gt;"));
    return SetElementInnerRml(Id, Escaped);
}

bool SRmlUiWidget::SetElementProperty(const FString& Id, const FString& Property, const FString& Value)
{
    return NativeView && CheckResult(RmlUE_SetProperty(NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Property), TCHAR_TO_UTF8(*Value)));
}

bool SRmlUiWidget::SetElementAttribute(const FString& Id, const FString& Attribute, const FString& Value)
{
    return NativeView && CheckResult(RmlUE_SetAttribute(NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Attribute), TCHAR_TO_UTF8(*Value)));
}

bool SRmlUiWidget::GetElementAttribute(const FString& Id, const FString& Attribute, FString& Value) const
{
    char Buffer[16384]{};
    if (!NativeView || !RmlUE_GetAttribute(NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Attribute), Buffer, sizeof(Buffer)))
    {
        Value.Reset();
        return false;
    }
    Value = UTF8_TO_TCHAR(Buffer);
    return true;
}

void SRmlUiWidget::SetDebuggerVisible(bool bVisible)
{
    if (NativeView) RmlUE_SetDebuggerVisible(NativeView, bVisible);
}

void SRmlUiWidget::SetDesiredSize(FVector2D InSize)
{
    DesiredSize = InSize.ComponentMax(FVector2D(1.0, 1.0));
    Invalidate(EInvalidateWidgetReason::Layout);
}

void SRmlUiWidget::SetMaxTextureDimension(int32 InMaximum)
{
    MaxTextureDimension = FMath::Clamp(InMaximum, 64, 4096);
}

void SRmlUiWidget::SetUseSlateRenderer(bool bInUseSlateRenderer)
{
    if (bUseSlateRenderer == bInUseSlateRenderer) return;
    bUseSlateRenderer = bInUseSlateRenderer;
    if (!NativeView) return;
    OnNativeShutdown.Broadcast();
    ReleaseUnrealRenderResources(false);
    RmlUE_DestroyView(NativeView);
    NativeView = nullptr;
    bNativeShutdown = false;
    NativeDraws.Reset();
    NativeMaterialResources.Reset();
    ReloadDocument();
}

bool SRmlUiWidget::RegisterMaterial(FName Alias, UMaterialInterface* Material)
{
    if (Alias.IsNone() || !Material) return false;
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    if (const FMaterialResource* Existing = Materials.Find(Alias)) Registry.UnregisterUnreal(Existing->RegistryId);
    const FSlateMaterialBrush MaterialBrush(*Material, FVector2D(1.0, 1.0));
    FMaterialResource Resource;
    Resource.Brush = FDeferredCleanupSlateBrush::CreateBrush(MaterialBrush);
    Resource.RegistryId = Registry.RegisterUnreal(ERmlUiResourceType::SlateMaterialBrush, ERmlUiResourceBackend::Slate,
        0, 0, FString::Printf(TEXT("Material alias: %s"), *Alias.ToString()), Material);
    Materials.Add(Alias, MoveTemp(Resource));
    Invalidate(EInvalidateWidgetReason::Paint);
    return true;
}

void SRmlUiWidget::UnregisterMaterial(FName Alias)
{
    if (const FMaterialResource* Existing = Materials.Find(Alias))
        FRmlUiResourceRegistry::Get().UnregisterUnreal(Existing->RegistryId);
    Materials.Remove(Alias);
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SRmlUiWidget::SetBaseStyleSheet(RmlUE_StyleSheet* InStyleSheet)
{
    if (BaseStyleSheet == InStyleSheet) return;
    if (InStyleSheet) RmlUE_RetainStyleSheet(InStyleSheet);
    if (BaseStyleSheet) RmlUE_ReleaseStyleSheet(BaseStyleSheet);
    BaseStyleSheet = InStyleSheet;
    if (NativeView) CheckResult(RmlUE_SetBaseStyleSheet(NativeView, BaseStyleSheet));
}

void SRmlUiWidget::Tick(const FGeometry& Geometry, double InCurrentTime, float InDeltaTime)
{
    SLeafWidget::Tick(Geometry, InCurrentTime, InDeltaTime);
    const FVector2D LocalSize = Geometry.GetLocalSize();
    if (LocalSize.X <= 0 || LocalSize.Y <= 0 || !NativeView) return;
    PixelScale = FMath::Max(0.01f, Geometry.GetAccumulatedLayoutTransform().GetScale());
    const double LongestEdge = FMath::Max(LocalSize.X, LocalSize.Y) * PixelScale;
    if (LongestEdge > MaxTextureDimension) PixelScale *= MaxTextureDimension / LongestEdge;
    RenderFrame(FMath::Max(1, FMath::RoundToInt(LocalSize.X * PixelScale)),
        FMath::Max(1, FMath::RoundToInt(LocalSize.Y * PixelScale)), PixelScale);
}

bool SRmlUiWidget::RenderFrame(int32 Width, int32 Height, float DpRatio)
{
    if (!EnsureNativeView()) return false;
    Width = FMath::Clamp(Width, 1, MaxTextureDimension);
    Height = FMath::Clamp(Height, 1, MaxTextureDimension);
    PixelScale = FMath::Max(0.01f, DpRatio);
    if (!CheckResult(RmlUE_Resize(NativeView, Width, Height, PixelScale))) return false;
    RmlUE_View* BeforeScript = NativeView;
    OnBeforeRender.Broadcast(FApp::GetDeltaTime());
    if (!NativeView) return false;
    if (BeforeScript != NativeView && !CheckResult(RmlUE_Resize(NativeView, Width, Height, PixelScale))) return false;
    if (bUseSlateRenderer)
    {
        RmlUE_SlateFrame SlateFrame{};
        if (!CheckResult(RmlUE_RenderSlate(NativeView, &SlateFrame))) return false;
        if (SlateFrame.AbiVersion != RMLUE_SLATE_ABI_VERSION)
        {
            LastError = TEXT("RmlUi Slate command ABI version mismatch.");
            return false;
        }
        NativeDraws.Reset(SlateFrame.DrawCount);
        for (uint32 Index = 0; Index < SlateFrame.GeometryDeltaCount; ++Index)
        {
            const RmlUE_SlateGeometryDelta& Source = SlateFrame.GeometryDeltas[Index];
            if (Source.Action == RMLUE_SLATE_RESOURCE_DESTROY)
            {
                if (const FGeometryResource* Existing = NativeGeometries.Find(Source.Id))
                    MarkRmlUiSlateRhiGeometryPendingDestroy(Existing->RhiGeometry);
                NativeGeometries.Remove(Source.Id);
                continue;
            }
            if (Source.Action != RMLUE_SLATE_RESOURCE_CREATE ||
                (Source.VertexCount > 0 && !Source.Vertices) || (Source.IndexCount > 0 && !Source.Indices))
            {
                LastError = TEXT("Invalid Slate geometry resource delta.");
                return false;
            }
            if (const FGeometryResource* Existing = NativeGeometries.Find(Source.Id))
                MarkRmlUiSlateRhiGeometryPendingDestroy(Existing->RhiGeometry);
            FGeometryResource Resource;
            Resource.Vertices.Reserve(Source.VertexCount * 8);
            for (uint32 VertexIndex = 0; VertexIndex < Source.VertexCount; ++VertexIndex)
            {
                const RmlUE_SlateVertex& Vertex = Source.Vertices[VertexIndex];
                Resource.Vertices.Append({Vertex.X, Vertex.Y, Vertex.U, Vertex.V,
                    static_cast<float>(Vertex.R), static_cast<float>(Vertex.G), static_cast<float>(Vertex.B), static_cast<float>(Vertex.A)});
            }
            if (Source.IndexCount > 0) Resource.Indices.Append(Source.Indices, Source.IndexCount);
            const uint64 EstimatedBytes = static_cast<uint64>(Source.VertexCount) * sizeof(RmlUE_SlateVertex) +
                static_cast<uint64>(Source.IndexCount) * sizeof(uint32);
            Resource.RegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(ERmlUiResourceType::SlateGeometryCache,
                ERmlUiResourceBackend::Slate, RmlUE_GetViewResourceId(NativeView), EstimatedBytes,
                FString::Printf(TEXT("Slate geometry cache %llu"), Source.Id));
            Resource.VertexBufferRegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(
                ERmlUiResourceType::SlateVertexBuffer, ERmlUiResourceBackend::RHI, Resource.RegistryId,
                static_cast<uint64>(Source.VertexCount) * 20,
                FString::Printf(TEXT("Slate vertex buffer %llu"), Source.Id), nullptr,
                ERmlUiResourceState::PendingCreate);
            Resource.IndexBufferRegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(
                ERmlUiResourceType::SlateIndexBuffer, ERmlUiResourceBackend::RHI, Resource.RegistryId,
                static_cast<uint64>(Source.IndexCount) * sizeof(uint32),
                FString::Printf(TEXT("Slate index buffer %llu"), Source.Id), nullptr,
                ERmlUiResourceState::PendingCreate);
            Resource.RhiGeometry = CreateRmlUiSlateRhiGeometry(Resource.Vertices, Resource.Indices,
                Resource.RegistryId, Resource.VertexBufferRegistryId, Resource.IndexBufferRegistryId);
            if (!Resource.RhiGeometry.IsValid())
            {
                FRmlUiResourceRegistry::Get().UnregisterUnreal(Resource.VertexBufferRegistryId);
                FRmlUiResourceRegistry::Get().UnregisterUnreal(Resource.IndexBufferRegistryId);
                FRmlUiResourceRegistry::Get().UnregisterUnreal(Resource.RegistryId);
                LastError = FString::Printf(TEXT("Could not create Slate RHI geometry %llu."), Source.Id);
                return false;
            }
            NativeGeometries.Add(Source.Id, MoveTemp(Resource));
        }
        for (uint32 Index = 0; Index < SlateFrame.TextureCount; ++Index)
        {
            const RmlUE_SlateTexture& Source = SlateFrame.Textures[Index];
            if (Source.Action == RMLUE_SLATE_RESOURCE_DESTROY)
            {
                NativeMaterialResources.Remove(Source.Id);
                if (const FTextureResource* Existing = NativeTextures.Find(Source.Id))
                    FRmlUiResourceRegistry::Get().UnregisterUnreal(Existing->RegistryId);
                NativeTextures.Remove(Source.Id);
                continue;
            }
            if (Source.Action != RMLUE_SLATE_RESOURCE_CREATE)
            {
                LastError = TEXT("Invalid Slate texture resource delta.");
                return false;
            }
            if (Source.Kind == 1)
            {
                NativeMaterialResources.Add(Source.Id,
                    {FName(UTF8_TO_TCHAR(Source.MaterialAlias ? Source.MaterialAlias : "")), Source.MaterialSlot});
                continue;
            }
            if (!Source.PremultipliedRGBA || Source.Width <= 0 || Source.Height <= 0)
            {
                LastError = TEXT("Invalid Slate texture create payload.");
                return false;
            }
            if (const FTextureResource* Existing = NativeTextures.Find(Source.Id))
                FRmlUiResourceRegistry::Get().UnregisterUnreal(Existing->RegistryId);
            UTexture2D* NewTexture = UTexture2D::CreateTransient(Source.Width, Source.Height, PF_B8G8R8A8);
            if (!NewTexture)
            {
                LastError = TEXT("Could not allocate a Slate resource texture.");
                return false;
            }
            NewTexture->NeverStream = true;
            NewTexture->SRGB = true;
            NewTexture->Filter = TF_Bilinear;
            NewTexture->LODGroup = TEXTUREGROUP_UI;
            NewTexture->UpdateResource();
            const SIZE_T ByteCount = static_cast<SIZE_T>(Source.Width) * Source.Height * 4;
            uint8* Pixels = static_cast<uint8*>(FMemory::Malloc(ByteCount));
            for (SIZE_T Pixel = 0; Pixel < ByteCount; Pixel += 4)
            {
                Pixels[Pixel + 0] = Source.PremultipliedRGBA[Pixel + 2];
                Pixels[Pixel + 1] = Source.PremultipliedRGBA[Pixel + 1];
                Pixels[Pixel + 2] = Source.PremultipliedRGBA[Pixel + 0];
                Pixels[Pixel + 3] = Source.PremultipliedRGBA[Pixel + 3];
            }
            FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Source.Width, Source.Height);
            NewTexture->UpdateTextureRegions(0, 1, Region, Source.Width * 4, 4, Pixels,
                [](uint8* Data, const FUpdateTextureRegion2D* Regions) { FMemory::Free(Data); delete Regions; });
            FTextureResource Resource;
            Resource.Texture = NewTexture;
            Resource.Brush = FDeferredCleanupSlateBrush::CreateBrush(NewTexture);
            Resource.RegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(ERmlUiResourceType::UnrealTexture,
                ERmlUiResourceBackend::Slate, RmlUE_GetViewResourceId(NativeView), ByteCount,
                FString::Printf(TEXT("Slate texture %llu"), Source.Id), NewTexture);
            NativeTextures.Add(Source.Id, MoveTemp(Resource));
        }
        for (uint32 Index = 0; Index < SlateFrame.DrawCount; ++Index)
        {
            const RmlUE_SlateDraw& Source = SlateFrame.Draws[Index];
            if (!NativeGeometries.Contains(Source.GeometryId) ||
                (Source.ClipMaskCount > 0 && SlateFrame.ClipMasks == nullptr) ||
                Source.ClipMaskStart > SlateFrame.ClipMaskCount ||
                Source.ClipMaskCount > SlateFrame.ClipMaskCount - Source.ClipMaskStart)
            {
                LastError = FString::Printf(TEXT("Slate draw references invalid geometry or clip-mask range for %llu."), Source.GeometryId);
                return false;
            }
            FNativeDraw& Draw = NativeDraws.AddDefaulted_GetRef();
            Draw.GeometryId = Source.GeometryId;
            Draw.TextureId = Source.Texture;
            Draw.Translation = FVector2f(Source.TranslateX, Source.TranslateY);
            Draw.bTransform = Source.TransformEnabled != 0;
            Draw.Transform = FMatrix2x2(Source.TransformM00, Source.TransformM10, Source.TransformM01, Source.TransformM11);
            Draw.TransformTranslation = FVector2f(Source.TransformX, Source.TransformY);
            Draw.bScissor = Source.ScissorEnabled != 0;
            Draw.Scissor = FSlateRect(Source.ScissorX, Source.ScissorY, Source.ScissorX + Source.ScissorWidth, Source.ScissorY + Source.ScissorHeight);
            Draw.ClipMasks.Reserve(Source.ClipMaskCount);
            for (uint32 MaskIndex = 0; MaskIndex < Source.ClipMaskCount; ++MaskIndex)
            {
                const RmlUE_SlateClipMask& SourceMask = SlateFrame.ClipMasks[Source.ClipMaskStart + MaskIndex];
                if (!NativeGeometries.Contains(SourceMask.GeometryId) || SourceMask.Operation < RMLUE_CLIP_MASK_SET ||
                    SourceMask.Operation > RMLUE_CLIP_MASK_INTERSECT)
                {
                    LastError = FString::Printf(TEXT("Slate draw references invalid clip-mask geometry %llu."), SourceMask.GeometryId);
                    return false;
                }
                FNativeMask& Mask = Draw.ClipMasks.AddDefaulted_GetRef();
                Mask.GeometryId = SourceMask.GeometryId;
                Mask.Operation = SourceMask.Operation;
                Mask.Translation = FVector2f(SourceMask.TranslateX, SourceMask.TranslateY);
                Mask.bTransform = SourceMask.TransformEnabled != 0;
                Mask.Transform = FMatrix2x2(SourceMask.TransformM00, SourceMask.TransformM10,
                    SourceMask.TransformM01, SourceMask.TransformM11);
                Mask.TransformTranslation = FVector2f(SourceMask.TransformX, SourceMask.TransformY);
                Mask.bScissor = SourceMask.ScissorEnabled != 0;
                Mask.Scissor = FSlateRect(SourceMask.ScissorX, SourceMask.ScissorY,
                    SourceMask.ScissorX + SourceMask.ScissorWidth, SourceMask.ScissorY + SourceMask.ScissorHeight);
            }
        }
        if (SlateFrame.UnsupportedFeatures && SlateFrame.UnsupportedFeatures != UnsupportedSlateFeatures)
        {
            UE_LOG(LogRmlUiUnreal, Warning, TEXT("The experimental Slate renderer omitted RmlUi advanced feature mask 0x%X. Use the DX11 compatibility renderer for visual parity."), SlateFrame.UnsupportedFeatures);
        }
        UnsupportedSlateFeatures = SlateFrame.UnsupportedFeatures;
        FrameNumber = SlateFrame.Number;
        Invalidate(EInvalidateWidgetReason::Paint);
        DispatchEvents();
        return true;
    }
    RmlUE_Frame Frame{};
    if (!CheckResult(RmlUE_Render(NativeView, &Frame)) || !Frame.Pixels || Frame.Width != Width || Frame.Height != Height)
    {
        return false;
    }
    if (!Texture || TextureSize != FIntPoint(Width, Height))
    {
        UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!NewTexture)
        {
            LastError = TEXT("Could not allocate the RmlUi display texture.");
            return false;
        }
        NewTexture->NeverStream = true;
        NewTexture->SRGB = true;
        NewTexture->Filter = TF_Bilinear;
        NewTexture->AddressX = TA_Clamp;
        NewTexture->AddressY = TA_Clamp;
        NewTexture->LODGroup = TEXTUREGROUP_UI;
        NewTexture->UpdateResource();
        FRmlUiResourceRegistry::Get().UnregisterUnreal(TextureRegistryId);
        TextureBrush = FDeferredCleanupSlateBrush::CreateBrush(NewTexture);
        Texture = NewTexture;
        TextureSize = FIntPoint(Width, Height);
        TextureRegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(ERmlUiResourceType::UnrealTexture,
            ERmlUiResourceBackend::DX11, RmlUE_GetViewResourceId(NativeView), static_cast<uint64>(Width) * Height * 4,
            TEXT("DX11 display upload texture"), NewTexture);
    }
    if (!Texture->GetResource()) return false;

    // The bridge reuses its frame buffer; the RHI consumes this copy asynchronously.
    const SIZE_T ByteCount = static_cast<SIZE_T>(Width) * Height * 4;
    uint8* UploadPixels = static_cast<uint8*>(FMemory::Malloc(ByteCount));
    FMemory::Memcpy(UploadPixels, Frame.Pixels, ByteCount);
    FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
    Texture->UpdateTextureRegions(0, 1, Region, Width * 4, 4, UploadPixels,
        [](uint8* Pixels, const FUpdateTextureRegion2D* Regions)
        {
            FMemory::Free(Pixels);
            delete Regions;
        });
    FrameNumber = Frame.Number;
    Invalidate(EInvalidateWidgetReason::Paint);
    DispatchEvents();
    return true;
}

void SRmlUiWidget::DispatchEvents()
{
    RmlUE_Event Event{};
    for (int32 Count = 0; NativeView && Count < 1024 && RmlUE_PollEvent(NativeView, &Event); ++Count)
    {
        OnDocumentEvent.ExecuteIfBound(UTF8_TO_TCHAR(Event.Type), UTF8_TO_TCHAR(Event.ElementId), UTF8_TO_TCHAR(Event.Value));
    }
}

FVector2D SRmlUiWidget::ComputeDesiredSize(float) const
{
    return DesiredSize;
}

int32 SRmlUiWidget::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect& CullingRect,
    FSlateWindowElementList& Elements, int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const
{
    SlateRhiDrawCount = 0;
    SlateRhiMaskCount = 0;
    SlateFallbackDrawCount = 0;
    if (bUseSlateRenderer && NativeDraws.Num() > 0)
    {
        const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
        int32 RhiDrawElementIndex = 0;
        for (const FNativeDraw& Draw : NativeDraws)
        {
            const FGeometryResource* GeometryResource = NativeGeometries.Find(Draw.GeometryId);
            if (!GeometryResource) continue;
            const FSlateBrush* Brush = nullptr;
            const FNativeMaterialResource* NativeMaterial = NativeMaterialResources.Find(Draw.TextureId);
            if (NativeMaterial)
            {
                if (const FMaterialResource* Material = Materials.Find(NativeMaterial->Alias))
                    if (Material->Brush.IsValid())
                    {
                        Brush = Material->Brush->GetSlateBrush();
                        ++ResolvedMaterialDrawCount;
                        if (NativeMaterial->Slot >= 0 && NativeMaterial->Slot < UE_ARRAY_COUNT(ResolvedMaterialDrawCounts))
                            ++ResolvedMaterialDrawCounts[NativeMaterial->Slot];
                    }
            }
            const FTextureResource* TextureResource = NativeMaterial ? nullptr : NativeTextures.Find(Draw.TextureId);
            if (TextureResource)
            {
                if (TextureResource->Brush.IsValid()) Brush = TextureResource->Brush->GetSlateBrush();
            }

            bool bMasksReady = true;
            for (const FNativeMask& Mask : Draw.ClipMasks)
            {
                const FGeometryResource* MaskGeometry = NativeGeometries.Find(Mask.GeometryId);
                bMasksReady &= MaskGeometry && IsRmlUiSlateRhiGeometryReady(MaskGeometry->RhiGeometry);
            }
            FTextureRHIRef TextureRhi;
            if (!NativeMaterial && bMasksReady && IsRmlUiSlateRhiGeometryReady(GeometryResource->RhiGeometry))
            {
                if (Draw.TextureId == 0)
                {
                    TextureRhi = GWhiteTexture->TextureRHI;
                }
                else if (TextureResource && TextureResource->Texture && TextureResource->Texture->GetResource())
                {
                    TextureRhi = TextureResource->Texture->GetResource()->GetTextureRHI();
                }
            }
            if (TextureRhi.IsValid())
            {
                const auto ToWindowPosition = [&](FVector2f Position, FVector2f Translation, bool bTransform,
                    const FMatrix2x2& DrawTransform, FVector2f TransformTranslation)
                {
                    Position += Translation;
                    if (bTransform) Position = DrawTransform.TransformPoint(Position) + TransformTranslation;
                    Position /= PixelScale;
                    return TransformPoint(Transform, Position);
                };
                const auto ResolveScissor = [&](bool bScissor, const FSlateRect& Scissor)
                {
                    FSlateRect Result = CullingRect;
                    if (bScissor)
                    {
                        const FVector2D TopLeft = Geometry.LocalToAbsolute(
                            FVector2D(Scissor.Left, Scissor.Top) / PixelScale);
                        const FVector2D BottomRight = Geometry.LocalToAbsolute(
                            FVector2D(Scissor.Right, Scissor.Bottom) / PixelScale);
                        Result = Result.IntersectionWith(FSlateRect(TopLeft.X, TopLeft.Y, BottomRight.X, BottomRight.Y));
                    }
                    return Result;
                };
                const FVector2f Origin = ToWindowPosition(FVector2f::ZeroVector, Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation);
                FRmlUiSlateRhiDrawDesc RhiDraw;
                RhiDraw.Geometry = GeometryResource->RhiGeometry;
                RhiDraw.Texture = TextureRhi;
                RhiDraw.Origin = Origin;
                RhiDraw.AxisX = ToWindowPosition(FVector2f(1.0f, 0.0f), Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation) - Origin;
                RhiDraw.AxisY = ToWindowPosition(FVector2f(0.0f, 1.0f), Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation) - Origin;
                RhiDraw.ScissorRect = ResolveScissor(Draw.bScissor, Draw.Scissor);
                RhiDraw.GeometryId = Draw.GeometryId;
                RhiDraw.ClipMasks.Reserve(Draw.ClipMasks.Num());
                for (const FNativeMask& Mask : Draw.ClipMasks)
                {
                    const FGeometryResource* MaskGeometry = NativeGeometries.Find(Mask.GeometryId);
                    if (!MaskGeometry) continue;
                    FRmlUiSlateRhiMaskDesc& RhiMask = RhiDraw.ClipMasks.AddDefaulted_GetRef();
                    RhiMask.Geometry = MaskGeometry->RhiGeometry;
                    RhiMask.Operation = Mask.Operation;
                    RhiMask.GeometryId = Mask.GeometryId;
                    RhiMask.ScissorRect = ResolveScissor(Mask.bScissor, Mask.Scissor);
                    RhiMask.Origin = ToWindowPosition(FVector2f::ZeroVector, Mask.Translation, Mask.bTransform,
                        Mask.Transform, Mask.TransformTranslation);
                    RhiMask.AxisX = ToWindowPosition(FVector2f(1.0f, 0.0f), Mask.Translation, Mask.bTransform,
                        Mask.Transform, Mask.TransformTranslation) - RhiMask.Origin;
                    RhiMask.AxisY = ToWindowPosition(FVector2f(0.0f, 1.0f), Mask.Translation, Mask.bTransform,
                        Mask.Transform, Mask.TransformTranslation) - RhiMask.Origin;
                }
                if (RhiDraw.ScissorRect.Right > RhiDraw.ScissorRect.Left &&
                    RhiDraw.ScissorRect.Bottom > RhiDraw.ScissorRect.Top)
                {
                    if (RhiDrawElementIndex < SlateRhiDrawElements.Num())
                    {
                        UpdateRmlUiSlateRhiDraw(SlateRhiDrawElements[RhiDrawElementIndex], MoveTemp(RhiDraw));
                    }
                    else
                    {
                        SlateRhiDrawElements.Add(CreateRmlUiSlateRhiDraw(MoveTemp(RhiDraw)));
                    }
                    FSlateDrawElement::MakeCustom(Elements, LayerId++, SlateRhiDrawElements[RhiDrawElementIndex]);
                    ++RhiDrawElementIndex;
                    ++SlateRhiDrawCount;
                    SlateRhiMaskCount += Draw.ClipMasks.Num();
                }
                continue;
            }

            if (!Brush) Brush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
            TArray<FSlateVertex> Vertices;
            Vertices.Reserve(GeometryResource->Vertices.Num() / 8);
            for (int32 Offset = 0; Offset < GeometryResource->Vertices.Num(); Offset += 8)
            {
                FVector2f Position = FVector2f(GeometryResource->Vertices[Offset], GeometryResource->Vertices[Offset + 1]) + Draw.Translation;
                if (Draw.bTransform) Position = Draw.Transform.TransformPoint(Position) + Draw.TransformTranslation;
                Position /= PixelScale;
                const FColor Color(static_cast<uint8>(GeometryResource->Vertices[Offset + 4]), static_cast<uint8>(GeometryResource->Vertices[Offset + 5]),
                    static_cast<uint8>(GeometryResource->Vertices[Offset + 6]), static_cast<uint8>(GeometryResource->Vertices[Offset + 7]));
                Vertices.Add(FSlateVertex::Make(Transform, Position,
                    FVector2f(GeometryResource->Vertices[Offset + 2], GeometryResource->Vertices[Offset + 3]), Color));
            }
            TArray<SlateIndex> Indices;
            Indices.Reserve(GeometryResource->Indices.Num());
            for (uint32 Index : GeometryResource->Indices) Indices.Add(static_cast<SlateIndex>(Index));
            if (Draw.bScissor)
            {
                const FVector2D TopLeft = Geometry.LocalToAbsolute(FVector2D(Draw.Scissor.Left, Draw.Scissor.Top) / PixelScale);
                const FVector2D BottomRight = Geometry.LocalToAbsolute(FVector2D(Draw.Scissor.Right, Draw.Scissor.Bottom) / PixelScale);
                Elements.PushClip(FSlateClippingZone(FSlateRect(TopLeft.X, TopLeft.Y, BottomRight.X, BottomRight.Y)));
            }
            FSlateDrawElement::MakeCustomVerts(Elements, LayerId++,
                FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush), Vertices, Indices,
                nullptr, 0, 0, ESlateDrawEffect::None, ESlateBatchDrawFlag::PreMultipliedAlpha);
            ++SlateFallbackDrawCount;
            if (Draw.bScissor) Elements.PopClip();
        }
        for (int32 Index = RhiDrawElementIndex; Index < SlateRhiDrawElements.Num(); ++Index)
        {
            ResetRmlUiSlateRhiDraw(SlateRhiDrawElements[Index]);
        }
    }
    if (TextureBrush.IsValid())
    {
        FSlateDrawElement::MakeBox(Elements, LayerId, Geometry.ToPaintGeometry(), TextureBrush->GetSlateBrush(),
            ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect,
            Style.GetColorAndOpacityTint());
    }
    const FString Error = GetLastError();
    const FVector2D Size = Geometry.GetLocalSize();
    if (!Error.IsEmpty() && Size.X > 80 && Size.Y > 24)
    {
        const float Height = FMath::Min(40.0, Size.Y);
        const float Top = Size.Y - Height;
        FSlateDrawElement::MakeBox(Elements, ++LayerId,
            Geometry.ToPaintGeometry(FVector2f(Size.X, Height), FSlateLayoutTransform(FVector2f(0, Top))),
            FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, FLinearColor(0.18f, 0.025f, 0.025f, 0.98f));
        FString Caption = FString(TEXT("RmlUi: ")) + Error.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" "));
        const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10);
        const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
        bool bTruncated = false;
        while (Caption.Len() > 1 && Measure->Measure(Caption + TEXT("..."), Font).X > Size.X - 20)
        {
            Caption.LeftChopInline(1);
            bTruncated = true;
        }
        if (bTruncated) Caption += TEXT("...");
        FSlateDrawElement::MakeText(Elements, ++LayerId,
            Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(10, Top + 10))), Caption, Font,
            ESlateDrawEffect::None, FLinearColor::White);
    }
    return LayerId;
}

void SRmlUiWidget::UpdateMousePosition(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!NativeView) return;
    const FVector2D Position = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()) * PixelScale;
    RmlUE_MouseMove(NativeView, FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y), GetModifiers(Event));
}

FReply SRmlUiWidget::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!NativeView) return FReply::Unhandled();
    UpdateMousePosition(Geometry, Event);
    return FReply::Handled();
}

FReply SRmlUiWidget::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const int Button = GetMouseButton(Event.GetEffectingButton());
    if (!NativeView || Button < 0) return FReply::Unhandled();
    UpdateMousePosition(Geometry, Event);
    PressedMouseButtons.Add(Button);
    RmlUE_MouseButton(NativeView, Button, 1, GetModifiers(Event));
    return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse).CaptureMouse(SharedThis(this));
}

FReply SRmlUiWidget::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const int Button = GetMouseButton(Event.GetEffectingButton());
    if (!NativeView || Button < 0) return FReply::Unhandled();
    UpdateMousePosition(Geometry, Event);
    RmlUE_MouseButton(NativeView, Button, 0, GetModifiers(Event));
    PressedMouseButtons.Remove(Button);
    return PressedMouseButtons.IsEmpty() ? FReply::Handled().ReleaseMouseCapture() : FReply::Handled();
}

FReply SRmlUiWidget::OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event)
{
    return OnMouseButtonDown(Geometry, Event);
}

FReply SRmlUiWidget::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!NativeView) return FReply::Unhandled();
    UpdateMousePosition(Geometry, Event);
    RmlUE_MouseWheel(NativeView, Event.GetWheelDelta(), GetModifiers(Event));
    return FReply::Handled();
}

void SRmlUiWidget::OnMouseLeave(const FPointerEvent& Event)
{
    SLeafWidget::OnMouseLeave(Event);
    if (NativeView && !HasMouseCapture()) RmlUE_MouseLeave(NativeView);
}

void SRmlUiWidget::OnMouseCaptureLost(const FCaptureLostEvent& Event)
{
    SLeafWidget::OnMouseCaptureLost(Event);
    if (NativeView)
    {
        if (const FVector2D* TouchPosition = ActiveTouches.Find(Event.PointerIndex))
        {
            RmlUE_Touch(NativeView, Event.PointerIndex, TouchPosition->X, TouchPosition->Y, 3);
            ActiveTouches.Remove(Event.PointerIndex);
        }
        else
        {
            for (int Button : PressedMouseButtons) RmlUE_MouseButton(NativeView, Button, 0, 0);
            PressedMouseButtons.Empty();
        }
    }
}

FReply SRmlUiWidget::OnKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    const int Key = GetVirtualKey(Event);
    if (!NativeView || !Key) return FReply::Unhandled();
    RmlUE_Key(NativeView, Key, 1, GetModifiers(Event));
    return FReply::Handled();
}

FReply SRmlUiWidget::OnKeyUp(const FGeometry&, const FKeyEvent& Event)
{
    const int Key = GetVirtualKey(Event);
    if (!NativeView || !Key) return FReply::Unhandled();
    RmlUE_Key(NativeView, Key, 0, GetModifiers(Event));
    return FReply::Handled();
}

FReply SRmlUiWidget::OnKeyChar(const FGeometry&, const FCharacterEvent& Event)
{
    if (!NativeView) return FReply::Unhandled();
    const TCHAR Character = Event.GetCharacter();
    if (Character >= 0xd800 && Character <= 0xdbff)
    {
        PendingHighSurrogate = Character;
        return FReply::Handled();
    }
    FString Text;
    if (PendingHighSurrogate && Character >= 0xdc00 && Character <= 0xdfff) Text.AppendChar(PendingHighSurrogate);
    PendingHighSurrogate = 0;
    if ((Character >= 0x20 || Character == '\n' || Character == '\r') && !Event.IsControlDown() && !Event.IsAltDown())
    {
        Text.AppendChar(Character == '\r' ? '\n' : Character);
        RmlUE_Text(NativeView, TCHAR_TO_UTF8(*Text));
    }
    return FReply::Handled();
}

void SRmlUiWidget::OnFocusLost(const FFocusEvent& Event)
{
    SLeafWidget::OnFocusLost(Event);
    PendingHighSurrogate = 0;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    if (NativeView) RmlUE_FocusLost(NativeView);
}

FReply SRmlUiWidget::ForwardTouch(const FGeometry& Geometry, const FPointerEvent& Event, int32 Phase)
{
    if (!NativeView) return FReply::Unhandled();
    const FVector2D Position = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()) * PixelScale;
    if (Phase == 2) ActiveTouches.Remove(Event.GetPointerIndex());
    else ActiveTouches.Add(Event.GetPointerIndex(), Position);
    RmlUE_Touch(NativeView, Event.GetPointerIndex(), Position.X, Position.Y, Phase);
    if (Phase == 0) return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse).CaptureMouse(SharedThis(this));
    return Phase == 2 ? FReply::Handled().ReleaseMouseCapture() : FReply::Handled();
}

FReply SRmlUiWidget::OnTouchStarted(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 0); }
FReply SRmlUiWidget::OnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 1); }
FReply SRmlUiWidget::OnTouchEnded(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 2); }
