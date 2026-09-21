// Included by RmlUiBridge.cpp after the view/node helpers. No engine dependencies.
#include "RmlUiTextInputBridge.h"
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/TextInputHandler.h>

namespace {
class BridgeTextInputHandler final : public Rml::TextInputHandler {
public:
    explicit BridgeTextInputHandler(RmlUE_View* InView) : View(InView) {}
    RmlUE_View* View;
    Rml::TextInputContext* Active = nullptr;
    Rml::ObserverPtr<Rml::Element> Element;
    bool Composing = false;
    Rml::String Original;
    int OriginalAnchor = 0, OriginalCaret = 0;
    int CompositionStart = 0, CompositionEnd = 0;

    bool Editable() const {
        const auto* Control = Element ? rmlui_dynamic_cast<Rml::ElementFormControl*>(Element.get()) : nullptr;
        return Active && Control && !Control->IsDisabled() && !Element->HasAttribute("readonly");
    }
    void Dispatch(const char* Type, const Rml::String& Data = {}, bool Cancelled = false) {
        if (!Element) return;
        Rml::Dictionary Params;
        Params["data"] = Data;
        Params["value"] = Active ? Active->GetText() : Rml::String();
        Params["is_composing"] = Composing;
        Params["cancelled"] = Cancelled;
        Element->DispatchEvent(Type, Params);
    }
    void OnActivate(Rml::TextInputContext* Input) override {
        if (Active == Input) return;
        Finish(true);
        Active = Input;
        Element = Input && Input->GetElement() ? Input->GetElement()->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
    }
    void OnDeactivate(Rml::TextInputContext* Input) override {
        if (Active != Input) return;
        Finish(true);
        Active = nullptr;
        Element = {};
    }
    void OnDestroy(Rml::TextInputContext* Input) override {
        if (Active != Input) return;
        // The element is being destroyed: do not dispatch back into its destructing tree.
        Active = nullptr;
        Element = {};
        Composing = false;
        Original.clear();
    }
    bool Begin() {
        if (!Editable()) return false;
        if (Composing) return true;
        Original = Active->GetText();
        Active->GetSelectionRange(CompositionStart, CompositionEnd);
        OriginalCaret = Active->GetCursorPosition();
        OriginalAnchor = OriginalCaret == CompositionStart ? CompositionEnd : CompositionStart;
        Composing = true;
        Dispatch("compositionstart");
        return true;
    }
    bool Finish(bool Cancel) {
        if (!Composing || !Active) return false;
        Rml::String Data;
        if (Cancel) {
            Active->SetText(Original, 0, (int)Rml::StringUtilities::LengthUTF8(Active->GetText()));
            Active->SetSelectionRange(OriginalAnchor, OriginalCaret);
        } else {
            const Rml::String Text = Active->GetText();
            const int Start = Rml::StringUtilities::ConvertCharacterOffsetToByteOffset(Text, CompositionStart);
            const int End = Rml::StringUtilities::ConvertCharacterOffsetToByteOffset(Text, CompositionEnd);
            Data = Text.substr(Start, End - Start);
            Active->SetCompositionRange(CompositionStart, CompositionEnd);
            Active->CommitComposition(Data);
            const int NewLength = (int)Rml::StringUtilities::LengthUTF8(Active->GetText());
            const int SuffixLength = (int)Rml::StringUtilities::LengthUTF8(Text) - CompositionEnd;
            Active->SetCursorPosition(std::max(CompositionStart, NewLength - SuffixLength));
        }
        Active->SetCompositionRange(0, 0);
        Composing = false;
        CompositionStart = CompositionEnd = 0;
        Original.clear();
        Dispatch("compositionend", Data, Cancel);
        Dispatch("input", Data, Cancel);
        Dispatch("change", Data, Cancel);
        return true;
    }
};
std::unordered_map<RmlUE_View*, std::unique_ptr<BridgeTextInputHandler>> TextInputHandlers;
BridgeTextInputHandler* TextInputFor(RmlUE_View* View) {
    if (!ValidView(View)) return nullptr;
    const auto It = TextInputHandlers.find(View);
    return It != TextInputHandlers.end() ? It->second.get() : nullptr;
}
}

static Rml::TextInputHandler* RmlUE_CreateTextInputHandler(RmlUE_View* View) {
    auto Handler = std::make_unique<BridgeTextInputHandler>(View);
    auto* Result = Handler.get();
    TextInputHandlers[View] = std::move(Handler);
    return Result;
}
static void RmlUE_DestroyTextInputHandler(RmlUE_View* View) { TextInputHandlers.erase(View); }

