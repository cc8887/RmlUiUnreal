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
#include "Runtime/Launch/Resources/Version.h"
#include "RmlUiBridge.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiPerformance.h"
#include "RmlUiResourceRegistry.h"
#include "RmlUiSlateRhiRenderer.h"
#include "RmlUiTextInput.h"
#include "RmlUiUnrealModule.h"
#include "Slate/DeferredCleanupSlateBrush.h"
#include "SlateMaterialBrush.h"
#include "Styling/CoreStyle.h"
#include "TextureResource.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

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

FColor UnpremultiplyMaterialVertexColor(FColor Color)
{
    if (Color.A == 0) return FColor(255, 255, 255, 0);
    const auto Channel = [Alpha = int32(Color.A)](uint8 Value)
    {
        return static_cast<uint8>(FMath::Min((int32(Value) * 255 + Alpha / 2) / Alpha, 255));
    };
    return FColor(Channel(Color.R), Channel(Color.G), Channel(Color.B), Color.A);
}

FColor ApplyPremultipliedOpacity(FColor Color, float Opacity)
{
    const auto Scale = [Opacity](uint8 Value)
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Value) * Opacity), 0, 255));
    };
    return FColor(Scale(Color.R), Scale(Color.G), Scale(Color.B), Scale(Color.A));
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

float Cross2D(FVector2f Origin, FVector2f A, FVector2f B)
{
    return (A.X - Origin.X) * (B.Y - Origin.Y) - (A.Y - Origin.Y) * (B.X - Origin.X);
}

bool BuildFilledConvexHull(const TArray<float>& Vertices, const TArray<uint32>& Indices, TArray<FVector2f>& OutHull)
{
    OutHull.Reset();
    if (Vertices.Num() < 24 || Vertices.Num() % 8 != 0 || Indices.Num() < 3 || Indices.Num() % 3 != 0) return false;

    TArray<FVector2f> Points;
    Points.Reserve(Vertices.Num() / 8);
    for (int32 Offset = 0; Offset < Vertices.Num(); Offset += 8)
    {
        const FVector2f Point(Vertices[Offset], Vertices[Offset + 1]);
        if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y)) return false;
        Points.Add(Point);
    }
    Points.Sort([](FVector2f A, FVector2f B)
    {
        return A.X < B.X || (A.X == B.X && A.Y < B.Y);
    });
    for (int32 Index = Points.Num() - 1; Index > 0; --Index)
        if (Points[Index].Equals(Points[Index - 1], 0.01f)) Points.RemoveAt(Index, 1, EAllowShrinking::No);
    if (Points.Num() < 3) return false;

    TArray<FVector2f> Hull;
    Hull.Reserve(Points.Num() * 2);
    for (FVector2f Point : Points)
    {
        while (Hull.Num() >= 2 && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 0.001f) Hull.Pop(EAllowShrinking::No);
        Hull.Add(Point);
    }
    const int32 LowerCount = Hull.Num();
    for (int32 Index = Points.Num() - 2; Index >= 0; --Index)
    {
        const FVector2f Point = Points[Index];
        while (Hull.Num() > LowerCount && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 0.001f) Hull.Pop(EAllowShrinking::No);
        Hull.Add(Point);
    }
    Hull.Pop(EAllowShrinking::No);
    if (Hull.Num() < 3) return false;

    double HullAreaTwice = 0.0;
    for (int32 Index = 0; Index < Hull.Num(); ++Index)
        HullAreaTwice += static_cast<double>(Hull[Index].X) * Hull[(Index + 1) % Hull.Num()].Y -
            static_cast<double>(Hull[Index].Y) * Hull[(Index + 1) % Hull.Num()].X;
    HullAreaTwice = FMath::Abs(HullAreaTwice);

    double TriangleAreaTwice = 0.0;
    const int32 VertexCount = Vertices.Num() / 8;
    for (int32 Index = 0; Index < Indices.Num(); Index += 3)
    {
        if (Indices[Index] >= static_cast<uint32>(VertexCount) || Indices[Index + 1] >= static_cast<uint32>(VertexCount) ||
            Indices[Index + 2] >= static_cast<uint32>(VertexCount)) return false;
        const auto Position = [&](uint32 VertexIndex)
        {
            return FVector2f(Vertices[VertexIndex * 8], Vertices[VertexIndex * 8 + 1]);
        };
        TriangleAreaTwice += FMath::Abs(static_cast<double>(Cross2D(
            Position(Indices[Index]), Position(Indices[Index + 1]), Position(Indices[Index + 2]))));
    }
    const double AreaTolerance = FMath::Max(1.0, HullAreaTwice * 0.01);
    if (HullAreaTwice <= 0.01 || FMath::Abs(TriangleAreaTwice - HullAreaTwice) > AreaTolerance) return false;
    OutHull = MoveTemp(Hull);
    return true;
}
}

void SRmlUiWidget::Construct(const FArguments& InArgs)
{
    DesiredSize = InArgs._DesiredSize;
    MaxTextureDimension = FMath::Clamp(InArgs._MaxTextureDimension, 64, 4096);
    bUseSlateRenderer = InArgs._UseSlateRenderer;
    bUsePaintCache = InArgs._UsePaintCache;
    bSlatePaintCacheEligible = bUsePaintCache;
    OnDocumentEvent = InArgs._OnDocumentEvent;
    SetBaseStyleSheet(InArgs._BaseStyleSheet);
    SetCanTick(true);
    ForceVolatile(bUseSlateRenderer);
    SetClipping(EWidgetClipping::ClipToBounds);
    FRmlUiUnrealModule::Get().RegisterWidget(SharedThis(this));
    TextInputContext = MakeShared<FRmlUiTextInputMethodContext>(SharedThis(this));
    TextInputContext->Initialize();
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

TSharedPtr<ITextInputMethodContext> SRmlUiWidget::GetTextInputMethodContext() const
{
    return TextInputContext;
}

void SRmlUiWidget::CancelTextComposition()
{
    if (TextInputContext) TextInputContext->CancelComposition();
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

#if WITH_DEV_AUTOMATION_TESTS
bool SRmlUiWidget::OverrideClipMaskOperationForTesting(uint32 OwnerNode, int32 Operation)
{
    if (!OwnerNode || Operation < RMLUE_CLIP_MASK_SET || Operation > RMLUE_CLIP_MASK_INTERSECT)
        return false;
    const TArray<FNativeMaskRef>* MaskRefs = NativeMaskIndicesByOwnerNode.Find(OwnerNode);
    if (!MaskRefs || MaskRefs->IsEmpty()) return false;
    bool bChanged = false;
    for (const FNativeMaskRef& Ref : *MaskRefs)
    {
        if (!NativeDraws.IsValidIndex(Ref.DrawIndex) ||
            !NativeDraws[Ref.DrawIndex].ClipMasks.IsValidIndex(Ref.MaskIndex))
            return false;
        FNativeMask& Mask = NativeDraws[Ref.DrawIndex].ClipMasks[Ref.MaskIndex];
        Mask.Operation = Operation;
        bChanged = true;
    }
    if (bChanged)
    {
        MarkSlatePaintCachePending();
        Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
    }
    return bChanged;
}

bool SRmlUiWidget::GetClipMaskOperationsForTesting(uint32 VisualNode, TArray<int32>& OutOperations) const
{
    OutOperations.Reset();
    const TArray<int32>* DrawIndices = NativeDrawIndicesByVisualNode.Find(VisualNode);
    if (!DrawIndices) return false;
    for (const int32 DrawIndex : *DrawIndices)
    {
        if (!NativeDraws.IsValidIndex(DrawIndex) || NativeDraws[DrawIndex].ClipMasks.IsEmpty()) continue;
        OutOperations.Reserve(NativeDraws[DrawIndex].ClipMasks.Num());
        for (const FNativeMask& Mask : NativeDraws[DrawIndex].ClipMasks)
            OutOperations.Add(Mask.Operation);
        return true;
    }
    return false;
}
#endif

void SRmlUiWidget::ShutdownNative()
{
    if (TextInputContext) TextInputContext->Shutdown();
    if (NativeView)
    {
        FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelViewAnimations(NativeView);
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
    bHasSlateCommandSnapshot = false;
    NativeDrawIndicesByVisualNode.Reset();
    NativeMaskIndicesByOwnerNode.Reset();
    NativeMaterialResources.Reset();
    ResetSlateSchedule();
}

void SRmlUiWidget::ReleaseUnrealRenderResources(bool bIncludeMaterials)
{
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    Registry.UnregisterUnreal(TextureRegistryId);
    TextureRegistryId = 0;
    Texture = nullptr;
    TextureBrush.Reset();
    TextureSize = FIntPoint::ZeroValue;
    for (const auto& Submission : SlateRhiSubmissions) ResetRmlUiSlateRhiSubmission(Submission);
    SlateRhiSubmissions.Reset();
    SlateRhiSubmissionPaintFrame = MAX_uint64;
    SlateRhiSubmissionIndex = 0;
    for (const auto& Pair : NativeGeometries) MarkRmlUiSlateRhiGeometryPendingDestroy(Pair.Value.RhiGeometry);
    NativeGeometries.Reset();
    for (const auto& Pair : NativeTextures) Registry.UnregisterUnreal(Pair.Value.RegistryId);
    NativeTextures.Reset();
    if (bIncludeMaterials)
    {
        for (const auto& Pair : Materials)
        {
            for (const auto& TextureParameter : Pair.Value.TextureParameterRegistryIds)
                Registry.UnregisterUnreal(TextureParameter.Value);
            Registry.UnregisterUnreal(Pair.Value.RegistryId);
        }
        Materials.Reset();
    }
}

void SRmlUiWidget::ReparentMaterialResources(uint64 OwnerId)
{
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    for (const auto& Pair : Materials) Registry.ReparentUnreal(Pair.Value.RegistryId, OwnerId);
}

RmlUE_View* SRmlUiWidget::ExchangeNativeView(RmlUE_View* Replacement)
{
    check(IsInGameThread() && Replacement);
    if (TextInputContext) TextInputContext->Deactivate(true);
    if (BaseStyleSheet && !RmlUE_SetBaseStyleSheet(Replacement, BaseStyleSheet))
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("Could not attach the base style sheet to a replacement view: %s"), UTF8_TO_TCHAR(RmlUE_GetLastError()));
    }
    RmlUE_View* Previous = NativeView;
    if (Previous)
    {
        FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelViewAnimations(Previous);
    }
    ReleaseUnrealRenderResources(false);
    NativeDraws.Reset();
    bHasSlateCommandSnapshot = false;
    NativeDrawIndicesByVisualNode.Reset();
    NativeMaskIndicesByOwnerNode.Reset();
    NativeMaterialResources.Reset();
    NativeView = Replacement;
    ReparentMaterialResources(RmlUE_GetViewResourceId(NativeView));
    bNativeShutdown = false;
    DocumentError.Reset();
    LastError.Reset();
    FrameNumber = 0;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    ResetSlateSchedule();
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
    ResetSlateSchedule();
    ReparentMaterialResources(RmlUE_GetViewResourceId(NativeView));
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
    if (bLoaded) RequestScheduledRender();
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
    if (bLoaded) RequestScheduledRender();
    return bLoaded;
}

bool SRmlUiWidget::ReloadDocument()
{
    return InlineDocument.IsEmpty() ? LoadDocument(DocumentPath) : LoadDocumentFromString(InlineDocument, SourcePath);
}

bool SRmlUiWidget::SetElementInnerRml(const FString& Id, const FString& Rml)
{
    const bool bChanged = NativeView && CheckResult(RmlUE_SetInnerRml(
        NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Rml)));
    if (bChanged) RequestScheduledRender();
    return bChanged;
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
    const bool bChanged = NativeView && CheckResult(RmlUE_SetProperty(
        NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Property), TCHAR_TO_UTF8(*Value)));
    if (bChanged) RequestScheduledRender();
    return bChanged;
}

