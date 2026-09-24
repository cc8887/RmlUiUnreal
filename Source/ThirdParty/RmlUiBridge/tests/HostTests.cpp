#include "RmlUiBridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

static int Checks = 0;
static void Require(bool Condition, const char* Message)
{
    ++Checks;
    if (!Condition) {
        std::fprintf(stderr, "FAIL [%d]: %s; %s\n", Checks, Message, RmlUE_GetLastError());
        std::exit(1);
    }
}
static bool Near(float A, float B, float Epsilon = 0.6f) { return std::abs(A - B) <= Epsilon; }
static int ReadFile(void*, const char* Path, unsigned char** Data, size_t* Size)
{
    std::ifstream File(std::filesystem::u8path(Path), std::ios::binary);
    if (!File) return 0;
    std::vector<unsigned char> Bytes((std::istreambuf_iterator<char>(File)), {});
    *Size = Bytes.size(); *Data = static_cast<unsigned char*>(std::malloc(Bytes.size() + 1));
    if (!Bytes.empty()) std::memcpy(*Data, Bytes.data(), Bytes.size());
    return 1;
}
static int LoadImage(void*, const char*, unsigned char** Data, int* Width, int* Height)
{
    *Width = *Height = 1; *Data = static_cast<unsigned char*>(std::malloc(4));
    std::memset(*Data, 255, 4); return 1;
}
static void FreeBuffer(void*, void* Data) { std::free(Data); }
static void Log(void*, int Level, const char* Message) { std::fprintf(stderr, "RML[%d] %s\n", Level, Message); }

struct Fixture
{
    RmlUE_View* View = nullptr;
    explicit Fixture(const char* Markup, bool Slate = false) : View(Slate ? RmlUE_CreateSlateView(640, 480, 1) : RmlUE_CreateView(640, 480, 1))
    {
        Require(View != nullptr, "create host test native view");
        Require(RmlUE_LoadDocumentFromMemory(View, Markup, "host-tests.rml") != 0, "load host fixture");
    }
    ~Fixture()
    {
        RmlUE_SetNodeEventCallback(View, nullptr, nullptr);
        RmlUE_SetLayoutCallback(View, nullptr, nullptr);
        RmlUE_DestroyView(View);
    }
    void Update() { Require(RmlUE_Update(View) != 0, "complete explicit layout"); }
    RmlUE_Node Node(const char* Id)
    {
        const auto Result = RmlUE_FindNode(View, Id);
        Require(Result != 0, Id); return Result;
    }
    RmlUE_NodeMetrics Measure(RmlUE_Node Node)
    {
        RmlUE_NodeMetrics Result{}; RmlUE_LayoutInfo Info{};
        Require(RmlUE_MeasureNodes(View, &Node, 1, &Result, &Info) != 0, "read node geometry snapshot");
        Require(Result.Valid != 0, "measured node remains attached"); return Result;
    }
    std::string Attribute(RmlUE_Node Node, const char* Name)
    {
        char Value[256]{};
        Require(RmlUE_GetNodeAttribute(View, Node, Name, Value, sizeof(Value)) != 0, "read attribute");
        return Value;
    }
    void MoveTo(RmlUE_Node Node)
    {
        const auto M = Measure(Node);
        RmlUE_MouseMove(View, int(M.X + M.Width / 2), int(M.Y + M.Height / 2), 0);
    }
    void Click(RmlUE_Node Node) { MoveTo(Node); RmlUE_MouseButton(View, 0, 1, 0); RmlUE_MouseButton(View, 0, 0, 0); }
    RmlUE_Frame Render()
    {
        RmlUE_Frame Frame{}; Require(RmlUE_Render(View, &Frame) != 0, "render actual DX11 pixels"); return Frame;
    }
};

