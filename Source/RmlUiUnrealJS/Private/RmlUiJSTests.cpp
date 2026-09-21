#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "RmlUiJSContext.h"
#include "RmlUiJSRuntime.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiPerformance.h"
#include "RmlUiUnrealModule.h"
#include "RmlUiWidget.h"
#include "SRmlUiWidget.h"
#include "RmlUiBridge.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
bool FindSlateOpacity(const RmlUE_SlateFrame& Frame, uint32 Node, float& OutOpacity)
{
    for (uint32 Index = 0; Index < Frame.DrawCount; ++Index)
    {
        if (Frame.Draws[Index].VisualNode == Node)
        {
            OutOpacity = Frame.Draws[Index].VisualOpacity;
            return true;
        }
    }
    return false;
}

TSharedPtr<FJsonObject> ParseAnimationResult(const FString& Json)
{
    TSharedPtr<FJsonObject> Result;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result);
    return Result;
}

template<typename T>
void AppendPackedValue(TArray<uint8>& Bytes, const T& Value)
{
    const int32 Offset = Bytes.AddUninitialized(sizeof(T));
    FMemory::Memcpy(Bytes.GetData() + Offset, &Value, sizeof(T));
}

void AppendPackedHeader(TArray<uint8>& Bytes, uint32 Magic, uint32 Count)
{
    AppendPackedValue(Bytes, Magic);
    AppendPackedValue(Bytes, static_cast<uint16>(1));
    AppendPackedValue(Bytes, static_cast<uint16>(0));
    AppendPackedValue(Bytes, Count);
}
}

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
    const double InitialWakeTime = Context->GetNextWakeTimeSeconds(FPlatformTime::Seconds());
    TestTrue(TEXT("compiled JS publishes its interval deadline to UE"),
        FMath::IsFinite(InitialWakeTime) && InitialWakeTime > FPlatformTime::Seconds());
    const int32 InitialAdvanceCount = Runtime->AdvanceCount;
    const int32 InitialSkipCount = Runtime->AdvanceSkipCount;
    Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("early render does not enter Puerts frame callbacks"), Runtime->AdvanceCount, InitialAdvanceCount);
    TestTrue(TEXT("early render records a JS deadline skip"), Runtime->AdvanceSkipCount > InitialSkipCount);
    Context->SetWakeSchedule(0.0f, false);
    Slate->RenderFrame(1280, 800);
    TestEqual(TEXT("due JS deadline enters Puerts exactly once"), Runtime->AdvanceCount, InitialAdvanceCount + 1);
    TestTrue(TEXT("JS callback republishes its next interval deadline"),
        Context->GetNextWakeTimeSeconds(FPlatformTime::Seconds()) > FPlatformTime::Seconds());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlKeyframeAnimationBridgeTest,
    "RmlUiUnreal.JS.KeyframeAnimationBridge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlKeyframeAnimationBridgeTest::RunTest(const FString&)
{
    const FString FixtureDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Automation/KeyframeBridgeFixture"));
    const FString EntryPath = FPaths::Combine(FixtureDirectory, TEXT("entry.js"));
    const FString EntrySource =
        TEXT("const bridge = require('puerts').argv.getByName('bridge');\n")
        TEXT("const node = bridge.FindNode('keyframe-target');\n")
        TEXT("const frames = JSON.stringify([\n")
        TEXT("  {offset:0,value:1,easing:'ease-in-out'},\n")
        TEXT("  {offset:0.5,value:0.2,easing:'linear'},\n")
        TEXT("  {offset:1,value:0.8}\n")
        TEXT("]);\n")
        TEXT("const result = JSON.parse(bridge.StartNodeKeyframeAnimation(node, 'opacity', frames, ")
        TEXT("JSON.stringify({duration:1,iterations:1,direction:'normal',fill:'both',composite:'replace'})));\n")
        TEXT("const events = [];\n")
        TEXT("bridge.OnAnimationEvent.Add(json => { events.push(JSON.parse(json)); ")
        TEXT("bridge.ReportDebugState(JSON.stringify({result:result,node:node,events:events})); });\n")
        TEXT("bridge.ReportDebugState(JSON.stringify({result:result,node:node,events:events}));\n")
        TEXT("bridge.ReportReady();\n");
    IFileManager::Get().MakeDirectory(*FixtureDirectory, true);
    if (!TestTrue(TEXT("write minimal Puerts keyframe fixture"),
        FFileHelper::SaveStringToFile(EntrySource, *EntryPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;

    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    if (!TestNotNull(TEXT("create keyframe Slate view"), View)) return false;
    const char* Document =
        "<rml><head><style>body{margin:0;width:320px;height:200px;}"
        "#keyframe-target,.stagger-target{display:block;width:100px;height:50px;"
        "background-color:#fff;opacity:1;}</style></head>"
        "<body><div id='keyframe-target'/><div class='stagger-target'/><div class='stagger-target'/></body></rml>";
    if (!TestTrue(TEXT("load keyframe bridge document"),
        RmlUE_LoadDocumentFromMemory(View, Document, "keyframe-bridge.rml") != 0))
    {
        RmlUE_DestroyView(View);
        return false;
    }
    RmlUE_Update(View);
    RmlUE_SlateFrame BaselineFrame{};
    TestTrue(TEXT("establish retained Slate baseline"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);

    FRmlUiAnimationRuntime& AnimationRuntime =
        FRmlUiUnrealModule::Get().GetAnimationRuntime();
    const int32 DefinitionsBefore = AnimationRuntime.GetDefinitionCount();
    const int32 BindingsBefore = AnimationRuntime.GetBindingCount();
    const int32 ActiveBefore = AnimationRuntime.GetActiveAnimationCount();
    TStrongObjectPtr<URmlUiJSContext> Context(NewObject<URmlUiJSContext>());
    if (!TestTrue(TEXT("minimal Puerts keyframe bridge starts"),
        Context->Initialize(View, FixtureDirectory, TEXT("entry.js"),
            TEXT("keyframe-test"), TEXT("{}"), -1, {})))
    {
        AddError(Context->LastError);
        Context->Dispose();
        RmlUE_DestroyView(View);
        return false;
    }
    TestTrue(TEXT("JavaScript called structured keyframe bridge"),
        Context->DebugStateJson.Contains(TEXT("\"accepted\":true")) &&
        Context->DebugStateJson.Contains(TEXT("\"route\":\"native\"")));
    TSharedPtr<FJsonObject> DebugState;
    TestTrue(TEXT("structured keyframe debug state parses"),
        FJsonSerializer::Deserialize(
            TJsonReaderFactory<>::Create(Context->DebugStateJson), DebugState) &&
        DebugState.IsValid());
    FString InitialHandle;
    if (DebugState.IsValid())
    {
        const TSharedPtr<FJsonObject>* ResultObject = nullptr;
        if (DebugState->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject && ResultObject->IsValid())
            (*ResultObject)->TryGetStringField(TEXT("handle"), InitialHandle);
    }
    TestFalse(TEXT("structured result exposes an opaque string handle"), InitialHandle.IsEmpty());
    const uint32 Node = static_cast<uint32>(Context->FindNode(TEXT("keyframe-target")));
    TestTrue(TEXT("keyframe bridge target exists"), Node != 0);
    const TSharedPtr<FJsonObject> Snapshot = ParseAnimationResult(
        Context->ResolveAnimationHostSnapshot(FString::Printf(
            TEXT("{\"targets\":[%u,\"#keyframe-target\"],")
            TEXT("\"properties\":[\"opacity\",\"width\"],\"includeMetrics\":true}"),
            Node)));
    TestTrue(TEXT("animation HostSnapshot resolves selector and node in one call"),
        Snapshot.IsValid() && Snapshot->GetBoolField(TEXT("accepted")) &&
        Snapshot->GetStringField(TEXT("revision")).Len() > 0);
    if (Snapshot.IsValid() && Snapshot->GetBoolField(TEXT("accepted")))
    {
        const TArray<TSharedPtr<FJsonValue>>& SnapshotNodes = Snapshot->GetArrayField(TEXT("nodes"));
        TestEqual(TEXT("animation HostSnapshot deduplicates targets"), SnapshotNodes.Num(), 1);
        const TArray<TSharedPtr<FJsonValue>>& TargetGroups = Snapshot->GetArrayField(TEXT("targetGroups"));
        TestEqual(TEXT("animation HostSnapshot preserves one group per target spec"), TargetGroups.Num(), 2);
        if (TargetGroups.Num() == 2)
        {
            const TArray<TSharedPtr<FJsonValue>>& NodeGroup = TargetGroups[0]->AsArray();
            const TArray<TSharedPtr<FJsonValue>>& SelectorGroup = TargetGroups[1]->AsArray();
            TestTrue(TEXT("animation HostSnapshot groups retain duplicate target ownership"),
                NodeGroup.Num() == 1 && SelectorGroup.Num() == 1 &&
                NodeGroup[0]->AsNumber() == Node && SelectorGroup[0]->AsNumber() == Node);
        }
        if (SnapshotNodes.Num() == 1)
        {
            const TSharedPtr<FJsonObject> SnapshotNode = SnapshotNodes[0]->AsObject();
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const TSharedPtr<FJsonObject>* Metrics = nullptr;
            FString SnapshotOpacity;
            FString SnapshotWidth;
            TestTrue(TEXT("animation HostSnapshot contains computed values"),
                SnapshotNode.IsValid() && SnapshotNode->TryGetObjectField(TEXT("properties"), Properties) &&
                Properties && Properties->IsValid() &&
                (*Properties)->TryGetStringField(TEXT("opacity"), SnapshotOpacity) &&
                (*Properties)->TryGetStringField(TEXT("width"), SnapshotWidth) &&
                FMath::IsNearlyEqual(FCString::Atof(*SnapshotOpacity), 1.0f) &&
                SnapshotWidth == TEXT("100px"));
            TestTrue(TEXT("animation HostSnapshot contains ReferenceBox metrics"),
                SnapshotNode.IsValid() && SnapshotNode->TryGetObjectField(TEXT("metrics"), Metrics) &&
                Metrics && Metrics->IsValid() &&
                FMath::IsNearlyEqual((*Metrics)->GetNumberField(TEXT("width")), 100.0));
        }
    }
    const TSharedPtr<FJsonObject> MissingSnapshot = ParseAnimationResult(
        Context->ResolveAnimationHostSnapshot(
            TEXT("{\"targets\":[\"#missing-animation-target\"],\"properties\":[]}")));
    TestTrue(TEXT("animation HostSnapshot rejects an empty selector result"),
        MissingSnapshot.IsValid() && !MissingSnapshot->GetBoolField(TEXT("accepted")) &&
        MissingSnapshot->GetStringField(TEXT("error")) == TEXT("snapshot_target_not_found"));
    TestEqual(TEXT("keyframe bridge retains one immutable definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);
    TestEqual(TEXT("keyframe bridge retains one target binding"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 1);
    TestEqual(TEXT("keyframe bridge starts one ECS entity"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 1);
    AnimationRuntime.Advance(0.5f);
    RmlUE_SlateFrame MidpointFrame{};
    TestTrue(TEXT("keyframe bridge midpoint renders"),
        RmlUE_RenderSlate(View, &MidpointFrame) != 0);
    float MidpointOpacity = 0.0f;
    TestTrue(TEXT("keyframe bridge midpoint reaches RmlSlate draw"),
        FindSlateOpacity(MidpointFrame, Node, MidpointOpacity) &&
        FMath::IsNearlyEqual(MidpointOpacity, 0.2f, 0.001f));

    const FString ReplacementFrames =
        TEXT("[{\"offset\":0,\"value\":0.2},")
        TEXT("{\"offset\":1,\"value\":1,\"easing\":\"cubic-bezier(0.42,0,0.58,1)\"}]");
    const FString ReplacementResult = Context->StartNodeKeyframeAnimation(
        Node, TEXT("opacity"), ReplacementFrames,
        TEXT("{\"duration\":1,\"delay\":0.1,\"iterations\":2,\"playbackRate\":2,")
        TEXT("\"direction\":\"alternate\",\"fill\":\"both\",\"composite\":\"replace\"}"));
    TSharedPtr<FJsonObject> ReplacementObject;
    TestTrue(TEXT("keyframe bridge accepts structured timing options"),
        FJsonSerializer::Deserialize(
            TJsonReaderFactory<>::Create(ReplacementResult), ReplacementObject) &&
        ReplacementObject.IsValid() && ReplacementObject->GetBoolField(TEXT("accepted")));
    FString ReplacementHandle;
    if (ReplacementObject.IsValid())
        ReplacementObject->TryGetStringField(TEXT("handle"), ReplacementHandle);
    TestEqual(TEXT("replacement does not accumulate definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);
    TestEqual(TEXT("replacement does not accumulate bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 1);
    TestEqual(TEXT("replacement leaves one ECS entity"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 1);
    TestTrue(TEXT("replacement emits an event for the previous handle"),
        Context->DebugStateJson.Contains(TEXT("\"reason\":\"replaced\"")) &&
        Context->DebugStateJson.Contains(InitialHandle));
    const TSharedPtr<FJsonObject> StaleResult = ParseAnimationResult(
        Context->ControlAnimation(InitialHandle, TEXT("status"), 0.0));
    TestTrue(TEXT("replacement invalidates the previous opaque handle"),
        StaleResult.IsValid() &&
        StaleResult->GetStringField(TEXT("error")) == TEXT("stale_handle"));
    const TSharedPtr<FJsonObject> PauseResult = ParseAnimationResult(
        Context->ControlAnimation(ReplacementHandle, TEXT("pause"), 0.0));
    TestTrue(TEXT("structured control pauses the replacement"),
        PauseResult.IsValid() && PauseResult->GetBoolField(TEXT("accepted")) &&
        PauseResult->GetStringField(TEXT("state")) == TEXT("paused"));
    const TSharedPtr<FJsonObject> SeekResult = ParseAnimationResult(
        Context->ControlAnimation(ReplacementHandle, TEXT("seek"), 0.5));
    TestTrue(TEXT("structured control seeks the paused replacement"),
        SeekResult.IsValid() && SeekResult->GetBoolField(TEXT("accepted")));
    const TSharedPtr<FJsonObject> ResumeResult = ParseAnimationResult(
        Context->ControlAnimation(ReplacementHandle, TEXT("resume"), 0.0));
    TestTrue(TEXT("structured control resumes the replacement"),
        ResumeResult.IsValid() && ResumeResult->GetBoolField(TEXT("accepted")) &&
        ResumeResult->GetStringField(TEXT("state")) == TEXT("running"));
    const TSharedPtr<FJsonObject> CancelResult = ParseAnimationResult(
        Context->ControlAnimation(ReplacementHandle, TEXT("cancel"), 0.0));
    TestTrue(TEXT("structured keyframe cancellation succeeds"),
        CancelResult.IsValid() && CancelResult->GetBoolField(TEXT("accepted")) &&
        CancelResult->GetStringField(TEXT("state")) == TEXT("cancelled"));
    TestEqual(TEXT("cancellation releases adapter definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    TestEqual(TEXT("cancellation releases adapter binding"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);
    TestEqual(TEXT("cancellation removes adapter entity"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);
    TestTrue(TEXT("cancellation emits an event for the controlled handle"),
        Context->DebugStateJson.Contains(TEXT("\"reason\":\"cancelled\"")) &&
        Context->DebugStateJson.Contains(ReplacementHandle));

    const TSharedPtr<FJsonObject> NaturalResult = ParseAnimationResult(
        Context->StartNodeKeyframeAnimation(
            Node, TEXT("opacity"), ReplacementFrames, TEXT("{\"duration\":0.1}")));
    FString NaturalHandle;
    if (NaturalResult.IsValid()) NaturalResult->TryGetStringField(TEXT("handle"), NaturalHandle);
    AnimationRuntime.Advance(0.1f);
    TestTrue(TEXT("natural completion emits a finished event"),
        !NaturalHandle.IsEmpty() && Context->DebugStateJson.Contains(NaturalHandle) &&
        Context->DebugStateJson.Contains(TEXT("\"reason\":\"completed\"")) &&
        Context->DebugStateJson.Contains(TEXT("\"state\":\"finished\"")));
    TestEqual(TEXT("natural completion releases adapter definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    TestEqual(TEXT("natural completion releases adapter binding"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);

    const FString InvalidAnimationBatch = Context->StartNodeKeyframeAnimationBatch(
        FString::Printf(
            TEXT("[{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.2}")
            TEXT("},{")
            TEXT("\"node\":%u,\"property\":\"width\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":\"10px\"},{\"offset\":1,\"value\":\"20px\"}],")
            TEXT("\"options\":{\"duration\":0.2}")
            TEXT("}]"), Node, Node));
    const TSharedPtr<FJsonObject> InvalidAnimationBatchObject =
        ParseAnimationResult(InvalidAnimationBatch);
    TestTrue(TEXT("animation batch reports the failing request index"),
        InvalidAnimationBatchObject.IsValid() &&
        !InvalidAnimationBatchObject->GetBoolField(TEXT("accepted")) &&
        InvalidAnimationBatchObject->GetIntegerField(TEXT("failedIndex")) == 1 &&
        InvalidAnimationBatchObject->GetStringField(TEXT("error")) == TEXT("unsupported_property"));
    TestEqual(TEXT("rejected animation batch rolls back prepared definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    TestEqual(TEXT("rejected animation batch rolls back prepared bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);
    TestEqual(TEXT("rejected animation batch starts no ECS entity"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);

    const FString ValidAnimationBatch = Context->StartNodeKeyframeAnimationBatch(
        FString::Printf(
            TEXT("[{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.2}")
            TEXT("},{")
            TEXT("\"node\":%u,\"property\":\"transform\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":\"scale(1)\"},{\"offset\":1,\"value\":\"scale(2)\"}],")
            TEXT("\"options\":{\"duration\":0.2}")
            TEXT("}]"), Node, Node));
    const TSharedPtr<FJsonObject> ValidAnimationBatchObject =
        ParseAnimationResult(ValidAnimationBatch);
    TestTrue(TEXT("animation batch starts all prepared tracks"),
        ValidAnimationBatchObject.IsValid() &&
        ValidAnimationBatchObject->GetBoolField(TEXT("accepted")) &&
        ValidAnimationBatchObject->GetArrayField(TEXT("handles")).Num() == 2);
    TestEqual(TEXT("animation batch retains one definition per track"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 2);
    TestEqual(TEXT("animation batch retains one binding per track"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 2);
    TestEqual(TEXT("animation batch starts both ECS entities together"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 2);
    AnimationRuntime.Advance(0.1f);
    TestEqual(TEXT("animation batch tracks remain active at the shared midpoint"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 2);
    AnimationRuntime.Advance(0.1f);
    TestEqual(TEXT("animation batch tracks complete on the same runtime step"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);
    TestEqual(TEXT("animation batch completion releases definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    TestEqual(TEXT("animation batch completion releases bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);

    const FString DuplicateContributionBatch = Context->StartNodeKeyframeAnimationBatch(
        FString::Printf(
            TEXT("[{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.2,\"composite\":\"layered-replace\",\"compositionOrder\":0}")
            TEXT("},{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0.5},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.05,\"delay\":0.1,\"composite\":\"layered-replace\",\"compositionOrder\":0}")
            TEXT("}]") , Node, Node));
    const TSharedPtr<FJsonObject> DuplicateContributionObject =
        ParseAnimationResult(DuplicateContributionBatch);
    TestTrue(TEXT("animation batch rejects duplicate same-target contribution order"),
        DuplicateContributionObject.IsValid() &&
        !DuplicateContributionObject->GetBoolField(TEXT("accepted")) &&
        DuplicateContributionObject->GetStringField(TEXT("error")) ==
            TEXT("duplicate_batch_contribution_order"));

    const FString LayeredAnimationBatch = Context->StartNodeKeyframeAnimationBatch(
        FString::Printf(
            TEXT("[{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.2,\"composite\":\"layered-replace\",\"compositionOrder\":0}")
            TEXT("},{")
            TEXT("\"node\":%u,\"property\":\"opacity\",")
            TEXT("\"keyframes\":[{\"offset\":0,\"value\":0.5},{\"offset\":1,\"value\":1}],")
            TEXT("\"options\":{\"duration\":0.05,\"delay\":0.1,\"composite\":\"layered-replace\",\"compositionOrder\":1}")
            TEXT("}]") , Node, Node));
    const TSharedPtr<FJsonObject> LayeredAnimationObject =
        ParseAnimationResult(LayeredAnimationBatch);
    TestTrue(TEXT("animation bridge accepts ordered same-target contributions"),
        LayeredAnimationObject.IsValid() &&
        LayeredAnimationObject->GetBoolField(TEXT("accepted")) &&
        LayeredAnimationObject->GetArrayField(TEXT("handles")).Num() == 2);
    TestEqual(TEXT("different layered contents keep distinct definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 2);
    TestEqual(TEXT("different layered contents keep independent bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 2);
    FString UpperContributionHandle;
    if (LayeredAnimationObject.IsValid() &&
        LayeredAnimationObject->GetArrayField(TEXT("handles")).Num() == 2)
    {
        UpperContributionHandle = LayeredAnimationObject->GetArrayField(TEXT("handles"))[1]->AsString();
    }
    const TSharedPtr<FJsonObject> LayeredSeekResult = ParseAnimationResult(
        Context->ControlAnimation(UpperContributionHandle, TEXT("seek"), 0.125));
    TestTrue(TEXT("layered contribution rejects history-dependent direct seek"),
        LayeredSeekResult.IsValid() && !LayeredSeekResult->GetBoolField(TEXT("accepted")) &&
        LayeredSeekResult->GetStringField(TEXT("error")) == TEXT("control_failed"));
    TestEqual(TEXT("layered animation batch starts two ECS contributions"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 2);
    AnimationRuntime.Advance(0.15f);
    TestEqual(TEXT("upper layered contribution retires after its completion frame"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 1);
    AnimationRuntime.Advance(0.05f);
    TestEqual(TEXT("lower layered contribution completes independently"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);
    TestEqual(TEXT("layered completion releases prepared definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    TestEqual(TEXT("layered completion releases prepared bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);

    const TSharedPtr<FJsonObject> BatchResult = ParseAnimationResult(
        Context->ApplyNodePropertyBatch(FString::Printf(
            TEXT("[{\"node\":%u,\"property\":\"width\",\"value\":\"123px\"},")
            TEXT("{\"node\":%u,\"property\":\"height\",\"value\":\"45px\"}]"),
            Node, Node)));
    TestTrue(TEXT("JS fallback property batch applies in one bridge call"),
        BatchResult.IsValid() && BatchResult->GetBoolField(TEXT("accepted")) &&
        BatchResult->GetIntegerField(TEXT("applied")) == 2);
    TestEqual(TEXT("property batch updates width"),
        Context->GetComputedProperty(Node, TEXT("width")), FString(TEXT("123px")));
    const TSharedPtr<FJsonObject> InvalidBatch = ParseAnimationResult(
        Context->ApplyNodePropertyBatch(
            TEXT("[{\"node\":2147483647,\"property\":\"width\",\"value\":\"1px\"}]")));
    TestTrue(TEXT("property batch rejects stale targets before applying"),
        InvalidBatch.IsValid() && !InvalidBatch->GetBoolField(TEXT("accepted")) &&
        InvalidBatch->GetIntegerField(TEXT("applied")) == 0 &&
        InvalidBatch->GetStringField(TEXT("error")) == TEXT("stale_property_target"));

    const FString MixedTransform =
        TEXT("[{\"offset\":0,\"value\":\"translate(0px,0px)\"},")
        TEXT("{\"offset\":1,\"value\":\"rotate(90deg)\"}]");
    const TSharedPtr<FJsonObject> MixedResult = ParseAnimationResult(
        Context->StartNodeKeyframeAnimation(
            Node, TEXT("transform"), MixedTransform, TEXT("{\"duration\":1}")));
    TestTrue(TEXT("keyframe bridge reports mixed transform rejection"),
        MixedResult.IsValid() &&
        MixedResult->GetStringField(TEXT("route")) == TEXT("rejected") &&
        MixedResult->GetStringField(TEXT("error")) == TEXT("mixed_transform_primitives"));
    const TSharedPtr<FJsonObject> FillResult = ParseAnimationResult(
        Context->StartNodeKeyframeAnimation(
            Node, TEXT("opacity"), ReplacementFrames,
            TEXT("{\"duration\":1,\"fill\":\"none\"}")));
    TestTrue(TEXT("unsupported fill returns a diagnostic instead of silently changing semantics"),
        FillResult.IsValid() &&
        FillResult->GetStringField(TEXT("error")) == TEXT("unsupported_fill"));

    TArray<uint8> PlanBytes;
    AppendPackedHeader(PlanBytes, 0x31504152, 1);
    AppendPackedValue(PlanBytes, static_cast<uint8>(ERmlUiAnimatedProperty::Opacity));
    AppendPackedValue(PlanBytes, static_cast<uint8>(ERmlUiAnimationDirection::Normal));
    AppendPackedValue(PlanBytes, static_cast<uint16>(2));
    AppendPackedValue(PlanBytes, static_cast<uint32>(1));
    AppendPackedValue(PlanBytes, 0.1);
    AppendPackedValue(PlanBytes, 0.0);
    AppendPackedValue(PlanBytes, 1.0);
    const auto AppendLinearOpacityKeyframe = [](TArray<uint8>& Bytes, float Offset, float Value)
    {
        AppendPackedValue(Bytes, Offset);
        AppendPackedValue(Bytes, Value);
        AppendPackedValue(Bytes, static_cast<uint8>(ERmlUiAnimationEasingType::Linear));
        AppendPackedValue(Bytes, static_cast<uint8>(0));
        AppendPackedValue(Bytes, static_cast<uint8>(0));
        AppendPackedValue(Bytes, static_cast<uint8>(0));
        AppendPackedValue(Bytes, 0.0f);
        AppendPackedValue(Bytes, 0.0f);
        AppendPackedValue(Bytes, 1.0f);
        AppendPackedValue(Bytes, 1.0f);
    };
    AppendLinearOpacityKeyframe(PlanBytes, 0.0f, 0.25f);
    AppendLinearOpacityKeyframe(PlanBytes, 1.0f, 1.0f);
    FArrayBuffer PlanPayload;
    PlanPayload.Data = PlanBytes.GetData();
    PlanPayload.Length = PlanBytes.Num();
    const TSharedPtr<FJsonObject> PlanRegistration = ParseAnimationResult(
        Context->RegisterAnimationPlansPacked(PlanPayload));
    FString PlanHandleText;
    if (PlanRegistration.IsValid() && PlanRegistration->GetArrayField(TEXT("handles")).Num() == 1)
        PlanHandleText = PlanRegistration->GetArrayField(TEXT("handles"))[0]->AsString();
    uint64 PlanHandle = 0;
    LexTryParseString(PlanHandle, *PlanHandleText);
    TestTrue(TEXT("packed animation plan registers a generational handle"),
        PlanRegistration.IsValid() && PlanRegistration->GetBoolField(TEXT("accepted")) && PlanHandle != 0);
    TestEqual(TEXT("compiled plan retains one MovieScene definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);
    const TSharedPtr<FJsonObject> RegisteredPlanStats = ParseAnimationResult(
        Context->GetAnimationPlanCacheStats());
    TestTrue(TEXT("compiled plan cache exposes expanded allocation"),
        RegisteredPlanStats.IsValid() &&
        RegisteredPlanStats->GetIntegerField(TEXT("activePlans")) == 1 &&
        RegisteredPlanStats->GetNumberField(TEXT("allocatedBytes")) > 0.0);

    TArray<uint8> StartBytes;
    AppendPackedHeader(StartBytes, 0x31494152, 1);
    AppendPackedValue(StartBytes, static_cast<uint32>(PlanHandle));
    AppendPackedValue(StartBytes, static_cast<uint32>(PlanHandle >> 32));
    AppendPackedValue(StartBytes, Node);
    AppendPackedValue(StartBytes, static_cast<int32>(0));
    AppendPackedValue(StartBytes, static_cast<uint8>(0));
    for (int32 Reserved = 0; Reserved < 7; ++Reserved)
        AppendPackedValue(StartBytes, static_cast<uint8>(0));
    FArrayBuffer StartPayload;
    StartPayload.Data = StartBytes.GetData();
    StartPayload.Length = StartBytes.Num();
    const TSharedPtr<FJsonObject> CompiledStart = ParseAnimationResult(
        Context->StartCompiledAnimationBatchPacked(StartPayload));
    TestTrue(TEXT("compiled plan starts through the packed instance path"),
        CompiledStart.IsValid() && CompiledStart->GetBoolField(TEXT("accepted")) &&
        CompiledStart->GetArrayField(TEXT("handles")).Num() == 1);
    TestEqual(TEXT("compiled start creates one binding"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 1);
    AnimationRuntime.Advance(0.11f);
    TestEqual(TEXT("compiled completion releases its binding"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);
    TestEqual(TEXT("compiled completion retains the cached definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);

    TArray<uint8> ReleaseBytes;
    AppendPackedHeader(ReleaseBytes, 0x31524152, 1);
    AppendPackedValue(ReleaseBytes, static_cast<uint32>(PlanHandle));
    AppendPackedValue(ReleaseBytes, static_cast<uint32>(PlanHandle >> 32));
    FArrayBuffer ReleasePayload;
    ReleasePayload.Data = ReleaseBytes.GetData();
    ReleasePayload.Length = ReleaseBytes.Num();
    const TSharedPtr<FJsonObject> PlanRelease = ParseAnimationResult(
        Context->ReleaseAnimationPlansPacked(ReleasePayload));
    TestTrue(TEXT("completed compiled plan releases explicitly"),
        PlanRelease.IsValid() && PlanRelease->GetBoolField(TEXT("accepted")) &&
        PlanRelease->GetIntegerField(TEXT("released")) == 1);
    TestEqual(TEXT("explicit plan release frees the MovieScene definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    const TSharedPtr<FJsonObject> ReleasedPlanStats = ParseAnimationResult(
        Context->GetAnimationPlanCacheStats());
    TestTrue(TEXT("released plan cache statistics return to zero"),
        ReleasedPlanStats.IsValid() &&
        ReleasedPlanStats->GetIntegerField(TEXT("activePlans")) == 0 &&
        ReleasedPlanStats->GetNumberField(TEXT("allocatedBytes")) == 0.0);
    const TSharedPtr<FJsonObject> StalePlanStart = ParseAnimationResult(
        Context->StartCompiledAnimationBatchPacked(StartPayload));
    TestTrue(TEXT("released generation cannot start a new animation"),
        StalePlanStart.IsValid() && !StalePlanStart->GetBoolField(TEXT("accepted")) &&
        StalePlanStart->GetStringField(TEXT("error")) == TEXT("stale_plan_handle"));

    Context->CompiledAnimationPlanResidentLimitBytes = 1024;
    TArray<uint8> OversizedPlanBytes;
    AppendPackedHeader(OversizedPlanBytes, 0x31504152, 1);
    AppendPackedValue(OversizedPlanBytes, static_cast<uint8>(ERmlUiAnimatedProperty::Opacity));
    AppendPackedValue(OversizedPlanBytes, static_cast<uint8>(ERmlUiAnimationDirection::Normal));
    AppendPackedValue(OversizedPlanBytes, static_cast<uint16>(8));
    AppendPackedValue(OversizedPlanBytes, static_cast<uint32>(1));
    AppendPackedValue(OversizedPlanBytes, 0.1);
    AppendPackedValue(OversizedPlanBytes, 0.0);
    AppendPackedValue(OversizedPlanBytes, 1.0);
    for (int32 Index = 0; Index < 8; ++Index)
        AppendLinearOpacityKeyframe(OversizedPlanBytes, Index / 7.0f, Index / 7.0f);
    FArrayBuffer OversizedPlanPayload;
    OversizedPlanPayload.Data = OversizedPlanBytes.GetData();
    OversizedPlanPayload.Length = OversizedPlanBytes.Num();
    const TSharedPtr<FJsonObject> OversizedPlanResult = ParseAnimationResult(
        Context->RegisterAnimationPlansPacked(OversizedPlanPayload));
    TestTrue(TEXT("compiled plan resident hard limit rejects the entire registration"),
        OversizedPlanResult.IsValid() && !OversizedPlanResult->GetBoolField(TEXT("accepted")) &&
        OversizedPlanResult->GetStringField(TEXT("error")) == TEXT("plan_memory_capacity_exceeded"));
    TestEqual(TEXT("resident hard-limit rollback leaves no definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    const TSharedPtr<FJsonObject> RejectedPlanStats = ParseAnimationResult(
        Context->GetAnimationPlanCacheStats());
    TestTrue(TEXT("resident hard-limit rollback leaves cache statistics empty"),
        RejectedPlanStats.IsValid() &&
        RejectedPlanStats->GetIntegerField(TEXT("activePlans")) == 0 &&
        RejectedPlanStats->GetNumberField(TEXT("allocatedBytes")) == 0.0);
    TestTrue(TEXT("structured rejections do not poison page lifecycle state"),
        Context->LastError.IsEmpty());
    TestEqual(TEXT("rejected payload creates no definition"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    Context->Dispose();
    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlAnimationAdapterBundleTest,
    "RmlUiUnreal.JS.AnimationAdapterBundle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlAnimationAdapterBundleTest::RunTest(const FString&)
{
    const FString FixtureDirectory = FPaths::Combine(
        FPaths::ProjectPluginsDir(), TEXT("RmlUiUnreal/Content/RmlUi/Tests"));
    const FString EntryName = TEXT("animation-adapter-puerts-fixture.js");
    if (!TestTrue(TEXT("generated animation adapter bundle exists"),
        FPaths::FileExists(FPaths::Combine(FixtureDirectory, EntryName)))) return false;

    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    if (!TestNotNull(TEXT("create animation adapter Slate view"), View)) return false;
    const char* Document =
        "<rml><head><style>body{margin:0;width:320px;height:200px;}"
        "#keyframe-target{display:block;width:100px;height:100px;"
        "background-color:#fff;opacity:1;}"
        ".stagger-target{display:block;width:40px;height:40px;opacity:0.8;}</style></head>"
        "<body><div id='keyframe-target'/><div class='stagger-target'/>"
        "<div class='stagger-target'/></body></rml>";
    if (!TestTrue(TEXT("load animation adapter document"),
        RmlUE_LoadDocumentFromMemory(View, Document, "animation-adapter-bundle.rml") != 0))
    {
        RmlUE_DestroyView(View);
        return false;
    }
    RmlUE_Update(View);
    RmlUE_SlateFrame BaselineFrame{};
    TestTrue(TEXT("establish animation adapter retained baseline"),
        RmlUE_RenderSlate(View, &BaselineFrame) != 0 && BaselineFrame.Replayed == 0);

    FRmlUiAnimationRuntime& AnimationRuntime =
        FRmlUiUnrealModule::Get().GetAnimationRuntime();
    const int32 DefinitionsBefore = AnimationRuntime.GetDefinitionCount();
    const int32 BindingsBefore = AnimationRuntime.GetBindingCount();
    const int32 ActiveBefore = AnimationRuntime.GetActiveAnimationCount();
    TStrongObjectPtr<URmlUiJSContext> Context(NewObject<URmlUiJSContext>());
    if (!TestTrue(TEXT("real TypeScript animation adapter bundle starts in Puerts"),
        Context->Initialize(View, FixtureDirectory, EntryName,
            TEXT("animation-adapter-test"), TEXT("{}"), -1, {})))
    {
        AddError(Context->LastError);
        Context->Dispose();
        RmlUE_DestroyView(View);
        return false;
    }

    TSharedPtr<FJsonObject> DebugState;
    TestTrue(TEXT("animation adapter bundle reports structured state"),
        FJsonSerializer::Deserialize(
            TJsonReaderFactory<>::Create(Context->DebugStateJson), DebugState) &&
        DebugState.IsValid());
    TArray<FString> Handles;
    if (DebugState.IsValid())
    {
        const TSharedPtr<FJsonObject>* Versions = nullptr;
        TestTrue(TEXT("animation adapter bundle preserves locked library versions"),
            DebugState->TryGetObjectField(TEXT("versions"), Versions) && Versions && Versions->IsValid() &&
            (*Versions)->GetStringField(TEXT("animationJs")) == TEXT("0.5.0") &&
            (*Versions)->GetStringField(TEXT("animeJs")) == TEXT("4.5.0") &&
            (*Versions)->GetStringField(TEXT("gsap")) == TEXT("3.15.0"));
        TestEqual(TEXT("animation adapter bundle starts six tracks including merged GSAP and Anime timelines"),
            DebugState->GetIntegerField(TEXT("animationCount")), 6);
        const TArray<TSharedPtr<FJsonValue>>& HandleValues = DebugState->GetArrayField(TEXT("handles"));
        const TArray<TSharedPtr<FJsonValue>>& RouteValues = DebugState->GetArrayField(TEXT("routes"));
        const TArray<TSharedPtr<FJsonValue>>& StateValues = DebugState->GetArrayField(TEXT("states"));
        TestEqual(TEXT("animation adapter bundle exposes six handles"), HandleValues.Num(), 6);
        TestEqual(TEXT("animation adapter bundle reports six routes"), RouteValues.Num(), 6);
        TestEqual(TEXT("animation adapter bundle reports six states"), StateValues.Num(), 6);
        for (int32 Index = 0; Index < HandleValues.Num(); ++Index)
        {
            Handles.Add(HandleValues[Index]->AsString());
            TestEqual(TEXT("animation adapter bundle chooses the native route"),
                RouteValues[Index]->AsString(), FString(TEXT("native")));
            TestEqual(TEXT("animation adapter bundle starts in running state"),
                StateValues[Index]->AsString(), FString(TEXT("running")));
        }
    }
    TestTrue(TEXT("animation adapter bundle exposes only opaque handles"),
        Handles.Num() == 6 && !Handles.ContainsByPredicate([](const FString& Handle) { return Handle.IsEmpty(); }));
    TestEqual(TEXT("animation adapter bundle creates six immutable definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 6);
    TestEqual(TEXT("animation adapter bundle creates six target bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 6);
    TestEqual(TEXT("animation adapter bundle creates six ECS entities"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 6);

    for (const FString& Handle : Handles)
    {
        const TSharedPtr<FJsonObject> CancelResult = ParseAnimationResult(
            Context->ControlAnimation(Handle, TEXT("cancel"), 0.0));
        TestTrue(TEXT("animation adapter bundle entity can be cancelled"),
            CancelResult.IsValid() && CancelResult->GetBoolField(TEXT("accepted")));
    }
    TestEqual(TEXT("adapter cancellation retains compiled plan definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 6);
    TestEqual(TEXT("adapter cancellation releases all bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);
    TestEqual(TEXT("adapter cancellation removes all ECS entities"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);

    Context->Dispose();
    TestEqual(TEXT("adapter disposal releases compiled plan definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlAnimationPlanCacheWorkloadTest,
    "RmlUiUnreal.JS.AnimationPlanCacheWorkload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlAnimationPlanCacheWorkloadTest::RunTest(const FString&)
{
    const FString FixtureDirectory = FPaths::Combine(
        FPaths::ProjectPluginsDir(), TEXT("RmlUiUnreal/Content/RmlUi/Tests"));
    const FString EntryName = TEXT("animation-plan-cache-workload-puerts-fixture.js");
    if (!TestTrue(TEXT("generated animation plan-cache workload fixture exists"),
        FPaths::FileExists(FPaths::Combine(FixtureDirectory, EntryName)))) return false;

    FString Document =
        TEXT("<rml><head><style>body{margin:0;width:320px;height:200px;}"
             ".plan-cache-target{display:block;width:20px;height:12px;opacity:1;}"
             "</style></head><body>");
    for (int32 Index = 0; Index < 12; ++Index)
    {
        Document += FString::Printf(
            TEXT("<div id='plan-cache-target-%d' class='plan-cache-target'/>"), Index);
    }
    Document += TEXT("</body></rml>");

    RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
    if (!TestNotNull(TEXT("create animation plan-cache workload Slate view"), View)) return false;
    FTCHARToUTF8 DocumentUtf8(*Document);
    if (!TestTrue(TEXT("load animation plan-cache workload document"),
        RmlUE_LoadDocumentFromMemory(View, DocumentUtf8.Get(),
            "animation-plan-cache-workload.rml") != 0))
    {
        RmlUE_DestroyView(View);
        return false;
    }
    RmlUE_Update(View);

    FRmlUiAnimationRuntime& AnimationRuntime =
        FRmlUiUnrealModule::Get().GetAnimationRuntime();
    const int32 DefinitionsBefore = AnimationRuntime.GetDefinitionCount();
    const int32 BindingsBefore = AnimationRuntime.GetBindingCount();
    const int32 ActiveBefore = AnimationRuntime.GetActiveAnimationCount();
    TStrongObjectPtr<URmlUiJSContext> Context(NewObject<URmlUiJSContext>());
    Context->bUsePackedAnimationEvents = true;
    Context->bUseCompiledAnimationPlans = true;
    Context->CompiledAnimationPlanCacheMaxEntries = 8;
    Context->CompiledAnimationPlanCacheMaxBytes = 16 * 1024 * 1024;

    const double InitializeStartSeconds = FPlatformTime::Seconds();
    const bool bInitialized = Context->Initialize(
        View, FixtureDirectory, EntryName,
        TEXT("animation-plan-cache-workload"), TEXT("{}"), -1, {});
    const double InitializeWallMilliseconds =
        (FPlatformTime::Seconds() - InitializeStartSeconds) * 1000.0;
    if (!TestTrue(TEXT("real adapter plan-cache workload starts in Puerts"), bInitialized))
    {
        AddError(Context->LastError);
        Context->Dispose();
        RmlUE_DestroyView(View);
        return false;
    }

    TestEqual(TEXT("cold phase creates eight immutable definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 8);
    TestEqual(TEXT("cold phase creates eight target bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore + 8);
    TestEqual(TEXT("cold phase creates eight ECS entities"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 8);

    TArray<TSharedPtr<FJsonValue>> AdvanceWallValues;
    for (int32 PhaseIndex = 0; PhaseIndex < 3; ++PhaseIndex)
    {
        const double AdvanceStartSeconds = FPlatformTime::Seconds();
        AnimationRuntime.Advance(0.05f);
        const double AdvanceWallMilliseconds =
            (FPlatformTime::Seconds() - AdvanceStartSeconds) * 1000.0;
        AdvanceWallValues.Add(MakeShared<FJsonValueNumber>(AdvanceWallMilliseconds));
        if (PhaseIndex < 2)
        {
            TestEqual(*FString::Printf(TEXT("phase %d leaves the next eight bindings active"), PhaseIndex),
                AnimationRuntime.GetBindingCount(), BindingsBefore + 8);
            TestEqual(*FString::Printf(TEXT("phase %d leaves the next eight ECS entities active"), PhaseIndex),
                AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + 8);
        }
    }

    TSharedPtr<FJsonObject> DebugState;
    const bool bParsed = FJsonSerializer::Deserialize(
        TJsonReaderFactory<>::Create(Context->DebugStateJson), DebugState) &&
        DebugState.IsValid();
    TestTrue(TEXT("plan-cache workload reports structured final state"), bParsed);
    if (bParsed)
    {
        TestEqual(TEXT("plan-cache workload reports finished"),
            DebugState->GetStringField(TEXT("state")), FString(TEXT("finished")));
        TestEqual(TEXT("plan-cache workload registers twelve distinct plans"),
            DebugState->GetIntegerField(TEXT("registrations")), 12);
        TestEqual(TEXT("plan-cache workload records twelve plan-cache hits"),
            DebugState->GetIntegerField(TEXT("cacheHits")), 12);
        TestEqual(TEXT("plan-cache workload retains the eight-entry budget"),
            DebugState->GetIntegerField(TEXT("cachedPlans")), 8);
        TestEqual(TEXT("plan-cache workload evicts four inactive LRU plans"),
            DebugState->GetIntegerField(TEXT("evictions")), 4);
        TestEqual(TEXT("plan-cache workload has no active-plan budget pressure"),
            DebugState->GetIntegerField(TEXT("budgetPressure")), 0);
        TestEqual(TEXT("plan-cache workload releases every active cache binding"),
            DebugState->GetIntegerField(TEXT("activeBindings")), 0);
        TestTrue(TEXT("plan-cache workload retains non-zero compiled bytes"),
            DebugState->GetNumberField(TEXT("cacheBytes")) > 0.0);
        TestEqual(TEXT("frontend and native compiled-byte accounting agree"),
            DebugState->GetNumberField(TEXT("nativeAllocatedBytes")),
            DebugState->GetNumberField(TEXT("cacheBytes")));

        const TArray<TSharedPtr<FJsonValue>>& PhaseStats =
            DebugState->GetArrayField(TEXT("phaseStats"));
        TestEqual(TEXT("plan-cache workload reports all three phases"), PhaseStats.Num(), 3);
        const TCHAR* ExpectedNames[] = {TEXT("cold"), TEXT("churn"), TEXT("hot")};
        const int32 ExpectedRegistrations[] = {8, 12, 12};
        const int32 ExpectedHits[] = {0, 4, 12};
        const int32 ExpectedEvictions[] = {0, 4, 4};
        for (int32 PhaseIndex = 0; PhaseIndex < PhaseStats.Num() && PhaseIndex < 3; ++PhaseIndex)
        {
            const TSharedPtr<FJsonObject> Phase = PhaseStats[PhaseIndex]->AsObject();
            TestTrue(*FString::Printf(TEXT("phase %d statistics are an object"), PhaseIndex),
                Phase.IsValid());
            if (!Phase.IsValid()) continue;
            TestEqual(*FString::Printf(TEXT("phase %d name"), PhaseIndex),
                Phase->GetStringField(TEXT("name")), FString(ExpectedNames[PhaseIndex]));
            TestEqual(*FString::Printf(TEXT("phase %d cumulative registrations"), PhaseIndex),
                Phase->GetIntegerField(TEXT("registrations")), ExpectedRegistrations[PhaseIndex]);
            TestEqual(*FString::Printf(TEXT("phase %d cumulative cache hits"), PhaseIndex),
                Phase->GetIntegerField(TEXT("cacheHits")), ExpectedHits[PhaseIndex]);
            TestEqual(*FString::Printf(TEXT("phase %d cumulative evictions"), PhaseIndex),
                Phase->GetIntegerField(TEXT("evictions")), ExpectedEvictions[PhaseIndex]);
            TestEqual(*FString::Printf(TEXT("phase %d retains eight plans"), PhaseIndex),
                Phase->GetIntegerField(TEXT("cachedPlans")), 8);
            TestEqual(*FString::Printf(TEXT("phase %d releases active cache bindings"), PhaseIndex),
                Phase->GetIntegerField(TEXT("activeBindings")), 0);
        }
    }

    const TSharedPtr<FJsonObject> NativeStats = ParseAnimationResult(
        Context->GetAnimationPlanCacheStats());
    TestTrue(TEXT("native plan-cache workload statistics parse"), NativeStats.IsValid());
    if (NativeStats.IsValid())
    {
        TestEqual(TEXT("native cache retains eight plans"),
            NativeStats->GetIntegerField(TEXT("activePlans")), 8);
        if (bParsed)
        {
            TestEqual(TEXT("native cache bytes match frontend final statistics"),
                NativeStats->GetNumberField(TEXT("allocatedBytes")),
                DebugState->GetNumberField(TEXT("cacheBytes")));
        }
    }
    TestEqual(TEXT("completed workload retains eight compiled definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 8);
    TestEqual(TEXT("completed workload releases all target bindings"),
        AnimationRuntime.GetBindingCount(), BindingsBefore);
    TestEqual(TEXT("completed workload removes all ECS entities"),
        AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);

    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("scope"),
        TEXT("Controlled Animation.js, Anime.js, and GSAP adapter workload through Puerts, compiled-plan LRU, MovieScene ECS advance, batched completion, and Promise settlement"));
    Report->SetStringField(TEXT("fixture"), EntryName);
    Report->SetNumberField(TEXT("cacheMaxEntries"), 8);
    Report->SetNumberField(TEXT("cacheMaxBytes"), 16 * 1024 * 1024);
    Report->SetNumberField(TEXT("initializeWallMilliseconds"), InitializeWallMilliseconds);
    Report->SetArrayField(TEXT("advanceWallMilliseconds"), MoveTemp(AdvanceWallValues));
    if (bParsed) Report->SetObjectField(TEXT("debugState"), DebugState.ToSharedRef());
    if (NativeStats.IsValid()) Report->SetObjectField(TEXT("nativeCache"), NativeStats.ToSharedRef());
    FString ReportJson;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&ReportJson));
    const FString ReportDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Performance/AnimationRuntime"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    TestTrue(TEXT("write animation plan-cache workload report"),
        FFileHelper::SaveStringToFile(ReportJson,
            *FPaths::Combine(ReportDirectory,
                FString::Printf(TEXT("AnimationPlanCacheWorkload-UE%d%d.json"),
                    ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION)),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

    Context->Dispose();
    TestEqual(TEXT("workload disposal releases cached definitions"),
        AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    RmlUE_DestroyView(View);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlAnimationCompletionBurstTest,
    "RmlUiUnreal.JS.AnimationCompletionBurst",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlAnimationCompletionBurstTest::RunTest(const FString&)
{
    const FString FixtureDirectory = FPaths::Combine(
        FPaths::ProjectPluginsDir(), TEXT("RmlUiUnreal/Content/RmlUi/Tests"));
    const FString EntryName = TEXT("animation-completion-burst-puerts-fixture.js");
    if (!TestTrue(TEXT("generated animation completion burst fixture exists"),
        FPaths::FileExists(FPaths::Combine(FixtureDirectory, EntryName)))) return false;

    FRmlUiAnimationRuntime& AnimationRuntime =
        FRmlUiUnrealModule::Get().GetAnimationRuntime();
    TArray<TSharedPtr<FJsonValue>> ScenarioResults;
    for (const int32 Count : {100, 1000, 4096})
    {
        RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
        if (!TestNotNull(*FString::Printf(TEXT("create %d-track completion burst view"), Count), View))
            return false;
        const char* Document =
            "<rml><head><style>body{margin:0;width:320px;height:200px;}"
            "#completion-target{display:block;width:100px;height:100px;"
            "background-color:#fff;opacity:1;}</style></head>"
            "<body><div id='completion-target'/></body></rml>";
        if (!TestTrue(*FString::Printf(TEXT("load %d-track completion burst document"), Count),
            RmlUE_LoadDocumentFromMemory(View, Document, "animation-completion-burst.rml") != 0))
        {
            RmlUE_DestroyView(View);
            return false;
        }
        RmlUE_Update(View);

        const int32 DefinitionsBefore = AnimationRuntime.GetDefinitionCount();
        const int32 BindingsBefore = AnimationRuntime.GetBindingCount();
        const int32 ActiveBefore = AnimationRuntime.GetActiveAnimationCount();
        TStrongObjectPtr<URmlUiJSContext> Context(NewObject<URmlUiJSContext>());
        const FString State = FString::Printf(TEXT("{\"count\":%d}"), Count);
        if (!TestTrue(*FString::Printf(TEXT("start %d-track real Puerts adapter burst"), Count),
            Context->Initialize(View, FixtureDirectory, EntryName,
                TEXT("animation-completion-burst"), State, -1, {})))
        {
            AddError(Context->LastError);
            Context->Dispose();
            RmlUE_DestroyView(View);
            return false;
        }
        TestEqual(*FString::Printf(TEXT("%d-track burst creates immutable definitions"), Count),
            AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);
        TestEqual(*FString::Printf(TEXT("%d-track burst creates target bindings"), Count),
            AnimationRuntime.GetBindingCount(), BindingsBefore + Count);
        TestEqual(*FString::Printf(TEXT("%d-track burst creates ECS entities"), Count),
            AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + Count);

        const double StartSeconds = FPlatformTime::Seconds();
        AnimationRuntime.Advance(0.002f);
        const double WallMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

        TSharedPtr<FJsonObject> DebugState;
        const bool bParsed = FJsonSerializer::Deserialize(
            TJsonReaderFactory<>::Create(Context->DebugStateJson), DebugState) &&
            DebugState.IsValid();
        TestTrue(*FString::Printf(TEXT("%d-track completion state parses"), Count), bParsed);
        const int32 Settled = bParsed ? DebugState->GetIntegerField(TEXT("settled")) : 0;
        TestEqual(*FString::Printf(TEXT("%d-track finished Promises settle in the completion dispatch"), Count),
            Settled, Count);
        if (bParsed)
        {
            TestEqual(*FString::Printf(TEXT("%d-track completion crosses Puerts once"), Count),
                DebugState->GetIntegerField(TEXT("batchDispatches")), 1);
            TestEqual(*FString::Printf(TEXT("%d-track batch contains every completion event"), Count),
                DebugState->GetIntegerField(TEXT("batchEvents")), Count);
            TestEqual(*FString::Printf(TEXT("%d-track adapter reports finished state"), Count),
                DebugState->GetStringField(TEXT("state")), FString(TEXT("finished")));
            TestEqual(*FString::Printf(TEXT("%d-track adapter uses compiled packed starts"), Count),
                DebugState->GetStringField(TEXT("startTransport")), FString(TEXT("compiled-packed")));
            TestEqual(*FString::Printf(TEXT("%d-track adapter registers one plan"), Count),
                DebugState->GetIntegerField(TEXT("planRegistrations")), 1);
            TestEqual(*FString::Printf(TEXT("%d-track adapter reuses the plan"), Count),
                DebugState->GetIntegerField(TEXT("planCacheHits")), Count - 1);
        }
        TestEqual(*FString::Printf(TEXT("%d-track burst retains its compiled definition"), Count),
            AnimationRuntime.GetDefinitionCount(), DefinitionsBefore + 1);
        TestEqual(*FString::Printf(TEXT("%d-track burst releases bindings"), Count),
            AnimationRuntime.GetBindingCount(), BindingsBefore);
        TestEqual(*FString::Printf(TEXT("%d-track burst removes ECS entities"), Count),
            AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);

        TSharedRef<FJsonObject> Scenario = MakeShared<FJsonObject>();
        Scenario->SetNumberField(TEXT("trackCount"), Count);
        Scenario->SetNumberField(TEXT("settledPromiseCount"), Settled);
        Scenario->SetNumberField(TEXT("advanceWallMilliseconds"), WallMilliseconds);
        ScenarioResults.Add(MakeShared<FJsonValueObject>(Scenario));

        Context->Dispose();
        TestEqual(*FString::Printf(TEXT("%d-track disposal releases compiled definitions"), Count),
            AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
        RmlUE_DestroyView(View);
    }

    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("scope"),
        TEXT("AnimationRuntime.Advance including RmlUi commit, one batched Puerts completion delivery, JSON.parse, adapter cleanup, and Promise settlement"));
    Report->SetNumberField(TEXT("nativeBatchTrackLimit"), 4096);
    Report->SetStringField(TEXT("fixture"), EntryName);
    Report->SetArrayField(TEXT("scenarios"), MoveTemp(ScenarioResults));
    FString ReportJson;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&ReportJson));
    const FString ReportDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Performance/AnimationRuntime"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    TestTrue(TEXT("write real Puerts completion burst report"),
        FFileHelper::SaveStringToFile(ReportJson,
            *FPaths::Combine(ReportDirectory,
                FString::Printf(TEXT("AnimationPuertsCompletionBurst-UE%d%d.json"),
                    ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION)),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlAnimationCompletionFanoutTest,
    "RmlUiUnreal.JS.AnimationCompletionFanout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlAnimationCompletionFanoutTest::RunTest(const FString&)
{
    struct FFanoutScenario
    {
        const TCHAR* Name;
        int32 ContextCount;
        int32 BatchCount;
        int32 TracksPerContext;
        int32 ContinuationOperations;
        bool bUsePacked;
        bool bShareDefinitions = true;
        bool bUseCompiledPlans = false;
    };
    const FFanoutScenario Scenarios[] = {
        {TEXT("packed_unshared_one_context_one_batch"), 1, 1, 4096, 0, true, false},
        {TEXT("packed_one_context_one_batch"), 1, 1, 4096, 0, true},
        {TEXT("json_one_context_four_batches"), 1, 4, 4096, 0, false},
        {TEXT("packed_one_context_four_batches"), 1, 4, 4096, 0, true},
        {TEXT("packed_four_contexts_one_batch"), 4, 1, 1024, 0, true},
        {TEXT("json_four_contexts_one_batch"), 4, 1, 1024, 0, false},
        {TEXT("json_continuation_control"), 1, 1, 1000, 0, false},
        {TEXT("packed_continuation_control"), 1, 1, 1000, 0, true},
        {TEXT("packed_continuation_256_ops"), 1, 1, 1000, 256, true},
        {TEXT("compiled_one_context_one_batch"), 1, 1, 4096, 0, true, true, true},
        {TEXT("compiled_one_context_four_batches"), 1, 4, 4096, 0, true, true, true},
    };
    const FString FixtureDirectory = FPaths::Combine(
        FPaths::ProjectPluginsDir(), TEXT("RmlUiUnreal/Content/RmlUi/Tests"));
    const FString EntryName = TEXT("animation-completion-burst-puerts-fixture.js");
    if (!TestTrue(TEXT("generated animation fanout fixture exists"),
        FPaths::FileExists(FPaths::Combine(FixtureDirectory, EntryName)))) return false;

    const char* Document =
        "<rml><head><style>body{margin:0;width:320px;height:200px;}"
        ".completion-target{display:block;width:60px;height:30px;"
        "background-color:#fff;opacity:1;}</style></head><body>"
        "<div id='completion-target' class='completion-target'/>"
        "<div id='completion-target-0' class='completion-target'/>"
        "<div id='completion-target-1' class='completion-target'/>"
        "<div id='completion-target-2' class='completion-target'/>"
        "<div id='completion-target-3' class='completion-target'/>"
        "</body></rml>";
    FRmlUiAnimationRuntime& AnimationRuntime =
        FRmlUiUnrealModule::Get().GetAnimationRuntime();
    const bool bPreviouslyEnabled = FRmlUiPerformance::IsEnabled();
    FRmlUiPerformance::SetEnabled(true);
    TArray<TSharedPtr<FJsonValue>> ScenarioResults;

    for (const FFanoutScenario& ScenarioSpec : Scenarios)
    {
        const int32 TotalTracks = ScenarioSpec.ContextCount * ScenarioSpec.TracksPerContext;
        const int32 DefinitionsBefore = AnimationRuntime.GetDefinitionCount();
        const int32 BindingsBefore = AnimationRuntime.GetBindingCount();
        const int32 ActiveBefore = AnimationRuntime.GetActiveAnimationCount();
        TArray<RmlUE_View*> Views;
        TArray<TStrongObjectPtr<URmlUiJSContext>> Contexts;
        Views.Reserve(ScenarioSpec.ContextCount);
        Contexts.Reserve(ScenarioSpec.ContextCount);
        const auto Cleanup = [&]()
        {
            for (TStrongObjectPtr<URmlUiJSContext>& Context : Contexts)
            {
                Context->Dispose();
            }
            Contexts.Reset();
            for (RmlUE_View* View : Views)
            {
                RmlUE_DestroyView(View);
            }
            Views.Reset();
        };

        bool bStarted = true;
        double StartWallMilliseconds = 0.0;
        FRmlUiPerformance::Reset();
        for (int32 ContextIndex = 0; ContextIndex < ScenarioSpec.ContextCount; ++ContextIndex)
        {
            RmlUE_View* View = RmlUE_CreateSlateView(320, 200, 1.0f);
            if (!TestNotNull(*FString::Printf(TEXT("%s context %d creates a Slate view"),
                ScenarioSpec.Name, ContextIndex), View))
            {
                bStarted = false;
                break;
            }
            Views.Add(View);
            if (!TestTrue(*FString::Printf(TEXT("%s context %d loads the fanout document"),
                ScenarioSpec.Name, ContextIndex),
                RmlUE_LoadDocumentFromMemory(View, Document, "animation-completion-fanout.rml") != 0))
            {
                bStarted = false;
                break;
            }
            RmlUE_Update(View);
            Contexts.Emplace(NewObject<URmlUiJSContext>());
            Contexts.Last()->bUsePackedAnimationEvents = ScenarioSpec.bUsePacked;
            Contexts.Last()->bShareAnimationDefinitionsInBatch = ScenarioSpec.bShareDefinitions;
            Contexts.Last()->bUseCompiledAnimationPlans = ScenarioSpec.bUseCompiledPlans;
            const FString State = FString::Printf(
                TEXT("{\"count\":%d,\"batchCount\":%d,\"continuationOperations\":%d}"),
                ScenarioSpec.TracksPerContext, ScenarioSpec.BatchCount,
                ScenarioSpec.ContinuationOperations);
            const double StartSeconds = FPlatformTime::Seconds();
            const bool bInitialized = Contexts.Last()->Initialize(
                View, FixtureDirectory, EntryName,
                TEXT("animation-completion-fanout"), State, -1, {});
            StartWallMilliseconds += (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
            if (!TestTrue(*FString::Printf(TEXT("%s context %d starts the real adapter"),
                ScenarioSpec.Name, ContextIndex), bInitialized))
            {
                AddError(Contexts.Last()->LastError);
                bStarted = false;
                break;
            }
        }
        if (!bStarted)
        {
            Cleanup();
            FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
            return false;
        }

        const int32 ExpectedDefinitions = ScenarioSpec.bUseCompiledPlans
            ? ScenarioSpec.ContextCount
            : (ScenarioSpec.bShareDefinitions
                ? ScenarioSpec.ContextCount * ScenarioSpec.BatchCount
                : TotalTracks);
        const FRmlUiPerformanceSnapshot StartPerformance = FRmlUiPerformance::Snapshot();
        TestEqual(*FString::Printf(TEXT("%s creates immutable definitions"), ScenarioSpec.Name),
            AnimationRuntime.GetDefinitionCount(),
            DefinitionsBefore + ExpectedDefinitions);
        TestEqual(*FString::Printf(TEXT("%s creates target bindings"), ScenarioSpec.Name),
            AnimationRuntime.GetBindingCount(), BindingsBefore + TotalTracks);
        TestEqual(*FString::Printf(TEXT("%s creates ECS entities"), ScenarioSpec.Name),
            AnimationRuntime.GetActiveAnimationCount(), ActiveBefore + TotalTracks);
        TestEqual(*FString::Printf(TEXT("%s profiles registered definitions"), ScenarioSpec.Name),
            StartPerformance.WorkCount(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationDefinitionsRegistered),
            static_cast<uint64>(ExpectedDefinitions));
        TestEqual(*FString::Printf(TEXT("%s profiles reused definitions"), ScenarioSpec.Name),
            StartPerformance.WorkCount(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationDefinitionsReused),
            static_cast<uint64>(TotalTracks - ExpectedDefinitions));

        FRmlUiPerformance::Reset();
        const double StartSeconds = FPlatformTime::Seconds();
        AnimationRuntime.Advance(0.002f);
        const double WallMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
        const FRmlUiPerformanceSnapshot Performance = FRmlUiPerformance::Snapshot();

        int32 Settled = 0;
        int32 BatchDispatches = 0;
        int32 BatchEvents = 0;
        int32 ContinuationExecutions = 0;
        double ContinuationChecksum = 0.0;
        for (int32 ContextIndex = 0; ContextIndex < Contexts.Num(); ++ContextIndex)
        {
            TSharedPtr<FJsonObject> DebugState;
            const bool bParsed = FJsonSerializer::Deserialize(
                TJsonReaderFactory<>::Create(Contexts[ContextIndex]->DebugStateJson), DebugState) &&
                DebugState.IsValid();
            TestTrue(*FString::Printf(TEXT("%s context %d completion state parses"),
                ScenarioSpec.Name, ContextIndex), bParsed);
            if (!bParsed) continue;
            Settled += DebugState->GetIntegerField(TEXT("settled"));
            BatchDispatches += DebugState->GetIntegerField(TEXT("batchDispatches"));
            BatchEvents += DebugState->GetIntegerField(TEXT("batchEvents"));
            ContinuationExecutions += DebugState->GetIntegerField(TEXT("continuationExecutions"));
            ContinuationChecksum += DebugState->GetNumberField(TEXT("continuationChecksum"));
            TestEqual(*FString::Printf(TEXT("%s context %d uses requested completion transport"),
                ScenarioSpec.Name, ContextIndex),
                DebugState->GetStringField(TEXT("batchTransport")),
                ScenarioSpec.bUsePacked ? FString(TEXT("packed")) : FString(TEXT("json")));
            TestEqual(*FString::Printf(TEXT("%s context %d reports finished"),
                ScenarioSpec.Name, ContextIndex),
                DebugState->GetStringField(TEXT("state")), FString(TEXT("finished")));
            TestEqual(*FString::Printf(TEXT("%s context %d uses requested start transport"),
                ScenarioSpec.Name, ContextIndex),
                DebugState->GetStringField(TEXT("startTransport")),
                ScenarioSpec.bUseCompiledPlans
                    ? FString(TEXT("compiled-packed")) : FString(TEXT("json")));
        }
        TestEqual(*FString::Printf(TEXT("%s settles every Promise"), ScenarioSpec.Name),
            Settled, TotalTracks);
        TestEqual(*FString::Printf(TEXT("%s crosses Puerts once per Context"), ScenarioSpec.Name),
            BatchDispatches, ScenarioSpec.ContextCount);
        TestEqual(*FString::Printf(TEXT("%s batches every completion event"), ScenarioSpec.Name),
            BatchEvents, TotalTracks);
        TestEqual(*FString::Printf(TEXT("%s profiles one completion batch per Context"),
            ScenarioSpec.Name),
            Performance.WorkCount(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationCompletionEventBatches),
            static_cast<uint64>(ScenarioSpec.ContextCount));
        TestEqual(*FString::Printf(TEXT("%s profiles every completion event"), ScenarioSpec.Name),
            Performance.WorkCount(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationCompletionEvents),
            static_cast<uint64>(TotalTracks));
        TestEqual(*FString::Printf(TEXT("%s executes every Promise continuation"), ScenarioSpec.Name),
            ContinuationExecutions, TotalTracks);
        if (ScenarioSpec.ContinuationOperations > 0)
        {
            TestTrue(*FString::Printf(TEXT("%s retains continuation work"), ScenarioSpec.Name),
                ContinuationChecksum > 0.0);
        }
        TestEqual(*FString::Printf(TEXT("%s releases definitions"), ScenarioSpec.Name),
            AnimationRuntime.GetDefinitionCount(),
            DefinitionsBefore + (ScenarioSpec.bUseCompiledPlans ? ExpectedDefinitions : 0));
        TestEqual(*FString::Printf(TEXT("%s releases bindings"), ScenarioSpec.Name),
            AnimationRuntime.GetBindingCount(), BindingsBefore);
        TestEqual(*FString::Printf(TEXT("%s removes ECS entities"), ScenarioSpec.Name),
            AnimationRuntime.GetActiveAnimationCount(), ActiveBefore);

        TSharedRef<FJsonObject> ScenarioResult = MakeShared<FJsonObject>();
        ScenarioResult->SetStringField(TEXT("name"), ScenarioSpec.Name);
        ScenarioResult->SetNumberField(TEXT("contextCount"), ScenarioSpec.ContextCount);
        ScenarioResult->SetNumberField(TEXT("startBatchCount"),
            ScenarioSpec.ContextCount * ScenarioSpec.BatchCount);
        ScenarioResult->SetNumberField(TEXT("trackCount"), TotalTracks);
        ScenarioResult->SetNumberField(TEXT("continuationOperationsPerPromise"),
            ScenarioSpec.ContinuationOperations);
        ScenarioResult->SetStringField(TEXT("completionTransport"),
            ScenarioSpec.bUsePacked ? TEXT("packed") : TEXT("json"));
        ScenarioResult->SetBoolField(TEXT("sharedDefinitions"), ScenarioSpec.bShareDefinitions);
        ScenarioResult->SetStringField(TEXT("startTransport"),
            ScenarioSpec.bUseCompiledPlans ? TEXT("compiled-packed") : TEXT("json"));
        ScenarioResult->SetNumberField(TEXT("registeredDefinitionCount"), ExpectedDefinitions);
        ScenarioResult->SetNumberField(TEXT("startWallMilliseconds"), StartWallMilliseconds);
        ScenarioResult->SetNumberField(TEXT("startParseMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationStartBatchParse));
        ScenarioResult->SetNumberField(TEXT("startPrepareMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationStartBatchPrepare));
        ScenarioResult->SetNumberField(TEXT("startPlayMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationStartBatchPlay));
        ScenarioResult->SetNumberField(TEXT("planRegisterMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationPlanRegisterPacked));
        ScenarioResult->SetNumberField(TEXT("compiledPrepareMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompiledBatchPrepare));
        ScenarioResult->SetNumberField(TEXT("compiledPlayMilliseconds"),
            StartPerformance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompiledBatchPlay));
        ScenarioResult->SetNumberField(TEXT("completionBatchDispatchCount"), BatchDispatches);
        ScenarioResult->SetNumberField(TEXT("settledPromiseCount"), Settled);
        ScenarioResult->SetNumberField(TEXT("continuationChecksum"), ContinuationChecksum);
        ScenarioResult->SetNumberField(TEXT("advanceWallMilliseconds"), WallMilliseconds);
        ScenarioResult->SetNumberField(TEXT("completionSerializeMilliseconds"),
            Performance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompletionEventSerialize));
        ScenarioResult->SetNumberField(TEXT("completionPackMilliseconds"),
            Performance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompletionEventPack));
        ScenarioResult->SetNumberField(TEXT("completionDispatchMilliseconds"),
            Performance.Milliseconds(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompletionEventDispatch));
        ScenarioResults.Add(MakeShared<FJsonValueObject>(ScenarioResult));
        Cleanup();
        TestEqual(*FString::Printf(TEXT("%s disposal releases cached definitions"), ScenarioSpec.Name),
            AnimationRuntime.GetDefinitionCount(), DefinitionsBefore);
    }

    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("scope"),
        TEXT("Real adapter native batch startup plus AnimationRuntime.Advance across multiple Puerts Contexts and controlled Promise continuation work"));
    Report->SetStringField(TEXT("fixture"), EntryName);
    Report->SetArrayField(TEXT("scenarios"), MoveTemp(ScenarioResults));
    FString ReportJson;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&ReportJson));
    const FString ReportDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Performance/AnimationRuntime"));
    IFileManager::Get().MakeDirectory(*ReportDirectory, true);
    TestTrue(TEXT("write Puerts completion fanout report"),
        FFileHelper::SaveStringToFile(ReportJson,
            *FPaths::Combine(ReportDirectory,
                FString::Printf(TEXT("AnimationPuertsCompletionFanout-UE%d%d.json"),
                    ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION)),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FRmlUiPerformance::SetEnabled(bPreviouslyEnabled);
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