bool SRmlUiWidget::SetElementAttribute(const FString& Id, const FString& Attribute, const FString& Value)
{
    const bool bChanged = NativeView && CheckResult(RmlUE_SetAttribute(
        NativeView, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Attribute), TCHAR_TO_UTF8(*Value)));
    if (bChanged) RequestScheduledRender();
    return bChanged;
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
    if (TextInputContext) TextInputContext->Deactivate(true);
    bUseSlateRenderer = bInUseSlateRenderer;
    if (!NativeView) return;
    FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelViewAnimations(NativeView);
    OnNativeShutdown.Broadcast();
    ReleaseUnrealRenderResources(false);
    ReparentMaterialResources(0);
    RmlUE_DestroyView(NativeView);
    NativeView = nullptr;
    bNativeShutdown = false;
    NativeDraws.Reset();
    bHasSlateCommandSnapshot = false;
    NativeDrawIndicesByVisualNode.Reset();
    NativeMaskIndicesByOwnerNode.Reset();
    NativeMaterialResources.Reset();
    ResetSlateSchedule();
    ReloadDocument();
}

bool SRmlUiWidget::RegisterMaterial(FName Alias, UMaterialInterface* Material)
{
    if (Alias.IsNone() || !Material) return false;
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    if (const FMaterialResource* Existing = Materials.Find(Alias))
    {
        for (const auto& TextureParameter : Existing->TextureParameterRegistryIds)
            Registry.UnregisterUnreal(TextureParameter.Value);
        Registry.UnregisterUnreal(Existing->RegistryId);
    }
    const FSlateMaterialBrush MaterialBrush(*Material, FVector2D(1.0, 1.0));
    FMaterialResource Resource;
    Resource.Brush = FDeferredCleanupSlateBrush::CreateBrush(MaterialBrush);
    Resource.RegistryId = Registry.RegisterUnreal(ERmlUiResourceType::SlateMaterialBrush, ERmlUiResourceBackend::Slate,
        NativeView ? RmlUE_GetViewResourceId(NativeView) : 0, 0,
        FString::Printf(TEXT("Material alias: %s"), *Alias.ToString()), Material);
    const EBlendMode BlendMode = Material->GetBlendMode();
    Resource.bUsePremultipliedVertexColor = BlendMode == BLEND_AlphaComposite;
    Resource.bSupportsInheritedOpacity = BlendMode == BLEND_Translucent || BlendMode == BLEND_Additive ||
        BlendMode == BLEND_AlphaComposite || BlendMode == BLEND_AlphaHoldout;
    Materials.Add(Alias, MoveTemp(Resource));
    Invalidate(EInvalidateWidgetReason::Paint);
    return true;
}

void SRmlUiWidget::UnregisterMaterial(FName Alias)
{
    if (const FMaterialResource* Existing = Materials.Find(Alias))
    {
        for (const auto& TextureParameter : Existing->TextureParameterRegistryIds)
            FRmlUiResourceRegistry::Get().UnregisterUnreal(TextureParameter.Value);
        FRmlUiResourceRegistry::Get().UnregisterUnreal(Existing->RegistryId);
    }
    Materials.Remove(Alias);
    Invalidate(EInvalidateWidgetReason::Paint);
}

bool SRmlUiWidget::TrackMaterialTexture(FName Alias, FName Parameter, UTexture* Value)
{
    if (Alias.IsNone() || Parameter.IsNone()) return false;
    FMaterialResource* Material = Materials.Find(Alias);
    if (!Material) return false;
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    uint64* ExistingId = Material->TextureParameterRegistryIds.Find(Parameter);
    if (!Value)
    {
        if (ExistingId) Registry.UnregisterUnreal(*ExistingId);
        Material->TextureParameterRegistryIds.Remove(Parameter);
        Invalidate(EInvalidateWidgetReason::Paint);
        return true;
    }
    const uint64 EstimatedBytes = Value->CalcTextureMemorySizeEnum(TMC_ResidentMips);
    if (ExistingId && Registry.UpdateUnrealObject(*ExistingId, EstimatedBytes, Value))
    {
        Invalidate(EInvalidateWidgetReason::Paint);
        return true;
    }
    const uint64 RegistryId = Registry.RegisterUnreal(ERmlUiResourceType::MaterialParameterTexture,
        ERmlUiResourceBackend::Slate, Material->RegistryId, EstimatedBytes,
        FString::Printf(TEXT("Material texture: %s.%s"), *Alias.ToString(), *Parameter.ToString()), Value);
    Material->TextureParameterRegistryIds.Add(Parameter, RegistryId);
    Invalidate(EInvalidateWidgetReason::Paint);
    return true;
}