static void TestVariablesAndNodes()
{
    Fixture F(R"(<rml><head><style>
body { margin:0; width:640px; height:480px; font-family:LatoLatin; background-color:#000; }
:root { --size:52px; --alpha:0.5; }
#branch { --size:64px; }
.sized { display:block; width:var(--size); height:20px; }
#alpha { position:absolute; left:300px; top:20px; width:40px; height:40px; background-color:hsla(210,50%,40%,var(--alpha)); }
</style></head><body><div id="inherited" class="sized item"/><div id="branch"><div id="overridden" class="sized item"/></div><div id="alpha"/></body></rml>)");
    F.Update();
    const auto Root = RmlUE_GetRootNode(F.View), Inherited = F.Node("inherited"), Branch = F.Node("branch"), Overridden = F.Node("overridden");
    char RootSize[128]{};
    RmlUE_GetComputedProperty(F.View, Root, "--size", RootSize, sizeof(RootSize));
    std::fprintf(stderr, "root variable diagnostic: root=%u --size='%s', descendant width=%g, branch width=%g\n", Root, RootSize, F.Measure(Inherited).Width, F.Measure(Overridden).Width);
    Require(Near(F.Measure(Inherited).Width, 52), ":root CSS custom property inherited by descendant");
    Require(Near(F.Measure(Overridden).Width, 64), "nearest ancestor custom property overrides root token");
    Require(RmlUE_SetNodeProperty(F.View, Root, "--size", "76px") != 0, "mutate custom property at document root");
    F.Update();
    Require(Near(F.Measure(Inherited).Width, 76), "root token mutation relayouts dependent descendant");
    Require(Near(F.Measure(Overridden).Width, 64), "ancestor override survives root token mutation");
    auto Frame = F.Render();
    const int InitialBlue = Frame.Pixels[(30 * Frame.Width + 310) * 4];
    Require(InitialBlue > 65 && InitialBlue < 90, "hsla alpha 0.5 produces half-blue over black");
    Require(RmlUE_SetNodeProperty(F.View, Root, "--alpha", "0.25") != 0, "change runtime Tailwind-style alpha token");
    Frame = F.Render();
    const int QuarterBlue = Frame.Pixels[(30 * Frame.Width + 310) * 4];
    std::fprintf(stderr, "custom-property hsla blue channel: alpha .5=%d -> alpha .25=%d\n", InitialBlue, QuarterBlue);
    Require(QuarterBlue > 28 && QuarterBlue < 48 && Near(float(QuarterBlue * 2), float(InitialBlue), 4), "native hsla parser applies changed fractional alpha to pixels");

    Require(RmlUE_QueryNode(F.View, Root, "#branch > .item") == Overridden, "query selector child combinator preserves tracked identity");
    Require(RmlUE_QueryNodes(F.View, Root, ".item", nullptr, 0) == 2, "query count supports two-pass allocation");
    RmlUE_Node Matches[3]{0, 0, 0xabcdefu};
    Require(RmlUE_QueryNodes(F.View, Root, ".item", Matches, 1) == 2 && Matches[0] == Inherited && Matches[1] == 0, "bounded query returns full count without overwriting capacity");
    Require(RmlUE_QueryNodes(F.View, Root, ".item", Matches, 2) == 2 && Matches[1] == Overridden && Matches[2] == 0xabcdefu, "query maintains DOM order and buffer boundary");
    Require(RmlUE_ContainsNode(F.View, Root, Overridden) && RmlUE_ContainsNode(F.View, Branch, Branch), "contains includes self and descendants");
    Require(!RmlUE_ContainsNode(F.View, Branch, Inherited), "contains rejects sibling subtree");
    Require(RmlUE_ChildNodes(F.View, Branch, Matches, 2) == 1 && Matches[0] == Overridden, "children returns direct DOM children");
    Require(RmlUE_SetNodeClass(F.View, Inherited, "selected", 1) != 0 && RmlUE_QueryNode(F.View, Root, ".selected") == Inherited, "classList add changes selector matches");
    Require(F.Attribute(Inherited, "class").find("selected") != std::string::npos, "class attribute and classList contains observe native class addition");
    Require(RmlUE_SetNodeClass(F.View, Inherited, "selected", 0) != 0 && RmlUE_QueryNode(F.View, Root, ".selected") == 0, "classList remove changes selector matches");
    Require(F.Attribute(Inherited, "class").find("selected") == std::string::npos, "class attribute and classList contains observe native class removal");
    Require(!RmlUE_SetNodeClass(F.View, Inherited, "two classes", 1), "classList rejects whitespace token");
    const auto Child = RmlUE_CreateNode(F.View, 0, "span"), Text = RmlUE_CreateNode(F.View, 1, "dynamic");
    Require(Child && Text && Child != Text, "new element and text receive distinct stable handles");
    Require(RmlUE_InsertNode(F.View, Text, Child, 0) && RmlUE_InsertNode(F.View, Child, Branch, Overridden), "insert text subtree before anchor");
    Require(RmlUE_ChildNodes(F.View, Branch, Matches, 2) == 2 && Matches[0] == Child && Matches[1] == Overridden, "children reflects insertion order");
    Require(RmlUE_InsertNode(F.View, Child, Root, 0) && RmlUE_ParentNode(F.View, Child) == Root && RmlUE_IsNodeValid(F.View, Text), "reparent preserves element and descendant handles");
    Require(!RmlUE_InsertNode(F.View, Child, Text, 0), "reject insertion into own subtree");
    Require(RmlUE_RemoveNode(F.View, Child) != 0 && !RmlUE_IsNodeValid(F.View, Child) && !RmlUE_IsNodeValid(F.View, Text), "removal invalidates entire subtree handles");
    Require(!RmlUE_RemoveNode(F.View, Child) && !RmlUE_SetNodeText(F.View, Text, "stale"), "stale removed handles are rejected");
    const auto Replacement = RmlUE_CreateNode(F.View, 0, "div");
    Require(Replacement && Replacement != Child && Replacement != Text, "destroyed node handles are never reused");
    Require(RmlUE_QueryNodes(F.View, Child, "*", nullptr, 0) == -1 && RmlUE_ChildNodes(F.View, Child, nullptr, 0) == -1, "queries reject stale root handles");
    Require(!RmlUE_RemoveNode(F.View, Root), "document root cannot be removed");
    std::fprintf(stderr, "PASS custom properties and stable node APIs\n");
}

struct LayoutEvents { int Calls = 0; uint64_t Revision = 0; };
static void OnLayout(void* User, uint64_t Revision) { auto& E = *static_cast<LayoutEvents*>(User); ++E.Calls; E.Revision = Revision; }
static void TestMeasure()
{
    Fixture F(R"(<rml><head><style>
body { margin:0; width:640px; height:480px; font-family:LatoLatin; }
#box { position:absolute; left:20px; top:30px; width:80px; height:40px; transform:translate(35px,18px); }
#clip { position:absolute; left:200px; top:30px; width:80px; height:70px; overflow:hidden; }
#overflow { display:block; width:160px; height:140px; }
</style></head><body><div id="box"/><div id="clip"><div id="overflow"/></div></body></rml>)");
    LayoutEvents Events; RmlUE_SetLayoutCallback(F.View, OnLayout, &Events); F.Update();
    const auto Box = F.Node("box"), Overflow = F.Node("overflow");
    RmlUE_Node Nodes[]{Box, Overflow, 0, 0x7fffffffu};
    RmlUE_NodeMetrics Metrics[4]{}; RmlUE_LayoutInfo Before{}, After{};
    Require(RmlUE_MeasureNodes(F.View, Nodes, 4, Metrics, &Before) != 0, "batch valid and invalid handles");
    Require(Before.Revision == Events.Revision && Before.Revision > 0 && Before.Width == 640 && Before.Height == 480 && Near(Before.Dpi, 1), "geometry snapshot reports completed layout revision and viewport");
    Require(Metrics[0].Valid && Metrics[0].Visible && Near(Metrics[0].LayoutX, 20) && Near(Metrics[0].LayoutY, 30), "untransformed layout position is separate from client rect");
    std::fprintf(stderr, "measure diagnostic: layout=(%g,%g,%g,%g), transformed=(%g,%g,%g,%g)\n", Metrics[0].LayoutX, Metrics[0].LayoutY, Metrics[0].LayoutWidth, Metrics[0].LayoutHeight, Metrics[0].X, Metrics[0].Y, Metrics[0].Width, Metrics[0].Height);
    Require(Near(Metrics[0].X, 55) && Near(Metrics[0].Y, 48) && Near(Metrics[0].Width, 80) && Near(Metrics[0].Height, 40), "client rectangle includes native transform");
    Require(Near(Metrics[1].ClipX, 200) && Near(Metrics[1].ClipY, 30) && Near(Metrics[1].ClipWidth, 80) && Near(Metrics[1].ClipHeight, 70), "ancestor overflow intersects measured clip rectangle");
    Require(!Metrics[2].Valid && !Metrics[3].Valid && Metrics[3].Node == Nodes[3], "invalid batch rows preserve requested identity and zero valid flag");
    const int LayoutCalls = Events.Calls;
    Require(RmlUE_SetNodeProperty(F.View, Box, "width", "120px") != 0, "queue layout-affecting style change");
    Require(RmlUE_MeasureNodes(F.View, Nodes, 4, Metrics, &After) && After.Revision == Before.Revision && Events.Calls == LayoutCalls && Near(Metrics[0].LayoutWidth, 80), "MeasureNodes does not force pending layout or increment callbacks");
    F.Update();
    Require(RmlUE_MeasureNodes(F.View, Nodes, 4, Metrics, &After) && After.Revision > Before.Revision && Events.Calls == LayoutCalls + 1 && Near(Metrics[0].LayoutWidth, 120), "single explicit update resolves mutation and publishes one revision");
    Require(RmlUE_MeasureNodes(F.View, nullptr, 0, nullptr, &After) && !RmlUE_MeasureNodes(F.View, nullptr, -1, nullptr, &After) && !RmlUE_MeasureNodes(F.View, Nodes, 1, nullptr, &After), "batch geometry validates bounds and supports empty snapshot");
    std::fprintf(stderr, "PASS geometry snapshots and transform coordinates\n");
}

struct EventRecord
{
    uint32_t Listener, Abi, Size;
    RmlUE_Node Target, Current, Related;
    std::string Type, Key, Code, PointerType;
    int DefaultPrevented, Cancelable, Pointer, Buttons, Repeat;
    float X, Y, LocalX, LocalY;
};
struct EventLog
{
    std::vector<EventRecord> Items;
    uint32_t Prevent = 0;
    int Count(uint32_t Id) const { return int(std::count_if(Items.begin(), Items.end(), [Id](const auto& E) { return E.Listener == Id; })); }
    const EventRecord& Last(uint32_t Id) const
    {
        for (auto I = Items.rbegin(); I != Items.rend(); ++I) if (I->Listener == Id) return *I;
        Require(false, "required event was dispatched"); return Items.back();
    }
};
static int OnEvent(void* User, uint32_t Listener, const RmlUE_NodeEvent* E)
{
    auto& Log = *static_cast<EventLog*>(User);
    Log.Items.push_back({Listener, E->AbiVersion, E->StructSize, E->Target, E->CurrentTarget, E->RelatedTarget,
        E->Type ? E->Type : "", E->KeyName ? E->KeyName : "", E->Code ? E->Code : "", E->PointerType ? E->PointerType : "",
        E->DefaultPrevented, E->Cancelable, E->PointerId, E->Buttons, E->Repeat, E->X, E->Y, E->LocalX, E->LocalY});
    return Listener == Log.Prevent ? 8 : 0;
}
static void Listen(Fixture& F, RmlUE_Node Node, const char* Type, uint32_t Id)
{
    Require(RmlUE_ListenNode(F.View, Node, Type, Id, 0) != 0, "attach host listener");
}
static void Key(Fixture& F, int Code, int Flags = 0) { RmlUE_Key(F.View, Code, 1, Flags); RmlUE_Key(F.View, Code, 0, Flags); }

static void TestDefaultAndModal()
{
    Fixture F(R"(<rml><head><style>
body { margin:0; width:640px; height:480px; font-family:LatoLatin; font-size:16px; }
input, button { width:100px; height:24px; tab-index:auto; }
#check { position:absolute; left:10px; top:10px; width:24px; height:24px; }
#background { position:absolute; left:10px; top:60px; }
#outer { position:absolute; left:180px; top:60px; width:220px; height:150px; }
#first { position:absolute; left:10px; top:10px; } #second { position:absolute; left:10px; top:50px; }
#inner { position:absolute; left:180px; top:240px; width:200px; height:80px; display:none; }
</style></head><body><input id="check" type="checkbox"/><input id="background" type="text" value="base"/>
<div id="outer"><input id="first" type="text" value=""/><button id="second">Second</button></div>
<div id="inner"><input id="nested" type="text" value=""/></div></body></rml>)");
    EventLog Events; RmlUE_SetNodeEventCallback(F.View, OnEvent, &Events); F.Update();
    const auto Root = RmlUE_GetRootNode(F.View), Check = F.Node("check"), Background = F.Node("background"), Outer = F.Node("outer"), First = F.Node("first"), Second = F.Node("second"), Inner = F.Node("inner"), Nested = F.Node("nested");
    Listen(F, Check, "click", 10); Listen(F, Check, "click", 11); Listen(F, Root, "click", 12); Listen(F, Check, "change", 13);
    Events.Prevent = 10; F.Click(Check);
    Require(Events.Count(10) == 1 && Events.Count(11) == 1 && Events.Count(12) == 1, "preventDefault retains later target and bubbling listeners");
    Require(Events.Last(11).DefaultPrevented && Events.Last(12).DefaultPrevented && Events.Last(10).Cancelable, "defaultPrevented propagates independently of event propagation");
    Require(F.Attribute(Check, "checked") == "false" && Events.Count(13) == 0, "preventDefault blocks native checkbox toggle and change");
    Require(Events.Last(11).Abi == RMLUE_HOST_ABI_VERSION && Events.Last(11).Size == sizeof(RmlUE_NodeEvent), "native callback carries explicit event ABI and size");
    Events.Prevent = 0; F.Click(Check);
    Require(F.Attribute(Check, "checked") == "true" && Events.Count(13) == 1, "uncancelled click still performs checkbox native default");

    Listen(F, Background, "click", 20); Listen(F, First, "keydown", 21);
    Listen(F, Background, "blur", 22); Listen(F, First, "focus", 23);
    Require(RmlUE_FocusNode(F.View, Background) != 0, "background can focus before modal opens");
    Require(RmlUE_SetModalRoot(F.View, Outer, First) != 0 && RmlUE_ActiveNode(F.View) == First, "opening modal applies explicit initial focus");
    // Verify focus transition endpoints independently of modal scope changes below.
    Key(F, 9); Require(RmlUE_ActiveNode(F.View) == Second, "modal Tab advances to next control");
    Key(F, 9); Require(RmlUE_ActiveNode(F.View) == First, "modal Tab wraps at last control");
    Key(F, 9, 1); Require(RmlUE_ActiveNode(F.View) == Second, "modal Shift-Tab wraps backwards");
    Require(!RmlUE_FocusNode(F.View, Background), "modal rejects programmatic background focus");
    F.Click(Background);
    Require(Events.Count(20) == 0 && RmlUE_ContainsNode(F.View, Outer, RmlUE_ActiveNode(F.View)), "modal blocks background click and focus transfer");
    RmlUE_Text(F.View, "X");
    Require(F.Attribute(Background, "value") == "base", "text cannot edit background while modal is active");
    Require(!RmlUE_SetModalRoot(F.View, Outer, Background), "modal rejects initial focus outside scope without clearing current modal");
    Require(RmlUE_SetNodeProperty(F.View, Inner, "display", "block") != 0, "show nested modal"); F.Update();
    Require(RmlUE_SetModalRoot(F.View, Inner, Nested) != 0 && RmlUE_ActiveNode(F.View) == Nested, "nested modal becomes sole active native scope");
    Key(F, 9); Require(RmlUE_ActiveNode(F.View) == Nested, "nested modal Tab remains in nested scope");
    F.Click(First); RmlUE_Text(F.View, "N");
    Require(RmlUE_ActiveNode(F.View) == Nested && F.Attribute(Nested, "value") == "N" && F.Attribute(First, "value").empty(), "parent modal is inert while child modal receives text");
    // The JavaScript modal stack restores the previous scope explicitly; the C API has one active scope.
    Require(RmlUE_SetNodeProperty(F.View, Inner, "display", "none") && RmlUE_SetModalRoot(F.View, Outer, Second), "closing nested modal restores prior root and opener"); F.Update();
    Require(RmlUE_ActiveNode(F.View) == Second, "parent modal opener focus restored");
    Require(RmlUE_SetModalRoot(F.View, 0, 0) && RmlUE_FocusNode(F.View, Background), "closing final modal releases background focus restriction");
    F.Click(Background); Require(Events.Count(20) == 1, "background click resumes after modal closes");
    Require(RmlUE_BlurNode(F.View, Background) && RmlUE_ActiveNode(F.View) != Background, "explicit blur clears active input");
    Require(RmlUE_FocusNode(F.View, Background) && RmlUE_FocusNode(F.View, First), "focus transition across sibling controls");
    std::fprintf(stderr, "relatedTarget diagnostic: blur=%u expected=%u, focus=%u expected=%u\n", Events.Last(22).Related, First, Events.Last(23).Related, Background);
    Require(Events.Last(22).Related == First && Events.Last(23).Related == Background, "blur and focus expose opposite endpoint as relatedTarget");
    std::fprintf(stderr, "PASS native preventDefault, modal keyboard and background isolation\n");
}

static void TestPointers()
{
    Fixture F(R"(<rml><head><style>
body { margin:0; width:640px; height:480px; font-family:LatoLatin; }
#capture { position:absolute; left:20px; top:30px; width:100px; height:80px; background-color:#f00; }
#other { position:absolute; left:300px; top:200px; width:100px; height:80px; background-color:#00f; }
#transformed { position:absolute; left:120px; top:300px; width:120px; height:100px; transform-origin:0px 0px; transform:scale(2) rotate(90deg); }
#rotated { position:absolute; left:10px; top:10px; width:50px; height:30px; background-color:#0f0; }
</style></head><body><div id="capture"/><div id="other"/><div id="transformed"><div id="rotated"/></div></body></rml>)");
    EventLog Events; RmlUE_SetNodeEventCallback(F.View, OnEvent, &Events); F.Update();
    const auto Capture = F.Node("capture"), Other = F.Node("other"), Root = RmlUE_GetRootNode(F.View);
    Listen(F, Capture, "gotpointercapture", 30); Listen(F, Capture, "lostpointercapture", 31); Listen(F, Capture, "pointermove", 32);
    Listen(F, Capture, "pointerup", 33); Listen(F, Capture, "pointercancel", 34); Listen(F, Root, "pointermove", 35);
    Require(!RmlUE_CaptureNode(F.View, Capture, 0), "capture requires active mouse press");
    F.MoveTo(Capture); RmlUE_MouseButton(F.View, 0, 1, 0);
    Require(RmlUE_CaptureNode(F.View, Capture, 0) && Events.Count(30) == 1, "mouse capture dispatches gotpointercapture");
    Require(Near(Events.Last(30).X, 70) && Near(Events.Last(30).Y, 70), "gotpointercapture retains last mouse client coordinates");
    Require(RmlUE_CaptureNode(F.View, Capture, 0) && Events.Count(30) == 1, "capturing same node twice is idempotent");
    F.MoveTo(Other);
    Require(Events.Last(32).Target == Capture && Events.Last(35).Target == Capture && Events.Last(32).Pointer == 0 && Events.Last(32).PointerType == "mouse" && Events.Last(32).Buttons == 1, "captured movement keeps target and mouse button metadata outside element");
    Require(Near(Events.Last(32).LocalX, Events.Last(32).X - 20) && Near(Events.Last(32).LocalY, Events.Last(32).Y - 30), "captured pointer reports local coordinates beyond element bounds");
    Require(!RmlUE_ReleaseCaptureNode(F.View, Other, 0), "nonowner cannot release capture");
    Require(RmlUE_ReleaseCaptureNode(F.View, Capture, 0) && Events.Count(31) == 1, "explicit release dispatches lostpointercapture once");
    Require(Near(Events.Last(31).X, 350) && Near(Events.Last(31).Y, 240), "lostpointercapture retains last mouse client coordinates");
    F.MoveTo(Other); Require(Events.Last(35).Target == Other, "released movement returns to hit testing");
    Require(RmlUE_CaptureNode(F.View, Capture, 0) != 0, "active mouse can recapture after release");
    RmlUE_MouseButton(F.View, 0, 0, 0);
    Require(Events.Count(33) == 1 && Events.Count(31) == 2 && Events.Last(33).Buttons == 0, "mouse up dispatches captured pointerup then implicit release");
    Require(!RmlUE_CaptureNode(F.View, Capture, 0), "mouse is inactive after final button release");
    RmlUE_Touch(F.View, 7, 60, 60, 0);
    Require(RmlUE_CaptureNode(F.View, Capture, 8) != 0, "touch capture uses native id plus one");
    Require(Near(Events.Last(30).X, 60) && Near(Events.Last(30).Y, 60), "touch gotcapture retains touch position independently of last mouse position");
    RmlUE_Touch(F.View, 7, 350, 240, 1);
    Require(Events.Last(32).Pointer == 8 && Events.Last(32).PointerType == "touch" && Events.Last(32).Target == Capture && Events.Last(32).Buttons == 1, "touch capture routes movement with distinct pointer identity and active button");
    RmlUE_Touch(F.View, 7, 350, 240, 3);
    Require(Events.Count(34) == 1 && Events.Last(34).Pointer == 8 && Events.Count(31) == 3, "touch cancellation dispatches cancel and lost capture");
    Require(Near(Events.Last(31).X, 350) && Near(Events.Last(31).Y, 240), "touch lostcapture retains final touch coordinates");
    Require(Events.Last(34).Buttons == 0, "touch pointercancel clears buttons metadata");
    F.MoveTo(Capture); RmlUE_MouseButton(F.View, 0, 1, 0);
    RmlUE_Touch(F.View, 8, 60, 60, 0);
    Require(RmlUE_CaptureNode(F.View, Capture, 9) != 0, "touch capture can coexist with pressed mouse");
    RmlUE_Touch(F.View, 8, 350, 240, 2);
    Require(Events.Last(33).Pointer == 9 && Events.Last(33).Buttons == 0, "touch pointerup is inactive even while separate mouse remains pressed");
    RmlUE_MouseButton(F.View, 0, 0, 0);
    const int LostBeforeRemoval = Events.Count(31);
    F.MoveTo(Capture); RmlUE_MouseButton(F.View, 0, 1, 0); Require(RmlUE_CaptureNode(F.View, Capture, 0) != 0, "capture before subtree removal");
    Require(RmlUE_RemoveNode(F.View, Capture) && !RmlUE_IsNodeValid(F.View, Capture), "delete captured node safely");
    Require(Events.Count(34) == 2 && Events.Count(31) == LostBeforeRemoval + 1 && Events.Last(34).Target == Capture, "deletion emits cancel and lostcapture before handle invalidation");
    Require(Events.Last(34).Buttons == 0, "deletion cancellation reports inactive pointer buttons");
    RmlUE_MouseButton(F.View, 0, 0, 0); F.MoveTo(Other);
    Require(Events.Last(35).Target == Other, "input continues after captured node destruction");
    const auto Rotated = F.Node("rotated"); Listen(F, Rotated, "pointermove", 36);
    F.Render();
    RmlUE_MouseMove(F.View, 70, 370, 0);
    Require(Events.Count(36) == 1 && Events.Last(36).Target == Rotated, "hit testing finds child under scaled and rotated parent");
    Require(Near(Events.Last(36).X, 70) && Near(Events.Last(36).Y, 370), "pointer client coordinates remain unprojected viewport pixels");
    Require(Near(Events.Last(36).LocalX, 25) && Near(Events.Last(36).LocalY, 15), "pointer local coordinates inverse-project scale and rotation exactly once");
    Listen(F, Other, "pointercancel", 40);
    F.MoveTo(Other); RmlUE_MouseButton(F.View, 0, 1, 0); RmlUE_FocusLost(F.View);
    Require(Events.Count(40) == 1 && Events.Last(40).Pointer == 0 && Events.Last(40).Buttons == 0 && Near(Events.Last(40).X, 350) && Near(Events.Last(40).Y, 240), "focus loss cancels uncaptured active mouse at its last position");
    RmlUE_FocusLost(F.View); Require(Events.Count(40) == 1, "repeated focus loss does not recancel completed mouse");
    RmlUE_Touch(F.View, 17, 350, 240, 0); RmlUE_FocusLost(F.View);
    Require(Events.Count(40) == 2 && Events.Last(40).Pointer == 18 && Events.Last(40).Buttons == 0 && Near(Events.Last(40).X, 350) && Near(Events.Last(40).Y, 240), "focus loss cancels uncaptured active touch independently of mouse");
    F.Click(Other); RmlUE_FocusLost(F.View);
    Require(Events.Count(40) == 2, "normal mouseup removes pending cancel target");
    RmlUE_Touch(F.View, 18, 350, 240, 0); RmlUE_Touch(F.View, 18, 350, 240, 2); RmlUE_FocusLost(F.View);
    Require(Events.Count(40) == 2, "normal touchend removes pending cancel target");
    F.MoveTo(Other); RmlUE_MouseButton(F.View, 0, 1, 0);
    Require(RmlUE_RemoveNode(F.View, Other) && Events.Count(40) == 3 && Events.Last(40).Buttons == 0 && !RmlUE_IsNodeValid(F.View, Other), "removing uncaptured pressed mouse target dispatches cancel before invalidation");
    RmlUE_MouseButton(F.View, 0, 0, 0);
    Listen(F, Rotated, "pointercancel", 41);
    RmlUE_Touch(F.View, 25, 70, 370, 0);
    Require(RmlUE_RemoveNode(F.View, Rotated) && Events.Count(41) == 1 && Events.Last(41).Pointer == 26 && Events.Last(41).Buttons == 0, "removing uncaptured touch target dispatches cancel with touch identity");
    RmlUE_FocusLost(F.View);
    Require(Events.Count(40) == 3 && Events.Count(41) == 1, "destroyed pointer targets are not cancelled twice on later focus loss");
    std::fprintf(stderr, "PASS mouse/touch capture, release and deletion cancellation\n");
}

struct PixelBounds { int X0 = 1000000, Y0 = 1000000, X1 = -1, Y1 = -1, Count = 0; };
static PixelBounds RedBounds(const RmlUE_Frame& Frame)
{
    PixelBounds B;
    for (int Y = 0; Y < Frame.Height; ++Y) for (int X = 0; X < Frame.Width; ++X) {
        const auto* P = Frame.Pixels + (Y * Frame.Width + X) * 4;
        if (P[2] > 240 && P[1] < 12 && P[0] < 12 && P[3] > 240) {
            B.X0 = std::min(B.X0, X); B.X1 = std::max(B.X1, X); B.Y0 = std::min(B.Y0, Y); B.Y1 = std::max(B.Y1, Y); ++B.Count;
        }
    }
    return B;
}
static void SaveFrame(const RmlUE_Frame& Frame, const std::filesystem::path& Path)
{
    if (Path.empty()) return;
    std::ofstream File(Path, std::ios::binary);
    File << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
    for (int I = 0; I < Frame.Width * Frame.Height; ++I) {
        const char RGB[]{char(Frame.Pixels[I * 4 + 2]), char(Frame.Pixels[I * 4 + 1]), char(Frame.Pixels[I * 4])};
        File.write(RGB, 3);
    }
    Require(File.good(), "save native animation pixel evidence");
}
static void TestAnimation(const std::filesystem::path& Output)
{
    Fixture F(R"(<rml><head><style>
body { margin:0; width:640px; height:480px; background-color:#000; }
#moving { position:absolute; left:20px; top:70px; width:24px; height:24px; background-color:#f00; }
</style></head><body><div id="moving"/></body></rml>)");
    const auto Moving = F.Node("moving");
    Require(!RmlUE_AnimateNode(F.View, Moving, "left", "20px", "220px", 2, 1), "animation rejects missing initial layout");
    auto Frame = F.Render(); const auto Start = RedBounds(Frame);
    Require(Start.Count >= 500 && Start.X0 == 20 && Start.Y0 == 70, "animation baseline located in actual pixel buffer");
    if (!Output.empty()) SaveFrame(Frame, Output / "host-animation-start.ppm");
    Require(RmlUE_AnimateNode(F.View, Moving, "left", "20px", "220px", 2, 1) != 0, "start native position animation after layout");
    F.Render();
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    Frame = F.Render(); const auto Moved = RedBounds(Frame);
    std::fprintf(stderr, "native animation pixels: x=%d -> %d, red pixels=%d -> %d\n", Start.X0, Moved.X0, Start.Count, Moved.Count);
    Require(Moved.Count >= 500 && Moved.X0 > Start.X0 + 5 && Moved.X0 < 210 && Moved.Y0 == Start.Y0, "native animation advances rendered position before reaching target");
    Require(Near(F.Measure(Moving).X, float(Moved.X0), 1.1f), "animated geometry agrees with actual colored pixels");
    if (!Output.empty()) SaveFrame(Frame, Output / "host-animation-moving.ppm");
    Require(RmlUE_CancelAnimation(F.View, Moving, "left") != 0, "cancel native animation at current value");
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    Frame = F.Render(); const auto Held = RedBounds(Frame);
    Require(Held.X0 == Moved.X0 && Held.X1 == Moved.X1 && Held.Count == Moved.Count, "cancel preserves current pixels instead of resetting or continuing animation");
    if (!Output.empty()) SaveFrame(Frame, Output / "host-animation-cancelled.ppm");
    Require(!RmlUE_AnimateNode(F.View, Moving, "left", "bad", "220px", 1, 1) && !RmlUE_AnimateNode(F.View, Moving, "left", "20px", "220px", 0, 1), "native animation validates property values and duration");
    std::fprintf(stderr, "PASS rendered animation displacement and cancellation retention\n");
}

static float ComputedFloat(RmlUE_View* View, RmlUE_Node Node, const char* Property)
{
    char Buffer[64]{};
    Require(RmlUE_GetComputedProperty(View, Node, Property, Buffer, sizeof(Buffer)) != 0, "read computed float property");
    return std::strtof(Buffer, nullptr);
}

static void TestAnimatedPropertyBatch()
{
    Fixture F(R"(<rml><head><style>body{margin:0;}#a,#b{position:absolute;left:20px;top:30px;display:block;opacity:1;width:10px;height:10px;transform-origin:0 0;background-color:#000;}</style></head><body><div id="a"/><div id="b"/></body></rml>)");
    F.Update();
    const auto A = F.Node("a");
    const auto B = F.Node("b");
    const RmlUE_FloatPropertyUpdate Updates[]{
        {F.View, A, RMLUE_FLOAT_PROPERTY_OPACITY, 0.25f},
        {F.View, B, RMLUE_FLOAT_PROPERTY_OPACITY, 0.75f},
    };
    Require(RmlUE_ApplyFloatProperties(Updates, 2) == 2, "apply two typed opacity values in one batch");
    Require(Near(ComputedFloat(F.View, A, "opacity"), 0.25f, 0.001f) && Near(ComputedFloat(F.View, B, "opacity"), 0.75f, 0.001f),
        "typed batch updates computed opacity without string parsing");

    const RmlUE_AnimatedPropertyUpdate Mixed[]{
        {F.View, A, RMLUE_ANIMATED_PROPERTY_OPACITY, {0.5f, 0.f, 0.f, 0.f, 0.f}},
        {F.View, B, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D, {12.f, 8.f, 1.f, 1.f, 0.f}},
    };
    Require(RmlUE_ApplyAnimatedProperties(Mixed, 2) == 2, "apply opacity and Transform2D through one tagged batch");
    F.Update();
    const auto Transformed = F.Measure(B);
    Require(Near(ComputedFloat(F.View, A, "opacity"), 0.5f, 0.001f) &&
        Near(Transformed.X, 32.f) && Near(Transformed.Y, 38.f) && Near(Transformed.Width, 10.f) && Near(Transformed.Height, 10.f),
        "tagged batch commits typed opacity and translated geometry");

    const RmlUE_AnimatedPropertyUpdate PaintAndDiscrete[]{
        {F.View, A, RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR, {0.2f, 0.4f, 0.6f, 0.5f}},
        {F.View, B, RMLUE_ANIMATED_PROPERTY_VISIBILITY, {0.f}},
    };
    Require(RmlUE_ApplyAnimatedProperties(PaintAndDiscrete, 2) == 2,
        "apply typed RGBA and discrete visibility in one batch");
    char PropertyBuffer[64]{};
    Require(RmlUE_GetComputedProperty(F.View, A, "background-color", PropertyBuffer, sizeof(PropertyBuffer)) != 0 &&
        std::strlen(PropertyBuffer) > 0, "typed RGBA reaches the computed background color");
    Require(RmlUE_GetComputedProperty(F.View, B, "visibility", PropertyBuffer, sizeof(PropertyBuffer)) != 0 &&
        std::strcmp(PropertyBuffer, "hidden") == 0, "typed visibility reaches the computed style");

    const RmlUE_AnimatedPropertyUpdate Skew[]{
        {F.View, B, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D, {0.f, 0.f, 1.f, 1.f, 0.f, 45.f, 0.f}},
    };
    Require(RmlUE_ApplyAnimatedProperties(Skew, 1) == 1, "apply typed affine skew");
    F.Update();
    const auto Skewed = F.Measure(B);
    Require(Near(Skewed.Width, 20.f, 0.01f) && Near(Skewed.Height, 10.f, 0.01f),
        "typed skew expands transformed geometry");

    const RmlUE_AnimatedPropertyUpdate Layout[] {
        {F.View, A, RMLUE_ANIMATED_PROPERTY_LEFT_PX, {45.f, 0.f, 0.f, 0.f, 0.f}},
        {F.View, A, RMLUE_ANIMATED_PROPERTY_WIDTH_PX, {32.f, 0.f, 0.f, 0.f, 0.f}},
        {F.View, B, RMLUE_ANIMATED_PROPERTY_TOP_PX, {64.f, 0.f, 0.f, 0.f, 0.f}},
    };
    Require(RmlUE_ApplyAnimatedProperties(Layout, 3) == 3,
        "apply px layout scalars through one typed batch");
    F.Update();
    const auto LayoutA = F.Measure(A);
    const auto LayoutB = F.Measure(B);
    Require(Near(LayoutA.X, 45.f) && Near(LayoutA.Width, 32.f) && Near(LayoutB.Y, 72.f),
        "typed px scalars drive RmlUi layout without string parsing");

    const RmlUE_AnimatedPropertyUpdate NegativeSize{
        F.View, A, RMLUE_ANIMATED_PROPERTY_WIDTH_PX, {-1.f, 0.f, 0.f, 0.f, 0.f}};
    Require(!RmlUE_ApplyAnimatedProperties(&NegativeSize, 1),
        "typed layout batch rejects negative size");

    const RmlUE_AnimatedPropertyUpdate Invalid[]{
        {F.View, A, RMLUE_ANIMATED_PROPERTY_OPACITY, {0.25f, 0.f, 0.f, 0.f, 0.f}},
        {F.View, 0xffffffffu, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D, {1.f, 2.f, 1.f, 1.f, 0.f}},
    };
    Require(!RmlUE_ApplyAnimatedProperties(Invalid, 2), "reject tagged batch containing a stale node");
    Require(Near(ComputedFloat(F.View, A, "opacity"), 0.5f, 0.001f), "failed tagged batch applies no earlier update");
    Require(!RmlUE_ApplyAnimatedProperties(Mixed, -1) && !RmlUE_ApplyAnimatedProperties(nullptr, 1), "validate tagged batch shape");
    std::fprintf(stderr, "PASS atomic typed animated property batch\n");
}

static void TestAnimationTargets()
{
    const char* Markup = R"(<rml><head><style>#box{opacity:1;}</style></head><body><div id="box"/></body></rml>)";
    Fixture First(Markup);
    Fixture Second(Markup);
    First.Update();
    Second.Update();
    const auto Box = First.Node("box");
    const RmlUE_AnimationTarget Target = RmlUE_ResolveAnimationTarget(First.View, Box);
    const RmlUE_AnimationTarget SecondTarget = RmlUE_ResolveAnimationTarget(Second.View, Second.Node("box"));
    Require(Target && RmlUE_IsAnimationTargetValid(First.View, Target),
        "resolve a binding-time animation target");
    Require(SecondTarget && SecondTarget != Target && !RmlUE_IsAnimationTargetValid(Second.View, Target),
        "animation target rejects a foreign view");
    Require(RmlUE_PrepareAnimationTargetProperty(
        First.View, Target, RMLUE_ANIMATED_PROPERTY_OPACITY),
        "capture the binding-time underlying opacity");

    RmlUE_AnimatedPropertyUpdate Update{First.View, Box, RMLUE_ANIMATED_PROPERTY_OPACITY,
        {0.4f, 0.f, 0.f, 0.f, 0.f}, Target};
    Require(RmlUE_ApplyAnimatedProperties(&Update, 1) == 1 &&
        Near(ComputedFloat(First.View, Box, "opacity"), 0.4f, 0.001f),
        "target update commits without a per-frame node lookup");

    const RmlUE_AnimationTarget SharedTarget = RmlUE_ResolveAnimationTarget(First.View, Box);
    Require(SharedTarget && RmlUE_PrepareAnimationTargetProperty(
        First.View, SharedTarget, RMLUE_ANIMATED_PROPERTY_OPACITY),
        "replacement target shares the original underlying style");
    Require(RmlUE_RestoreAnimationTargetProperty(First.View, Target, 0) &&
        Near(ComputedFloat(First.View, Box, "opacity"), 0.4f, 0.001f),
        "layered restore waits while another contribution owns the property");
    Require(RmlUE_ReleaseAnimationTarget(First.View, Target),
        "release the first shared animation target");
    Require(RmlUE_RestoreAnimationTargetProperty(First.View, SharedTarget, 0) &&
        Near(ComputedFloat(First.View, Box, "opacity"), 1.f, 0.001f),
        "last contribution restores the captured stylesheet value");

    Update.View = Second.View;
    Require(RmlUE_ApplyAnimatedProperties(&Update, 1) == 0,
        "target batch rejects a view-token mismatch");
    Update.View = First.View;
    Require(RmlUE_ReleaseAnimationTarget(First.View, SharedTarget) &&
        !RmlUE_IsAnimationTargetValid(First.View, Target),
        "released animation target becomes stale");
    const RmlUE_AnimationTarget Reused = RmlUE_ResolveAnimationTarget(First.View, Box);
    Require(Reused && Reused != Target && RmlUE_IsAnimationTargetValid(First.View, Reused),
        "reused target slot advances its generation");
    Require(RmlUE_LoadDocumentFromMemory(First.View, Markup, "replacement.rml") &&
        !RmlUE_IsAnimationTargetValid(First.View, Reused),
        "document replacement invalidates animation targets");
    Require(RmlUE_ReleaseAnimationTarget(Second.View, SecondTarget),
        "release independent view animation target");
    std::fprintf(stderr, "PASS binding-time animation target lifetime\n");
}

static void TestAnimatedVisualBatch()
{
    const char* Markup = R"(<rml><head><style>body{margin:0;}#box{display:block;width:40px;height:30px;opacity:1;}#box-child{display:block;width:40px;height:30px;background:#fff;}</style></head><body><div id="box"><div id="box-child"/></div></body></rml>)";
    Fixture Slate(Markup, true);
    Slate.Update();
    const auto Box = Slate.Node("box");
    const auto BoxChild = Slate.Node("box-child");
    RmlUE_AnimatedPropertyUpdate Update{Slate.View, Box, RMLUE_ANIMATED_PROPERTY_OPACITY,
        {0.25f, 0.f, 0.f, 0.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Box)};
    Require(Update.Target != 0, "resolve visual animation target");
    uint8_t Accepted = 0;
    RmlUE_SlateScheduleState ScheduleBefore{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &ScheduleBefore) != 0 && !ScheduleBefore.HasRecordedFrame,
        "new Slate view exposes an unrecorded schedule state");
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "Slate opacity enters visual commit sink");
    RmlUE_SlateScheduleState ScheduleVisual{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &ScheduleVisual) != 0 &&
        ScheduleVisual.VisualRevision != ScheduleBefore.VisualRevision,
        "visual override advances the scheduling revision");
    Require(Near(ComputedFloat(Slate.View, Box, "opacity"), 1.f, 0.001f),
        "visual commit does not dirty computed style");
    RmlUE_SlateFrame Frame{};
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 && Frame.VisualDeltaCount == 0,
        "render complete visual opacity command frame");
    bool FoundVisualDraw = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = Frame.Draws[Index];
        if (Draw.VisualNode == BoxChild)
        {
            FoundVisualDraw = true;
            Require(Near(Draw.VisualOpacity, 0.25f, 0.001f),
                "child draw inherits the target parent visual opacity ratio");
        }
    }
    Require(FoundVisualDraw, "visual node identity reaches Slate draw ABI");
    RmlUE_SlateReplayStats ReplayStats{};
    RmlUE_GetSlateReplayStats(Slate.View, &ReplayStats);
    Require(ReplayStats.FullRenderFrames == 1 && ReplayStats.ReplayedFrames == 0,
        "first visual frame records a complete retained snapshot");
    RmlUE_SlateScheduleState ScheduleRecorded{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &ScheduleRecorded) != 0 && ScheduleRecorded.HasRecordedFrame &&
        ScheduleRecorded.CanReplay && !std::isfinite(ScheduleRecorded.NextUpdateDelay),
        "idle retained frame exposes no pending RmlUi deadline");

    Update.Values[0] = 0.5f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "second visual opacity updates retained draw parameters");
    Update.Values[0] = 0.4f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "repeated visual update before replay remains accepted");
    RmlUE_SlateScheduleState ScheduleSecondVisual{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &ScheduleSecondVisual) != 0 &&
        ScheduleSecondVisual.VisualRevision != ScheduleRecorded.VisualRevision,
        "second visual update is observable without entering RenderSlate");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.GeometryDeltaCount == 0 && Frame.TextureCount == 0,
        "visual-only frame replays without resource deltas");
    Require(Frame.VisualDeltaCount == 1 && Frame.VisualDeltas && Frame.VisualDeltas[0].Node == BoxChild &&
        Near(Frame.VisualDeltas[0].VisualOpacity, 0.4f, 0.001f),
        "visual-only replay coalesces repeated parent writes to the inherited child delta");
    RmlUE_GetSlateReplayStats(Slate.View, &ReplayStats);
    Require(ReplayStats.FullRenderFrames == 1 && ReplayStats.ReplayedFrames == 1,
        "visual-only frame skips full element render traversal");
    FoundVisualDraw = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
        if (Frame.Draws[Index].VisualNode == BoxChild)
        {
            FoundVisualDraw = true;
            Require(Near(Frame.Draws[Index].VisualOpacity, 0.4f, 0.001f),
                "replayed draw receives updated visual opacity");
        }
    Require(FoundVisualDraw, "retained snapshot preserves visual node mapping");

    Require(RmlUE_ClearAnimatedVisualProperties(&Update, 1) == 1 &&
        RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas && Frame.VisualDeltas[0].Node == BoxChild &&
        Near(Frame.VisualDeltas[0].VisualOpacity, 1.f, 0.001f),
        "cleared override emits one neutral replay delta");
    Update.Values[0] = 0.5f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1 &&
        RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas && Frame.VisualDeltas[0].Node == BoxChild &&
        Near(Frame.VisualDeltas[0].VisualOpacity, 0.5f, 0.001f),
        "visual override can reactivate after its neutral delta was consumed");

    const uint64_t ContentBeforePropertyCommit = ScheduleSecondVisual.ContentRevision;
    Require(RmlUE_ApplyAnimatedProperties(&Update, 1) == 1 &&
        RmlUE_ClearAnimatedVisualProperties(&Update, 1) == 1,
        "final property commit clears visual override");
    RmlUE_SlateScheduleState SchedulePropertyCommit{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &SchedulePropertyCommit) != 0 &&
        SchedulePropertyCommit.ContentRevision != ContentBeforePropertyCommit,
        "property sink mutation is observable before the next Context update");
    Require(Near(ComputedFloat(Slate.View, Box, "opacity"), 0.5f, 0.001f),
        "final property value remains observable after clearing override");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 && Frame.VisualDeltaCount == 0,
        "render complete final property-backed frame");
    RmlUE_GetSlateReplayStats(Slate.View, &ReplayStats);
    Require(ReplayStats.FullRenderFrames == 2 && ReplayStats.ReplayedFrames == 3,
        "property commit invalidates retained snapshot and records a full frame");
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
        if (Frame.Draws[Index].VisualNode == BoxChild)
            Require(Near(Frame.Draws[Index].VisualOpacity, 1.f, 0.001f),
                "cleared override keeps source identity with neutral visual opacity");

    const char* TreeMarkup = R"(<rml><head><style>
