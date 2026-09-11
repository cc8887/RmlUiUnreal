#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "RmlUiJSRuntime.h"
#include "RmlUiWidget.h"
#include "SRmlUiWidget.h"
#include "RmlUiBridge.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlVueMountTest, "RmlUiUnreal.JS.VueMountAndReconcile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlVueMountTest::RunTest(const FString&)
{
    TStrongObjectPtr<URmlUiWidget> Widget(NewObject<URmlUiWidget>());
    TStrongObjectPtr<URmlUiJSRuntime> Runtime(NewObject<URmlUiJSRuntime>());
    if (!TestTrue(TEXT("Real Puerts/V8 mounts compiled Vue SFC"), Runtime->Start(Widget.Get(), TEXT(""), false))) return false;
    auto Slate = Widget->GetSlateRmlWidget();
    auto* Context = Runtime->GetContext();
    auto* View = Slate->GetNativeView();
    const auto OriginalNode = Context->FindNode(TEXT("vue-project-1"));
    TestTrue(TEXT("Vue created native grid nodes"), OriginalNode > 0);
    TestEqual(TEXT("Vue interpolation"), Context->GetText(Context->FindNode(TEXT("vue-preview-name"))), FString(TEXT("Northstar session")));
    const auto Click = [this, View](const char* Id) {
        RmlUE_Rect Rect{};
        if (!TestTrue(TEXT("Native event target has geometry"), RmlUE_GetElementRect(View, Id, &Rect) && Rect.Width > 0)) return;
        RmlUE_MouseMove(View, Rect.X + Rect.Width / 2, Rect.Y + Rect.Height / 2, 0);
        RmlUE_MouseButton(View, 0, 1, 0); RmlUE_MouseButton(View, 0, 0, 0);
    };
    Click("vue-reverse");
    Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("Keyed reorder preserves native node identity"), Context->FindNode(TEXT("vue-project-1")), OriginalNode);
    TestEqual(TEXT("Vue list reordered in native DOM"), Context->NextNode(Context->FindNode(TEXT("vue-project-3"))), Context->FindNode(TEXT("vue-project-2")));
    Click("vue-add"); Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("Reactive computed count"), Context->GetText(Context->FindNode(TEXT("vue-count"))), FString(TEXT("4")));
    Click("vue-enabled"); Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("Checkbox v-model updates Vue and native text"), Context->GetText(Context->FindNode(TEXT("vue-active"))), FString(TEXT("Paused")));
    Click("vue-name");
    RmlUE_Key(View, 0x41, 1, 2); RmlUE_Key(View, 0x41, 0, 2); RmlUE_Text(View, "Vue native input");
    Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("Text v-model updates interpolation"), Context->GetText(Context->FindNode(TEXT("vue-preview-name"))), FString(TEXT("Vue native input")));
    const FString BeforeReload = Context->CaptureState();
    const int32 PreviousCount = Runtime->ReloadCount;
    Runtime->Reload(); Slate->RenderFrame(1280, 800);
    Context = Runtime->GetContext(); View = Slate->GetNativeView();
    TestEqual(TEXT("Transactional Vue reload"), Runtime->ReloadCount, PreviousCount + 1);
    TestEqual(TEXT("State survives VM replacement"), Context->CaptureState(), BeforeReload);
    TestFalse(TEXT("Previous generation node handles are invalid"), Context->IsNodeValid(OriginalNode));
    for (const FIntPoint Size : {FIntPoint(1280, 800), FIntPoint(800, 600), FIntPoint(390, 844)}) {
        Slate->RenderFrame(Size.X, Size.Y);
        RmlUE_Frame Frame{};
        TestTrue(TEXT("Vue native rendering"), RmlUE_Render(View, &Frame) != 0 && Frame.Width == Size.X);
        RmlUE_Rect Rect{};
        TestTrue(TEXT("Grid stays within responsive viewport"), RmlUE_GetElementRect(View, "vue-workspace", &Rect) && Rect.X >= 0 && Rect.X + Rect.Width <= Size.X + 1);
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Frame.Width, Frame.Height,
            TArrayView64<const FColor>(reinterpret_cast<const FColor*>(Frame.Pixels), int64(Frame.Width) * Frame.Height), Png);
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VueTests"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        TestTrue(TEXT("Save Vue capture"), FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Directory, FString::Printf(TEXT("vue-%d.png"), Size.X))));
    }
    Runtime->Stop();
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlVueIsolationTest, "RmlUiUnreal.JS.MultipleViewsAndDisposal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlVueIsolationTest::RunTest(const FString&)
{
    TStrongObjectPtr<URmlUiWidget> First(NewObject<URmlUiWidget>()), Second(NewObject<URmlUiWidget>());
    TStrongObjectPtr<URmlUiJSRuntime> A(NewObject<URmlUiJSRuntime>()), B(NewObject<URmlUiJSRuntime>());
    if (!A->Start(First.Get(), TEXT(""), false) || !B->Start(Second.Get(), TEXT(""), false)) return TestTrue(TEXT("Both VMs mount"), false);
    const int32 Handle = A->GetContext()->FindNode(TEXT("vue-name"));
    TestFalse(TEXT("Handles cannot cross contexts"), B->GetContext()->IsNodeValid(Handle));
    int32 NodesBefore = 0, ListenersBefore = 0;
    RmlUE_GetNodeCounts(First->GetSlateRmlWidget()->GetNativeView(), &NodesBefore, &ListenersBefore);
    for (int32 Index = 0; Index < 10; ++Index) {
        A->Reload(); First->GetSlateRmlWidget()->RenderFrame(800, 600);
        int32 Nodes = 0, Listeners = 0;
        RmlUE_GetNodeCounts(First->GetSlateRmlWidget()->GetNativeView(), &Nodes, &Listeners);
        TestEqual(TEXT("Reload does not accumulate native handles"), Nodes, NodesBefore);
        TestEqual(TEXT("Reload does not accumulate native listeners"), Listeners, ListenersBefore);
        TestEqual(TEXT("Other VM is unaffected"), B->GetContext()->GetText(B->GetContext()->FindNode(TEXT("vue-count"))), FString(TEXT("3")));
    }
    A->Stop();
    TestTrue(TEXT("Second view renders after first VM destruction"), Second->GetSlateRmlWidget()->RenderFrame(800, 600));
    B->Stop();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlNodeContractTest, "RmlUiUnreal.JS.NativeNodeContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlNodeContractTest::RunTest(const FString&)
{
    RmlUE_View* View = RmlUE_CreateView(320, 200, 1);
    if (!View) return false;
    RmlUE_LoadDocumentFromMemory(View, "<rml><head><style>body { font-family:LatoLatin; } button { display:block;width:100px;height:40px; }</style></head><body/></rml>", "nodes.rml");
    const auto Root = RmlUE_GetRootNode(View), Parent = RmlUE_CreateNode(View, 0, "div"), Child = RmlUE_CreateNode(View, 0, "button");
    TestTrue(TEXT("Insert detached nodes"), RmlUE_InsertNode(View, Parent, Root, 0) && RmlUE_InsertNode(View, Child, Parent, 0));
    RmlUE_SetNodeAttribute(View, Child, "id", "node-click");
    RmlUE_SetNodeText(View, Child, "Safe <text> & data");
    char Text[128]{};
    TestTrue(TEXT("Read literal text"), RmlUE_GetNodeText(View, Child, Text, sizeof(Text)) != 0);
    TestEqual(TEXT("Text does not become markup"), FString(UTF8_TO_TCHAR(Text)), FString(TEXT("Safe <text> & data")));
    struct FEvents { int32 Root = 0; int32 Child = 0; bool Stop = true; } Events;
    RmlUE_SetNodeEventCallback(View, [](void* User, uint32 Id, const RmlUE_NodeEvent*) -> int {
        auto* State = static_cast<FEvents*>(User);
        if (Id == 1) { ++State->Root; return State->Stop ? 2 : 0; }
        ++State->Child; return 0;
    }, &Events);
    RmlUE_ListenNode(View, Root, "click", 1, 1);
    RmlUE_ListenNode(View, Child, "click", 2, 0);
    const auto Click = [View]() { RmlUE_Rect R{}; RmlUE_GetElementRect(View, "node-click", &R); RmlUE_MouseMove(View, R.X + R.Width / 2, R.Y + R.Height / 2, 0); RmlUE_MouseButton(View, 0, 1, 0); RmlUE_MouseButton(View, 0, 0, 0); };
    Click();
    TestEqual(TEXT("Capture invoked synchronously"), Events.Root, 1);
    TestEqual(TEXT("StopImmediatePropagation blocks target"), Events.Child, 0);
    Events.Stop = false; Click();
    TestEqual(TEXT("Target invoked with propagation enabled"), Events.Child, 1);
    RmlUE_UnlistenNode(View, 2); Click();
    TestEqual(TEXT("Unsubscribed callback not invoked"), Events.Child, 1);
    const auto Anchor = RmlUE_CreateNode(View, 2, "");
    RmlUE_InsertNode(View, Anchor, Parent, Child);
    TestEqual(TEXT("Fragment anchor preserves sibling order"), RmlUE_NextNode(View, Anchor), Child);
    RmlUE_RemoveNode(View, Parent);
    TestFalse(TEXT("Deleted descendants invalidate handles"), RmlUE_IsNodeValid(View, Child) != 0);
    TestFalse(TEXT("Deleted anchors invalidate handles"), RmlUE_IsNodeValid(View, Anchor) != 0);
    RmlUE_LoadDocumentFromMemory(View, "<rml><head/><body/></rml>", "replaced.rml");
    TestFalse(TEXT("Document replacement invalidates root handle"), RmlUE_IsNodeValid(View, Root) != 0);
    RmlUE_DestroyView(View);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlVueWatchTest, "RmlUiUnreal.JS.AutomaticManifestWatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
namespace {
class FVueWatchCommand final : public IAutomationLatentCommand
{
public:
    explicit FVueWatchCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (!Runtime.IsValid()) {
            Widget.Reset(NewObject<URmlUiWidget>());
            Runtime.Reset(NewObject<URmlUiJSRuntime>());
            if (!Test->TestTrue(TEXT("Watch fixture source mounts"), Runtime->Start(Widget.Get(), TEXT(""), false))) return true;
            const FString Source = FPaths::GetPath(Runtime->ActiveManifest);
            const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VueWatch"), FGuid::NewGuid().ToString());
            auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
            for (const TCHAR* Name : {TEXT("first"), TEXT("second")}) {
                if (!Test->TestTrue(TEXT("Stage watch fixture"), Platform.CopyDirectoryTree(*FPaths::Combine(Root, Name), *Source, true))) { Runtime->Stop(); return true; }
            }
            const FString Second = FPaths::Combine(Root, TEXT("second/manifest.json"));
            FString Text; FFileHelper::LoadFileToString(Text, *Second);
            TSharedPtr<FJsonObject> Json;
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json);
            Json->SetStringField(TEXT("version"), TEXT("watched-version"));
            Text.Reset(); FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
            FFileHelper::SaveStringToFile(Text, *Second);
            Pointer = FPaths::Combine(Root, TEXT("current.json"));
            FFileHelper::SaveStringToFile(TEXT("{\"manifest\":\"first/manifest.json\"}"), *Pointer);
            if (!Test->TestTrue(TEXT("File watcher starts"), Runtime->Start(Widget.Get(), Pointer, true))) return true;
            auto Slate = Widget->GetSlateRmlWidget();
            Slate->RenderFrame(800, 600);
            RmlUE_View* View = Slate->GetNativeView();
            RmlUE_Rect Rect{}; RmlUE_GetElementRect(View, "vue-add", &Rect);
            RmlUE_MouseMove(View, Rect.X + Rect.Width / 2, Rect.Y + Rect.Height / 2, 0);
            RmlUE_MouseButton(View, 0, 1, 0); RmlUE_MouseButton(View, 0, 0, 0);
            Slate->RenderFrame(800, 600);
            State = Runtime->GetContext()->CaptureState();
            Count = Runtime->ReloadCount;
            const FString Temporary = Pointer + TEXT(".tmp");
            FFileHelper::SaveStringToFile(TEXT("{\"manifest\":\"second/manifest.json\"}"), *Temporary);
            IFileManager::Get().Move(*Pointer, *Temporary, true);
            Deadline = FPlatformTime::Seconds() + 3;
            return false;
        }
        Widget->GetSlateRmlWidget()->RenderFrame(800, 600);
        if (Runtime->ReloadCount == Count && FPlatformTime::Seconds() < Deadline) return false;
        Test->TestEqual(TEXT("Pointer save automatically activates new version"), Runtime->ActiveVersion, FString(TEXT("watched-version")));
        Test->TestEqual(TEXT("Exactly one automatic reload"), Runtime->ReloadCount, Count + 1);
        Test->TestEqual(TEXT("Automatic reload preserves interactive state"), Runtime->GetContext()->CaptureState(), State);
        Runtime->Stop();
        return true;
    }
private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<URmlUiWidget> Widget;
    TStrongObjectPtr<URmlUiJSRuntime> Runtime;
    FString Pointer, State;
    int32 Count = 0;
    double Deadline = 0;
};
}
bool FRmlVueWatchTest::RunTest(const FString&)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FVueWatchCommand>(this));
    return true;
}
#endif
