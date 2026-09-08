#include "Modules/ModuleManager.h"
#include "SRmlUiWidget.h"
#include "RmlUiUnrealModule.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

class FRmlUiUnrealEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner("RmlUiPreview",
            FOnSpawnTab::CreateRaw(this, &FRmlUiUnrealEditorModule::SpawnPreview))
            .SetDisplayName(FText::FromString("RmlUi Preview"))
            .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
            .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));
    }

    virtual void ShutdownModule() override
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner("RmlUiPreview");
    }

private:
    TSharedRef<SDockTab> SpawnPreview(const FSpawnTabArgs&)
    {
        const FString Document = FPaths::Combine(IPluginManager::Get().FindPlugin("RmlUiUnreal")->GetContentDir(), "RmlUi/Demo.rml");
        TSharedRef<SRmlUiWidget> Preview = SNew(SRmlUiWidget).DocumentPath(Document);
        TSharedRef<SEditableTextBox> Path = SNew(SEditableTextBox).Text(FText::FromString(Document));
        auto Load = [Preview, Path]() { Preview->LoadDocument(Path->GetText().ToString()); return FReply::Handled(); };
        return SNew(SDockTab).TabRole(ETabRole::NomadTab)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(6)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 6, 0)[Path]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                [
                    SNew(SButton).ToolTipText(FText::FromString("Open document"))
                    .OnClicked_Lambda([Preview, Path]() {
                        TArray<FString> Files;
                        const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
                        if (FDesktopPlatformModule::Get()->OpenFileDialog(Parent, TEXT("Open RmlUi document"),
                            FPaths::GetPath(Path->GetText().ToString()), TEXT(""), TEXT("RmlUi documents|*.rml;*.html;*.htm"), 0, Files) && Files.Num())
                        {
                            Path->SetText(FText::FromString(Files[0]));
                            Preview->LoadDocument(Files[0]);
                        }
                        return FReply::Handled();
                    })
                    [SNew(SImage).Image(FAppStyle::GetBrush("Icons.FolderOpen"))]
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
                [SNew(SButton).ToolTipText(FText::FromString("Reload document"))
                    .OnClicked_Lambda(Load)[SNew(SImage).Image(FAppStyle::GetBrush("Icons.Refresh"))]]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [SNew(SCheckBox).OnCheckStateChanged_Lambda([Preview](ECheckBoxState State) {
                    Preview->SetDebuggerVisible(State == ECheckBoxState::Checked);
                })[SNew(STextBlock).Text(FText::FromString("Inspector"))]]
            ]
            + SVerticalBox::Slot().FillHeight(1)[Preview]
        ];
    }
};

IMPLEMENT_MODULE(FRmlUiUnrealEditorModule, RmlUiUnrealEditor)