body{margin:0;}
#parent{display:block;width:80px;height:60px;background:#f00;opacity:1;}
#child{display:block;width:60px;height:20px;background:#0f0;}
#local{display:block;width:60px;height:20px;background:#00f;opacity:0.6;}
#local-child{display:block;width:40px;height:10px;background:#fff;}
</style></head><body><div id="parent"><div id="child"/><div id="local"><div id="local-child"/></div></div></body></rml>)";
    Fixture Tree(TreeMarkup, true);
    Tree.Update();
    RmlUE_SlateFrame TreeFrame{};
    Require(RmlUE_RenderSlate(Tree.View, &TreeFrame) != 0 && TreeFrame.Replayed == 0,
        "record opacity subtree fixture");
    const auto Parent = Tree.Node("parent");
    const auto Child = Tree.Node("child");
    const auto Local = Tree.Node("local");
    const auto LocalChild = Tree.Node("local-child");
    RmlUE_AnimatedPropertyUpdate TreeUpdate{Tree.View, Parent, RMLUE_ANIMATED_PROPERTY_OPACITY,
        {0.25f, 0.f, 0.f, 0.f, 0.f}, RmlUE_ResolveAnimationTarget(Tree.View, Parent)};
    Accepted = 0;
    Require(TreeUpdate.Target && RmlUE_ApplyAnimatedVisualProperties(&TreeUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "parent opacity enters retained subtree visual sink");
    Require(RmlUE_RenderSlate(Tree.View, &TreeFrame) != 0 && TreeFrame.Replayed == 1 &&
        TreeFrame.VisualDeltaCount == 2,
        "parent opacity replay updates direct and inherited child visual slots");
    bool FoundParentDraw = false;
    bool FoundChildDraw = false;
    bool FoundLocalDraw = false;
    bool FoundLocalChildDraw = false;
    for (uint32_t Index = 0; Index < TreeFrame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = TreeFrame.Draws[Index];
        if (Draw.VisualNode == Parent) { FoundParentDraw = true; Require(Near(Draw.VisualOpacity, 0.25f, 0.001f), "parent draw receives opacity ratio"); }
        if (Draw.VisualNode == Child) { FoundChildDraw = true; Require(Near(Draw.VisualOpacity, 0.25f, 0.001f), "inherited child draw receives parent opacity ratio"); }
        if (Draw.VisualNode == Local) { FoundLocalDraw = true; Require(Near(Draw.VisualOpacity, 1.f, 0.001f), "local opacity branch remains an independent baked value"); }
        if (Draw.VisualNode == LocalChild) { FoundLocalChildDraw = true; Require(Near(Draw.VisualOpacity, 1.f, 0.001f), "descendants of local opacity branch remain independent"); }
    }
    Require(FoundParentDraw && FoundChildDraw && FoundLocalDraw && FoundLocalChildDraw,
        "opacity subtree fixture exposes every expected visual node");
    for (uint32_t Index = 0; Index < TreeFrame.VisualDeltaCount; ++Index)
        Require(TreeFrame.VisualDeltas[Index].Node != Local && TreeFrame.VisualDeltas[Index].Node != LocalChild,
            "parent opacity delta stops at a local opacity boundary");
    Require(RmlUE_ClearAnimatedVisualProperties(&TreeUpdate, 1) == 1 &&
        RmlUE_RenderSlate(Tree.View, &TreeFrame) != 0 && TreeFrame.Replayed == 1 &&
        TreeFrame.VisualDeltaCount == 2,
        "clearing parent opacity resets every inherited subtree visual slot");
    Require(RmlUE_ReleaseAnimationTarget(Tree.View, TreeUpdate.Target),
        "release opacity subtree animation target");

    Fixture Dx11(Markup);
    Dx11.Update();
    Update.View = Dx11.View;
    Update.Node = Dx11.Node("box");
    Require(!RmlUE_IsAnimationTargetValid(Dx11.View, Update.Target),
        "visual animation target cannot cross views");
    Update.Target = RmlUE_ResolveAnimationTarget(Dx11.View, Update.Node);
    Accepted = 1;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 0 && Accepted == 0,
        "DX11 view declines visual commit for property fallback");
    RmlUE_SlateScheduleState InvalidSchedule{};
    Require(!RmlUE_GetSlateScheduleState(Dx11.View, &InvalidSchedule),
        "Slate schedule state rejects a DX11 compatibility view");
    Require(RmlUE_ReleaseAnimationTarget(Dx11.View, Update.Target),
        "release DX11 animation target");
    std::fprintf(stderr, "PASS Slate visual opacity sink and property fallback\n");
}