bool SRmlUiWidget::AnalyzeMaterialOpacityGeometry(FGeometryResource& Geometry) const
{
    if (Geometry.bMaterialOpacityAnalyzed) return Geometry.bMaterialOpacitySupported;
    Geometry.bMaterialOpacityAnalyzed = true;
    Geometry.MaterialOpacitySections.Reset();
    if (Geometry.Vertices.Num() < 24 || Geometry.Vertices.Num() % 8 != 0 ||
        Geometry.Indices.Num() < 3 || Geometry.Indices.Num() % 3 != 0)
    {
        return false;
    }

    constexpr int32 MaxMaterialOpacitySections = 1024;
    TArray<FMaterialOpacitySection> Sections;
    const int32 VertexCount = Geometry.Vertices.Num() / 8;
    FVector2f Minimum(MAX_flt, MAX_flt);
    FVector2f Maximum(-MAX_flt, -MAX_flt);
    for (int32 Offset = 0; Offset < Geometry.Vertices.Num(); Offset += 8)
    {
        const FVector2f Position(Geometry.Vertices[Offset], Geometry.Vertices[Offset + 1]);
        if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)) return false;
        Minimum.X = FMath::Min(Minimum.X, Position.X);
        Minimum.Y = FMath::Min(Minimum.Y, Position.Y);
        Maximum.X = FMath::Max(Maximum.X, Position.X);
        Maximum.Y = FMath::Max(Maximum.Y, Position.Y);
    }
    const FVector2f Extent = Maximum - Minimum;
    if (Extent.X <= UE_SMALL_NUMBER || Extent.Y <= UE_SMALL_NUMBER) return false;

    for (int32 Offset = 0; Offset < Geometry.Vertices.Num(); Offset += 8)
    {
        const FVector2f Position(Geometry.Vertices[Offset], Geometry.Vertices[Offset + 1]);
        const FVector2f ExpectedUv((Position.X - Minimum.X) / Extent.X,
            (Position.Y - Minimum.Y) / Extent.Y);
        if (!FMath::IsNearlyEqual(Geometry.Vertices[Offset + 2], ExpectedUv.X, 0.001f) ||
            !FMath::IsNearlyEqual(Geometry.Vertices[Offset + 3], ExpectedUv.Y, 0.001f))
        {
            return false;
        }
    }

    const auto PositionAt = [&](uint32 VertexIndex)
    {
        return FVector2f(Geometry.Vertices[VertexIndex * 8], Geometry.Vertices[VertexIndex * 8 + 1]);
    };
    for (uint32 Index : Geometry.Indices)
        if (Index >= static_cast<uint32>(VertexCount)) return false;

    // Slate custom-material vertices ignore tint, so decompose non-box geometry into clipped box draws
    // while preserving one full-element material UV mapping.
    const auto BuildSectionHull = [&](int32 IndexStart, int32 IndexCount, TArray<FVector2f>& OutHull)
    {
        OutHull.Reset();
        TArray<FVector2f, TInlineAllocator<8>> Points;
        double TriangleAreaTwice = 0.0;
        for (int32 Index = IndexStart; Index < IndexStart + IndexCount; Index += 3)
        {
            const FVector2f A = PositionAt(Geometry.Indices[Index]);
            const FVector2f B = PositionAt(Geometry.Indices[Index + 1]);
            const FVector2f C = PositionAt(Geometry.Indices[Index + 2]);
            const double AreaTwice = FMath::Abs(static_cast<double>(Cross2D(A, B, C)));
            if (AreaTwice <= 0.001) continue;
            TriangleAreaTwice += AreaTwice;
            Points.Add(A);
            Points.Add(B);
            Points.Add(C);
        }
        Points.Sort([](FVector2f A, FVector2f B)
        {
            return A.X < B.X || (A.X == B.X && A.Y < B.Y);
        });
        for (int32 Index = Points.Num() - 1; Index > 0; --Index)
            if (Points[Index].Equals(Points[Index - 1], 0.01f)) Points.RemoveAt(Index, 1, EAllowShrinking::No);
        if (Points.Num() < 3 || Points.Num() > 4 || TriangleAreaTwice <= 0.001) return false;

        TArray<FVector2f, TInlineAllocator<8>> Hull;
        for (FVector2f Point : Points)
        {
            while (Hull.Num() >= 2 && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 0.001f)
                Hull.Pop(EAllowShrinking::No);
            Hull.Add(Point);
        }
        const int32 LowerCount = Hull.Num();
        for (int32 Index = Points.Num() - 2; Index >= 0; --Index)
        {
            const FVector2f Point = Points[Index];
            while (Hull.Num() > LowerCount && Cross2D(Hull[Hull.Num() - 2], Hull.Last(), Point) <= 0.001f)
                Hull.Pop(EAllowShrinking::No);
            Hull.Add(Point);
        }
        Hull.Pop(EAllowShrinking::No);
        if (Hull.Num() < 3 || Hull.Num() > 4) return false;

        double HullAreaTwice = 0.0;
        for (int32 Index = 0; Index < Hull.Num(); ++Index)
            HullAreaTwice += static_cast<double>(Hull[Index].X) * Hull[(Index + 1) % Hull.Num()].Y -
                static_cast<double>(Hull[Index].Y) * Hull[(Index + 1) % Hull.Num()].X;
        HullAreaTwice = FMath::Abs(HullAreaTwice);
        if (FMath::Abs(HullAreaTwice - TriangleAreaTwice) > FMath::Max(0.1, HullAreaTwice * 0.01)) return false;
        OutHull.Append(Hull.GetData(), Hull.Num());
        return true;
    };

    if (Geometry.bFilledConvex && Geometry.ConvexHull.Num() <= 12)
    {
        FMaterialOpacitySection& Section = Sections.AddDefaulted_GetRef();
        Section.ConvexHull = Geometry.ConvexHull;
    }
    else
    {
        for (int32 Index = 0; Index < Geometry.Indices.Num();)
        {
            FMaterialOpacitySection Section;
            const bool bMergedPair = Index + 6 <= Geometry.Indices.Num() &&
                BuildSectionHull(Index, 6, Section.ConvexHull);
            if (!bMergedPair && !BuildSectionHull(Index, 3, Section.ConvexHull)) return false;
            Sections.Add(MoveTemp(Section));
            if (Sections.Num() > MaxMaterialOpacitySections) return false;
            Index += bMergedPair ? 6 : 3;
        }
    }

    bool bUsesVertex[4] = {};
    uint8 CornerMask = 0;
    if (Geometry.Vertices.Num() == 4 * 8 && Geometry.Indices.Num() == 6)
    {
        for (uint32 Index : Geometry.Indices)
        {
            if (Index >= UE_ARRAY_COUNT(bUsesVertex)) return false;
            bUsesVertex[Index] = true;
        }
        for (int32 Offset = 0; Offset < Geometry.Vertices.Num(); Offset += 8)
        {
            const FVector2f Position(Geometry.Vertices[Offset], Geometry.Vertices[Offset + 1]);
            const bool bLeft = FMath::IsNearlyEqual(Position.X, Minimum.X);
            const bool bRight = FMath::IsNearlyEqual(Position.X, Maximum.X);
            const bool bTop = FMath::IsNearlyEqual(Position.Y, Minimum.Y);
            const bool bBottom = FMath::IsNearlyEqual(Position.Y, Maximum.Y);
            if ((!bLeft && !bRight) || (!bTop && !bBottom)) break;
            CornerMask |= 1u << ((bRight ? 1 : 0) + (bBottom ? 2 : 0));
        }
    }
    Geometry.MaterialBoundsMinimum = Minimum;
    Geometry.MaterialBoundsMaximum = Maximum;
    bool bUsesEveryVertex = true;
    for (bool bUsed : bUsesVertex) bUsesEveryVertex &= bUsed;
    Geometry.bMaterialOpacityFullBox = CornerMask == 0x0f && bUsesEveryVertex;
    Geometry.MaterialOpacitySections = MoveTemp(Sections);
    Geometry.bMaterialOpacitySupported = !Geometry.MaterialOpacitySections.IsEmpty();
    return Geometry.bMaterialOpacitySupported;
}

bool SRmlUiWidget::CanApplyMaterialOpacityAsBoxes(const FNativeDraw&, const FGeometryResource& Geometry,
    const FNativeMaterialResource& Binding, const FMaterialResource& Material) const
{
    if (!Material.bSupportsInheritedOpacity ||
        (Binding.Slot != RMLUE_MATERIAL_SLOT_BACKGROUND && Binding.Slot != RMLUE_MATERIAL_SLOT_BORDER) ||
        !Geometry.bMaterialOpacityAnalyzed || !Geometry.bMaterialOpacitySupported)
    {
        return false;
    }
    const float Alpha = Geometry.Vertices[7];
    for (int32 Offset = 7; Offset < Geometry.Vertices.Num(); Offset += 8)
        if (!FMath::IsNearlyEqual(Geometry.Vertices[Offset], Alpha, 0.5f)) return false;
    return true;
}

void SRmlUiWidget::CaptureSlateClipTopology(TArray<uint64>& OutTokens, uint64& OutMaskRefs) const
{
    OutTokens.Reset();
    OutMaskRefs = 0;
    const auto AddFloat = [&OutTokens](float Value)
    {
        OutTokens.Add(FMath::AsUInt(Value));
    };
    const auto AddRect = [&AddFloat](const FSlateRect& Rect)
    {
        AddFloat(Rect.Left);
        AddFloat(Rect.Top);
        AddFloat(Rect.Right);
        AddFloat(Rect.Bottom);
    };
    for (const FNativeDraw& Draw : NativeDraws)
    {
        if (!Draw.bScissor && Draw.ClipMasks.IsEmpty()) continue;
        OutTokens.Add(0xd24a57e000000001ull);
        OutTokens.Add(Draw.VisualNode);
        OutTokens.Add(Draw.bScissor ? 1 : 0);
        if (Draw.bScissor) AddRect(Draw.Scissor);
        OutTokens.Add(Draw.ClipMasks.Num());
        for (const FNativeMask& Mask : Draw.ClipMasks)
        {
            OutTokens.Add(0x4d41534b00000001ull);
            OutTokens.Add(Mask.GeometryId);
            OutTokens.Add(Mask.OwnerNode);
            OutTokens.Add(static_cast<uint64>(Mask.Operation));
            AddFloat(Mask.Translation.X);
            AddFloat(Mask.Translation.Y);
            OutTokens.Add(Mask.bScissor ? 1 : 0);
            if (Mask.bScissor) AddRect(Mask.Scissor);
            ++OutMaskRefs;
        }
    }
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
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Tick);
    CSV_SCOPED_TIMING_STAT(RmlUi, Tick);
    const ERmlUiPerformanceBackend Backend = bUseSlateRenderer
        ? ERmlUiPerformanceBackend::Slate : ERmlUiPerformanceBackend::DX11;
    FScopedRmlUiPerformanceTimer PerfTimer(Backend, ERmlUiPerformanceStage::Tick);
    SLeafWidget::Tick(Geometry, InCurrentTime, InDeltaTime);
    const FVector2D LocalSize = Geometry.GetLocalSize();
    if (LocalSize.X <= 0 || LocalSize.Y <= 0 || !NativeView) return;
    PixelScale = FMath::Max(0.01f, Geometry.GetAccumulatedLayoutTransform().GetScale());
    const double LongestEdge = FMath::Max(LocalSize.X, LocalSize.Y) * PixelScale;
    if (LongestEdge > MaxTextureDimension) PixelScale *= MaxTextureDimension / LongestEdge;
    const int32 Width = FMath::Max(1, FMath::RoundToInt(LocalSize.X * PixelScale));
    const int32 Height = FMath::Max(1, FMath::RoundToInt(LocalSize.Y * PixelScale));
    if (!bUseSlateRenderer)
    {
        RenderFrame(Width, Height, PixelScale);
    }
    else if (!bHasSlateSchedule || Width != LastScheduledWidth || Height != LastScheduledHeight ||
        !FMath::IsNearlyEqual(PixelScale, LastScheduledDpRatio))
    {
        FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledResizeWakes);
        RenderFrame(Width, Height, PixelScale);
    }
    else
    {
        RmlUE_View* BeforeScript = NativeView;
        const uint64 BeforeRenderCycles = BroadcastBeforeRender();
        if (!NativeView) return;
        if (BeforeScript != NativeView) ResetSlateSchedule();

        RmlUE_SlateScheduleState State{};
        if (!CheckResult(RmlUE_GetSlateScheduleState(NativeView, &State))) return;
        const double NowSeconds = FPlatformTime::Seconds();
        if (FMath::IsFinite(State.NextUpdateDelay))
            NextScheduledUpdateTime = FMath::Min(NextScheduledUpdateTime,
                NowSeconds + FMath::Max(0.0, State.NextUpdateDelay));

        const bool bContentWake = !bHasSlateSchedule || !State.HasRecordedFrame ||
            State.ContentRevision != LastScheduledContentRevision;
        const bool bVisualWake = State.VisualRevision != LastScheduledVisualRevision;
        const bool bDeadlineWake = NowSeconds >= NextScheduledUpdateTime;
        const bool bExplicitWake = bScheduledRenderRequested;
        if (bContentWake || bVisualWake || bDeadlineWake || bExplicitWake)
        {
            if (bContentWake) FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledContentWakes);
            if (bVisualWake) FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledVisualWakes);
            if (bDeadlineWake) FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledDeadlineWakes);
            if (bExplicitWake) FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledExplicitWakes);
            RenderFrameInternal(Width, Height, PixelScale, false, BeforeRenderCycles);
        }
        else
        {
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ScheduledRenderSkips);
        }
    }
    if (TextInputContext) TextInputContext->Update(Geometry, PixelScale);
    UpdateSlatePaintCacheReadiness();
    ArmScheduledActiveTimer(FPlatformTime::Seconds());
}

