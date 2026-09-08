#include "SRmlUiWidget.h"

#include "Engine/Texture2D.h"
#include "RHITypes.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "RmlUiBridge.h"
#include "RmlUiUnrealModule.h"
#include "Slate/DeferredCleanupSlateBrush.h"
#include "Styling/CoreStyle.h"

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
    OnDocumentEvent = InArgs._OnDocumentEvent;
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

void SRmlUiWidget::ShutdownNative()
{
    if (NativeView)
    {
        RmlUE_DestroyView(NativeView);
        NativeView = nullptr;
    }
    bNativeShutdown = true;
    ActiveTouches.Empty();
    PressedMouseButtons.Empty();
    Texture = nullptr;
    TextureBrush.Reset();
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
    NativeView = RmlUE_CreateView(FMath::Clamp(FMath::RoundToInt(DesiredSize.X), 1, MaxTextureDimension),
        FMath::Clamp(FMath::RoundToInt(DesiredSize.Y), 1, MaxTextureDimension), 1.0f);
    return CheckResult(NativeView != nullptr);
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
        TextureBrush = FDeferredCleanupSlateBrush::CreateBrush(NewTexture);
        Texture = NewTexture;
        TextureSize = FIntPoint(Width, Height);
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

int32 SRmlUiWidget::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
    FSlateWindowElementList& Elements, int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const
{
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