static void TestAnimatedVisualBackgroundColor()
{
    const char* Markup = R"(<rml><head><style>
body{margin:0;}#box{display:block;width:80px;height:50px;background-color:#ff0000;border:4px #00ff00;opacity:0.5;}
#shadow{display:block;width:40px;height:30px;background:#fff;box-shadow:2px 2px 3px #000;}
</style></head><body><div id="box"/><div id="shadow"/></body></rml>)";
    Fixture Slate(Markup, true);
    Slate.Update();
    const auto Box = Slate.Node("box");
    const auto Shadow = Slate.Node("shadow");
    RmlUE_SlateFrame Frame{};
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0,
        "record background-color retained fixture");
    bool FoundBackground = false;
    bool FoundBorder = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = Frame.Draws[Index];
        if (Draw.VisualNode != Box) continue;
        FoundBackground |= Draw.PaintRole == RMLUE_PAINT_ROLE_BACKGROUND;
        FoundBorder |= Draw.PaintRole == RMLUE_PAINT_ROLE_BORDER;
    }
    Require(FoundBackground && FoundBorder, "background and border use independent semantic draws");

    RmlUE_AnimatedPropertyUpdate Update{Slate.View, Box, RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR,
        {0.2f, 0.4f, 0.8f, 0.5f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Box)};
    Require(Update.Target && RmlUE_PrepareAnimationTargetProperty(Slate.View, Update.Target, Update.Property),
        "prepare retained background-color binding");
    RmlUE_SlateScheduleState Before{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &Before) != 0, "read background schedule baseline");
    uint8_t Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "background-color enters retained visual sink");
    RmlUE_SlateScheduleState After{};
    Require(RmlUE_GetSlateScheduleState(Slate.View, &After) != 0 &&
        After.ContentRevision == Before.ContentRevision && After.VisualRevision != Before.VisualRevision,
        "background-color advances only visual revision");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas[0].ColorChanged &&
        Frame.VisualDeltas[0].PaintRole == RMLUE_PAINT_ROLE_BACKGROUND,
        "background-color replays one role-filtered delta");
    bool FoundReplacement = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = Frame.Draws[Index];
        if (Draw.VisualNode == Box && Draw.PaintRole == RMLUE_PAINT_ROLE_BACKGROUND)
        {
            FoundReplacement = true;
            Require(Draw.VisualColorEnabled && Near(Draw.VisualColorR, 0.05f, 0.001f) &&
                Near(Draw.VisualColorG, 0.10f, 0.001f) && Near(Draw.VisualColorB, 0.20f, 0.001f) &&
                Near(Draw.VisualColorA, 0.25f, 0.001f),
                "replacement color is encoded-space premultiplied by color alpha and element opacity");
        }
        if (Draw.VisualNode == Box && Draw.PaintRole == RMLUE_PAINT_ROLE_BORDER)
            Require(!Draw.VisualColorEnabled, "background update does not tint border draw");
    }
    Require(FoundReplacement, "retained frame exposes replaced background draw");
    Require(RmlUE_ClearAnimatedVisualProperties(&Update, 1) == 1 &&
        RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltas[0].ColorChanged && !Frame.VisualDeltas[0].VisualColorEnabled,
        "clearing background override emits a role-filtered reset");
    Require(RmlUE_ReleaseAnimationTarget(Slate.View, Update.Target), "release background animation target");

    RmlUE_AnimatedPropertyUpdate ShadowUpdate{Slate.View, Shadow, RMLUE_ANIMATED_PROPERTY_BACKGROUND_COLOR,
        {1.f, 0.f, 0.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Shadow)};
    Accepted = 1;
    Require(ShadowUpdate.Target && RmlUE_ApplyAnimatedVisualProperties(&ShadowUpdate, 1, &Accepted) == 0 && Accepted == 0,
        "combined box-shadow geometry declines retained background color for property fallback");
    Require(RmlUE_ReleaseAnimationTarget(Slate.View, ShadowUpdate.Target), "release shadow fallback target");
    std::fprintf(stderr, "PASS retained background-color role and fallback\n");
}