bool SRmlUiWidget::RenderFrame(int32 Width, int32 Height, float DpRatio)
{
    return RenderFrameInternal(Width, Height, DpRatio, true);
}

uint64 SRmlUiWidget::BroadcastBeforeRender()
{
    const ERmlUiPerformanceBackend Backend = bUseSlateRenderer
        ? ERmlUiPerformanceBackend::Slate : ERmlUiPerformanceBackend::DX11;
    const uint64 Start = FPlatformTime::Cycles64();
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_BeforeRender);
    FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::BeforeRender);
    OnBeforeRender.Broadcast(FApp::GetDeltaTime());
    return FPlatformTime::Cycles64() - Start;
}

void SRmlUiWidget::ResetSlateSchedule()
{
    if (const TSharedPtr<FActiveTimerHandle> Timer = ScheduledActiveTimer.Pin())
    {
        UnRegisterActiveTimer(Timer.ToSharedRef());
    }
    ScheduledActiveTimer.Reset();
    ScheduledActiveTimerDeadline = TNumericLimits<double>::Max();
    LastScheduledContentRevision = 0;
    LastScheduledVisualRevision = 0;
    NextScheduledUpdateTime = TNumericLimits<double>::Max();
    LastScheduledWidth = 0;
    LastScheduledHeight = 0;
    LastScheduledDpRatio = 0.0f;
    bHasSlateSchedule = false;
    bScheduledRenderRequested = true;
    bSlatePaintCacheEligible = bUsePaintCache;
    bSlatePaintCacheReady = false;
    bSlatePaintResourcesWaiting = false;
    SlatePaintCacheRejectReason = bUsePaintCache
        ? ERmlUiPaintCacheRejectReason::None : ERmlUiPaintCacheRejectReason::Disabled;
    if (bUseSlateRenderer) ForceVolatile(true);
    SetCanTick(true);
}

void SRmlUiWidget::CaptureSlateSchedule(double NowSeconds)
{
    if (!bUseSlateRenderer || !NativeView) return;
    RmlUE_SlateScheduleState State{};
    if (!RmlUE_GetSlateScheduleState(NativeView, &State)) return;
    LastScheduledContentRevision = State.ContentRevision;
    LastScheduledVisualRevision = State.VisualRevision;
    NextScheduledUpdateTime = FMath::IsFinite(State.NextUpdateDelay)
        ? NowSeconds + FMath::Max(0.0, State.NextUpdateDelay)
        : TNumericLimits<double>::Max();
    bHasSlateSchedule = State.HasRecordedFrame != 0;
}

bool SRmlUiWidget::AreSlatePaintResourcesReady() const
{
    for (const FNativeDraw& Draw : NativeDraws)
    {
        const bool bMaterialDraw = NativeMaterialResources.Contains(Draw.TextureId);
        if (bMaterialDraw)
        {
            continue;
        }

        if (!NativeGeometries.Contains(Draw.GeometryId)) return false;
        if (Draw.TextureId != 0)
        {
            const FTextureResource* TextureResource = NativeTextures.Find(Draw.TextureId);
            const ::FTextureResource* RenderResource = TextureResource && TextureResource->Texture
                ? TextureResource->Texture->GetResource() : nullptr;
            if (!TextureResource || !TextureResource->Brush.IsValid() || !RenderResource ||
                !RenderResource->TextureRHI.IsValid())
            {
                return false;
            }
        }
    }
    return true;
}

ERmlUiPaintCacheRejectReason SRmlUiWidget::EvaluateSlatePaintCache() const
{
    if (!bUsePaintCache) return ERmlUiPaintCacheRejectReason::Disabled;
    for (const FNativeDraw& Draw : NativeDraws)
    {
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8
        if (!NativeMaterialResources.Contains(Draw.TextureId))
        {
            const FTextureResource* TextureResource = NativeTextures.Find(Draw.TextureId);
            if (TextureResource && TextureResource->bHasColoredTranslucentPixels)
                return ERmlUiPaintCacheRejectReason::LegacyColoredTranslucentTexture;
        }
#endif
    }
    return ERmlUiPaintCacheRejectReason::None;
}

void SRmlUiWidget::MarkSlatePaintCachePending()
{
    if (!bUseSlateRenderer || !bSlatePaintCacheReady) return;
    bSlatePaintCacheReady = false;
    bSlatePaintResourcesWaiting = false;
    FRmlUiPerformance::RecordPaintCache(NativeView ? RmlUE_GetViewResourceId(NativeView) : 0,
        FrameNumber, ERmlUiPaintCacheEvent::Invalidated, SlatePaintCacheRejectReason, NativeDraws.Num());
    ForceVolatile(true);
    Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
}

void SRmlUiWidget::UpdateSlatePaintCacheReadiness()
{
    if (!bUseSlateRenderer || !bUsePaintCache || !bSlatePaintCacheEligible || bSlatePaintCacheReady || !bHasSlateSchedule)
    {
        return;
    }
    if (!AreSlatePaintResourcesReady())
    {
        if (!bSlatePaintResourcesWaiting)
        {
            bSlatePaintResourcesWaiting = true;
            FRmlUiPerformance::RecordPaintCache(NativeView ? RmlUE_GetViewResourceId(NativeView) : 0,
                FrameNumber, ERmlUiPaintCacheEvent::ResourceWait, SlatePaintCacheRejectReason, NativeDraws.Num());
        }
        return;
    }
    bSlatePaintResourcesWaiting = false;
    bSlatePaintCacheReady = true;
    FRmlUiPerformance::RecordPaintCache(NativeView ? RmlUE_GetViewResourceId(NativeView) : 0,
        FrameNumber, ERmlUiPaintCacheEvent::Activated, SlatePaintCacheRejectReason, NativeDraws.Num());
    ForceVolatile(false);
    Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
}

void SRmlUiWidget::RequestScheduledRender()
{
    bScheduledRenderRequested = true;
    ArmScheduledActiveTimer(FPlatformTime::Seconds());
}

void SRmlUiWidget::SetExternalWakeDeadline(const void* Owner, double AbsoluteTimeSeconds)
{
    if (!Owner) return;
    if (FMath::IsFinite(AbsoluteTimeSeconds)) ExternalWakeDeadlines.Add(Owner, AbsoluteTimeSeconds);
    else ExternalWakeDeadlines.Remove(Owner);
    ArmScheduledActiveTimer(FPlatformTime::Seconds());
}

void SRmlUiWidget::ClearExternalWakeDeadline(const void* Owner)
{
    if (!Owner) return;
    ExternalWakeDeadlines.Remove(Owner);
    ArmScheduledActiveTimer(FPlatformTime::Seconds());
}

void SRmlUiWidget::ArmScheduledActiveTimer(double NowSeconds)
{
    if (!bUseSlateRenderer || bNativeShutdown)
    {
        SetCanTick(!bNativeShutdown);
        return;
    }

    double Deadline = NextScheduledUpdateTime;
    for (const auto& Pair : ExternalWakeDeadlines) Deadline = FMath::Min(Deadline, Pair.Value);
    if (bUsePaintCache && bSlatePaintCacheEligible && !bSlatePaintCacheReady)
        Deadline = FMath::Min(Deadline, NowSeconds + 1.0 / 60.0);
    FRmlUiAnimationRuntime* AnimationRuntime = nullptr;
    bool bHasActiveAnimations = false;
    if (NativeView)
    {
        AnimationRuntime = &FRmlUiUnrealModule::Get().GetAnimationRuntime();
        bHasActiveAnimations = AnimationRuntime->HasActiveAnimations(NativeView);
        if (!bHasActiveAnimations)
        {
            const double AnimationWakeDelay = AnimationRuntime->GetNextWakeDelaySeconds(NativeView);
            if (AnimationWakeDelay < TNumericLimits<double>::Max())
            {
                Deadline = FMath::Min(Deadline, NowSeconds + AnimationWakeDelay);
            }
        }
    }
    if (!bHasSlateSchedule || bScheduledRenderRequested || bHasActiveAnimations)
    {
        Deadline = NowSeconds;
    }

    if (!FMath::IsFinite(Deadline) || Deadline >= TNumericLimits<double>::Max())
    {
        if (const TSharedPtr<FActiveTimerHandle> Timer = ScheduledActiveTimer.Pin())
            UnRegisterActiveTimer(Timer.ToSharedRef());
        ScheduledActiveTimer.Reset();
        ScheduledActiveTimerDeadline = TNumericLimits<double>::Max();
        SetCanTick(false);
        return;
    }

    if (const TSharedPtr<FActiveTimerHandle> Timer = ScheduledActiveTimer.Pin())
    {
        if (ScheduledActiveTimerDeadline <= Deadline + 0.0005)
        {
            SetCanTick(false);
            return;
        }
        UnRegisterActiveTimer(Timer.ToSharedRef());
    }
    ScheduledActiveTimerDeadline = Deadline;
    ScheduledActiveTimer = RegisterActiveTimer(
        static_cast<float>(FMath::Max(0.0, Deadline - NowSeconds)),
        FWidgetActiveTimerDelegate::CreateSP(this, &SRmlUiWidget::HandleScheduledActiveTimer));
    SetCanTick(false);
}

