#include "RmlUiTextInput.h"

#include "SRmlUiWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Widgets/SWindow.h"

namespace
{
// Windows TSF ACP is UTF-16. The RmlUi ABI uses Unicode scalars.
int32 ToScalar(const FString& Text, uint32 Offset, bool RoundUp = false)
{
    const int32 Limit = FMath::Min<uint32>(Offset, Text.Len());
    int32 Index = 0, Scalar = 0;
    while (Index < Limit)
    {
        const bool Pair = Text[Index] >= 0xd800 && Text[Index] <= 0xdbff && Index + 1 < Text.Len() &&
            Text[Index + 1] >= 0xdc00 && Text[Index + 1] <= 0xdfff;
        if (Pair && Index + 1 == Limit && !RoundUp) break;
        Index += Pair ? 2 : 1;
        ++Scalar;
    }
    return Scalar;
}
uint32 ToAcp(const FString& Text, int32 Scalar)
{
    int32 Index = 0;
    for (int32 Count = 0; Count < Scalar && Index < Text.Len(); ++Count)
    {
        const bool Pair = Text[Index] >= 0xd800 && Text[Index] <= 0xdbff && Index + 1 < Text.Len() &&
            Text[Index + 1] >= 0xdc00 && Text[Index + 1] <= 0xdfff;
        Index += Pair ? 2 : 1;
    }
    return Index;
}
uint32 RangeEnd(uint32 Begin, uint32 Length, int32 Total)
{
    return static_cast<uint32>(FMath::Min<uint64>(static_cast<uint64>(Begin) + Length, Total));
}
}

FRmlUiTextInputMethodContext::FRmlUiTextInputMethodContext(TWeakPtr<SRmlUiWidget> InOwner) : Owner(InOwner) {}

void FRmlUiTextInputMethodContext::Initialize()
{
    if (bRegistered || !FSlateApplication::IsInitialized()) return;
    System = FSlateApplication::Get().GetTextInputMethodSystem();
    if (!System) return;
    Notifier = System->RegisterContext(SharedThis(this));
    bRegistered = Notifier.IsValid();
    if (Notifier) Notifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Created);
}

void FRmlUiTextInputMethodContext::Shutdown()
{
    Deactivate(true);
    if (bRegistered && System && FSlateApplication::IsInitialized())
    {
        if (Notifier) Notifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Destroyed);
        System->UnregisterContext(SharedThis(this));
    }
    Notifier.Reset();
    System = nullptr;
    bRegistered = false;
}

RmlUE_View* FRmlUiTextInputMethodContext::View() const
{
    const auto Widget = Owner.Pin();
    return Widget ? Widget->GetNativeView() : nullptr;
}

FString FRmlUiTextInputMethodContext::ReadText() const
{
    RmlUE_View* Native = View();
    if (!Native) return FString();
    const int32 Capacity = RmlUE_TextInputGetText(Native, nullptr, 0);
    if (Capacity <= 0) return FString();
    TArray<ANSICHAR> Buffer;
    Buffer.SetNumZeroed(Capacity);
    if (!RmlUE_TextInputGetText(Native, Buffer.GetData(), Buffer.Num())) return FString();
    return UTF8_TO_TCHAR(Buffer.GetData());
}

void FRmlUiTextInputMethodContext::RefreshSnapshot()
{
    CachedState = {};
    if (RmlUE_View* Native = View()) RmlUE_TextInputGetState(Native, &CachedState);
    CachedText = ReadText();
}