static void TestAnimatedVisualTransformBatch()
{
    const char* Markup = R"(<rml><head><style>
body{margin:0;width:640px;height:480px;}
#box{position:absolute;left:20px;top:30px;width:40px;height:30px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}
#no-baseline{position:absolute;left:250px;top:30px;width:20px;height:20px;background:#fff;}
#parent{position:absolute;left:300px;top:30px;width:40px;height:30px;transform-origin:0px 0px;transform:translate(0px,0px);}
#parent-child{display:block;width:20px;height:15px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}
#release-box{position:absolute;left:500px;top:100px;width:20px;height:20px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}
#clipped-parent{position:absolute;left:400px;top:30px;width:20px;height:20px;overflow:hidden;border-radius:4px;transform:translate(0px,0px);}
#clipped-child{display:block;width:40px;height:40px;background:#fff;}
#outer-clip{position:absolute;left:400px;top:80px;width:30px;height:20px;overflow:hidden;}
#outer-clipped-target{display:block;width:50px;height:20px;background:#fff;transform-origin:0px 0px;transform:translate(0px,0px);}
#multi-mask-root{position:absolute;left:450px;top:150px;width:70px;height:50px;transform-origin:0px 0px;transform:translate(0px,0px);}
#multi-mask-outer{display:block;width:60px;height:40px;overflow:hidden;border-radius:8px;}
#multi-mask-inner{display:block;position:relative;left:8px;top:5px;width:40px;height:30px;overflow:hidden;border-radius:6px;transform-origin:0px 0px;transform:translate(0px,0px);}
#multi-mask-fill{display:block;width:70px;height:40px;background:#fff;}
</style></head><body><div id="box"/><div id="no-baseline"/><div id="parent"><div id="parent-child"/></div><div id="release-box"/><div id="clipped-parent"><div id="clipped-child"/></div><div id="outer-clip"><div id="outer-clipped-target"/></div><div id="multi-mask-root"><div id="multi-mask-outer"><div id="multi-mask-inner"><div id="multi-mask-fill"/></div></div></div></body></rml>)";
    Fixture Slate(Markup, true);
    EventLog Events;
    RmlUE_SetNodeEventCallback(Slate.View, OnEvent, &Events);
    Slate.Update();
    const auto Box = Slate.Node("box");
    const auto NoBaseline = Slate.Node("no-baseline");
    const auto Parent = Slate.Node("parent");
    const auto ParentChild = Slate.Node("parent-child");
    const auto ReleaseBox = Slate.Node("release-box");
    const auto ClippedParent = Slate.Node("clipped-parent");
    const auto ClippedChild = Slate.Node("clipped-child");
    const auto OuterClippedTarget = Slate.Node("outer-clipped-target");
    const auto MultiMaskRoot = Slate.Node("multi-mask-root");
    const auto MultiMaskOuter = Slate.Node("multi-mask-outer");
    const auto MultiMaskInner = Slate.Node("multi-mask-inner");
    const auto MultiMaskFill = Slate.Node("multi-mask-fill");
    Listen(Slate, Box, "click", 70);
    Listen(Slate, ClippedChild, "click", 71);
    Listen(Slate, MultiMaskFill, "click", 72);

    RmlUE_SlateFrame Frame{};
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0,
        "record baseline Transform2D retained frame");
    const auto Before = Slate.Measure(Box);
    RmlUE_AnimatedPropertyUpdate Update{Slate.View, Box, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {100.f, 50.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Box)};
    uint8_t Accepted = 0;
    Require(Update.Target && RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "eligible leaf Transform2D enters visual commit sink");
    const auto Moved = Slate.Measure(Box);
    Require(Near(Moved.LayoutX, Before.LayoutX) && Near(Moved.LayoutY, Before.LayoutY) &&
        Near(Moved.X, Before.X + 100.f) && Near(Moved.Y, Before.Y + 50.f),
        "visual Transform2D synchronizes transformed bounds without changing layout");

    RmlUE_MouseMove(Slate.View, int(Before.X + 5.f), int(Before.Y + 5.f), 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(70) == 0, "visual Transform2D removes hit target from its old coordinates");
    RmlUE_MouseMove(Slate.View, int(Moved.X + 5.f), int(Moved.Y + 5.f), 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(70) == 1, "visual Transform2D updates RmlUi hit testing immediately");

    Update.Values[2] = 2.f;
    Update.Values[3] = 0.5f;
    Update.Values[4] = 90.f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "visual Transform2D accepts combined non-uniform scale and rotation");
    const auto Rotated = Slate.Measure(Box);
    Require(Near(Rotated.X, Before.X + 85.f, 0.01f) && Near(Rotated.Y, Before.Y + 50.f, 0.01f) &&
        Near(Rotated.Width, 15.f, 0.01f) && Near(Rotated.Height, 80.f, 0.01f),
        "direct affine Transform2D preserves rotation, scale and transform-origin semantics");
    Update.Values[2] = 1.f;
    Update.Values[3] = 1.f;
    Update.Values[4] = 0.f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "visual Transform2D returns to translation-only state");

    RmlUE_AnimatedPropertyUpdate OpacityUpdate{Slate.View, Box, RMLUE_ANIMATED_PROPERTY_OPACITY,
        {0.5f, 0.f, 0.f, 0.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Box)};
    Require(OpacityUpdate.Target && RmlUE_PrepareAnimationTargetProperty(
        Slate.View, OpacityUpdate.Target, OpacityUpdate.Property),
        "prepare opacity visual state during animation binding");
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&OpacityUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "opacity can share a visual frame with Transform2D on the same node");
    RmlUE_AnimatedVisualCommitStats CommitStats;
    std::memset(&CommitStats, 0xff, sizeof(CommitStats));
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualPropertiesProfiled(
        &OpacityUpdate, 1, &Accepted, &CommitStats) == 1 && Accepted == 1 &&
        CommitStats.ValidateNanoseconds != UINT64_MAX && CommitStats.ApplyNanoseconds != UINT64_MAX,
        "profiled visual commit preserves behavior and writes validation/apply timing");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas && Frame.VisualDeltas[0].Node == Box &&
        Frame.VisualDeltas[0].TransformChanged && Frame.VisualDeltas[0].OpacityChanged &&
        Near(Frame.VisualDeltas[0].VisualOpacity, 0.5f, 0.001f),
        "opacity and Transform2D coalesce into one node delta without full element traversal");
    bool FoundMovedDraw = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
        if (Frame.Draws[Index].VisualNode == Box)
        {
            FoundMovedDraw = true;
            Require(Frame.Draws[Index].TransformEnabled && Near(Frame.Draws[Index].TransformX, 100.f, 0.01f) &&
                Near(Frame.Draws[Index].TransformY, 50.f, 0.01f) && Near(Frame.Draws[Index].VisualOpacity, 0.5f, 0.001f),
                "retained draw carries synchronized Transform2D and opacity values");
        }
    Require(FoundMovedDraw, "Transform2D retained snapshot preserves source draw identity");

    const auto ChildBefore = Slate.Measure(ParentChild);
    RmlUE_AnimatedPropertyUpdate ParentUpdate{Slate.View, Parent, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {15.f, 12.f, 1.f, 1.f, 0.f}, 0};
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&ParentUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "Transform2D visual sink accepts an unclipped element subtree");
    const auto ChildMoved = Slate.Measure(ParentChild);
    Require(Near(ChildMoved.X, ChildBefore.X + 15.f) && Near(ChildMoved.Y, ChildBefore.Y + 12.f),
        "parent visual Transform2D synchronizes descendant bounds");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas[0].Node == ParentChild &&
        Frame.VisualDeltas[0].TransformChanged,
        "parent visual Transform2D emits the affected descendant draw delta");

    ParentUpdate.Values[0] = 20.f;
    ParentUpdate.Values[1] = 10.f;
    RmlUE_AnimatedPropertyUpdate ChildUpdate{Slate.View, ParentChild, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {5.f, 4.f, 1.f, 1.f, 0.f}, 0};
    RmlUE_AnimatedPropertyUpdate NestedUpdates[]{ChildUpdate, ParentUpdate};
    uint8_t NestedAccepted[2]{};
    Require(RmlUE_ApplyAnimatedVisualProperties(NestedUpdates, 2, NestedAccepted) == 2 &&
        NestedAccepted[0] == 1 && NestedAccepted[1] == 1,
        "one visual batch accepts nested Transform2D targets");
    const auto ChildNested = Slate.Measure(ParentChild);
    Require(Near(ChildNested.X, ChildBefore.X + 25.f) && Near(ChildNested.Y, ChildBefore.Y + 14.f),
        "batched parent and child Transform2D values compose before publication");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1 &&
        Frame.VisualDeltaCount == 1 && Frame.VisualDeltas[0].Node == ParentChild &&
        Frame.VisualDeltas[0].TransformChanged,
        "nested Transform2D targets coalesce to the final descendant matrix");

    float OuterScissorX = 0.f, OuterScissorWidth = 0.f;
    bool FoundOuterScissor = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
        if (Frame.Draws[Index].VisualNode == OuterClippedTarget && Frame.Draws[Index].ScissorEnabled)
        {
            FoundOuterScissor = true;
            OuterScissorX = Frame.Draws[Index].ScissorX;
            OuterScissorWidth = Frame.Draws[Index].ScissorWidth;
        }
    Require(FoundOuterScissor && OuterScissorWidth > 0.f,
        "external overflow ancestor records a retained scissor for the transform target");
    const auto OuterBefore = Slate.Measure(OuterClippedTarget);
    RmlUE_AnimatedPropertyUpdate OuterUpdate{Slate.View, OuterClippedTarget, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {8.f, 0.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, OuterClippedTarget)};
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&OuterUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "Transform2D visual sink accepts a target under an external scissor-only ancestor");
    const auto OuterMoved = Slate.Measure(OuterClippedTarget);
    Require(Near(OuterMoved.X, OuterBefore.X + 8.f) && Near(OuterMoved.Y, OuterBefore.Y),
        "external scissor target keeps transformed measurement and hit-test state");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "external scissor Transform2D remains on retained replay");
    bool FoundStableOuterScissor = false;
    for (uint32_t Index = 0; Index < Frame.DrawCount; ++Index)
        if (Frame.Draws[Index].VisualNode == OuterClippedTarget && Frame.Draws[Index].ScissorEnabled)
            FoundStableOuterScissor |= Near(Frame.Draws[Index].ScissorX, OuterScissorX) &&
                Near(Frame.Draws[Index].ScissorWidth, OuterScissorWidth);
    Require(FoundStableOuterScissor,
        "external scissor remains fixed while only the descendant transform changes");

    bool FoundOwnedClipMask = false;
    float ClipMaskBeforeX = 0.f, ClipMaskBeforeY = 0.f;
    for (uint32_t Index = 0; Index < Frame.ClipMaskCount; ++Index)
        if (Frame.ClipMasks[Index].OwnerNode == ClippedParent)
        {
            FoundOwnedClipMask = true;
            ClipMaskBeforeX = Frame.ClipMasks[Index].TransformX;
            ClipMaskBeforeY = Frame.ClipMasks[Index].TransformY;
        }
    Require(FoundOwnedClipMask, "retained clip mask carries its stable clipping-element owner");

    RmlUE_AnimatedPropertyUpdate ClippedUpdate{Slate.View, ClippedParent, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {12.f, 8.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, ClippedParent)};
    Accepted = 0;
    Require(ClippedUpdate.Target && RmlUE_ApplyAnimatedVisualProperties(&ClippedUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "Transform2D visual sink accepts an internally clipped subtree with an owned mask");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "owned clip-mask Transform2D remains on retained replay");
    bool FoundMaskDelta = false, FoundMovedMask = false;
    for (uint32_t Index = 0; Index < Frame.VisualDeltaCount; ++Index)
        FoundMaskDelta |= Frame.VisualDeltas[Index].Node == ClippedParent &&
            Frame.VisualDeltas[Index].ClipMaskTransformChanged != 0;
    for (uint32_t Index = 0; Index < Frame.ClipMaskCount; ++Index)
        if (Frame.ClipMasks[Index].OwnerNode == ClippedParent)
            FoundMovedMask |= Near(Frame.ClipMasks[Index].TransformX, ClipMaskBeforeX + 12.f, 0.01f) &&
                Near(Frame.ClipMasks[Index].TransformY, ClipMaskBeforeY + 8.f, 0.01f);
    Require(FoundMaskDelta && FoundMovedMask,
        "owned clip mask publishes an affine delta without rebuilding geometry or clip topology");
    RmlUE_MouseMove(Slate.View, 405, 35, 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(71) == 0, "dynamic clip mask removes the clipped child from its old coordinates");
    RmlUE_MouseMove(Slate.View, 417, 43, 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(71) == 1, "dynamic clip mask and content share the new hit-test coordinates");

    bool FoundOuterOwner = false, FoundInnerOwner = false;
    float MultiOuterBeforeX = 0.f, MultiOuterBeforeY = 0.f;
    float MultiInnerBeforeX = 0.f, MultiInnerBeforeY = 0.f;
    for (uint32_t Index = 0; Index < Frame.ClipMaskCount; ++Index)
    {
        const RmlUE_SlateClipMask& Mask = Frame.ClipMasks[Index];
        if (Mask.OwnerNode == MultiMaskOuter && !FoundOuterOwner)
        {
            FoundOuterOwner = true;
            MultiOuterBeforeX = Mask.TransformX;
            MultiOuterBeforeY = Mask.TransformY;
        }
        if (Mask.OwnerNode == MultiMaskInner && !FoundInnerOwner)
        {
            FoundInnerOwner = true;
            MultiInnerBeforeX = Mask.TransformX;
            MultiInnerBeforeY = Mask.TransformY;
        }
    }
    Require(FoundOuterOwner && FoundInnerOwner,
        "nested rounded clips retain two distinct mask owners");
    RmlUE_AnimatedPropertyUpdate MultiMaskUpdates[]{
        {Slate.View, MultiMaskInner, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
            {6.f, 4.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, MultiMaskInner)},
        {Slate.View, MultiMaskRoot, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
            {20.f, 10.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, MultiMaskRoot)}};
    uint8_t MultiMaskAccepted[2]{};
    Require(MultiMaskUpdates[0].Target && MultiMaskUpdates[1].Target &&
        RmlUE_ApplyAnimatedVisualProperties(MultiMaskUpdates, 2, MultiMaskAccepted) == 2 &&
        MultiMaskAccepted[0] == 1 && MultiMaskAccepted[1] == 1,
        "one visual batch accepts parent and nested mask-owner transforms");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "multiple dynamic mask owners remain on retained replay");
    bool FoundOuterDelta = false, FoundInnerDelta = false;
    bool FoundMovedOuter = false, FoundMovedInner = false;
    for (uint32_t Index = 0; Index < Frame.VisualDeltaCount; ++Index)
    {
        const RmlUE_SlateVisualDelta& Delta = Frame.VisualDeltas[Index];
        FoundOuterDelta |= Delta.Node == MultiMaskOuter && Delta.ClipMaskTransformChanged != 0;
        FoundInnerDelta |= Delta.Node == MultiMaskInner && Delta.ClipMaskTransformChanged != 0;
    }
    for (uint32_t Index = 0; Index < Frame.ClipMaskCount; ++Index)
    {
        const RmlUE_SlateClipMask& Mask = Frame.ClipMasks[Index];
        if (Mask.OwnerNode == MultiMaskOuter)
            FoundMovedOuter |= Near(Mask.TransformX, MultiOuterBeforeX + 20.f, 0.01f) &&
                Near(Mask.TransformY, MultiOuterBeforeY + 10.f, 0.01f);
        if (Mask.OwnerNode == MultiMaskInner)
            FoundMovedInner |= Near(Mask.TransformX, MultiInnerBeforeX + 26.f, 0.01f) &&
                Near(Mask.TransformY, MultiInnerBeforeY + 14.f, 0.01f);
    }
    Require(FoundOuterDelta && FoundInnerDelta && FoundMovedOuter && FoundMovedInner,
        "nested mask deltas compose parent and local owner transforms independently");
    RmlUE_MouseMove(Slate.View, 470, 165, 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(72) == 0, "nested dynamic masks remove the child from old coordinates");
    RmlUE_MouseMove(Slate.View, 495, 175, 0);
    RmlUE_MouseButton(Slate.View, 0, 1, 0); RmlUE_MouseButton(Slate.View, 0, 0, 0);
    Require(Events.Count(72) == 1, "nested dynamic masks share composed hit-test coordinates");

    RmlUE_AnimatedPropertyUpdate Unsupported{
        Slate.View, NoBaseline, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D, {1.f, 2.f, 1.f, 1.f, 0.f}, 0};
    uint8_t UnsupportedAccepted = 1;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Unsupported, 1, &UnsupportedAccepted) == 0 &&
        UnsupportedAccepted == 0,
        "Transform2D visual sink still declines a missing transform baseline");

    Require(RmlUE_ApplyAnimatedProperties(NestedUpdates, 2) == 2 &&
        RmlUE_ClearAnimatedVisualProperties(NestedUpdates, 2) == 2,
        "nested Transform2D final properties clear both visual overrides");
    Require(RmlUE_ClearAnimatedVisualProperties(&OuterUpdate, 1) == 1,
        "external scissor Transform2D visual override clears independently");
    Require(RmlUE_ClearAnimatedVisualProperties(&ClippedUpdate, 1) == 1,
        "owned clip-mask Transform2D visual override clears independently");
    Require(RmlUE_ApplyAnimatedProperties(MultiMaskUpdates, 2) == 2 &&
        RmlUE_ClearAnimatedVisualProperties(MultiMaskUpdates, 2) == 2,
        "nested mask-owner final properties clear both visual overrides");
    Require(RmlUE_ClearAnimatedVisualProperties(&OpacityUpdate, 1) == 1,
        "clear the coalesced opacity override independently from Transform2D");
    Require(RmlUE_ReleaseAnimationTarget(Slate.View, OpacityUpdate.Target),
        "release prepared opacity visual animation target");

    Require(RmlUE_ApplyAnimatedProperties(&Update, 1) == 1 &&
        RmlUE_ClearAnimatedVisualProperties(&Update, 1) == 1,
        "final Transform2D property commit clears the visual override");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 && Frame.VisualDeltaCount == 0,
        "final Transform2D property frame rebuilds retained commands once");
    const auto Final = Slate.Measure(Box);
    Require(Near(Final.X, Moved.X) && Near(Final.Y, Moved.Y),
        "final property transform takes over without a position discontinuity");

    Update.Values[0] = 120.f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "final property transform remains an eligible visual baseline");
    const RmlUE_Node Child = RmlUE_CreateNode(Slate.View, 0, "span");
    Require(Child && RmlUE_InsertNode(Slate.View, Child, Box, 0),
        "mutate an active Transform2D target into an element subtree");
    Update.Values[0] = 140.f;
    Accepted = 1;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 0 && Accepted == 0 &&
        RmlUE_ApplyAnimatedProperties(&Update, 1) == 1,
        "eligibility change clears the old visual override before property fallback");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0,
        "fallback after an eligibility change rebuilds a property-backed frame");
    const auto Fallback = Slate.Measure(Box);
    Require(Near(Fallback.X, Before.X + 140.f),
        "property fallback is not masked by the retired visual override");
    Require(RmlUE_ReleaseAnimationTarget(Slate.View, Update.Target),
        "release Transform2D visual animation target");

    const auto ReleaseBefore = Slate.Measure(ReleaseBox);
    RmlUE_AnimatedPropertyUpdate ReleaseUpdate{Slate.View, ReleaseBox, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {25.f, 10.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, ReleaseBox)};
    Accepted = 0;
    Require(ReleaseUpdate.Target &&
        RmlUE_ApplyAnimatedVisualProperties(&ReleaseUpdate, 1, &Accepted) == 1 && Accepted == 1,
        "apply Transform2D before direct target release");
    const auto ReleaseMoved = Slate.Measure(ReleaseBox);
    Require(Near(ReleaseMoved.X, ReleaseBefore.X + 25.f) && Near(ReleaseMoved.Y, ReleaseBefore.Y + 10.f),
        "active target release fixture observes the visual override");
    Require(RmlUE_ReleaseAnimationTarget(Slate.View, ReleaseUpdate.Target),
        "direct target release clears an active Transform2D override");
    const auto ReleaseRestored = Slate.Measure(ReleaseBox);
    Require(Near(ReleaseRestored.X, ReleaseBefore.X) && Near(ReleaseRestored.Y, ReleaseBefore.Y),
        "direct target release restores the baseline transform immediately");
    std::fprintf(stderr, "PASS Slate visual Transform2D sink, hit testing and fallback\n");
}