EActiveTimerReturnType SRmlUiWidget::HandleScheduledActiveTimer(double, float)
{
    ScheduledActiveTimer.Reset();
    ScheduledActiveTimerDeadline = TNumericLimits<double>::Max();
    SetCanTick(true);
    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Slate,
        ERmlUiPerformanceWork::ScheduledActiveTimerWakes);
    return EActiveTimerReturnType::Stop;
}

bool SRmlUiWidget::RenderFrameInternal(int32 Width, int32 Height, float DpRatio,
    bool bBroadcastBeforeRender, uint64 PrecomputedBeforeRenderCycles)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_RenderFrame);
    CSV_SCOPED_TIMING_STAT(RmlUi, RenderFrame);
    const ERmlUiPerformanceBackend Backend = bUseSlateRenderer
        ? ERmlUiPerformanceBackend::Slate : ERmlUiPerformanceBackend::DX11;
    FScopedRmlUiPerformanceTimer RenderTimer(Backend, ERmlUiPerformanceStage::RenderFrame);
    if (!EnsureNativeView()) return false;
    Width = FMath::Clamp(Width, 1, MaxTextureDimension);
    Height = FMath::Clamp(Height, 1, MaxTextureDimension);
    PixelScale = FMath::Max(0.01f, DpRatio);
    {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::Resize);
        if (!CheckResult(RmlUE_Resize(NativeView, Width, Height, PixelScale))) return false;
    }
    RmlUE_View* BeforeScript = NativeView;
    const uint64 BeforeRenderCycles = bBroadcastBeforeRender
        ? BroadcastBeforeRender() : PrecomputedBeforeRenderCycles;
    if (!NativeView) return false;
    if (BeforeScript != NativeView && !CheckResult(RmlUE_Resize(NativeView, Width, Height, PixelScale))) return false;
    if (bUseSlateRenderer)
    {
        RmlUE_SlateFrame SlateFrame{};
        const uint64 BridgeStart = FPlatformTime::Cycles64();
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Bridge_RenderSlate);
            FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::BridgeRender);
            if (!CheckResult(RmlUE_RenderSlate(NativeView, &SlateFrame))) return false;
        }
        const uint64 BridgeCycles = FPlatformTime::Cycles64() - BridgeStart;
        if (SlateFrame.AbiVersion != RMLUE_SLATE_ABI_VERSION)
        {
            LastError = TEXT("RmlUi Slate command ABI version mismatch.");
            return false;
        }
        if (SlateFrame.Replayed && (SlateFrame.GeometryDeltaCount != 0 || SlateFrame.TextureCount != 0))
        {
            LastError = TEXT("Retained Slate replay cannot contain resource deltas.");
            return false;
        }
        const uint64 DecodeStart = FPlatformTime::Cycles64();
        {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::GeometryDeltas);
        for (uint32 Index = 0; Index < SlateFrame.GeometryDeltaCount; ++Index)
        {
            const RmlUE_SlateGeometryDelta& Source = SlateFrame.GeometryDeltas[Index];
            if (Source.Action == RMLUE_SLATE_RESOURCE_DESTROY)
            {
                FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::GeometryDestroys);
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
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::GeometryCreates);
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::Vertices, Source.VertexCount);
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::Indices, Source.IndexCount);
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
            Resource.bFilledConvex = BuildFilledConvexHull(Resource.Vertices, Resource.Indices, Resource.ConvexHull);
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
        }
        {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::TextureDeltas);
        for (uint32 Index = 0; Index < SlateFrame.TextureCount; ++Index)
        {
            const RmlUE_SlateTexture& Source = SlateFrame.Textures[Index];
            if (Source.Action == RMLUE_SLATE_RESOURCE_DESTROY)
            {
                FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::TextureDestroys);
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
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::TextureCreates);
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::TextureUploadBytes, ByteCount);
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
            for (SIZE_T Pixel = 0; Pixel < ByteCount; Pixel += 4)
            {
                const uint8 Alpha = Source.PremultipliedRGBA[Pixel + 3];
                Resource.bHasColoredTranslucentPixels |= Alpha != 255 &&
                    (Source.PremultipliedRGBA[Pixel] != Alpha ||
                        Source.PremultipliedRGBA[Pixel + 1] != Alpha ||
                        Source.PremultipliedRGBA[Pixel + 2] != Alpha);
            }
            Resource.RegistryId = FRmlUiResourceRegistry::Get().RegisterUnreal(ERmlUiResourceType::UnrealTexture,
                ERmlUiResourceBackend::Slate, RmlUE_GetViewResourceId(NativeView), ByteCount,
                FString::Printf(TEXT("Slate texture %llu"), Source.Id), NewTexture);
            NativeTextures.Add(Source.Id, MoveTemp(Resource));
        }
        }
        uint32 FrameUnsupportedFeatures = SlateFrame.UnsupportedFeatures;
        if (SlateFrame.Replayed) FrameUnsupportedFeatures |= UnsupportedSlateFeatures;
        bool bClipTopologyChanged = false;
        uint64 ClipTopologyMaskRefsBefore = 0;
        uint64 ClipTopologyMaskRefsAfter = 0;
        TArray<uint64> PreviousClipTopology;
        const bool bCompareClipTopology = !SlateFrame.Replayed && bHasSlateCommandSnapshot &&
            FRmlUiPerformance::IsEnabled();
        if (bCompareClipTopology)
            CaptureSlateClipTopology(PreviousClipTopology, ClipTopologyMaskRefsBefore);
        {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::DrawDecode);
        if (SlateFrame.Replayed)
        {
            if (SlateFrame.DrawCount != static_cast<uint32>(NativeDraws.Num()) ||
                (SlateFrame.VisualDeltaCount > 0 && !SlateFrame.VisualDeltas))
            {
                LastError = TEXT("Invalid retained Slate replay frame.");
                return false;
            }
            for (uint32 DeltaIndex = 0; DeltaIndex < SlateFrame.VisualDeltaCount; ++DeltaIndex)
            {
                const RmlUE_SlateVisualDelta& Delta = SlateFrame.VisualDeltas[DeltaIndex];
                if (!Delta.Node || (Delta.OpacityChanged != 0 && Delta.OpacityChanged != 1) ||
                    (Delta.TransformChanged != 0 && Delta.TransformChanged != 1) ||
                    (Delta.ClipMaskTransformChanged != 0 && Delta.ClipMaskTransformChanged != 1) ||
                    (!Delta.OpacityChanged && !Delta.TransformChanged && !Delta.ClipMaskTransformChanged) ||
                    ((Delta.TransformChanged || Delta.ClipMaskTransformChanged) &&
                        Delta.TransformEnabled != 0 && Delta.TransformEnabled != 1) ||
                    (Delta.OpacityChanged && !FMath::IsFinite(Delta.VisualOpacity)) ||
                    ((Delta.TransformChanged || Delta.ClipMaskTransformChanged) &&
                        (!FMath::IsFinite(Delta.TransformM00) || !FMath::IsFinite(Delta.TransformM01) ||
                         !FMath::IsFinite(Delta.TransformM10) || !FMath::IsFinite(Delta.TransformM11) ||
                         !FMath::IsFinite(Delta.TransformX) || !FMath::IsFinite(Delta.TransformY))))
                {
                    LastError = TEXT("Slate replay contains an invalid visual delta.");
                    return false;
                }
                const TArray<int32>* DrawIndices = NativeDrawIndicesByVisualNode.Find(Delta.Node);
                if ((Delta.OpacityChanged || Delta.TransformChanged) && !DrawIndices)
                {
                    LastError = FString::Printf(TEXT("Slate replay references unknown visual node %u."), Delta.Node);
                    return false;
                }
                if (DrawIndices) for (const int32 DrawIndex : *DrawIndices)
                {
                    if (!NativeDraws.IsValidIndex(DrawIndex))
                    {
                        LastError = TEXT("Slate replay references an invalid retained draw index.");
                        return false;
                    }
                    FNativeDraw& Draw = NativeDraws[DrawIndex];
                    if (Delta.OpacityChanged)
                        Draw.VisualOpacity = FMath::Max(Delta.VisualOpacity, 0.0f);
                    if (Delta.TransformChanged)
                    {
                        Draw.bTransform = Delta.TransformEnabled != 0;
                        Draw.Transform = FMatrix2x2(Delta.TransformM00, Delta.TransformM10,
                            Delta.TransformM01, Delta.TransformM11);
                        Draw.TransformTranslation = FVector2f(Delta.TransformX, Delta.TransformY);
                    }
                }
                if (Delta.ClipMaskTransformChanged)
                {
                    const TArray<FNativeMaskRef>* MaskRefs = NativeMaskIndicesByOwnerNode.Find(Delta.Node);
                    if (!MaskRefs)
                    {
                        LastError = FString::Printf(TEXT("Slate replay references unknown clip-mask owner node %u."), Delta.Node);
                        return false;
                    }
                    for (const FNativeMaskRef& Ref : *MaskRefs)
                    {
                        if (!NativeDraws.IsValidIndex(Ref.DrawIndex) ||
                            !NativeDraws[Ref.DrawIndex].ClipMasks.IsValidIndex(Ref.MaskIndex))
                        {
                            LastError = TEXT("Slate replay references an invalid retained clip-mask index.");
                            return false;
                        }
                        FNativeMask& Mask = NativeDraws[Ref.DrawIndex].ClipMasks[Ref.MaskIndex];
                        Mask.bTransform = Delta.TransformEnabled != 0;
                        Mask.Transform = FMatrix2x2(Delta.TransformM00, Delta.TransformM10,
                            Delta.TransformM01, Delta.TransformM11);
                        Mask.TransformTranslation = FVector2f(Delta.TransformX, Delta.TransformY);
                    }
                }
            }
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::DrawRecordsReused, NativeDraws.Num());
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::VisualDeltaUpdates, SlateFrame.VisualDeltaCount);
        }
        else
        {
            MarkSlatePaintCachePending();
            if ((SlateFrame.DrawCount > 0 && !SlateFrame.Draws) || SlateFrame.VisualDeltaCount != 0)
            {
                LastError = TEXT("Invalid complete Slate command frame.");
                return false;
            }
            bHasSlateCommandSnapshot = false;
            NativeDraws.Reset(SlateFrame.DrawCount);
            NativeDrawIndicesByVisualNode.Reset();
            NativeMaskIndicesByOwnerNode.Reset();
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
                Draw.VisualNode = Source.VisualNode;
                Draw.VisualOpacity = FMath::Max(Source.VisualOpacity, 0.0f);
                if (Draw.VisualNode) NativeDrawIndicesByVisualNode.FindOrAdd(Draw.VisualNode).Add(NativeDraws.Num() - 1);
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
                    Mask.OwnerNode = SourceMask.OwnerNode;
                    Mask.Operation = SourceMask.Operation;
                    Mask.Translation = FVector2f(SourceMask.TranslateX, SourceMask.TranslateY);
                    Mask.bTransform = SourceMask.TransformEnabled != 0;
                    Mask.Transform = FMatrix2x2(SourceMask.TransformM00, SourceMask.TransformM10,
                        SourceMask.TransformM01, SourceMask.TransformM11);
                    Mask.TransformTranslation = FVector2f(SourceMask.TransformX, SourceMask.TransformY);
                    Mask.bScissor = SourceMask.ScissorEnabled != 0;
                    Mask.Scissor = FSlateRect(SourceMask.ScissorX, SourceMask.ScissorY,
                        SourceMask.ScissorX + SourceMask.ScissorWidth, SourceMask.ScissorY + SourceMask.ScissorHeight);
                    if (Mask.OwnerNode)
                        NativeMaskIndicesByOwnerNode.FindOrAdd(Mask.OwnerNode).Add(
                            FNativeMaskRef{static_cast<int32>(Index), Draw.ClipMasks.Num() - 1});
                }
                if (NativeMaterialResources.Contains(Draw.TextureId) && !Draw.ClipMasks.IsEmpty())
                {
                    int32 ClipPlaneCount = 0;
                    for (const FNativeMask& Mask : Draw.ClipMasks)
                    {
                        const FGeometryResource* MaskGeometry = NativeGeometries.Find(Mask.GeometryId);
                        if (Mask.Operation == RMLUE_CLIP_MASK_SET_INVERSE || !MaskGeometry || !MaskGeometry->bFilledConvex)
                        {
                            Draw.bMaterialClipSupported = false;
                            break;
                        }
                        ClipPlaneCount += MaskGeometry->ConvexHull.Num() + (Mask.bScissor ? 1 : 0);
                    }
                    if (ClipPlaneCount > 240) Draw.bMaterialClipSupported = false;
                    if (!Draw.bMaterialClipSupported) FrameUnsupportedFeatures |= RMLUE_UNSUPPORTED_CLIP_MASK;
                }
                if (const FNativeMaterialResource* Binding = NativeMaterialResources.Find(Draw.TextureId))
                {
                    const FMaterialResource* Material = Materials.Find(Binding->Alias);
                    FGeometryResource* DrawGeometry = NativeGeometries.Find(Draw.GeometryId);
                    if (Material && DrawGeometry)
                        for (int32 VertexOffset = 7; VertexOffset < DrawGeometry->Vertices.Num(); VertexOffset += 8)
                            if (DrawGeometry->Vertices[VertexOffset] < 254.5f)
                            {
                                AnalyzeMaterialOpacityGeometry(*DrawGeometry);
                                if (!CanApplyMaterialOpacityAsBoxes(Draw, *DrawGeometry, *Binding, *Material))
                                    FrameUnsupportedFeatures |= RMLUE_UNSUPPORTED_MATERIAL_BLEND_OPACITY;
                                break;
                            }
                }
            }
            bHasSlateCommandSnapshot = true;
            if (bCompareClipTopology)
            {
                TArray<uint64> CurrentClipTopology;
                CaptureSlateClipTopology(CurrentClipTopology, ClipTopologyMaskRefsAfter);
                bClipTopologyChanged = PreviousClipTopology != CurrentClipTopology;
            }
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::DrawRecordsDecoded, NativeDraws.Num());
            SlatePaintCacheRejectReason = EvaluateSlatePaintCache();
            bSlatePaintCacheEligible = SlatePaintCacheRejectReason == ERmlUiPaintCacheRejectReason::None;
            bSlatePaintResourcesWaiting = false;
            FRmlUiPerformance::RecordPaintCache(RmlUE_GetViewResourceId(NativeView), SlateFrame.Number,
                ERmlUiPaintCacheEvent::Evaluated, SlatePaintCacheRejectReason, NativeDraws.Num());
            if (!bSlatePaintCacheEligible)
            {
                bSlatePaintCacheReady = false;
                ForceVolatile(true);
                Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
            }
        }
        }
        const uint64 DecodeCycles = FPlatformTime::Cycles64() - DecodeStart;
        uint64 ClipMaskCount = 0;
        for (const FNativeDraw& Draw : NativeDraws) ClipMaskCount += Draw.ClipMasks.Num();
        FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::Draws, NativeDraws.Num());
        FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::ClipMasks, ClipMaskCount);
        if (FrameUnsupportedFeatures && FrameUnsupportedFeatures != UnsupportedSlateFeatures)
        {
            UE_LOG(LogRmlUiUnreal, Warning, TEXT("The experimental Slate renderer cannot reproduce RmlUi feature mask 0x%X. Use the DX11 compatibility renderer for visual parity."), FrameUnsupportedFeatures);
        }
        UnsupportedSlateFeatures = FrameUnsupportedFeatures;
        FrameNumber = SlateFrame.Number;
        LastScheduledWidth = Width;
        LastScheduledHeight = Height;
        LastScheduledDpRatio = PixelScale;
        bScheduledRenderRequested = false;
        CaptureSlateSchedule(FPlatformTime::Seconds());
        if (SlateFrame.Replayed && bSlatePaintCacheReady)
        {
            FRmlUiPerformance::RecordPaintCache(RmlUE_GetViewResourceId(NativeView), FrameNumber,
                ERmlUiPaintCacheEvent::Invalidated, SlatePaintCacheRejectReason, NativeDraws.Num());
        }
        Invalidate(EInvalidateWidgetReason::Paint);
        {
            FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::EventDispatch);
            DispatchEvents();
        }
        FRmlUiPerformanceFrame Metrics;
        Metrics.ViewId = RmlUE_GetViewResourceId(NativeView);
        Metrics.FrameId = FrameNumber;
        Metrics.Backend = Backend;
        Metrics.Width = Width;
        Metrics.Height = Height;
        Metrics.BridgeCycles = BridgeCycles;
        Metrics.BeforeRenderCycles = BeforeRenderCycles;
        Metrics.DecodeCycles = DecodeCycles;
        Metrics.Draws = NativeDraws.Num();
        Metrics.ClipMasks = ClipMaskCount;
        Metrics.UnsupportedFeatures = FrameUnsupportedFeatures;
        Metrics.ClipTopologyMaskRefsBefore = ClipTopologyMaskRefsBefore;
        Metrics.ClipTopologyMaskRefsAfter = ClipTopologyMaskRefsAfter;
        Metrics.bSlateReplayed = SlateFrame.Replayed != 0;
        Metrics.bClipTopologyChanged = bClipTopologyChanged;
        FRmlUiPerformance::RecordFrame(Metrics);
        return true;
    }
    RmlUE_Frame Frame{};
    const uint64 BridgeStart = FPlatformTime::Cycles64();
    bool bRendered = false;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Bridge_RenderDx11);
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::BridgeRender);
        bRendered = CheckResult(RmlUE_Render(NativeView, &Frame));
    }
    const uint64 BridgeCycles = FPlatformTime::Cycles64() - BridgeStart;
    if (!bRendered || !Frame.Pixels || Frame.Width != Width || Frame.Height != Height)
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
    uint8* UploadPixels = nullptr;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_Dx11UploadPrepare);
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::UploadPrepare);
        UploadPixels = static_cast<uint8*>(FMemory::Malloc(ByteCount));
        FMemory::Memcpy(UploadPixels, Frame.Pixels, ByteCount);
    }
    FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
    Texture->UpdateTextureRegions(0, 1, Region, Width * 4, 4, UploadPixels,
        [](uint8* Pixels, const FUpdateTextureRegion2D* Regions)
        {
            FMemory::Free(Pixels);
            delete Regions;
        });
    FrameNumber = Frame.Number;
    Invalidate(EInvalidateWidgetReason::Paint);
    {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::EventDispatch);
        DispatchEvents();
    }
    FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::FullFrameUploadBytes, ByteCount);
    FRmlUiPerformanceFrame Metrics;
    Metrics.ViewId = RmlUE_GetViewResourceId(NativeView);
    Metrics.FrameId = FrameNumber;
    Metrics.Backend = Backend;
    Metrics.Width = Width;
    Metrics.Height = Height;
    Metrics.BridgeCycles = BridgeCycles;
    Metrics.BeforeRenderCycles = BeforeRenderCycles;
    Metrics.UploadBytes = ByteCount;
    FRmlUiPerformance::RecordFrame(Metrics);
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
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_OnPaint);
    CSV_SCOPED_TIMING_STAT(RmlUi, OnPaint);
    const ERmlUiPerformanceBackend Backend = bUseSlateRenderer
        ? ERmlUiPerformanceBackend::Slate : ERmlUiPerformanceBackend::DX11;
    FScopedRmlUiPerformanceTimer PaintTimer(Backend, ERmlUiPerformanceStage::OnPaint);
    SlateRhiDrawCount = 0;
    SlateRhiMaskCount = 0;
    SlateMaterialClipDrawCount = 0;
    SlateMaterialOpacitySectionDrawCount = 0;
    SlateFallbackDrawCount = 0;
    if (bUseSlateRenderer && NativeDraws.Num() > 0)
    {
        if (SlateRhiSubmissionPaintFrame != GFrameCounter)
        {
            SlateRhiSubmissionPaintFrame = GFrameCounter;
            SlateRhiSubmissionIndex = 0;
        }
        const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
        TArray<FRmlUiSlateRhiDrawDesc> PendingRhiDraws;
        const auto FlushRhiSubmission = [&]()
        {
            if (PendingRhiDraws.IsEmpty()) return;
            if (SlateRhiSubmissionIndex < SlateRhiSubmissions.Num())
            {
                UpdateRmlUiSlateRhiSubmission(
                    SlateRhiSubmissions[SlateRhiSubmissionIndex], MoveTemp(PendingRhiDraws));
            }
            else
            {
                SlateRhiSubmissions.Add(CreateRmlUiSlateRhiSubmission(MoveTemp(PendingRhiDraws)));
            }
            FSlateDrawElement::MakeCustom(
                Elements, LayerId++, SlateRhiSubmissions[SlateRhiSubmissionIndex]);
            ++SlateRhiSubmissionIndex;
            FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::RhiSubmissions);
            PendingRhiDraws.Reset();
        };
        for (const FNativeDraw& Draw : NativeDraws)
        {
            const FGeometryResource* GeometryResource = NativeGeometries.Find(Draw.GeometryId);
            if (!GeometryResource) continue;
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
            const FVector2f CullingCorners[] = {
                FVector2f(CullingRect.Left, CullingRect.Top), FVector2f(CullingRect.Right, CullingRect.Top),
                FVector2f(CullingRect.Left, CullingRect.Bottom), FVector2f(CullingRect.Right, CullingRect.Bottom)};
            const auto PushConvexClip = [&](TConstArrayView<FVector2f> Hull)
            {
                int32 Count = 0;
                if (Hull.Num() < 3) return Count;
                FVector2f Centroid = FVector2f::ZeroVector;
                for (FVector2f Point : Hull) Centroid += Point;
                Centroid /= static_cast<float>(Hull.Num());
                for (int32 HullIndex = 0; HullIndex < Hull.Num(); ++HullIndex)
                {
                    const FVector2f Start = Hull[HullIndex];
                    const FVector2f End = Hull[(HullIndex + 1) % Hull.Num()];
                    const FVector2f Edge = End - Start;
                    const float EdgeLength = Edge.Size();
                    if (EdgeLength <= UE_SMALL_NUMBER) continue;
                    const FVector2f Tangent = Edge / EdgeLength;
                    FVector2f InwardNormal(-Tangent.Y, Tangent.X);
                    if (FVector2f::DotProduct(Centroid - (Start + End) * 0.5f, InwardNormal) < 0.0f)
                        InwardNormal *= -1.0f;
                    float MinimumTangent = 0.0f;
                    float MaximumTangent = EdgeLength;
                    float MaximumNormal = FVector2f::DotProduct(Centroid - Start, InwardNormal);
                    for (FVector2f Corner : CullingCorners)
                    {
                        MinimumTangent = FMath::Min(MinimumTangent, FVector2f::DotProduct(Corner - Start, Tangent));
                        MaximumTangent = FMath::Max(MaximumTangent, FVector2f::DotProduct(Corner - Start, Tangent));
                        MaximumNormal = FMath::Max(MaximumNormal, FVector2f::DotProduct(Corner - Start, InwardNormal));
                    }
                    constexpr float Padding = 2.0f;
                    const FVector2f NearStart = Start + Tangent * (MinimumTangent - Padding);
                    const FVector2f NearEnd = Start + Tangent * (MaximumTangent + Padding);
                    const FVector2f Extrusion = InwardNormal * (MaximumNormal + Padding);
                    Elements.PushClip(FSlateClippingZone(NearStart, NearEnd, NearStart + Extrusion, NearEnd + Extrusion));
                    ++Count;
                }
                return Count;
            };
            const FSlateBrush* Brush = nullptr;
            const FNativeMaterialResource* NativeMaterial = NativeMaterialResources.Find(Draw.TextureId);
            const FMaterialResource* ResolvedMaterial = nullptr;
            if (NativeMaterial)
            {
                if (const FMaterialResource* Material = Materials.Find(NativeMaterial->Alias))
                    if (Material->Brush.IsValid())
                    {
                        ResolvedMaterial = Material;
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
            if (!NativeMaterial && bMasksReady &&
                IsRmlUiSlateRhiGeometryReady(GeometryResource->RhiGeometry))
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
            const uint64 SlateRhiPaintStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
            if (TextureRhi.IsValid())
            {
                const FVector2f Origin = ToWindowPosition(FVector2f::ZeroVector, Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation);
                FRmlUiSlateRhiDrawDesc RhiDraw;
                RhiDraw.ViewId = NativeView ? RmlUE_GetViewResourceId(NativeView) : 0;
                RhiDraw.FrameId = FrameNumber;
                RhiDraw.Geometry = GeometryResource->RhiGeometry;
                RhiDraw.Texture = TextureRhi;
                RhiDraw.Origin = Origin;
                RhiDraw.AxisX = ToWindowPosition(FVector2f(1.0f, 0.0f), Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation) - Origin;
                RhiDraw.AxisY = ToWindowPosition(FVector2f(0.0f, 1.0f), Draw.Translation, Draw.bTransform,
                    Draw.Transform, Draw.TransformTranslation) - Origin;
                RhiDraw.ScissorRect = ResolveScissor(Draw.bScissor, Draw.Scissor);
                RhiDraw.GeometryId = Draw.GeometryId;
                RhiDraw.VisualOpacity = Draw.VisualOpacity;
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
                    PendingRhiDraws.Add(MoveTemp(RhiDraw));
                    ++SlateRhiDrawCount;
                    SlateRhiMaskCount += Draw.ClipMasks.Num();
                }
                if (SlateRhiPaintStart)
                    FRmlUiPerformance::AddCycles(Backend, ERmlUiPerformanceStage::PaintSlateRhi,
                        FPlatformTime::Cycles64() - SlateRhiPaintStart);
                continue;
            }

            FlushRhiSubmission();
            const uint64 SlateFallbackPaintStart = FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
            if (!Brush) Brush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
            const uint8 MaterialOpacity = NativeMaterial && GeometryResource->Vertices.Num() >= 8
                ? static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
                    GeometryResource->Vertices[7] * Draw.VisualOpacity), 0, 255)) : 255;
            const bool bUseMaterialOpacityBoxes = NativeMaterial && ResolvedMaterial && MaterialOpacity < 255 &&
                CanApplyMaterialOpacityAsBoxes(Draw, *GeometryResource, *NativeMaterial, *ResolvedMaterial);
            int32 PushedClipCount = 0;
            if (Draw.bScissor)
            {
                Elements.PushClip(FSlateClippingZone(ResolveScissor(true, Draw.Scissor)));
                ++PushedClipCount;
            }
            if (NativeMaterial && Draw.bMaterialClipSupported && !Draw.ClipMasks.IsEmpty())
            {
                for (const FNativeMask& Mask : Draw.ClipMasks)
                {
                    if (Mask.bScissor)
                    {
                        Elements.PushClip(FSlateClippingZone(ResolveScissor(true, Mask.Scissor)));
                        ++PushedClipCount;
                    }
                    const FGeometryResource* MaskGeometry = NativeGeometries.Find(Mask.GeometryId);
                    if (!MaskGeometry) continue;
                    TArray<FVector2f, TInlineAllocator<32>> Hull;
                    Hull.Reserve(MaskGeometry->ConvexHull.Num());
                    for (FVector2f Point : MaskGeometry->ConvexHull)
                    {
                        Point = ToWindowPosition(Point, Mask.Translation, Mask.bTransform,
                            Mask.Transform, Mask.TransformTranslation);
                        Hull.Add(Point);
                    }
                    PushedClipCount += PushConvexClip(Hull);
                }
            }
            if (bUseMaterialOpacityBoxes)
            {
                const FVector2f Minimum = GeometryResource->MaterialBoundsMinimum;
                const FVector2f Maximum = GeometryResource->MaterialBoundsMaximum;
                const FVector2f Size = (Maximum - Minimum) / PixelScale;
                FPaintGeometry MaterialGeometry = Geometry.ToPaintGeometry(
                    Size, FSlateLayoutTransform((Minimum + Draw.Translation) / PixelScale));
                if (Draw.bTransform)
                {
                    const FVector2f TransformedOrigin =
                        (Draw.Transform.TransformPoint(Minimum + Draw.Translation) + Draw.TransformTranslation) / PixelScale;
                    MaterialGeometry = Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(),
                        FSlateRenderTransform(Draw.Transform, TransformedOrigin), FVector2f::ZeroVector);
                }
                const float Opacity = MaterialOpacity / 255.0f;
                const FLinearColor Tint = ResolvedMaterial->bUsePremultipliedVertexColor
                    ? FLinearColor(Opacity, Opacity, Opacity, Opacity)
                    : FLinearColor(1.0f, 1.0f, 1.0f, Opacity);
                if (GeometryResource->bMaterialOpacityFullBox)
                {
                    FSlateDrawElement::MakeBox(Elements, LayerId, MaterialGeometry, Brush, ESlateDrawEffect::None, Tint);
                    ++SlateMaterialOpacitySectionDrawCount;
                }
                else
                {
                    for (const FMaterialOpacitySection& Section : GeometryResource->MaterialOpacitySections)
                    {
                        TArray<FVector2f, TInlineAllocator<16>> WindowHull;
                        WindowHull.Reserve(Section.ConvexHull.Num());
                        for (FVector2f Point : Section.ConvexHull)
                            WindowHull.Add(ToWindowPosition(Point, Draw.Translation, Draw.bTransform,
                                Draw.Transform, Draw.TransformTranslation));
                        const int32 SectionClipCount = PushConvexClip(WindowHull);
                        if (SectionClipCount > 0)
                        {
                            FSlateDrawElement::MakeBox(Elements, LayerId, MaterialGeometry, Brush,
                                ESlateDrawEffect::None, Tint);
                            ++SlateMaterialOpacitySectionDrawCount;
                        }
                        for (int32 Index = 0; Index < SectionClipCount; ++Index) Elements.PopClip();
                    }
                }
                ++LayerId;
            }
            else
            {
                TArray<FSlateVertex> Vertices;
                Vertices.Reserve(GeometryResource->Vertices.Num() / 8);
                for (int32 Offset = 0; Offset < GeometryResource->Vertices.Num(); Offset += 8)
                {
                    FVector2f Position = FVector2f(GeometryResource->Vertices[Offset],
                        GeometryResource->Vertices[Offset + 1]) + Draw.Translation;
                    if (Draw.bTransform)
                        Position = Draw.Transform.TransformPoint(Position) + Draw.TransformTranslation;
                    Position /= PixelScale;
                    FColor Color(static_cast<uint8>(GeometryResource->Vertices[Offset + 4]),
                        static_cast<uint8>(GeometryResource->Vertices[Offset + 5]),
                        static_cast<uint8>(GeometryResource->Vertices[Offset + 6]),
                        static_cast<uint8>(GeometryResource->Vertices[Offset + 7]));
                    if (ResolvedMaterial && !ResolvedMaterial->bUsePremultipliedVertexColor)
                    {
                        Color = UnpremultiplyMaterialVertexColor(Color);
                        Color.A = static_cast<uint8>(FMath::Clamp(
                            FMath::RoundToInt(static_cast<float>(Color.A) * Draw.VisualOpacity), 0, 255));
                    }
                    else
                    {
                        Color = ApplyPremultipliedOpacity(Color, Draw.VisualOpacity);
                    }
                    Vertices.Add(FSlateVertex::Make(Transform, Position,
                        FVector2f(GeometryResource->Vertices[Offset + 2],
                            GeometryResource->Vertices[Offset + 3]), Color));
                }
                TArray<SlateIndex> Indices;
                Indices.Reserve(GeometryResource->Indices.Num());
                for (uint32 Index : GeometryResource->Indices) Indices.Add(static_cast<SlateIndex>(Index));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
                FSlateDrawElement::MakeCustomVerts(Elements, LayerId++,
                    FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush), Vertices, Indices,
                    nullptr, 0, 0, ESlateDrawEffect::None, ESlateBatchDrawFlag::PreMultipliedAlpha);