void FRmlUiTextInputMethodContext::Update(const FGeometry& Geometry, float PixelScale)
{
    bSuppressCommitCharacter = false;
    CachedGeometry = Geometry;
    CachedPixelScale = FMath::Max(PixelScale, 0.01f);
    const auto Widget = Owner.Pin();
    if (Widget && FSlateApplication::IsInitialized()) Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
    const auto Previous = CachedState;
    const FString PreviousText = CachedText;
    RefreshSnapshot();
    const bool ShouldActivate = Widget && Widget->HasKeyboardFocus() && CachedState.Active && !CachedState.ReadOnly;
    if (bActive && (!ShouldActivate || PlatformNode != CachedState.Node))
    {
        // Native focus changes have already cancelled the old composition in OnDeactivate.
        TGuardValue<bool> Guard(bSwitchingPlatformFocus, true);
        if (System && bRegistered) System->DeactivateContext(SharedThis(this));
        bActive = false;
        PlatformNode = 0;
    }
    if (ShouldActivate && !bActive && System && bRegistered)
    {
        bActive = true;
        PlatformNode = CachedState.Node;
        System->ActivateContext(SharedThis(this));
    }
    if (bActive && Notifier)
    {
        if (PreviousText != CachedText) Notifier->NotifyTextChanged(0, PreviousText.Len(), CachedText.Len());
        if (Previous.Node != CachedState.Node || Previous.SelectionStart != CachedState.SelectionStart ||
            Previous.SelectionEnd != CachedState.SelectionEnd || Previous.Caret != CachedState.Caret)
            Notifier->NotifySelectionChanged();
        Notifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Changed);
    }
}

void FRmlUiTextInputMethodContext::Deactivate(bool Cancel)
{
    if (Cancel) CancelComposition();
    if (bActive && System && bRegistered && FSlateApplication::IsInitialized()) System->DeactivateContext(SharedThis(this));
    bActive = false;
    PlatformNode = 0;
    CachedState = {};
    CachedText.Reset();
}

void FRmlUiTextInputMethodContext::CancelComposition()
{
    if (bCancelling) return;
    TGuardValue<bool> Guard(bCancelling, true);
    if (RmlUE_View* Native = View()) RmlUE_TextInputEndComposition(Native, 1);
    // Restore native state before asking TSF to finish; reentrant EndComposition is then a no-op.
    if (bActive && Notifier && FSlateApplication::IsInitialized()) Notifier->CancelComposition();
    RefreshSnapshot();
    bSuppressCommitCharacter = true;
}

bool FRmlUiTextInputMethodContext::ConsumeCompositionKey(const FKey& Key)
{
    if (IsComposing() && (Key == EKeys::Enter || Key == EKeys::Escape))
    {
        bSuppressCommitCharacter = true;
        if (Key == EKeys::Escape) CancelComposition();
        return true;
    }
    return bSuppressCommitCharacter && Key == EKeys::Enter;
}

bool FRmlUiTextInputMethodContext::ConsumeCompositionCharacter(TCHAR Character)
{
    if (IsComposing()) return true;
    if (bSuppressCommitCharacter && (Character == '\r' || Character == '\n'))
    {
        bSuppressCommitCharacter = false;
        return true;
    }
    return false;
}

bool FRmlUiTextInputMethodContext::IsComposing() { RmlUE_View* Native = View(); return Native && RmlUE_TextInputIsComposing(Native); }
bool FRmlUiTextInputMethodContext::IsReadOnly() { RmlUE_TextInputState State{}; RmlUE_View* Native = View(); return !Native || !RmlUE_TextInputGetState(Native, &State) || State.ReadOnly; }
uint32 FRmlUiTextInputMethodContext::GetTextLength() { return ReadText().Len(); }

void FRmlUiTextInputMethodContext::GetSelectionRange(uint32& Begin, uint32& Length, ECaretPosition& Caret)
{
    RmlUE_TextInputState State{};
    if (RmlUE_View* Native = View()) RmlUE_TextInputGetState(Native, &State);
    const FString Text = ReadText();
    Begin = ToAcp(Text, State.SelectionStart);
    Length = ToAcp(Text, State.SelectionEnd) - Begin;
    Caret = State.Caret == State.SelectionStart ? ECaretPosition::Beginning : ECaretPosition::Ending;
}

void FRmlUiTextInputMethodContext::SetSelectionRange(uint32 Begin, uint32 Length, ECaretPosition Caret)
{
    if (bSwitchingPlatformFocus) return;
    if (RmlUE_View* Native = View())
    {
        const FString Text = ReadText();
        const int32 Start = ToScalar(Text, Begin);
        const int32 End = ToScalar(Text, RangeEnd(Begin, Length, Text.Len()), true);
        RmlUE_TextInputSetSelection(Native, Caret == ECaretPosition::Beginning ? End : Start,
            Caret == ECaretPosition::Beginning ? Start : End);
        RefreshSnapshot();
    }
}