int RmlUE_TextInputGetState(RmlUE_View* View, RmlUE_TextInputState* State) {
    if (!State) return 0;
    *State = {};
    State->Version = 1;
    auto* H = TextInputFor(View);
    if (!H || !H->Active || !H->Element) return 0;
    State->Active = 1;
    State->Node = View->Track(H->Element.get());
    State->ReadOnly = H->Editable() ? 0 : 1;
    State->Composing = H->Composing ? 1 : 0;
    State->Length = (int)Rml::StringUtilities::LengthUTF8(H->Active->GetText());
    H->Active->GetSelectionRange(State->SelectionStart, State->SelectionEnd);
    State->Caret = H->Active->GetCursorPosition();
    State->CompositionStart = H->CompositionStart;
    State->CompositionEnd = H->CompositionEnd;
    return 1;
}
int RmlUE_TextInputGetText(RmlUE_View* View, char* Text, size_t Capacity) {
    auto* H = TextInputFor(View);
    if (!H || !H->Active) return 0;
    const Rml::String Value = H->Active->GetText();
    if (Text && Capacity) CopyString(Text, Capacity, Value);
    return (int)Value.size() + 1;
}
int RmlUE_TextInputSetSelection(RmlUE_View* View, int Anchor, int Caret) {
    auto* H = TextInputFor(View);
    if (!H || !H->Active) return 0;
    const int Length = (int)Rml::StringUtilities::LengthUTF8(H->Active->GetText());
    H->Active->SetSelectionRange(std::clamp(Anchor, 0, Length), std::clamp(Caret, 0, Length));
    return 1;
}
int RmlUE_TextInputReplace(RmlUE_View* View, int Start, int End, const char* Text) {
    auto* H = TextInputFor(View);
    if (!H || !H->Editable() || !Text) return 0;
    const int Length = (int)Rml::StringUtilities::LengthUTF8(H->Active->GetText());
    if (Start < 0 || End < Start || End > Length) return 0;
    const Rml::String Replacement(Text);
    H->Active->SetText(Replacement, Start, End);
    const int Inserted = (int)Rml::StringUtilities::LengthUTF8(Replacement);
    H->Active->SetCursorPosition(Start + Inserted);
    if (H->Composing) {
        H->CompositionStart = Start;
        H->CompositionEnd = Start + Inserted;
        H->Active->SetCompositionRange(H->CompositionStart, H->CompositionEnd);
        H->Dispatch("compositionupdate", Text);
    }
    H->Dispatch("input", Text);
    H->Dispatch("change", Text);
    return 1;
}
int RmlUE_TextInputBeginComposition(RmlUE_View* View) {
    auto* H = TextInputFor(View); return H && H->Begin() ? 1 : 0;
}
int RmlUE_TextInputUpdateComposition(RmlUE_View* View, int Start, int End) {
    auto* H = TextInputFor(View);
    if (!H || !H->Active || !H->Composing) return 0;
    const int Length = (int)Rml::StringUtilities::LengthUTF8(H->Active->GetText());
    if (Start < 0 || End < Start || End > Length) return 0;
    H->CompositionStart = Start;
    H->CompositionEnd = End;
    H->Active->SetCompositionRange(Start, End);
    return 1;
}
int RmlUE_TextInputEndComposition(RmlUE_View* View, int Cancel) {
    auto* H = TextInputFor(View); return H && H->Finish(Cancel != 0) ? 1 : 0;
}
int RmlUE_TextInputIsComposing(RmlUE_View* View) {
    auto* H = TextInputFor(View); return H && H->Composing ? 1 : 0;
}
int RmlUE_TextInputGetBounds(RmlUE_View* View, int Start, int End, RmlUE_Rect* Bounds, int* Clipped) {
    if (Bounds) *Bounds = {};
    if (Clipped) *Clipped = 1;
    auto* H = TextInputFor(View);
    Rml::Rectanglef Rect, Screen;
    if (!Bounds || !H || !H->Active || !H->Active->GetTextBounds(Start, End, Rect)) return 0;
    *Bounds = {Rect.Left(), Rect.Top(), Rect.Width(), Rect.Height()};
    if (Clipped && H->Active->GetBoundingBox(Screen))
    {
        Rml::Rectanglei Region;
        if (H->Element && Rml::ElementUtilities::GetClippingRegion(H->Element.get(), Region, nullptr, true))
            Screen = Screen.Intersect(Rml::Rectanglef(Region));
        *Clipped = Rect.Left() < Screen.Left() || Rect.Right() > Screen.Right() || Rect.Top() < Screen.Top() || Rect.Bottom() > Screen.Bottom();
    }
    return 1;
}
int RmlUE_TextInputGetScreenBounds(RmlUE_View* View, RmlUE_Rect* Bounds) {
    if (Bounds) *Bounds = {};
    auto* H = TextInputFor(View);
    Rml::Rectanglef Rect;
    if (!Bounds || !H || !H->Active || !H->Active->GetBoundingBox(Rect)) return 0;
    *Bounds = {Rect.Left(), Rect.Top(), Rect.Width(), Rect.Height()};
    return 1;
}
int RmlUE_TextInputHitTest(RmlUE_View* View, float X, float Y) {
    auto* H = TextInputFor(View); return H && H->Active ? H->Active->GetCharacterIndexAtPoint({X, Y}) : -1;
}