#else
                FSlateDrawElement::MakeCustomVerts(Elements, LayerId++,
                    FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush), Vertices, Indices,
                    nullptr, 0, 0, ESlateDrawEffect::None);
#endif
            }
            ++SlateFallbackDrawCount;
            if (NativeMaterial && Draw.bMaterialClipSupported && !Draw.ClipMasks.IsEmpty()) ++SlateMaterialClipDrawCount;
            while (PushedClipCount > 0)
            {
                Elements.PopClip();
                --PushedClipCount;
            }
            if (SlateFallbackPaintStart)
                FRmlUiPerformance::AddCycles(Backend,
                    NativeMaterial ? ERmlUiPerformanceStage::PaintMaterial : ERmlUiPerformanceStage::PaintFallback,
                    FPlatformTime::Cycles64() - SlateFallbackPaintStart);
        }
        FlushRhiSubmission();
        for (int32 Index = SlateRhiSubmissionIndex; Index < SlateRhiSubmissions.Num(); ++Index)
            ResetRmlUiSlateRhiSubmission(SlateRhiSubmissions[Index]);
    }
    if (TextureBrush.IsValid())
    {
        FScopedRmlUiPerformanceTimer Timer(Backend, ERmlUiPerformanceStage::PaintFallback);
        FSlateDrawElement::MakeBox(Elements, LayerId, Geometry.ToPaintGeometry(), TextureBrush->GetSlateBrush(),
            ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect,
            Style.GetColorAndOpacityTint());
    }
    FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::MaterialDraws, SlateMaterialClipDrawCount);
    FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::MaterialOpacitySections, SlateMaterialOpacitySectionDrawCount);
    FRmlUiPerformance::AddWork(Backend, ERmlUiPerformanceWork::FallbackDraws, SlateFallbackDrawCount);
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
    RequestScheduledRender();
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
    RequestScheduledRender();
    return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse).CaptureMouse(SharedThis(this));
}

