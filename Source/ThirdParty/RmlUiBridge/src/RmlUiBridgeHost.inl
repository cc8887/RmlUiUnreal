// Host ABI 2 implementation. Included after View/GetNode so opaque handles remain private.

static bool InModalScope(RmlUE_View* View, Rml::Element* Element)
{
    return !View->ModalRoot || (Element && View->ModalRoot->Contains(Element));
}

static bool IsFocusable(Rml::Element* Element)
{
    if (!Element || !Element->IsVisible(true) || Element->HasAttribute("disabled")) return false;
    if (Element->GetComputedValues().focus() == Rml::Style::Focus::None) return false;
    const auto Tab = Element->GetAttribute<Rml::String>("tabindex", "");
    if (Tab == "-1") return false;
    if (!Tab.empty()) return true;
    const auto& Tag = Element->GetTagName();
    return Element->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto ||
        Tag == "button" || Tag == "textarea" || Tag == "select" ||
        (Tag == "input" && Element->GetAttribute<Rml::String>("type", "text") != "hidden");
}

static void CollectFocusable(Rml::Element* Root, std::vector<Rml::Element*>& Result)
{
    if (IsFocusable(Root)) Result.push_back(Root);
    for (int I = 0; I < Root->GetNumChildren(); ++I) CollectFocusable(Root->GetChild(I), Result);
}

static void FocusModalNext(RmlUE_View* View, bool Backwards)
{
    auto* Root = View->ModalRoot.get();
    if (!Root) return;
    std::vector<Rml::Element*> Candidates;
    CollectFocusable(Root, Candidates);
    if (Candidates.empty()) { Root->Focus(true); return; }
    auto Found = std::find(Candidates.begin(), Candidates.end(), View->Context->GetFocusElement());
    int Index = Found == Candidates.end() ? (Backwards ? 0 : -1) : static_cast<int>(Found - Candidates.begin());
    Index = (Index + (Backwards ? -1 : 1) + static_cast<int>(Candidates.size())) % static_cast<int>(Candidates.size());
    Candidates[Index]->Focus(true);
}

void RmlUE_View::InputGuard::ProcessEvent(Rml::Event& Event)
{
    if (!View || !View->ModalRoot || Event.GetTargetElement()->GetOwnerDocument() != View->Document) return;
    auto* Target = Event.GetTargetElement();
    if (!InModalScope(View, Target)) { Event.PreventDefault(); Event.StopImmediatePropagation(); return; }
    if (Event.GetType() == "keydown" && Event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_TAB) {
        Event.PreventDefault();
        FocusModalNext(View, Event.GetParameter<int>("shift_key", 0) != 0);
    }
    if (Event.GetType() == "mousewheel") {
        auto* Scrollable = Target->GetClosestScrollableContainer();
        if (!InModalScope(View, Scrollable)) Event.PreventDefault();
    }
}

static std::string HostKeyName(int Key, bool Shift)
{
    using namespace Rml::Input;
    if (Key >= KI_A && Key <= KI_Z) return std::string(1, static_cast<char>((Shift ? 'A' : 'a') + Key - KI_A));
    if (Key >= KI_0 && Key <= KI_9) return std::string(1, Shift ? ")!@#$%^&*("[Key-KI_0] : static_cast<char>('0'+Key-KI_0));
    if (Key >= KI_F1 && Key <= KI_F24) return "F" + std::to_string(Key - KI_F1 + 1);
    if (Key >= KI_NUMPAD0 && Key <= KI_NUMPAD9) return std::string(1, static_cast<char>('0'+Key-KI_NUMPAD0));
    switch (Key) {
        case KI_RETURN: return "Enter"; case KI_ESCAPE: return "Escape"; case KI_TAB: return "Tab";
        case KI_BACK: return "Backspace"; case KI_DELETE: return "Delete"; case KI_INSERT: return "Insert";
        case KI_LEFT: return "ArrowLeft"; case KI_RIGHT: return "ArrowRight"; case KI_UP: return "ArrowUp"; case KI_DOWN: return "ArrowDown";
        case KI_HOME: return "Home"; case KI_END: return "End"; case KI_PRIOR: return "PageUp"; case KI_NEXT: return "PageDown";
        case KI_SPACE: return " "; case KI_LSHIFT: case KI_RSHIFT: return "Shift";
        case KI_LCONTROL: case KI_RCONTROL: return "Control"; case KI_LMENU: case KI_RMENU: return "Alt";
        case KI_LWIN: case KI_RWIN: return "Meta"; case KI_CAPITAL: return "CapsLock"; case KI_NUMLOCK: return "NumLock";
        case KI_OEM_MINUS: return Shift ? "_" : "-"; case KI_OEM_PLUS: return Shift ? "+" : "=";
        case KI_OEM_COMMA: return Shift ? "<" : ","; case KI_OEM_PERIOD: return Shift ? ">" : ".";
        case KI_OEM_1: return Shift ? ":" : ";"; case KI_OEM_2: return Shift ? "?" : "/";
        case KI_OEM_3: return Shift ? "~" : "`"; case KI_OEM_4: return Shift ? "{" : "[";
        case KI_OEM_6: return Shift ? "}" : "]"; case KI_OEM_5: return Shift ? "|" : "\\";
        case KI_OEM_7: return Shift ? "\"" : "'"; default: return "Unidentified";
    }
}

static std::string HostKeyCode(int Key)
{
    using namespace Rml::Input;
    if (Key >= KI_A && Key <= KI_Z) return "Key" + std::string(1, static_cast<char>('A'+Key-KI_A));
    if (Key >= KI_0 && Key <= KI_9) return "Digit" + std::to_string(Key-KI_0);
    if (Key >= KI_NUMPAD0 && Key <= KI_NUMPAD9) return "Numpad" + std::to_string(Key-KI_NUMPAD0);
    switch (Key) {
        case KI_LSHIFT: return "ShiftLeft"; case KI_RSHIFT: return "ShiftRight";
        case KI_LCONTROL: return "ControlLeft"; case KI_RCONTROL: return "ControlRight";
        case KI_LMENU: return "AltLeft"; case KI_RMENU: return "AltRight";
        case KI_SPACE: return "Space"; default: return HostKeyName(Key, false);
    }
}

