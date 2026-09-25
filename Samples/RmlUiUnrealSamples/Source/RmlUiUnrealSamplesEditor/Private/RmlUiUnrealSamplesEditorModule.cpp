#include "Modules/ModuleManager.h"

#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "RmlUiActorObserverService.h"
#include "RmlUiChatTransport.h"
#include "RmlUiDemoHostService.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiJSRuntime.h"
#include "RmlUiUnrealModule.h"
#include "RmlUiWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const FName DemoTabName(TEXT("RmlUIDemo"));

UWorld* CurrentEditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

class SRmlUiDemoEditorPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRmlUiDemoEditorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&)
    {
        const FRmlUiAnimationRuntime& AnimationRuntime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
        DefinitionsBeforeStart = AnimationRuntime.GetDefinitionCount();
        BindingsBeforeStart = AnimationRuntime.GetBindingCount();
        RmlWidget = TStrongObjectPtr<URmlUiWidget>(NewObject<URmlUiWidget>());
        Runtime = TStrongObjectPtr<URmlUiJSRuntime>(NewObject<URmlUiJSRuntime>());
        Observer = TStrongObjectPtr<URmlUiActorObserverService>(NewObject<URmlUiActorObserverService>());
        Host = TStrongObjectPtr<URmlUiDemoHostService>(NewObject<URmlUiDemoHostService>());
        Probe = TStrongObjectPtr<URmlUiDemoProbeObject>(NewObject<URmlUiDemoProbeObject>(Host.Get()));
        Chat = TStrongObjectPtr<URmlUiChatTransport>(NewObject<URmlUiChatTransport>());
        RmlWidget->bUseSlateRenderer = true;
        ObservedWorld = CurrentEditorWorld();
        Observer->Initialize(ObservedWorld.Get());
        Observer->AttachMaterialShowcase(RmlWidget.Get());
        Runtime->RegisterService(TEXT("actorObserver"), Observer.Get());
        Host->Probe = Probe.Get();
        Runtime->RegisterService(TEXT("host"), Host.Get());
        Chat->SetApiKey(FPlatformMisc::GetEnvironmentVariable(TEXT("RMLUI_CHAT_API_KEY")));
        Chat->Attach(Runtime.Get(), TEXT("http://127.0.0.1:4180/v1/chat/completions"), TEXT("local-demo"), TEXT("openai"));

        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnrealSamples"));
        const FString Manifest = Plugin.IsValid()
            ? FPaths::Combine(Plugin->GetContentDir(), TEXT("ActorObserver/current.json"))
            : FString();
        const bool bStarted = Plugin.IsValid() && Runtime->Start(RmlWidget.Get(), Manifest, true);
        if (bStarted)
        {
            ChildSlot
            [
                SNew(SBorder)
                .Padding(0)
                .BorderImage(FAppStyle::GetBrush(TEXT("Brushes.Panel")))
                [RmlWidget->TakeWidget()]
            ];
        }
        else
        {
            const FString Error = Plugin.IsValid() ? Runtime->LastError : TEXT("RmlUiUnrealSamples plugin was not found.");
            ChildSlot
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString(TEXT("Could not start RmlUIDemo: ")) + Error))
                .AutoWrapText(true)
            ];
        }
    }

    virtual ~SRmlUiDemoEditorPanel() override
    {
        if (Chat.IsValid()) Chat->Stop();
        if (Runtime.IsValid()) Runtime->Stop();
        Chat.Reset();
        Probe.Reset();
        Host.Reset();
        Runtime.Reset();
        Observer.Reset();
        RmlWidget.Reset();
    }

    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
    {
        SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
        UWorld* EditorWorld = CurrentEditorWorld();
        if (Observer.IsValid() && EditorWorld != ObservedWorld.Get())
        {
            ObservedWorld = EditorWorld;
            Observer->Initialize(EditorWorld);
        }
    }

#if WITH_DEV_AUTOMATION_TESTS
    bool IsRuntimeReady() const
    {
        return Runtime.IsValid() && Runtime->GetContext() && Runtime->GetContext()->bReady;
    }

    UWorld* GetObservedWorld() const { return ObservedWorld.Get(); }

    FString CaptureActorSnapshot() const
    {
        return Observer.IsValid() ? Observer->GetActorSnapshot() : FString();
    }

    bool IsMaterialShowcaseReady() const { return Observer.IsValid() && Observer->IsUiMaterialReady(); }
    int32 GetCssMotionDefinitionDelta() const
    {
        return FRmlUiUnrealModule::Get().GetAnimationRuntime().GetDefinitionCount() - DefinitionsBeforeStart;
    }
    int32 GetCssMotionBindingDelta() const
    {
        return FRmlUiUnrealModule::Get().GetAnimationRuntime().GetBindingCount() - BindingsBeforeStart;
    }