static void TestAnimatedVisualMaskTopology()
{
    const char* Markup = R"(<rml><head><style>
body{margin:0;width:320px;height:240px;}
#topology-parent{position:absolute;left:40px;top:30px;width:80px;height:60px;overflow:hidden;border-radius:14px;transform-origin:0px 0px;transform:translate(0px,0px);}
#topology-fill{display:block;width:120px;height:90px;background:#fff;}
</style></head><body><div id="topology-parent"><div id="topology-fill"/></div></body></rml>)";
    Fixture Slate(Markup, true);
    Slate.Update();
    const auto Parent = Slate.Node("topology-parent");
    const auto Fill = Slate.Node("topology-fill");
    RmlUE_SlateFrame Frame{};

    const auto HasOwnerMask = [&](const RmlUE_SlateFrame& Current)
    {
        for (uint32_t Index = 0; Index < Current.ClipMaskCount; ++Index)
            if (Current.ClipMasks[Index].OwnerNode == Parent) return true;
        return false;
    };
    const auto FillMaskCount = [&](const RmlUE_SlateFrame& Current)
    {
        for (uint32_t Index = 0; Index < Current.DrawCount; ++Index)
            if (Current.Draws[Index].VisualNode == Fill) return Current.Draws[Index].ClipMaskCount;
        return UINT32_MAX;
    };
    const auto StabilizeReplay = [&]()
    {
        for (int Attempt = 0; Attempt < 3; ++Attempt)
        {
            if (!RmlUE_RenderSlate(Slate.View, &Frame)) return false;
            if (Frame.Replayed) return true;
        }
        return false;
    };

    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 &&
        HasOwnerMask(Frame) && FillMaskCount(Frame) > 0,
        "topology fixture records an owned rounded clip mask and child clip chain");
    Require(StabilizeReplay(), "unchanged mask topology converges to retained replay");

    RmlUE_AnimatedPropertyUpdate Update{Slate.View, Parent, RMLUE_ANIMATED_PROPERTY_TRANSFORM_2D,
        {10.f, 0.f, 1.f, 1.f, 0.f}, RmlUE_ResolveAnimationTarget(Slate.View, Parent)};
    uint8_t Accepted = 0;
    Require(Update.Target &&
        RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1,
        "stable owned-mask topology accepts an affine visual update");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "stable owned-mask topology remains on retained replay");

    Require(RmlUE_SetNodeProperty(Slate.View, Parent, "overflow", "visible") != 0,
        "remove an active target clip topology");
    Update.Values[0] = 20.f;
    Accepted = 1;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 0 && Accepted == 0 &&
        RmlUE_ApplyAnimatedProperties(&Update, 1) == 1,
        "content revision rejects stale mask bindings before property fallback");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 &&
        !HasOwnerMask(Frame) && FillMaskCount(Frame) == 0,
        "full record removes the stale mask owner and child clip chain");
    Require(StabilizeReplay(), "unclipped topology converges before visual replay resumes");
    Update.Values[0] = 30.f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1 &&
        RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "rebuilt unclipped topology accepts affine-only visual replay");

    Require(RmlUE_SetNodeProperty(Slate.View, Parent, "overflow", "hidden") != 0,
        "restore the active target clip topology");
    Update.Values[0] = 40.f;
    Accepted = 1;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 0 && Accepted == 0 &&
        RmlUE_ApplyAnimatedProperties(&Update, 1) == 1,
        "restored topology also rejects stale visual bindings before fallback");
    Require(RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 0 &&
        HasOwnerMask(Frame) && FillMaskCount(Frame) > 0,
        "full record restores the mask owner and rebuilt child clip chain");
    Require(StabilizeReplay(), "restored mask topology converges before visual replay resumes");
    Update.Values[0] = 50.f;
    Accepted = 0;
    Require(RmlUE_ApplyAnimatedVisualProperties(&Update, 1, &Accepted) == 1 && Accepted == 1 &&
        RmlUE_RenderSlate(Slate.View, &Frame) != 0 && Frame.Replayed == 1,
        "rebuilt owned-mask topology resumes affine-only visual replay");
    bool FoundMaskDelta = false;
    for (uint32_t Index = 0; Index < Frame.VisualDeltaCount; ++Index)
        FoundMaskDelta |= Frame.VisualDeltas[Index].Node == Parent &&
            Frame.VisualDeltas[Index].ClipMaskTransformChanged != 0;
    Require(FoundMaskDelta,
        "resumed replay publishes a delta for the rebuilt mask owner");
    Require(RmlUE_ApplyAnimatedProperties(&Update, 1) == 1 &&
        RmlUE_ClearAnimatedVisualProperties(&Update, 1) == 1 &&
        RmlUE_ReleaseAnimationTarget(Slate.View, Update.Target),
        "commit and release the topology transition target");
    std::fprintf(stderr, "PASS Slate visual mask topology fallback and replay convergence\n");
}