static Rml::Dictionary PointerParameters(RmlUE_View* View, int Pointer, float X, float Y, int Button, int Flags, int Buttons = -1)
{
    Rml::Dictionary Parameters;
    Parameters["mouse_x"] = X; Parameters["mouse_y"] = Y; Parameters["button"] = Button; Parameters["pointer_id"] = Pointer;
    Parameters["shift_key"] = (Flags & 1) != 0; Parameters["ctrl_key"] = (Flags & 2) != 0;
    Parameters["alt_key"] = (Flags & 4) != 0; Parameters["meta_key"] = (Flags & 32) != 0;
    if (Buttons >= 0) Parameters["buttons"] = Buttons;
    return Parameters;
}

static Rml::Dictionary CaptureParameters(RmlUE_View* View, int Pointer, int Buttons = -1)
{
    Rml::Vector2f Position(View->MouseX, View->MouseY);
    if (Pointer > 0) {
        auto Found = View->TouchPositions.find(Pointer - 1);
        Position = Found == View->TouchPositions.end() ? Rml::Vector2f(0, 0) : Found->second;
    }
    return PointerParameters(View, Pointer, Position.x, Position.y, -1, Pointer == 0 ? View->InputFlags : 0, Buttons);
}

static bool DispatchPointer(RmlUE_View* View, const char* Type, int Pointer, float X, float Y, int Button, int Flags)
{
    const bool Down = std::strcmp(Type, "pointerdown") == 0;
    const bool Ended = std::strcmp(Type, "pointercancel") == 0 ||
        (std::strcmp(Type, "pointerup") == 0 && (Pointer > 0 || View->PressedButtons.empty()));
    if (Ended) View->PointerDownTargets.erase(Pointer);
    auto Found = View->PointerCaptures.find(Pointer);
    auto* Target = Found != View->PointerCaptures.end() && Found->second ? Found->second.get() :
        View->Context->GetElementAtPoint({X, Y});
    if (!Target || !InModalScope(View, Target)) return false;
    if (Down && !View->PointerDownTargets[Pointer]) View->PointerDownTargets[Pointer] = Target->GetObserverPtr();
    const auto Parameters = PointerParameters(View, Pointer, X, Y, Button, Flags,
        std::strcmp(Type, "pointercancel") == 0 ? 0 : -1);
    return Target->DispatchEvent(Type, Parameters);
}

static void CancelPointerCaptures(RmlUE_View* View, Rml::Element* Subtree)
{
    // End captured and uncaptured pointer streams before dispatch, so a reentrant
    // removal/focus-loss callback cannot cancel the same active pointer twice.
    const bool WasCancelling = View->CancellingPointers;
    View->CancellingPointers = true;
    std::vector<std::pair<int, Rml::ObserverPtr<Rml::Element>>> Cancelled;
    std::vector<std::pair<int, Rml::ObserverPtr<Rml::Element>>> Uncaptured;
    for (auto It = View->PointerCaptures.begin(); It != View->PointerCaptures.end();) {
        if (!Subtree || !It->second || Subtree->Contains(It->second.get())) {
            View->PointerDownTargets.erase(It->first);
            Cancelled.push_back(*It); It = View->PointerCaptures.erase(It);
        }
        else ++It;
    }
    for (auto It = View->PointerDownTargets.begin(); It != View->PointerDownTargets.end();) {
        if (!Subtree || !It->second || Subtree->Contains(It->second.get())) {
            // A pointer captured outside the removed subtree remains active there.
            if (!View->PointerCaptures.count(It->first)) Uncaptured.push_back(*It);
            It = View->PointerDownTargets.erase(It);
        } else ++It;
    }
    for (auto& Pair : Cancelled) if (Pair.second) {
        const auto Parameters = CaptureParameters(View, Pair.first, 0);
        Pair.second->DispatchEvent("pointercancel", Parameters);
        if (Pair.second) Pair.second->DispatchEvent("lostpointercapture", Parameters);
    }
    for (auto& Pair : Uncaptured) if (Pair.second)
        Pair.second->DispatchEvent("pointercancel", CaptureParameters(View, Pair.first, 0));
    View->CancellingPointers = WasCancelling;
}

static bool HostPropertyAllowed(RmlUE_View* View, const char* Name, const char* Value)
{
    if (!View->StrictCapabilities || !View->SlateRenderer || !Value) return true;
    auto Canonical = [](const char* Input) {
        std::string Text(Input);
        std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
        const auto First = Text.find_first_not_of(" \t\n\r");
        return First == std::string::npos ? std::string() : Text.substr(First, Text.find_last_not_of(" \t\n\r") - First + 1);
    };
    const std::string Property = Canonical(Name), Text = Canonical(Value);
    auto HasFunction = [&](const char* Function) {
        size_t At = Text.find(Function);
        while (At != std::string::npos) {
            const auto After = Text.find_first_not_of(" \t\n\r", At + std::strlen(Function));
            const bool Boundary = At == 0 || (!std::isalnum(static_cast<unsigned char>(Text[At - 1])) && Text[At - 1] != '-' && Text[At - 1] != '_');
            if (Boundary && After != std::string::npos && Text[After] == '(') return true;
            At = Text.find(Function, At + 1);
        }
        return false;
    };
    if (Text == "none") return true;
    if (Property == "box-shadow" || Property == "filter" || Property == "backdrop-filter" || Property == "mask-image")
        return Fail("slate-rhi cannot execute dynamic layer/filter property: " + Property) != 0;
    // Check custom token values too: otherwise a later var() substitution can hide unsupported renderer work.
    for (const char* Function : {"perspective", "matrix3d", "rotatex", "rotatey", "rotate3d", "translatez", "translate3d", "scalez", "scale3d"}) {
        if (HasFunction(Function)) return Fail("slate-rhi cannot execute a dynamic 3D transform, including custom tokens: " + Property) != 0;
    }
    if ((Property == "perspective" || Property == "transform-origin-z") && Text != "0" && Text != "0px" && Text != "0dp")
        return Fail("slate-rhi cannot execute a dynamic perspective or depth property: " + Property) != 0;
    for (const char* Function : {"shader", "horizontal-gradient", "vertical-gradient", "linear-gradient", "radial-gradient", "conic-gradient", "repeating-linear-gradient", "repeating-radial-gradient", "repeating-conic-gradient"}) {
        if (HasFunction(Function)) return Fail("slate-rhi cannot execute a dynamic gradient/shader, including custom tokens: " + Property) != 0;
    }
    if ((Property == "transform" || Property == "transform-origin" || Property == "decorator") && HasFunction("var"))
        return Fail("Dynamic renderer properties require an explicit validated value; unresolved var() is not supported: " + Property) != 0;
    if (Property == "transform-origin") {
        Rml::PropertyDictionary Parsed;
        if (Rml::StyleSheetSpecification::ParsePropertyDeclaration(Parsed, Property, Value)) {
            const auto* Z = Parsed.GetProperty(Rml::PropertyId::TransformOriginZ);
            if (Z && Z->value.Get<float>() != 0)
                return Fail("slate-rhi cannot execute a dynamic transform origin with depth.") != 0;
        }
    }
    return true;
}