void FRmlUiTextInputMethodContext::GetTextInRange(uint32 Begin, uint32 Length, FString& Text)
{
    const FString All = ReadText();
    const uint32 Start = ToAcp(All, ToScalar(All, Begin));
    const uint32 End = ToAcp(All, ToScalar(All, RangeEnd(Begin, Length, All.Len()), true));
    Text = All.Mid(Start, End - Start);
}

void FRmlUiTextInputMethodContext::SetTextInRange(uint32 Begin, uint32 Length, const FString& Text)
{
    if (bSwitchingPlatformFocus || IsReadOnly()) return;
    if (RmlUE_View* Native = View())
    {
        const FString Previous = ReadText();
        RmlUE_TextInputReplace(Native, ToScalar(Previous, Begin),
            ToScalar(Previous, RangeEnd(Begin, Length, Previous.Len()), true), TCHAR_TO_UTF8(*Text));
        RefreshSnapshot();
    }
}

int32 FRmlUiTextInputMethodContext::GetCharacterIndexFromPoint(const FVector2D& Point)
{
    RmlUE_View* Native = View();
    if (!Native) return INDEX_NONE;
    const FVector2D Local = CachedGeometry.AbsoluteToLocal(Point) * CachedPixelScale;
    const int32 Index = RmlUE_TextInputHitTest(Native, Local.X, Local.Y);
    return Index < 0 ? INDEX_NONE : static_cast<int32>(ToAcp(ReadText(), Index));
}

void FRmlUiTextInputMethodContext::ConvertBounds(const RmlUE_Rect& Rect, FVector2D& Position, FVector2D& Size) const
{
    Position = CachedGeometry.LocalToAbsolute(FVector2D(Rect.X, Rect.Y) / CachedPixelScale);
    const FVector2D End = CachedGeometry.LocalToAbsolute(FVector2D(Rect.X + Rect.Width, Rect.Y + Rect.Height) / CachedPixelScale);
    Size = End - Position;
}

bool FRmlUiTextInputMethodContext::GetTextBounds(uint32 Begin, uint32 Length, FVector2D& Position, FVector2D& Size)
{
    Position = Size = FVector2D::ZeroVector;
    RmlUE_View* Native = View();
    if (!Native) return true;
    const FString Text = ReadText();
    RmlUE_Rect Rect{};
    int Clipped = 1;
    if (!RmlUE_TextInputGetBounds(Native, ToScalar(Text, Begin), ToScalar(Text, RangeEnd(Begin, Length, Text.Len()), true), &Rect, &Clipped)) return true;
    ConvertBounds(Rect, Position, Size);
    return Clipped != 0;
}

void FRmlUiTextInputMethodContext::GetScreenBounds(FVector2D& Position, FVector2D& Size)
{
    Position = Size = FVector2D::ZeroVector;
    RmlUE_Rect Rect{};
    if (RmlUE_View* Native = View(); Native && RmlUE_TextInputGetScreenBounds(Native, &Rect)) ConvertBounds(Rect, Position, Size);
}

TSharedPtr<FGenericWindow> FRmlUiTextInputMethodContext::GetWindow() { const auto Pinned = Window.Pin(); return Pinned ? Pinned->GetNativeWindow() : nullptr; }
void FRmlUiTextInputMethodContext::BeginComposition() { if (bSwitchingPlatformFocus) return; if (RmlUE_View* Native = View()) RmlUE_TextInputBeginComposition(Native); RefreshSnapshot(); }
void FRmlUiTextInputMethodContext::UpdateCompositionRange(int32 Begin, uint32 Length)
{
    if (bSwitchingPlatformFocus) return;
    if (RmlUE_View* Native = View())
    {
        const FString Text = ReadText();
        const uint32 Start = FMath::Max(Begin, 0);
        RmlUE_TextInputUpdateComposition(Native, ToScalar(Text, Start), ToScalar(Text, RangeEnd(Start, Length, Text.Len()), true));
    }
}
void FRmlUiTextInputMethodContext::EndComposition()
{
    if (bCancelling || bSwitchingPlatformFocus) return;
    if (RmlUE_View* Native = View()) RmlUE_TextInputEndComposition(Native, 0);
    RefreshSnapshot();
    bSuppressCommitCharacter = true;
}