static void TestStrictCapabilities()
{
    const char* Markup = R"(<rml><head><style>body{margin:0;width:640px;height:480px;}#box{display:block;width:40px;height:30px;}</style></head><body><div id="box"/></body></rml>)";
    Fixture Slate(Markup, true); Slate.Update();
    const auto Box = Slate.Node("box"); RmlUE_SetStrictCapabilities(Slate.View, 1);
    const char* Unsupported[][2] = {
        {"filter", "blur(2px)"}, {"backdrop-filter", "blur(2px)"}, {"box-shadow", "2px 2px 2px #fff"}, {"mask-image", "none, none"},
        {"transform", "translate3d(1px,2px,3px)"}, {"transform", "scaleZ(2)"}, {"transform", "RoTaTeX(20deg)"},
        {"transform", "rotate3d(1,0,0,20deg)"}, {"transform", "scale3d(1,1,2)"}, {"perspective", "400px"},
        {"transform-origin-z", "1px"}, {"transform-origin", "50% 50% 1px"},
        {"decorator", "linear-gradient(90deg,#f00,#00f)"}, {"--effect", "shader(test)"}, {"--motion", "translate3d(1px,2px,3px)"},
        {"transform", "var(--motion)"}, {"decorator", "var(--effect)"}
    };
    for (const auto& Case : Unsupported) {
        Require(!RmlUE_SetNodeProperty(Slate.View, Box, Case[0], Case[1]), "strict Slate rejects unsupported dynamic renderer capability");
        const std::string Reason = RmlUE_GetLastError();
        Require(Reason.find("slate-rhi") != std::string::npos || Reason.find("explicit validated value") != std::string::npos, "strict rejection identifies capability rather than generic CSS parse failure");
    }
    Require(RmlUE_SetNodeProperty(Slate.View, Box, "filter", "none") && RmlUE_SetNodeProperty(Slate.View, Box, "transform", "translate(12px,8px)"), "strict Slate accepts no effect and explicit 2D transform");
    Require(RmlUE_SetNodeProperty(Slate.View, Box, "--color", "#123456") && RmlUE_SetNodeProperty(Slate.View, Box, "--size", "60px"), "strict Slate keeps safe dynamic theme and size tokens");
    Require(!RmlUE_SetNodeAttribute(Slate.View, Box, "style", "filter:blur(2px)") && !RmlUE_SetProperty(Slate.View, "box", "filter", "blur(2px)"), "legacy and style-attribute mutation cannot bypass strict gate");
    Fixture Dx11(Markup); Dx11.Update(); const auto DxBox = Dx11.Node("box"); RmlUE_SetStrictCapabilities(Dx11.View, 1);
    Require(RmlUE_SetNodeProperty(Dx11.View, DxBox, "filter", "blur(2px)") && RmlUE_SetNodeProperty(Dx11.View, DxBox, "decorator", "linear-gradient(90deg,#f00,#00f)"), "full DX11 renderer retains filter and shader support in strict mode");
    Dx11.Render();
    std::fprintf(stderr, "PASS backend-specific strict capability admission\n");
}