uint32_t RmlUE_GetHostAbiVersion() { return RMLUE_HOST_ABI_VERSION; }
void RmlUE_SetStrictCapabilities(RmlUE_View* View, int Enabled) { if (ValidView(View)) View->StrictCapabilities = Enabled != 0; }
RmlUE_Node RmlUE_QueryNode(RmlUE_View* View, RmlUE_Node Root, const char* Selector)
{
    auto* Element = GetNode(View, Root);
    return Element && Selector ? View->Track(Element->QuerySelector(Selector)) : 0;
}
int RmlUE_QueryNodes(RmlUE_View* View, RmlUE_Node Root, const char* Selector, RmlUE_Node* Nodes, int Capacity)
{
    auto* Element = GetNode(View, Root);
    if (!Element || !Selector || Capacity < 0 || (Capacity && !Nodes)) return -1;
    Rml::ElementList Matches; Element->QuerySelectorAll(Matches, Selector);
    for (int I = 0; I < std::min(Capacity, static_cast<int>(Matches.size())); ++I) Nodes[I] = View->Track(Matches[I]);
    return static_cast<int>(Matches.size());
}
int RmlUE_ChildNodes(RmlUE_View* View, RmlUE_Node Parent, RmlUE_Node* Nodes, int Capacity)
{
    auto* Element = GetNode(View, Parent);
    if (!Element || Capacity < 0 || (Capacity && !Nodes)) return -1;
    const int Count = Element->GetNumChildren();
    for (int I = 0; I < std::min(Capacity, Count); ++I) Nodes[I] = View->Track(Element->GetChild(I));
    return Count;
}
int RmlUE_ContainsNode(RmlUE_View* View, RmlUE_Node Parent, RmlUE_Node Child)
{
    auto* P = GetNode(View, Parent); auto* C = GetNode(View, Child); return P && C && P->Contains(C);
}
RmlUE_Node RmlUE_ActiveNode(RmlUE_View* View) { return ValidView(View) ? View->Track(View->Context->GetFocusElement()) : 0; }
int RmlUE_BlurNode(RmlUE_View* View, RmlUE_Node Node) { auto* Element = GetNode(View, Node); if (!Element) return 0; Element->Blur(); View->MarkContentDirty(); return 1; }
int RmlUE_GetComputedProperty(RmlUE_View* View, RmlUE_Node Node, const char* Name, char* Value, size_t Capacity)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name || !Value || !Capacity) return 0;
    auto* Property = Element->GetProperty(Name);
    const std::string Text = Property ? Property->ToString() : "";
    if (Text.size() >= Capacity) return Fail("Computed property buffer is too small.");
    CopyString(Value, Capacity, Text); return 1;
}
int RmlUE_SetNodeClass(RmlUE_View* View, RmlUE_Node Node, const char* Name, int Enabled)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name || !*Name || std::strpbrk(Name, " \t\n\r")) return 0;
    Element->SetClass(Name, Enabled != 0); View->MarkContentDirty(); return 1;
}
int RmlUE_SetModalRoot(RmlUE_View* View, RmlUE_Node Root, RmlUE_Node InitialFocus)
{
    if (!ValidView(View)) return 0;
    if (!Root) { View->ModalRoot = nullptr; View->MarkContentDirty(); return 1; }
    auto* Element = GetNode(View, Root);
    auto* Initial = InitialFocus ? GetNode(View, InitialFocus) : nullptr;
    if (!Element || Element->GetOwnerDocument() != View->Document || (InitialFocus && (!Initial || !Element->Contains(Initial))))
        return Fail("Modal root and initial focus must be attached to the same document subtree.");
    CancelPointerCaptures(View, nullptr);
    View->ModalRoot = Element->GetObserverPtr();
    if (!Initial || !Initial->Focus(true)) FocusModalNext(View, false);
    View->MarkContentDirty();
    return 1;
}
int RmlUE_CaptureNode(RmlUE_View* View, RmlUE_Node Node, int PointerId)
{
    auto* Element = GetNode(View, Node);
    if (!Element || View->CancellingPointers || !InModalScope(View, Element) || (PointerId == 0 ? View->PressedButtons.empty() : !View->Touches.count(PointerId - 1)))
        return Fail("Pointer capture requires an active pointer and an attached node in the modal scope.");
    if (Element->GetOwnerDocument() != View->Document) return Fail("Cannot capture a detached node.");
    auto Existing = View->PointerCaptures.find(PointerId);
    if (Existing != View->PointerCaptures.end()) {
        if (Existing->second.get() == Element) return 1;
        auto Previous = Existing->second;
        View->PointerCaptures.erase(Existing);
        if (Previous) {
            const auto Lost = CaptureParameters(View, PointerId);
            Previous->DispatchEvent("lostpointercapture", Lost);
        }
    }
    View->PointerCaptures[PointerId] = Element->GetObserverPtr();
    const auto Parameters = CaptureParameters(View, PointerId); Element->DispatchEvent("gotpointercapture", Parameters);
    return 1;
}
int RmlUE_ReleaseCaptureNode(RmlUE_View* View, RmlUE_Node Node, int PointerId)
{
    auto* Element = GetNode(View, Node); if (!Element) return 0;
    auto Found = View->PointerCaptures.find(PointerId);
    if (Found == View->PointerCaptures.end()) return 1;
    if (Found->second.get() != Element) return Fail("Pointer capture belongs to another node.");
    View->PointerCaptures.erase(Found);
    const auto Parameters = CaptureParameters(View, PointerId); Element->DispatchEvent("lostpointercapture", Parameters);
    return 1;
}