FReply SRmlUiWidget::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
    const int Button = GetMouseButton(Event.GetEffectingButton());
    if (!NativeView || Button < 0) return FReply::Unhandled();
    UpdateMousePosition(Geometry, Event);
    RmlUE_MouseButton(NativeView, Button, 0, GetModifiers(Event));
    RequestScheduledRender();
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
    RequestScheduledRender();
    return FReply::Handled();
}

void SRmlUiWidget::OnMouseLeave(const FPointerEvent& Event)
{
    SLeafWidget::OnMouseLeave(Event);
    if (NativeView && !HasMouseCapture())
    {
        RmlUE_MouseLeave(NativeView);
        RequestScheduledRender();
    }
}

void SRmlUiWidget::OnMouseCaptureLost(const FCaptureLostEvent& Event)
{
    SLeafWidget::OnMouseCaptureLost(Event);
    if (NativeView)
    {
        if (const FVector2D* TouchPosition = ActiveTouches.Find(Event.PointerIndex))
        {
            RmlUE_Touch(NativeView, Event.PointerIndex, TouchPosition->X, TouchPosition->Y, 3);
            RequestScheduledRender();
            ActiveTouches.Remove(Event.PointerIndex);
        }
        else
        {
            for (int Button : PressedMouseButtons) RmlUE_MouseButton(NativeView, Button, 0, 0);
            PressedMouseButtons.Empty();
            RequestScheduledRender();
        }
    }
}