int main(int Count, char** Arguments)
{
    Require(Count >= 2, "usage: RmlUiHostTests font.ttf [evidence-directory] [all|nodes|measure|modal|pointer|animation|float-batch|targets|visual-batch|capabilities]");
    RmlUE_Host Host{}; Host.ReadFile = ReadFile; Host.LoadImage = LoadImage; Host.FreeBuffer = FreeBuffer; Host.Log = Log;
    Require(RmlUE_Initialize(&Host) != 0 && RmlUE_LoadFont(Arguments[1], 0) != 0, "initialize native host test runtime and font");
    Require(RmlUE_GetHostAbiVersion() == RMLUE_HOST_ABI_VERSION, "host ABI matches linked header");
    const auto Output = Count > 2 ? std::filesystem::u8path(Arguments[2]) : std::filesystem::path();
    if (!Output.empty()) std::filesystem::create_directories(Output);
    const std::string Suite = Count > 3 ? Arguments[3] : "all";
    Require(Suite == "all" || Suite == "nodes" || Suite == "measure" || Suite == "modal" || Suite == "pointer" || Suite == "animation" || Suite == "float-batch" || Suite == "targets" || Suite == "visual-batch" || Suite == "capabilities", "known test suite");
    if (Suite == "all" || Suite == "nodes") TestVariablesAndNodes();
    if (Suite == "all" || Suite == "measure") TestMeasure();
    if (Suite == "all" || Suite == "modal") TestDefaultAndModal();
    if (Suite == "all" || Suite == "pointer") TestPointers();
    if (Suite == "all" || Suite == "animation") TestAnimation(Output);
    if (Suite == "all" || Suite == "float-batch") TestAnimatedPropertyBatch();
    if (Suite == "all" || Suite == "float-batch" || Suite == "targets") TestAnimationTargets();
    if (Suite == "all" || Suite == "visual-batch") TestAnimatedVisualBatch();
    if (Suite == "all" || Suite == "visual-batch") TestAnimatedVisualBackgroundColor();
    if (Suite == "all" || Suite == "visual-batch") TestAnimatedVisualTransformBatch();
    if (Suite == "all" || Suite == "visual-batch") TestAnimatedVisualMaskTopology();
    if (Suite == "all" || Suite == "capabilities") TestStrictCapabilities();
    RmlUE_Shutdown();
    std::fprintf(stderr, "PASS RmlUiHostTests: %d checks; suite=%s; real DX11 rendering, host input and geometry\n", Checks, Suite.c_str());
    return 0;
}