static RmlUE_Rect ElementRectangle(Rml::Element* Element, Rml::BoxArea Area, bool Transformed)
{
    const auto Position = Element->GetAbsoluteOffset(Area);
    const auto Size = Element->GetBox().GetSize(Area);
    RmlUE_Rect Rect{Position.x, Position.y, Size.x, Size.y};
    const auto* State = Transformed ? Element->GetTransformState() : nullptr;
    const auto* Matrix = State ? State->GetTransform() : nullptr;
    if (Matrix) {
        float X0 = FLT_MAX, Y0 = FLT_MAX, X1 = -FLT_MAX, Y1 = -FLT_MAX;
        for (const auto& Point : {Rml::Vector2f(Position.x, Position.y), Rml::Vector2f(Position.x + Size.x, Position.y),
            Rml::Vector2f(Position.x, Position.y + Size.y), Rml::Vector2f(Position.x + Size.x, Position.y + Size.y)}) {
            auto P = *Matrix * Rml::Vector4f(Point.x, Point.y, 0, 1);
            if (std::abs(P.w) < 1e-6f) return {0, 0, 0, 0};
            const float X = P.x/P.w, Y = P.y/P.w;
            X0 = std::min(X0, X); Y0 = std::min(Y0, Y); X1 = std::max(X1, X); Y1 = std::max(Y1, Y);
        }
        Rect = {X0, Y0, X1-X0, Y1-Y0};
    }
    return Rect;
}