#endif

private:
    TStrongObjectPtr<URmlUiWidget> RmlWidget;
    TStrongObjectPtr<URmlUiJSRuntime> Runtime;
    TStrongObjectPtr<URmlUiActorObserverService> Observer;
    TStrongObjectPtr<URmlUiDemoHostService> Host;
    TStrongObjectPtr<URmlUiDemoProbeObject> Probe;
    TStrongObjectPtr<URmlUiChatTransport> Chat;
    TWeakObjectPtr<UWorld> ObservedWorld;
    int32 DefinitionsBeforeStart = 0;
    int32 BindingsBeforeStart = 0;
};
}

class FRmlUiUnrealSamplesEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(DemoTabName,
            FOnSpawnTab::CreateRaw(this, &FRmlUiUnrealSamplesEditorModule::SpawnDemo))
            .SetDisplayName(FText::FromString(TEXT("RmlUIDemo")))
            .SetTooltipText(FText::FromString(TEXT("RmlUi Unreal demonstration")))
            .SetMenuType(ETabSpawnerMenuType::Hidden)
            .SetAutoGenerateMenuEntry(false);
    }

    virtual void ShutdownModule() override
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DemoTabName);
    }

private:
    TSharedRef<SDockTab> SpawnDemo(const FSpawnTabArgs&)
    {
        return SNew(SDockTab)
            .TabRole(ETabRole::NomadTab)
            [SNew(SRmlUiDemoEditorPanel)];
    }
};

IMPLEMENT_MODULE(FRmlUiUnrealSamplesEditorModule, RmlUiUnrealSamplesEditor)

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiDemoEditorTabTest, "RmlUi.Demo.EditorTab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiDemoEditorTabTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("Old Actor Observer tab spawner is removed"), FGlobalTabmanager::Get()->HasTabSpawner(TEXT("RmlUiActorObserver")));
    const TSharedPtr<FTabSpawnerEntry> Spawner = FGlobalTabmanager::Get()->FindTabSpawnerFor(DemoTabName);
    if (!TestTrue(TEXT("RmlUIDemo tab spawner remains available"), Spawner.IsValid())) return false;
    TestTrue(TEXT("RmlUIDemo has no editor menu entry"), Spawner->IsHidden());
    bool bDemoInMenu = false;
    for (const TWeakPtr<FTabSpawnerEntry>& Entry : FGlobalTabmanager::Get()->CollectSpawners())
    {
        const TSharedPtr<FTabSpawnerEntry> MenuEntry = Entry.Pin();
        bDemoInMenu |= MenuEntry.IsValid() && MenuEntry->GetTabType() == DemoTabName;
    }
    TestFalse(TEXT("RmlUIDemo is absent from generated editor menus"), bDemoInMenu);
    const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(DemoTabName);
    if (!TestTrue(TEXT("RmlUIDemo Nomad tab opens programmatically"), Tab.IsValid())) return false;
    const TSharedRef<SRmlUiDemoEditorPanel> Panel =
        StaticCastSharedRef<SRmlUiDemoEditorPanel>(Tab->GetContent());
    TestTrue(TEXT("Vue runtime reports ready inside the editor tab"), Panel->IsRuntimeReady());
    TestEqual(TEXT("Panel observes the active editor world"), Panel->GetObservedWorld(), CurrentEditorWorld());
    const FString Snapshot = Panel->CaptureActorSnapshot();
    TestTrue(TEXT("Editor-world snapshot contains the actor collection"), Snapshot.Contains(TEXT("\"actors\":[")));
    TestTrue(TEXT("Actor Observer registers its UE UI material showcase"), Panel->IsMaterialShowcaseReady());
    TestEqual(TEXT("CSS Motion bundle installs ten MovieScene definitions"), Panel->GetCssMotionDefinitionDelta(), 10);
    TestEqual(TEXT("CSS Motion bundle binds ten native tracks"), Panel->GetCssMotionBindingDelta(), 10);
    Tab->RequestCloseTab();
    return true;
}
#endif
