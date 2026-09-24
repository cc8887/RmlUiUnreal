#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "SRmlUiWidget.h"
#include "RmlUiTextInputBridge.h"
#include "RmlUiUnrealModule.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Widgets/SWindow.h"

namespace
{
class FRmlUiImeScenario final : public IAutomationLatentCommand
{
public:
    explicit FRmlUiImeScenario(FAutomationTestBase* InTest) : Test(InTest) {}
    static int Record(void* User, uint32, const RmlUE_NodeEvent* Event)
    {
        auto* Self = static_cast<FRmlUiImeScenario*>(User);
        if (FCStringAnsi::Strcmp(Event->Type, "compositionstart") == 0) Self->bStarted |= Event->IsComposing != 0;
        if (FCStringAnsi::Strcmp(Event->Type, "compositionupdate") == 0) Self->bUpdated |= Event->IsComposing != 0;
        if (FCStringAnsi::Strcmp(Event->Type, "compositionend") == 0) Self->bEnded |= Event->IsComposing == 0;
        if (FCStringAnsi::Strcmp(Event->Type, "input") == 0) Self->bComposingInput |= Event->IsComposing != 0;
        return 0;
    }
    FString Text() const
    {
        FString Result;
        Ime->GetTextInRange(0, Ime->GetTextLength(), Result);
        return Result;
    }
    void Focus(const char* Id)
    {
        RmlUE_FocusNode(Widget->GetNativeView(), RmlUE_FindNode(Widget->GetNativeView(), Id));
        FSlateApplication::Get().SetKeyboardFocus(Widget.ToSharedRef(), EFocusCause::SetDirectly);
    }
    bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Now < Next) return false;
        if (Stage == 0)
        {
            if (!Test->TestTrue(TEXT("Native module initialized"), FRmlUiUnrealModule::Get().IsInitialized())) return true;
            const FString Font = FPaths::EngineContentDir() / TEXT("Slate/Fonts/DroidSansFallback.ttf");
            Test->TestTrue(TEXT("CJK fallback font loaded"), RmlUE_LoadFont(TCHAR_TO_UTF8(*Font), 1) != 0);
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(false).DesiredSize(FVector2D(640, 280)).SourcePath(TEXT("/rmlui-tests/windows-ime-context.rml")).InlineDocument(TEXT(
                "<rml><head><style>body{font-family:LatoLatin;font-size:22px;background-color:#132029;}"
                "input,textarea{display:block;width:520px;height:34px;margin:12px;color:#ffffff;"
                "background-color:#293d4a;border:1px #738d9c;caret-color:#ff0000;}textarea{height:72px;}"
                "</style></head><body><div id='container'><input id='edit' value='A&#x1f600;B'/>"
                "<input id='limited' value='ab' maxlength='3'/><textarea id='multi'>line1\n第二行</textarea>"
                "<input id='readonly' value='locked' readonly='readonly'/></div></body></rml>"));
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi native Windows IME")))
                .ClientSize(FVector2D(640, 280)).UseOSWindowBorder(false).CreateTitleBar(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)
                .AdjustInitialSizeAndPositionForDPIScale(false)[Widget.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Ime = Widget->GetTextInputMethodContext();
            if (!Test->TestTrue(TEXT("Actual ITextInputMethodContext is available"), Ime.IsValid())) return Finish();
        }
        else if (Stage == 1)
        {
            Focus("edit");
            RmlUE_View* View = Widget->GetNativeView();
            const auto Node = RmlUE_FindNode(View, "edit");
            RmlUE_SetNodeEventCallback(View, &Record, this);
            uint32 Id = 100;
            for (const char* Type : {"compositionstart", "compositionupdate", "compositionend", "input"})
                RmlUE_ListenNode(View, Node, Type, Id++, 0);
            Test->TestEqual(TEXT("UTF-16 ACP counts the surrogate pair"), Ime->GetTextLength(), 4u);
            Ime->SetSelectionRange(1, 2, ITextInputMethodContext::ECaretPosition::Beginning);
            uint32 Begin = 0, Length = 0;
            auto Caret = ITextInputMethodContext::ECaretPosition::Ending;
            Ime->GetSelectionRange(Begin, Length, Caret);
            Test->TestTrue(TEXT("UTF-16 selection and reverse caret survive native scalar conversion"), Begin == 1 && Length == 2 && Caret == ITextInputMethodContext::ECaretPosition::Beginning);
            Ime->BeginComposition();
            Ime->SetTextInRange(1, 2, TEXT("ni"));
            Ime->UpdateCompositionRange(1, 2);
            Test->TestEqual(TEXT("Preedit replaces the selected surrogate pair"), Text(), FString(TEXT("AniB")));
            Test->TestTrue(TEXT("Native preedit is composing"), Ime->IsComposing());
            Widget->OnKeyDown(Widget->GetCachedGeometry(), FKeyEvent(EKeys::Enter, FModifierKeysState(), 0, false, 13, 13));
            Widget->OnKeyChar(Widget->GetCachedGeometry(), FCharacterEvent('\r', FModifierKeysState(), 0, false));
            Test->TestEqual(TEXT("Enter confirming IME does not insert newline or submit"), Text(), FString(TEXT("AniB")));
        }
        else if (Stage == 2)
        {
            if (auto* System = FSlateApplication::Get().GetTextInputMethodSystem())
                Test->TestTrue(TEXT("Windows text input system activated this context"), System->IsActiveContext(Ime.ToSharedRef()));
            FVector2D Position, Size;
            Test->TestFalse(TEXT("Candidate caret is not clipped"), Ime->GetTextBounds(3, 0, Position, Size));
            Test->TestTrue(TEXT("Candidate window has real nonzero caret bounds"), Size.X > 0 && Size.Y > 8);
            Test->TestEqual(TEXT("Screen point maps back to caret ACP"), Ime->GetCharacterIndexFromPoint(Position + FVector2D(0, Size.Y * 0.5)), 3);
            RmlUE_Rect NativeBounds{};
            int Clipped = 0;
            Test->TestTrue(TEXT("Native caret geometry available"), RmlUE_TextInputGetBounds(Widget->GetNativeView(), 3, 3, &NativeBounds, &Clipped) != 0);
            RmlUE_Frame Frame{};
            Test->TestTrue(TEXT("Render actual composing text and caret"), RmlUE_Render(Widget->GetNativeView(), &Frame) != 0);
            int RedPixels = 0;
            if (Frame.Pixels)
            {
                const FColor* Pixels = reinterpret_cast<const FColor*>(Frame.Pixels);
                for (int Y = FMath::Max(0, FMath::FloorToInt(NativeBounds.Y) - 1); Y < FMath::Min(Frame.Height, FMath::CeilToInt(NativeBounds.Y + NativeBounds.Height) + 1); ++Y)
                    for (int X = FMath::Max(0, FMath::FloorToInt(NativeBounds.X) - 1); X < FMath::Min(Frame.Width, FMath::CeilToInt(NativeBounds.X + NativeBounds.Width) + 1); ++X)
                        if (const FColor P = Pixels[Y * Frame.Width + X]; P.R > 200 && P.G < 70 && P.B < 70) ++RedPixels;
            }
            Test->TestTrue(TEXT("Candidate rectangle encloses rendered red caret pixels"), RedPixels > 4);
            Ime->SetTextInRange(1, 2, TEXT("你好"));
            Ime->UpdateCompositionRange(1, 2);
            Ime->EndComposition();
            Test->TestEqual(TEXT("CJK candidate commit preserves suffix"), Text(), FString(TEXT("A你好B")));
            Test->TestFalse(TEXT("Commit leaves composing state"), Ime->IsComposing());
            Test->TestTrue(TEXT("Composition events and input carry isComposing"), bStarted && bUpdated && bEnded && bComposingInput);
        }
        else if (Stage == 3)
        {
            Ime->SetSelectionRange(1, 2, ITextInputMethodContext::ECaretPosition::Ending);
            Ime->BeginComposition();
            Ime->SetTextInRange(1, 2, TEXT("临时"));
            Widget->CancelTextComposition();
            Test->TestEqual(TEXT("Cancel restores committed text"), Text(), FString(TEXT("A你好B")));
            uint32 Begin = 0, Length = 0;
            auto Caret = ITextInputMethodContext::ECaretPosition::Beginning;
            Ime->GetSelectionRange(Begin, Length, Caret);
            Test->TestTrue(TEXT("Cancel restores original selection"), Begin == 1 && Length == 2);
            Focus("limited");
            Ime->SetSelectionRange(1, 1, ITextInputMethodContext::ECaretPosition::Ending);
            Ime->BeginComposition();
            Ime->SetTextInRange(1, 1, FString(TEXT("中")) + FString::Chr(0xd83d) + FString::Chr(0xde00) + TEXT("文"));
            Ime->UpdateCompositionRange(1, 4);
            Ime->EndComposition();
            Test->TestEqual(TEXT("Commit maxlength counts scalars and preserves complete surrogate pair"), Text(), FString(TEXT("a中")) + FString::Chr(0xd83d) + FString::Chr(0xde00));
            Focus("multi");
        }
        else if (Stage == 4)
        {
            FVector2D First, FirstSize, Second, SecondSize;
            Ime->GetTextBounds(0, 0, First, FirstSize);
            Ime->GetTextBounds(6, 0, Second, SecondSize);
            Test->TestTrue(TEXT("Multiline candidate geometry follows actual line layout"), Second.Y > First.Y + FirstSize.Y * 0.5);
            TArray<FColor> Pixels;
            FIntVector Dimensions = FIntVector::ZeroValue;
            FlushRenderingCommands();
            const bool Captured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Dimensions);
            Test->TestTrue(TEXT("UE Slate IME screenshot captured"), Captured && !Pixels.IsEmpty());
            if (Captured && !Pixels.IsEmpty())
            {
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(Dimensions.X, Dimensions.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                const FString Directory = FPaths::ProjectSavedDir() / TEXT("RmlUiTests");
                IFileManager::Get().MakeDirectory(*Directory, true);
                Test->TestTrue(TEXT("IME screenshot saved"), FFileHelper::SaveArrayToFile(Png, *(Directory / TEXT("windows-ime-context.png"))));
            }
            Focus("readonly");
            Test->TestTrue(TEXT("Read-only input reports read-only to TSF"), Ime->IsReadOnly());
            Ime->SetTextInRange(0, 6, TEXT("changed"));
            Test->TestEqual(TEXT("TSF cannot edit read-only text"), Text(), FString(TEXT("locked")));
            Focus("edit");
            Ime->SetSelectionRange(0, 1, ITextInputMethodContext::ECaretPosition::Ending);
            Ime->BeginComposition();
            Ime->SetTextInRange(0, 1, TEXT("未提交"));
            Widget->OnFocusLost(FFocusEvent(EFocusCause::Cleared, 0));
            Test->TestFalse(TEXT("Losing Slate focus cancels composition"), Ime->IsComposing());
            Focus("edit");
            Test->TestEqual(TEXT("Losing focus restored text"), Text(), FString(TEXT("A你好B")));
        }
        else if (Stage == 5)
        {
            Ime->BeginComposition();
            Ime->SetTextInRange(0, 1, TEXT("销毁"));
            Widget->SetElementInnerRml(TEXT("container"), TEXT(""));
            Test->TestFalse(TEXT("Destroying focused input invalidates native context"), Ime->IsComposing());
            Test->TestEqual(TEXT("Stale platform context reads an empty safe snapshot"), Ime->GetTextLength(), 0u);
            Widget->ShutdownNative();
            Test->TestTrue(TEXT("Shutdown context is read-only"), Ime->IsReadOnly());
            Ime->EndComposition();
            return Finish();
        }
        ++Stage;
        Next = Now + (Stage == 1 ? 0.5 : 0.15);
        return false;
    }
private:
    bool Finish()
    {
        if (Widget && Widget->GetNativeView()) RmlUE_SetNodeEventCallback(Widget->GetNativeView(), nullptr, nullptr);
        if (Widget) Widget->ShutdownNative();
        if (Window) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        return true;
    }
    FAutomationTestBase* Test;
    TSharedPtr<SRmlUiWidget> Widget;
    TSharedPtr<SWindow> Window;
    TSharedPtr<ITextInputMethodContext> Ime;
    int Stage = 0;
    double Next = 0;
    bool bStarted = false, bUpdated = false, bEnded = false, bComposingInput = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiWindowsImeTest, "RmlUi.Input.WindowsImeContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlUiWindowsImeTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FRmlUiImeScenario(this));
    return true;
}
#endif