int RmlUE_MeasureNodes(RmlUE_View* View, const RmlUE_Node* Nodes, int Count, RmlUE_NodeMetrics* Metrics, RmlUE_LayoutInfo* Info)
{
    if (!ValidView(View) || Count < 0 || Count > 16384 || (Count && (!Nodes || !Metrics)) || !Info) return 0;
    *Info = {View->LayoutRevision, View->Width, View->Height, View->DpRatio};
    for (int I = 0; I < Count; ++I) {
        auto& M = Metrics[I]; M = {}; M.Node = Nodes[I];
        const auto Found = View->Nodes.find(Nodes[I]);
        if (Found == View->Nodes.end() || !Found->second.Element) continue;
        auto* Element = Found->second.Element.get();
        if (Element->GetOwnerDocument() != View->Document) continue;
        M.Valid = 1; M.Visible = Element->IsVisible(true);
        auto Layout = ElementRectangle(Element, Rml::BoxArea::Border, false);
        auto Rect = ElementRectangle(Element, Rml::BoxArea::Border, true);
        M.X = Rect.X; M.Y = Rect.Y; M.Width = Rect.Width; M.Height = Rect.Height;
        M.LayoutX = Layout.X; M.LayoutY = Layout.Y; M.LayoutWidth = Layout.Width; M.LayoutHeight = Layout.Height;
        M.ScrollTop = Element->GetScrollTop(); M.ScrollLeft = Element->GetScrollLeft();
        M.ScrollWidth = Element->GetScrollWidth(); M.ScrollHeight = Element->GetScrollHeight();
        M.ClientWidth = Element->GetClientWidth(); M.ClientHeight = Element->GetClientHeight();
        float X0 = 0, Y0 = 0, X1 = static_cast<float>(View->Width), Y1 = static_cast<float>(View->Height);
        for (auto* Parent = Element->GetParentNode(); Parent; Parent = Parent->GetParentNode()) {
            const auto& Style = Parent->GetComputedValues();
            const auto Clip = ElementRectangle(Parent, Rml::BoxArea::Padding, true);
            if (Style.overflow_x() != Rml::Style::Overflow::Visible) { X0 = std::max(X0, Clip.X); X1 = std::min(X1, Clip.X+Clip.Width); }
            if (Style.overflow_y() != Rml::Style::Overflow::Visible) { Y0 = std::max(Y0, Clip.Y); Y1 = std::min(Y1, Clip.Y+Clip.Height); }
        }
        M.ClipX = X0; M.ClipY = Y0; M.ClipWidth = std::max(0.f, X1-X0); M.ClipHeight = std::max(0.f, Y1-Y0);
    }
    return 1;
}
void RmlUE_SetLayoutCallback(RmlUE_View* View, RmlUE_LayoutCallback Callback, void* User)
{
    if (ValidView(View)) { View->LayoutCallback = Callback; View->LayoutUser = Callback ? User : nullptr; }
}
int RmlUE_AnimateNode(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* From, const char* To, float Duration, int Iterations)
{
    auto* Element = GetNode(View, Node);
    if (!Element || !Name || !From || !To || !std::isfinite(Duration) || Duration <= 0 || Iterations < -1 || Iterations == 0) return 0;
    if (!View->LayoutRevision) return Fail("Animation requires a completed layout.");
    if (!HostPropertyAllowed(View, Name, From) || !HostPropertyAllowed(View, Name, To)) return 0;
    const auto* Definition = Rml::StyleSheetSpecification::GetProperty(Name);
    Rml::Property Start, End;
    if (!Definition || !Definition->ParseValue(Start, From) || !Definition->ParseValue(End, To)) return Fail("Invalid native animation property/value.");
    if (!Element->Animate(Name, End, Duration, Rml::Tween{}, Iterations, false, 0, &Start)) return Fail("Cannot start native animation.");
    View->MarkContentDirty(); return 1;
}
int RmlUE_CancelAnimation(RmlUE_View* View, RmlUE_Node Node, const char* Name)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name) return 0;
    Element->CancelAnimation(Name); return 1;
}
RmlUE_AnimationTarget RmlUE_ResolveAnimationTarget(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node);
    return Element ? View->ResolveAnimationTarget(Node, Element) : 0;
}
static bool AnimationPropertyIds(uint32_t Property, std::vector<Rml::PropertyId>& OutIds)
{
    OutIds.clear();
    switch (Property) {
    case RMLUE_ANIMATED_PROPERTY_OPACITY: OutIds.push_back(Rml::PropertyId::Opacity); break;
    case RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D: OutIds.push_back(Rml::PropertyId::Transform); break;
    case RMLUE_ANIMATED_PROPERTY_LEFT_PX: OutIds.push_back(Rml::PropertyId::Left); break;
    case RMLUE_ANIMATED_PROPERTY_TOP_PX: OutIds.push_back(Rml::PropertyId::Top); break;
    case RMLUE_ANIMATED_PROPERTY_RIGHT_PX: OutIds.push_back(Rml::PropertyId::Right); break;
    case RMLUE_ANIMATED_PROPERTY_BOTTOM_PX: OutIds.push_back(Rml::PropertyId::Bottom); break;
    case RMLUE_ANIMATED_PROPERTY_WIDTH_PX: OutIds.push_back(Rml::PropertyId::Width); break;
    case RMLUE_ANIMATED_PROPERTY_HEIGHT_PX: OutIds.push_back(Rml::PropertyId::Height); break;
    case RMLUE_ANIMATED_PROPERTY_VISIBILITY: OutIds.push_back(Rml::PropertyId::Visibility); break;
    case RMLUE_ANIMATED_PROPERTY_COLOR: OutIds.push_back(Rml::PropertyId::Color); break;
    case RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR: OutIds.push_back(Rml::PropertyId::BackgroundColor); break;
    case RMLUE_ANIMATED_PROPERTY_BORDER_COLOR:
        OutIds = {Rml::PropertyId::BorderTopColor, Rml::PropertyId::BorderRightColor,
            Rml::PropertyId::BorderBottomColor, Rml::PropertyId::BorderLeftColor};
        break;
    case RMLUE_ANIMATED_PROPERTY_IMAGE_COLOR: OutIds.push_back(Rml::PropertyId::ImageColor); break;
    default: return false;
    }
    return true;
}
int RmlUE_PrepareAnimationTargetProperty(
    RmlUE_View* View, RmlUE_AnimationTarget Target, uint32_t Property)
{
    if (!ValidView(View) || !Target ||
        (Property != RMLUE_ANIMATED_PROPERTY_OPACITY &&
            Property != RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D &&
            Property != RMLUE_ANIMATED_PROPERTY_LEFT_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_TOP_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_RIGHT_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_BOTTOM_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_WIDTH_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_HEIGHT_PX &&
            Property != RMLUE_ANIMATED_PROPERTY_VISIBILITY &&
            Property != RMLUE_ANIMATED_PROPERTY_COLOR &&
            Property != RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR &&
            Property != RMLUE_ANIMATED_PROPERTY_BORDER_COLOR &&
            Property != RMLUE_ANIMATED_PROPERTY_IMAGE_COLOR)) return 0;
    auto* Record = View->FindAnimationTarget(Target);
    if (!Record) return 0;
    if (Record->PreparedBaseProperty && Record->PreparedBaseProperty != Property) return 0;
    if (!Record->PreparedBaseProperty) {
        for (const auto& Other : View->AnimationTargets) {
            if (&Other != Record && Other.Active && Other.Element.get() == Record->Element.get() &&
                Other.PreparedBaseProperty == Property) {
                Record->BaseLocalProperties = Other.BaseLocalProperties;
                break;
            }
        }
        if (Record->BaseLocalProperties.empty()) {
            thread_local std::vector<Rml::PropertyId> PropertyIds;
            if (!AnimationPropertyIds(Property, PropertyIds)) return 0;
            for (Rml::PropertyId PropertyId : PropertyIds) {
                const Rml::Property* Local = Record->Element->GetLocalProperty(PropertyId);
                Record->BaseLocalProperties.emplace_back(PropertyId,
                    Local ? std::optional<Rml::Property>(*Local) : std::nullopt);
            }
        }
        Record->PreparedBaseProperty = Property;
    }
    if (Property != RMLUE_ANIMATED_PROPERTY_OPACITY &&
        Property != RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D &&
        Property != RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR) return 1;
    const uint32_t PropertyBit = 1u << Property;
    if (Record->PreparedVisualProperties & PropertyBit) return 1;
    if (Property == RMLUE_ANIMATED_PROPERTY_OPACITY && View->SlateRenderer)
    {
        Record->PreparedOpacityState = View->SlateRenderer->RetainVisualOpacityBinding(Record->Node);
        if (!Record->PreparedOpacityState) return 0;
    }
    else if (Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR && View->SlateRenderer)
    {
        Record->PreparedBackgroundColorState = View->SlateRenderer->RetainVisualBackgroundColorBinding(Record->Node);
        if (!Record->PreparedBackgroundColorState) return 0;
    }
    Record->PreparedVisualProperties |= PropertyBit;
    return 1;
}
int RmlUE_IsAnimationTargetValid(RmlUE_View* View, RmlUE_AnimationTarget Target)
{
    return ValidView(View) && View->FindAnimationTarget(Target) ? 1 : 0;
}
int RmlUE_ReleaseAnimationTarget(RmlUE_View* View, RmlUE_AnimationTarget Target)
{
    return ValidView(View) && View->ReleaseAnimationTarget(Target) ? 1 : 0;
}
int RmlUE_RestoreAnimationTargetProperty(
    RmlUE_View* View, RmlUE_AnimationTarget Target, int RestoreWhenShared)
{
    if (!ValidView(View)) return 0;
    auto* Record = View->FindAnimationTarget(Target);
    if (!Record || !Record->PreparedBaseProperty || Record->BaseLocalProperties.empty()) return 0;
    if (!RestoreWhenShared) {
        for (const auto& Other : View->AnimationTargets) {
            if (&Other != Record && Other.Active && Other.Element.get() == Record->Element.get() &&
                Other.PreparedBaseProperty == Record->PreparedBaseProperty)
                return 1;
        }
    }
    if (View->SlateRenderer) {
        if (Record->PreparedBaseProperty == RMLUE_ANIMATED_PROPERTY_OPACITY)
            View->SlateRenderer->ClearVisualOpacity(Record->Node);
        else if (Record->PreparedBaseProperty == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR)
            View->SlateRenderer->ClearVisualBackgroundColor(Record->Node);
        else if (Record->PreparedBaseProperty == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D)
            View->SlateRenderer->ClearVisualTransform(Record->Node, Record->Element.get(), Target);
    }
    for (const auto& Saved : Record->BaseLocalProperties) {
        if (Saved.second) Record->Element->SetProperty(Saved.first, *Saved.second);
        else Record->Element->RemoveProperty(Saved.first);
    }
    View->MarkContentDirty();
    return 1;
}
static Rml::Element* ResolveAnimatedElement(const RmlUE_AnimatedPropertyUpdate& Update,
    bool ViewValidated = false, RmlUE_View::AnimationTargetRecord** OutTarget = nullptr)
{
    if (OutTarget) *OutTarget = nullptr;
    if ((!ViewValidated && !ValidView(Update.View)) || !Update.View) return nullptr;
    if (!Update.Target) {
        const auto It = Update.View->Nodes.find(Update.Node);
        return It != Update.View->Nodes.end() && It->second.Element ? It->second.Element.get() : nullptr;
    }
    auto* Record = Update.View->FindAnimationTarget(Update.Target);
    if (OutTarget) *OutTarget = Record;
    return Record && (!Update.Node || Record->Node == Update.Node) ? Record->Element.get() : nullptr;
}
int RmlUE_ApplyAnimatedProperties(const RmlUE_AnimatedPropertyUpdate* Updates, int Count)
{
    if (Count < 0 || Count > 1048576 || (Count && !Updates)) return Fail("Invalid animated property update batch.");
    if (!Count) return 0;
    thread_local std::vector<Rml::Element*> Elements;
    thread_local std::vector<RmlUE_View*> DirtyViews;
    Elements.clear();
    Elements.reserve(static_cast<size_t>(Count));
    DirtyViews.clear();
    DirtyViews.reserve(static_cast<size_t>(Count));
    RmlUE_View* ValidatedView = nullptr;
    for (int I = 0; I < Count; ++I) {
        const auto& Update = Updates[I];
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY) {
            if (!std::isfinite(Update.Values[0]) || Update.Values[0] < 0.f || Update.Values[0] > 1.f)
                return Fail("Invalid typed opacity update.");
        }
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D) {
            for (float Value : Update.Values)
                if (!std::isfinite(Value)) return Fail("Invalid typed Transform2D update.");
        }
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_VISIBILITY) {
            if (Update.Values[0] != 0.f && Update.Values[0] != 1.f)
                return Fail("Invalid typed visibility update.");
        }
        else if (Update.Property >= RMLUE_ANIMATED_PROPERTY_COLOR &&
            Update.Property <= RMLUE_ANIMATED_PROPERTY_IMAGE_COLOR) {
            for (int Component = 0; Component < 4; ++Component)
                if (!std::isfinite(Update.Values[Component]) || Update.Values[Component] < 0.f || Update.Values[Component] > 1.f)
                    return Fail("Invalid typed color update.");
        }
        else if (Update.Property >= RMLUE_ANIMATED_PROPERTY_LEFT_PX &&
            Update.Property <= RMLUE_ANIMATED_PROPERTY_HEIGHT_PX) {
            if (!std::isfinite(Update.Values[0]) ||
                ((Update.Property == RMLUE_ANIMATED_PROPERTY_WIDTH_PX ||
                    Update.Property == RMLUE_ANIMATED_PROPERTY_HEIGHT_PX) && Update.Values[0] < 0.f))
                return Fail("Invalid typed layout scalar update.");
        }
        else return Fail("Unknown typed animated property.");
        if (Update.View != ValidatedView) {
            if (!ValidView(Update.View)) return Fail("Animated property update targets a stale view or node.");
            ValidatedView = Update.View;
        }
        auto* Element = ResolveAnimatedElement(Update, true);
        if (!Element) return Fail("Animated property update targets a stale view or node.");
        Elements.push_back(Element);
        if (std::find(DirtyViews.begin(), DirtyViews.end(), Update.View) == DirtyViews.end())
            DirtyViews.push_back(Update.View);
    }
    for (int I = 0; I < Count; ++I) {
        const auto& Update = Updates[I];
        bool Applied = false;
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY) {
            Applied = Elements[static_cast<size_t>(I)]->SetProperty(
                Rml::PropertyId::Opacity, Rml::Property(Update.Values[0], Rml::Unit::NUMBER));
        }
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D) {
            Rml::Transform::PrimitiveList Primitives{
                Rml::Transforms::Translate2D(Update.Values[0], Update.Values[1], Rml::Unit::PX),
                Rml::Transforms::Rotate2D(Update.Values[4], Rml::Unit::DEG),
                Rml::Transforms::Skew2D(Update.Values[5], Update.Values[6], Rml::Unit::DEG),
                Rml::Transforms::Scale2D(Update.Values[2], Update.Values[3])};
            Applied = Elements[static_cast<size_t>(I)]->SetProperty(
                Rml::PropertyId::Transform, Rml::Transform::MakeProperty(std::move(Primitives)));
        }
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_VISIBILITY) {
            Applied = Elements[static_cast<size_t>(I)]->SetProperty(Rml::PropertyId::Visibility,
                Rml::Property(Update.Values[0] > 0.f ? Rml::Style::Visibility::Visible : Rml::Style::Visibility::Hidden));
        }
        else if (Update.Property >= RMLUE_ANIMATED_PROPERTY_COLOR &&
            Update.Property <= RMLUE_ANIMATED_PROPERTY_IMAGE_COLOR) {
            const auto Byte = [](float Value) { return static_cast<uint8_t>(std::lround(Value * 255.f)); };
            const Rml::Property ColorProperty(Rml::Colourb(Byte(Update.Values[0]), Byte(Update.Values[1]),
                Byte(Update.Values[2]), Byte(Update.Values[3])), Rml::Unit::COLOUR);
            if (Update.Property == RMLUE_ANIMATED_PROPERTY_BORDER_COLOR) {
                Applied = Elements[static_cast<size_t>(I)]->SetProperty(Rml::PropertyId::BorderTopColor, ColorProperty) &&
                    Elements[static_cast<size_t>(I)]->SetProperty(Rml::PropertyId::BorderRightColor, ColorProperty) &&
                    Elements[static_cast<size_t>(I)]->SetProperty(Rml::PropertyId::BorderBottomColor, ColorProperty) &&
                    Elements[static_cast<size_t>(I)]->SetProperty(Rml::PropertyId::BorderLeftColor, ColorProperty);
            }
            else {
                const Rml::PropertyId PropertyId = Update.Property == RMLUE_ANIMATED_PROPERTY_COLOR ? Rml::PropertyId::Color :
                    Update.Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR ? Rml::PropertyId::BackgroundColor : Rml::PropertyId::ImageColor;
                Applied = Elements[static_cast<size_t>(I)]->SetProperty(PropertyId, ColorProperty);
            }
        }
        else {
            Rml::PropertyId PropertyId = Rml::PropertyId::Invalid;
            switch (Update.Property) {
            case RMLUE_ANIMATED_PROPERTY_LEFT_PX: PropertyId = Rml::PropertyId::Left; break;
            case RMLUE_ANIMATED_PROPERTY_TOP_PX: PropertyId = Rml::PropertyId::Top; break;
            case RMLUE_ANIMATED_PROPERTY_RIGHT_PX: PropertyId = Rml::PropertyId::Right; break;
            case RMLUE_ANIMATED_PROPERTY_BOTTOM_PX: PropertyId = Rml::PropertyId::Bottom; break;
            case RMLUE_ANIMATED_PROPERTY_WIDTH_PX: PropertyId = Rml::PropertyId::Width; break;
            case RMLUE_ANIMATED_PROPERTY_HEIGHT_PX: PropertyId = Rml::PropertyId::Height; break;
            default: break;
            }
            Applied = PropertyId != Rml::PropertyId::Invalid &&
                Elements[static_cast<size_t>(I)]->SetProperty(
                    PropertyId, Rml::Property(Update.Values[0], Rml::Unit::PX));
        }
        if (!Applied) return Fail("Cannot apply typed animated property update.");
    }
    for (RmlUE_View* View : DirtyViews) View->MarkContentDirty();
    return Count;
}
static int ApplyAnimatedVisualPropertiesImpl(
    const RmlUE_AnimatedPropertyUpdate* Updates, int Count, uint8_t* Accepted,
    RmlUE_AnimatedVisualCommitStats* Stats)
{
    // Background color is retained only when the renderer recorded an independent, untextured
    // Background paint role. Other colors continue through the property sink because text, image,
    // material, shadow, and border-side ownership/operations are not represented yet.
    using Clock = std::chrono::steady_clock;
    const auto NanosecondsSince = [](Clock::time_point Start) -> uint64_t {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - Start).count());
    };
    if (Stats) *Stats = {};
    if (Count < 0 || Count > 1048576 || (Count && (!Updates || !Accepted)))
    {
        Fail("Invalid animated visual property update batch.");
        return -1;
    }
    if (!Count) return 0;
    thread_local std::vector<float> BaseOpacities;
    thread_local std::vector<Rml::Element*> Elements;
    thread_local std::vector<void*> PreparedOpacityStates;
    thread_local std::vector<void*> PreparedBackgroundColorStates;
    BaseOpacities.clear();
    BaseOpacities.reserve(static_cast<size_t>(Count));
    Elements.clear();
    Elements.reserve(static_cast<size_t>(Count));
    PreparedOpacityStates.clear();
    PreparedOpacityStates.reserve(static_cast<size_t>(Count));
    PreparedBackgroundColorStates.clear();
    PreparedBackgroundColorStates.reserve(static_cast<size_t>(Count));
    const Clock::time_point ValidateStart = Stats ? Clock::now() : Clock::time_point{};
    RmlUE_View* ValidatedView = nullptr;
    for (int I = 0; I < Count; ++I) {
        Accepted[I] = 0;
        const auto& Update = Updates[I];
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY &&
            (!std::isfinite(Update.Values[0]) || Update.Values[0] < 0.f || Update.Values[0] > 1.f))
        {
            Fail("Invalid visual opacity update.");
            return -1;
        }
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D)
            for (float Value : Update.Values)
                if (!std::isfinite(Value)) { Fail("Invalid visual Transform2D update."); return -1; }
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR)
            for (int Channel = 0; Channel < 4; ++Channel)
                if (!std::isfinite(Update.Values[Channel]) || Update.Values[Channel] < 0.f || Update.Values[Channel] > 1.f)
                { Fail("Invalid visual background-color update."); return -1; }
        if (Update.View != ValidatedView) {
            if (!ValidView(Update.View)) return -1;
            ValidatedView = Update.View;
        }
        RmlUE_View::AnimationTargetRecord* TargetRecord = nullptr;
        auto* Element = ResolveAnimatedElement(Update, true, &TargetRecord);
        if (!Element) return -1;
        Elements.push_back(Element);
        PreparedOpacityStates.push_back(TargetRecord ? TargetRecord->PreparedOpacityState : nullptr);
        PreparedBackgroundColorStates.push_back(TargetRecord ? TargetRecord->PreparedBackgroundColorState : nullptr);
        BaseOpacities.push_back((Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY ||
            Update.Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR) && Update.View->SlateRenderer
            ? Element->GetComputedValues().opacity() : 0.f);
    }
    if (Stats) Stats->ValidateNanoseconds = NanosecondsSince(ValidateStart);
    const Clock::time_point PrepareStart = Stats ? Clock::now() : Clock::time_point{};
    thread_local std::vector<uint8_t> PreparedTransforms;
    thread_local std::vector<SlateCommandRenderer*> TransformRenderers;
    PreparedTransforms.assign(static_cast<size_t>(Count), 0);
    TransformRenderers.clear();
    for (int I = 0; I < Count; ++I)
    {
        const auto& Update = Updates[I];
        if (Update.Property != RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D || !Update.View->SlateRenderer) continue;
        SlateCommandRenderer* Renderer = Update.View->SlateRenderer.get();
        if (std::find(TransformRenderers.begin(), TransformRenderers.end(), Renderer) == TransformRenderers.end())
        {
            TransformRenderers.push_back(Renderer);
            Renderer->BeginVisualTransformBatch();
        }
    }
    if (Stats) Stats->PrepareNanoseconds = NanosecondsSince(PrepareStart);
    const Clock::time_point ApplyStart = Stats ? Clock::now() : Clock::time_point{};
    int Applied = 0;
    for (int I = 0; I < Count; ++I) {
        const auto& Update = Updates[I];
        if (!Update.View->SlateRenderer) continue;
        bool Committed = false;
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY &&
            BaseOpacities[static_cast<size_t>(I)] > 1.f / 255.f)
            Committed = Update.View->SlateRenderer->SetVisualOpacity(
                Update.Node, Update.Values[0], BaseOpacities[static_cast<size_t>(I)],
                PreparedOpacityStates[static_cast<size_t>(I)]);
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR)
            Committed = Update.View->SlateRenderer->SetVisualBackgroundColor(
                Update.Node, Update.Values, BaseOpacities[static_cast<size_t>(I)],
                PreparedBackgroundColorStates[static_cast<size_t>(I)]);
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D) {
            Committed = Update.View->SlateRenderer->PrepareVisualTransform(
                Update.Node, Elements[static_cast<size_t>(I)], Update.Values, Update.Target);
            if (Committed) {
                PreparedTransforms[static_cast<size_t>(I)] = 1;
                continue;
            }
        }
        if (!Committed) continue;
        Accepted[I] = 1;
        ++Applied;
    }
    if (Stats) Stats->ApplyNanoseconds = NanosecondsSince(ApplyStart);
    const Clock::time_point SynchronizeStart = Stats ? Clock::now() : Clock::time_point{};
    for (SlateCommandRenderer* Renderer : TransformRenderers)
        Renderer->SynchronizeVisualTransformBatch();
    if (Stats) Stats->SynchronizeNanoseconds = NanosecondsSince(SynchronizeStart);
    const Clock::time_point PublishStart = Stats ? Clock::now() : Clock::time_point{};
    for (int I = 0; I < Count; ++I) {
        if (!PreparedTransforms[static_cast<size_t>(I)]) continue;
        const auto& Update = Updates[I];
        if (!Update.View->SlateRenderer->PublishVisualTransform(
            Update.Node, Elements[static_cast<size_t>(I)], Update.Target))
            continue;
        Accepted[I] = 1;
        ++Applied;
    }
    if (Stats) Stats->PublishNanoseconds = NanosecondsSince(PublishStart);
    return Applied;
}
int RmlUE_ApplyAnimatedVisualProperties(
    const RmlUE_AnimatedPropertyUpdate* Updates, int Count, uint8_t* Accepted)
{
    return ApplyAnimatedVisualPropertiesImpl(Updates, Count, Accepted, nullptr);
}
int RmlUE_ApplyAnimatedVisualPropertiesProfiled(
    const RmlUE_AnimatedPropertyUpdate* Updates, int Count, uint8_t* Accepted,
    RmlUE_AnimatedVisualCommitStats* Stats)
{
    if (!Stats) return Fail("Animated visual commit profiling requires an output stats struct.");
    return ApplyAnimatedVisualPropertiesImpl(Updates, Count, Accepted, Stats);
}
int RmlUE_ClearAnimatedVisualProperties(const RmlUE_AnimatedPropertyUpdate* Updates, int Count)
{
    if (Count < 0 || Count > 1048576 || (Count && !Updates)) return -1;
    RmlUE_View* ValidatedView = nullptr;
    for (int I = 0; I < Count; ++I) {
        if (Updates[I].View != ValidatedView) {
            if (!ValidView(Updates[I].View)) return -1;
            ValidatedView = Updates[I].View;
        }
        if (!ResolveAnimatedElement(Updates[I], true)) return -1;
    }
    int Cleared = 0;
    for (int I = 0; I < Count; ++I) {
        const auto& Update = Updates[I];
        if (!Update.View->SlateRenderer) continue;
        if (Update.Property == RMLUE_ANIMATED_PROPERTY_OPACITY &&
            Update.View->SlateRenderer->ClearVisualOpacity(Update.Node)) ++Cleared;
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR &&
            Update.View->SlateRenderer->ClearVisualBackgroundColor(Update.Node)) ++Cleared;
        else if (Update.Property == RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D &&
            Update.View->SlateRenderer->ClearVisualTransform(
                Update.Node, ResolveAnimatedElement(Update, true), Update.Target)) ++Cleared;
    }
    return Cleared;
}
int RmlUE_ApplyFloatProperties(const RmlUE_FloatPropertyUpdate* Updates, int Count)
{
    if (Count < 0 || Count > 1048576 || (Count && !Updates)) return Fail("Invalid float property update batch.");
    std::vector<RmlUE_AnimatedPropertyUpdate> Tagged;
    Tagged.reserve(static_cast<size_t>(std::max(Count, 0)));
    for (int I = 0; I < Count; ++I)
        Tagged.push_back({Updates[I].View, Updates[I].Node, Updates[I].Property, {Updates[I].Value, 0.f, 0.f, 0.f, 0.f}, 0});
    return RmlUE_ApplyAnimatedProperties(Tagged.data(), Count);
}
