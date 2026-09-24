#include "Modules/ModuleManager.h"

#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "RmlUiActorObserverService.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiJSRuntime.h"
#include "RmlUiUnrealModule.h"
#include "RmlUiWidget.h"
#include "Styling/AppStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

namespace
{
const FName ActorObserverTabName(TEXT("RmlUiActorObserver"));

UWorld* CurrentEditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

class SRmlUiActorObserverEditorPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRmlUiActorObserverEditorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&)
    {
        const FRmlUiAnimationRuntime& AnimationRuntime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
        DefinitionsBeforeStart = AnimationRuntime.GetDefinitionCount();
        BindingsBeforeStart = AnimationRuntime.GetBindingCount();
        RmlWidget = TStrongObjectPtr<URmlUiWidget>(NewObject<URmlUiWidget>());
        Runtime = TStrongObjectPtr<URmlUiJSRuntime>(NewObject<URmlUiJSRuntime>());
        Observer = TStrongObjectPtr<URmlUiActorObserverService>(NewObject<URmlUiActorObserverService>());
        RmlWidget->bUseSlateRenderer = true;
        ObservedWorld = CurrentEditorWorld();
        Observer->Initialize(ObservedWorld.Get());
        Observer->AttachMaterialShowcase(RmlWidget.Get());
        Runtime->RegisterService(TEXT("actorObserver"), Observer.Get());

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
                .Text(FText::FromString(FString(TEXT("Could not start Actor Observer: ")) + Error))
                .AutoWrapText(true)
            ];
        }
    }

    virtual ~SRmlUiActorObserverEditorPanel() override
    {
        if (Runtime.IsValid()) Runtime->Stop();
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
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(ActorObserverTabName,
            FOnSpawnTab::CreateRaw(this, &FRmlUiUnrealSamplesEditorModule::SpawnActorObserver))
            .SetDisplayName(FText::FromString(TEXT("RmlUi Actor Observer")))
            .SetTooltipText(FText::FromString(TEXT("Inspect Actors in the current editor level")))
            .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
            .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("LevelEditor.Tabs.Outliner")));
    }

    virtual void ShutdownModule() override
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ActorObserverTabName);
    }

private:
    TSharedRef<SDockTab> SpawnActorObserver(const FSpawnTabArgs&)
    {
        return SNew(SDockTab)
            .TabRole(ETabRole::NomadTab)
            [SNew(SRmlUiActorObserverEditorPanel)];
    }
};

IMPLEMENT_MODULE(FRmlUiUnrealSamplesEditorModule, RmlUiUnrealSamplesEditor)

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiActorObserverEditorTabTest, "RmlUi.ActorObserver.EditorTab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiActorObserverEditorTabTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(ActorObserverTabName);
    if (!TestTrue(TEXT("Actor Observer Nomad tab opens"), Tab.IsValid())) return false;
    const TSharedRef<SRmlUiActorObserverEditorPanel> Panel =
        StaticCastSharedRef<SRmlUiActorObserverEditorPanel>(Tab->GetContent());
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