FReply SRmlUiWidget::OnKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    if (TextInputContext && TextInputContext->ConsumeCompositionKey(Event.GetKey())) return FReply::Handled();
    const int Key = GetVirtualKey(Event);
    if (!NativeView || !Key) return FReply::Unhandled();
    RmlUE_Key(NativeView, Key, 1, GetModifiers(Event));
    RequestScheduledRender();
    return FReply::Handled();
}

FReply SRmlUiWidget::OnKeyUp(const FGeometry&, const FKeyEvent& Event)
{
    if (TextInputContext && TextInputContext->ConsumeCompositionKey(Event.GetKey())) return FReply::Handled();
    const int Key = GetVirtualKey(Event);
    if (!NativeView || !Key) return FReply::Unhandled();
    RmlUE_Key(NativeView, Key, 0, GetModifiers(Event));
    RequestScheduledRender();
    return FReply::Handled();
}

FReply SRmlUiWidget::OnKeyChar(const FGeometry&, const FCharacterEvent& Event)
{
    if (!NativeView) return FReply::Unhandled();
    const TCHAR Character = Event.GetCharacter();
    if (TextInputContext && TextInputContext->ConsumeCompositionCharacter(Character))
    {
        PendingHighSurrogate = 0;
        return FReply::Handled();
    }
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
        RequestScheduledRender();
    }
    return FReply::Handled();
}

FReply SRmlUiWidget::OnFocusReceived(const FGeometry& Geometry, const FFocusEvent& Event)
{
    if (TextInputContext) TextInputContext->Update(Geometry, PixelScale);
    return FReply::Handled();
}

void SRmlUiWidget::OnFocusLost(const FFocusEvent& Event)
{
    SLeafWidget::OnFocusLost(Event);
    if (TextInputContext) TextInputContext->Deactivate(true);
    PendingHighSurrogate = 0;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    if (NativeView)
    {
        RmlUE_FocusLost(NativeView);
        RequestScheduledRender();
    }
}

FReply SRmlUiWidget::ForwardTouch(const FGeometry& Geometry, const FPointerEvent& Event, int32 Phase)
{
    if (!NativeView) return FReply::Unhandled();
    const FVector2D Position = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()) * PixelScale;
    if (Phase == 2) ActiveTouches.Remove(Event.GetPointerIndex());
    else ActiveTouches.Add(Event.GetPointerIndex(), Position);
    RmlUE_Touch(NativeView, Event.GetPointerIndex(), Position.X, Position.Y, Phase);
    RequestScheduledRender();
    if (Phase == 0) return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse).CaptureMouse(SharedThis(this));
    return Phase == 2 ? FReply::Handled().ReleaseMouseCapture() : FReply::Handled();
}

FReply SRmlUiWidget::OnTouchStarted(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 0); }
FReply SRmlUiWidget::OnTouchMoved(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 1); }
FReply SRmlUiWidget::OnTouchEnded(const FGeometry& Geometry, const FPointerEvent& Event) { return ForwardTouch(Geometry, Event, 2); }
