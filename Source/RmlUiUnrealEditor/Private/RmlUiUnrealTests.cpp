#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "RmlUiBridge.h"
#include "RmlUiResourceRegistry.h"
#include "RmlUiUnrealModule.h"
#include "SRmlUiWidget.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"

namespace RmlUiTests
{
static FString ContentPath(const TCHAR* Relative)
{
    return FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(), TEXT("RmlUi"), Relative);
}

static FString ArtifactPath(const TCHAR* Name)
{
    FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiTests"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    return FPaths::Combine(Directory, Name);
}

static bool SaveFrame(const RmlUE_Frame& Frame, const TCHAR* Name)
{
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(Frame.Width, Frame.Height,
        TArrayView64<const FColor>(reinterpret_cast<const FColor*>(Frame.Pixels), int64(Frame.Width) * Frame.Height), Png);
    return FFileHelper::SaveArrayToFile(Png, *ArtifactPath(Name));
}

struct FView
{
    RmlUE_View* Handle = nullptr;
    FView(int Width = 640, int Height = 480) { Handle = RmlUE_CreateView(Width, Height, 1.0f); }
    ~FView() { if (Handle) RmlUE_DestroyView(Handle); }
};

static FColor Pixel(const RmlUE_Frame& Frame, int X, int Y)
{
    return reinterpret_cast<const FColor*>(Frame.Pixels)[Y * Frame.Width + X];
}

static void Click(RmlUE_View* View, const RmlUE_Rect& Rect)
{
    RmlUE_MouseMove(View, int(Rect.X + Rect.Width * 0.5f), int(Rect.Y + Rect.Height * 0.5f), 0);
    RmlUE_MouseButton(View, 0, 1, 0);
    RmlUE_MouseButton(View, 0, 0, 0);
}

static TWeakObjectPtr<UObject> FindOwnedObject(uint64 OwnerId, ERmlUiResourceType Type)
{
    for (const FRmlUiResourceInfo& Info : FRmlUiResourceRegistry::Get().SnapshotOwnedBy(OwnerId, true))
    {
        if (Info.Type == Type) return Info.Object;
    }
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiResourceRegistryTest, "RmlUiUnreal.Resources.Registry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiResourceRegistryTest::RunTest(const FString&)
{
    FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    UTexture2D* Object = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
    if (!TestNotNull(TEXT("Registry test UObject"), Object)) return false;
    const uint64 UnrealId = Registry.RegisterUnreal(ERmlUiResourceType::UnrealTexture, ERmlUiResourceBackend::Slate,
        77, 4096, TEXT("Registry test texture"), Object);
    const uint64 ChildId = Registry.RegisterUnreal(ERmlUiResourceType::SlateVertexBuffer,
        ERmlUiResourceBackend::RHI, UnrealId, 128, TEXT("Registry test child"), nullptr,
        ERmlUiResourceState::PendingCreate);
    const uint64 GrandchildId = Registry.RegisterUnreal(ERmlUiResourceType::SlateIndexBuffer,
        ERmlUiResourceBackend::RHI, ChildId, 64, TEXT("Registry test grandchild"));

    bool bFoundUnreal = false;
    bool bVisitorCouldSnapshot = false;
    Registry.Visit([&](const FRmlUiResourceInfo& Info)
    {
        if (Info.Id != UnrealId) return;
        bFoundUnreal = Info.Domain == ERmlUiResourceDomain::Unreal && Info.OwnerId == 77 &&
            Info.EstimatedBytes == 4096 && Info.Object.Get() == Object;
        bVisitorCouldSnapshot = Registry.Snapshot().ContainsByPredicate(
            [UnrealId](const FRmlUiResourceInfo& Item) { return Item.Id == UnrealId; });
    });
    TestTrue(TEXT("Unreal resource exposes metadata and a non-owning UObject reference"), bFoundUnreal);
    TestTrue(TEXT("Visitor executes outside the registry lock"), bVisitorCouldSnapshot);
    TestEqual(TEXT("Direct owner traversal excludes descendants"), Registry.SnapshotOwnedBy(77, false).Num(), 1);
    TestEqual(TEXT("Recursive owner traversal includes descendants"), Registry.SnapshotOwnedBy(77, true).Num(), 3);
    int32 VisitedDescendants = 0;
    bool bOwnedVisitorCouldSnapshot = false;
    Registry.VisitOwnedBy(UnrealId, true, [&](const FRmlUiResourceInfo&)
    {
        ++VisitedDescendants;
        bOwnedVisitorCouldSnapshot = !Registry.Snapshot().IsEmpty();
    });
    TestEqual(TEXT("Owner visitor walks child and grandchild"), VisitedDescendants, 2);
    TestTrue(TEXT("Owner visitor executes outside the registry lock"), bOwnedVisitorCouldSnapshot);
    Registry.SetUnrealState(ChildId, ERmlUiResourceState::Live);
    const FRmlUiResourceInfo* ChildInfo = Registry.Snapshot().FindByPredicate(
        [ChildId](const FRmlUiResourceInfo& Info) { return Info.Id == ChildId; });
    TestTrue(TEXT("Resource lifecycle state can be updated"),
        ChildInfo && ChildInfo->State == ERmlUiResourceState::Live);

    Registry.UpdateUnreal(UnrealId, 8192);
    const TArray<FRmlUiResourceInfo> Updated = Registry.Snapshot();
    const FRmlUiResourceInfo* UpdatedInfo = Updated.FindByPredicate(
        [UnrealId](const FRmlUiResourceInfo& Info) { return Info.Id == UnrealId; });
    TestTrue(TEXT("Resource estimate can be updated"), UpdatedInfo && UpdatedInfo->EstimatedBytes == 8192);
    TestTrue(TEXT("Unreal resource can be reparented without replacing its stable ID"),
        Registry.ReparentUnreal(UnrealId, 88));
    TestTrue(TEXT("Reparent removes the resource tree from its previous owner"),
        Registry.SnapshotOwnedBy(77, true).IsEmpty());
    TestEqual(TEXT("Reparent preserves recursive child ownership under the new owner"),
        Registry.SnapshotOwnedBy(88, true).Num(), 3);
    Registry.UnregisterUnreal(GrandchildId);
    Registry.UnregisterUnreal(ChildId);
    Registry.UnregisterUnreal(UnrealId);
    TestFalse(TEXT("Unregistered Unreal resource is absent"), Registry.Snapshot().ContainsByPredicate(
        [UnrealId](const FRmlUiResourceInfo& Info) { return Info.Id == UnrealId; }));

    uint64 NativeId = 0;
    {
        RmlUiTests::FView View(80, 60);
        if (!TestNotNull(TEXT("Native registry view"), View.Handle)) return false;
        NativeId = RmlUE_GetViewResourceId(View.Handle);
        const TArray<FRmlUiResourceInfo> NativeSnapshot = Registry.Snapshot();
        const FRmlUiResourceInfo* NativeInfo = NativeSnapshot.FindByPredicate(
            [NativeId](const FRmlUiResourceInfo& Info) { return Info.Id == NativeId; });
        TestTrue(TEXT("Native bridge events enter the unified registry"), NativeInfo &&
            NativeInfo->Domain == ERmlUiResourceDomain::Native && NativeInfo->Backend == ERmlUiResourceBackend::DX11);
        TestTrue(TEXT("Native frame buffer is linked to its view"), NativeSnapshot.ContainsByPredicate(
            [NativeId](const FRmlUiResourceInfo& Info)
            {
                return Info.Type == ERmlUiResourceType::FrameBuffer && Info.OwnerId == NativeId && Info.EstimatedBytes == 80ull * 60 * 12;
            }));
    }
    TestFalse(TEXT("Native destroy event removes the view"), Registry.Snapshot().ContainsByPredicate(
        [NativeId](const FRmlUiResourceInfo& Info) { return Info.Id == NativeId; }));

    TSharedRef<SRmlUiWidget> SlateWidget = SNew(SRmlUiWidget).UseSlateRenderer(true)
        .DocumentPath(RmlUiTests::ContentPath(TEXT("Grid.html")));
    if (!TestTrue(TEXT("Slate cache first frame"), SlateWidget->RenderFrame(96, 64))) return false;
    const uint64 SlateOwnerId = RmlUE_GetViewResourceId(SlateWidget->GetNativeView());
    TArray<uint64> FirstGeometryCacheIds;
    for (const FRmlUiResourceInfo& Info : Registry.Snapshot())
    {
        if (Info.OwnerId == SlateOwnerId && Info.Type == ERmlUiResourceType::SlateGeometryCache)
            FirstGeometryCacheIds.Add(Info.Id);
        TestFalse(TEXT("Slate view has no legacy DX11-owned resources"),
            Info.OwnerId == SlateOwnerId && Info.Backend == ERmlUiResourceBackend::DX11);
    }
    TestTrue(TEXT("UE registers geometry created by the incremental ABI"), FirstGeometryCacheIds.Num() > 0);
    FlushRenderingCommands();
    const TArray<FRmlUiResourceInfo> OwnedSlateResources = Registry.SnapshotOwnedBy(SlateOwnerId, true);
    TestTrue(TEXT("Geometry cache owns live RHI vertex buffers"), OwnedSlateResources.ContainsByPredicate(
        [](const FRmlUiResourceInfo& Info)
        {
            return Info.Type == ERmlUiResourceType::SlateVertexBuffer &&
                Info.Backend == ERmlUiResourceBackend::RHI && Info.State == ERmlUiResourceState::Live;
        }));
    TestTrue(TEXT("Geometry cache owns live RHI index buffers"), OwnedSlateResources.ContainsByPredicate(
        [](const FRmlUiResourceInfo& Info)
        {
            return Info.Type == ERmlUiResourceType::SlateIndexBuffer &&
                Info.Backend == ERmlUiResourceBackend::RHI && Info.State == ERmlUiResourceState::Live;
        }));
    TestTrue(TEXT("Slate geometry reports ready after render-thread initialization"),
        SlateWidget->GetReadySlateRhiGeometryCount() > 0);
    TestTrue(TEXT("Slate cache unchanged frame"), SlateWidget->RenderFrame(96, 64));
    int32 CachedGeometryCount = 0;
    for (const FRmlUiResourceInfo& Info : Registry.Snapshot())
        if (Info.OwnerId == SlateOwnerId && Info.Type == ERmlUiResourceType::SlateGeometryCache) ++CachedGeometryCount;
    TestEqual(TEXT("Unchanged frame retains the same UE geometry cache"), CachedGeometryCount, FirstGeometryCacheIds.Num());

    TestTrue(TEXT("Replace incremental Slate document"),
        SlateWidget->LoadDocument(RmlUiTests::ContentPath(TEXT("Grid.html"))));
    TestTrue(TEXT("Render replacement through cache deltas"), SlateWidget->RenderFrame(96, 64));
    FlushRenderingCommands();
    const TArray<FRmlUiResourceInfo> ReplacedResources = Registry.Snapshot();
    for (uint64 OldId : FirstGeometryCacheIds)
        TestFalse(TEXT("Replacement removes stale UE geometry cache entries"), ReplacedResources.ContainsByPredicate(
            [OldId](const FRmlUiResourceInfo& Info) { return Info.Id == OldId; }));
    TestTrue(TEXT("Replacement registers new UE geometry cache entries"), ReplacedResources.ContainsByPredicate(
        [SlateOwnerId](const FRmlUiResourceInfo& Info)
        {
            return Info.OwnerId == SlateOwnerId && Info.Type == ERmlUiResourceType::SlateGeometryCache;
        }));
    SlateWidget->ShutdownNative();
    FlushRenderingCommands();
    TestTrue(TEXT("Slate shutdown removes its complete resource tree"),
        Registry.SnapshotOwnedBy(SlateOwnerId, true).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiMultipleSlateWidgetsTest, "RmlUiUnreal.Resources.MultipleSlateWidgets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiMultipleSlateWidgetsTest::RunTest(const FString&)
{
    FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    const FString FirstDocument = TEXT(R"RML(<rml><head><style>
body { width: 100%; height: 100%; margin: 0; background-color: #a32f3f; }
#panel { display: block; width: 80px; height: 48px; decorator: ue-material(shared.panel); }
</style></head><body><div id="panel" data-owner="first"></div></body></rml>)RML");
    const FString SecondDocument = TEXT(R"RML(<rml><head><style>
body { width: 100%; height: 100%; margin: 0; background-color: #285f9f; }
#panel { display: block; width: 72px; height: 56px; decorator: ue-material(shared.panel); }
</style></head><body><div id="panel" data-owner="second"></div></body></rml>)RML");

    TSharedRef<SRmlUiWidget> FirstWidget = SNew(SRmlUiWidget).UseSlateRenderer(true)
        .InlineDocument(FirstDocument).SourcePath(TEXT("/multi-first.rml")).DesiredSize(FVector2D(160, 100));
    TSharedRef<SRmlUiWidget> SecondWidget = SNew(SRmlUiWidget).UseSlateRenderer(true)
        .InlineDocument(SecondDocument).SourcePath(TEXT("/multi-second.rml")).DesiredSize(FVector2D(160, 100));
    UMaterialInterface* UiMaterial = UMaterial::GetDefaultMaterial(MD_UI);
    TestNotNull(TEXT("Default UI material is available"), UiMaterial);
    TestTrue(TEXT("First widget registers its material alias"), UiMaterial && FirstWidget->RegisterMaterial(TEXT("shared.panel"), UiMaterial));
    TestTrue(TEXT("Second widget registers the same alias independently"), UiMaterial && SecondWidget->RegisterMaterial(TEXT("shared.panel"), UiMaterial));
    TestTrue(TEXT("First widget renders its command stream"), FirstWidget->RenderFrame(160, 100));
    TestTrue(TEXT("Second widget renders its command stream"), SecondWidget->RenderFrame(160, 100));
    FlushRenderingCommands();

    const uint64 FirstOwnerId = RmlUE_GetViewResourceId(FirstWidget->GetNativeView());
    const uint64 SecondOwnerId = RmlUE_GetViewResourceId(SecondWidget->GetNativeView());
    TestTrue(TEXT("Multiple widgets have distinct stable View owners"), FirstOwnerId != 0 && SecondOwnerId != 0 && FirstOwnerId != SecondOwnerId);
    FString FirstOwner, SecondOwner;
    TestTrue(TEXT("First widget retains its own DOM state"),
        FirstWidget->GetElementAttribute(TEXT("panel"), TEXT("data-owner"), FirstOwner) && FirstOwner == TEXT("first"));
    TestTrue(TEXT("Second widget retains its own DOM state"),
        SecondWidget->GetElementAttribute(TEXT("panel"), TEXT("data-owner"), SecondOwner) && SecondOwner == TEXT("second"));

    const TArray<FRmlUiResourceInfo> FirstResources = Registry.SnapshotOwnedBy(FirstOwnerId, true);
    const TArray<FRmlUiResourceInfo> SecondResources = Registry.SnapshotOwnedBy(SecondOwnerId, true);
    TestTrue(TEXT("First widget owns live render resources"), !FirstResources.IsEmpty());
    TestTrue(TEXT("Second widget owns live render resources"), !SecondResources.IsEmpty());
    TestTrue(TEXT("Each widget owns its registered Slate material brush"),
        FirstResources.ContainsByPredicate([](const FRmlUiResourceInfo& Info)
        {
            return Info.Type == ERmlUiResourceType::SlateMaterialBrush && Info.Backend == ERmlUiResourceBackend::Slate;
        }) && SecondResources.ContainsByPredicate([](const FRmlUiResourceInfo& Info)
        {
            return Info.Type == ERmlUiResourceType::SlateMaterialBrush && Info.Backend == ERmlUiResourceBackend::Slate;
        }));
    bool bResourceTreesDisjoint = true;
    for (const FRmlUiResourceInfo& First : FirstResources)
    {
        bResourceTreesDisjoint &= !SecondResources.ContainsByPredicate(
            [&First](const FRmlUiResourceInfo& Second) { return Second.Id == First.Id; });
    }
    TestTrue(TEXT("Multiple widget resource trees do not share owned records"), bResourceTreesDisjoint);

    const uint64 SecondFrameBeforeRelease = SecondWidget->GetFrameNumber();
    FirstWidget->ShutdownNative();
    FlushRenderingCommands();
    TestTrue(TEXT("Releasing one widget removes only its complete resource tree"),
        Registry.SnapshotOwnedBy(FirstOwnerId, true).IsEmpty());
    TestEqual(TEXT("The other widget resource tree survives peer shutdown"),
        Registry.SnapshotOwnedBy(SecondOwnerId, true).Num(), SecondResources.Num());
    TestTrue(TEXT("The surviving widget continues rendering"),
        SecondWidget->RenderFrame(160, 100) && SecondWidget->GetFrameNumber() > SecondFrameBeforeRelease);
    TestFalse(TEXT("A shut down widget cannot recreate resources implicitly"), FirstWidget->RenderFrame(160, 100));

    SecondWidget->ShutdownNative();
    FlushRenderingCommands();
    TestTrue(TEXT("Releasing the final widget removes its complete resource tree"),
        Registry.SnapshotOwnedBy(SecondOwnerId, true).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiMultiWidgetResourceStressTest, "RmlUiUnreal.Resources.MultiWidgetStress",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiMultiWidgetResourceStressTest::RunTest(const FString&)
{
    FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
    FRmlUiResourceRegistry& Registry = FRmlUiResourceRegistry::Get();
    UMaterialInterface* UiMaterial = UMaterial::GetDefaultMaterial(MD_UI);
    if (!TestNotNull(TEXT("Default UI material is available for resource stress"), UiMaterial)) return false;

    constexpr int32 CyclesPerScale = 4;
    constexpr int32 ExpectedWidgetCount = (1 + 4 + 8) * CyclesPerScale;
    int32 CreatedWidgetCount = 0;
    int32 PeakOwnedResourceCount = 0;
    for (const int32 WidgetCount : { 1, 4, 8 })
    {
        for (int32 Cycle = 0; Cycle < CyclesPerScale; ++Cycle)
        {
            TArray<TSharedPtr<SRmlUiWidget>> Widgets;
            TArray<uint64> OwnerIds;
            TArray<TWeakObjectPtr<UObject>> ReplacedMaterials;
            TArray<TWeakObjectPtr<UObject>> ActiveMaterials;
            TSet<uint64> UniqueOwners;
            Widgets.Reserve(WidgetCount);
            OwnerIds.Reserve(WidgetCount);

            for (int32 Index = 0; Index < WidgetCount; ++Index)
            {
                const FString InitialDocument = FString::Printf(TEXT(R"RML(<rml><head><style>
body { width: 100%%; height: 100%%; margin: 0; background-color: #%02x3040; }
#panel { display: block; width: 72px; height: 48px; decorator: ue-material(stress.panel); }
</style></head><body><div id="panel" data-state="initial-%d-%d"></div></body></rml>)RML"),
                    32 + Index, Cycle, Index);
                TSharedPtr<SRmlUiWidget> Widget = SNew(SRmlUiWidget).UseSlateRenderer(true)
                    .InlineDocument(InitialDocument)
                    .SourcePath(FString::Printf(TEXT("/stress/%d/%d/initial.rml"), WidgetCount, Cycle))
                    .DesiredSize(FVector2D(128, 80));

                UMaterialInstanceDynamic* InitialMaterial = UMaterialInstanceDynamic::Create(UiMaterial, GetTransientPackage());
                TestNotNull(TEXT("Create initial stress MID"), InitialMaterial);
                TestTrue(TEXT("Register initial stress material"),
                    InitialMaterial && Widget->RegisterMaterial(TEXT("stress.panel"), InitialMaterial));
                TestTrue(TEXT("Render initial stress document"), Widget->RenderFrame(128, 80));

                const uint64 OwnerId = RmlUE_GetViewResourceId(Widget->GetNativeView());
                TestTrue(TEXT("Stress Widget has a stable View owner"), OwnerId != 0 && !UniqueOwners.Contains(OwnerId));
                UniqueOwners.Add(OwnerId);
                OwnerIds.Add(OwnerId);

                UMaterialInstanceDynamic* ReplacementMaterial = UMaterialInstanceDynamic::Create(UiMaterial, GetTransientPackage());
                TestNotNull(TEXT("Create replacement stress MID"), ReplacementMaterial);
                ReplacedMaterials.Add(RmlUiTests::FindOwnedObject(OwnerId, ERmlUiResourceType::SlateMaterialBrush));
                TestTrue(TEXT("Replace stress material under the same alias"),
                    ReplacementMaterial && Widget->RegisterMaterial(TEXT("stress.panel"), ReplacementMaterial));

                const FString ReloadedDocument = FString::Printf(TEXT(R"RML(<rml><head><style>
body { width: 100%%; height: 100%%; margin: 0; background-color: #2030%02x; }
#panel { display: block; width: 76px; height: 52px; decorator: ue-material(stress.panel); }
</style></head><body><div id="panel" data-state="reloaded-%d-%d"></div></body></rml>)RML"),
                    48 + Index, Cycle, Index);
                TestTrue(TEXT("Reload stress document"), Widget->LoadDocumentFromString(ReloadedDocument,
                    FString::Printf(TEXT("/stress/%d/%d/reloaded.rml"), WidgetCount, Cycle)));
                TestTrue(TEXT("Render reloaded stress document"), Widget->RenderFrame(128, 80));
                ActiveMaterials.Add(RmlUiTests::FindOwnedObject(OwnerId, ERmlUiResourceType::SlateMaterialBrush));
                Widgets.Add(MoveTemp(Widget));
                ++CreatedWidgetCount;
            }

            FlushRenderingCommands();
            CollectGarbage(RF_NoFlags, true);
            for (int32 Index = 0; Index < WidgetCount; ++Index)
            {
                const TArray<FRmlUiResourceInfo> OwnedResources = Registry.SnapshotOwnedBy(OwnerIds[Index], true);
                PeakOwnedResourceCount = FMath::Max(PeakOwnedResourceCount, OwnedResources.Num());
                TestTrue(TEXT("Every active stress Widget owns render resources"), !OwnedResources.IsEmpty());
                TestTrue(TEXT("Active stress material survives GC through the Slate brush"), ActiveMaterials[Index].IsValid());
                TestFalse(TEXT("Replaced stress material is collectable"), ReplacedMaterials[Index].IsValid());
            }

            for (int32 Index = WidgetCount - 1; Index >= 0; --Index)
            {
                if ((Index & 1) == 0) Widgets[Index]->ShutdownNative();
                Widgets[Index].Reset();
            }
            Widgets.Reset();
            FlushRenderingCommands();
            CollectGarbage(RF_NoFlags, true);
            for (int32 Index = 0; Index < WidgetCount; ++Index)
            {
                TestTrue(TEXT("Released stress Widget leaves no owned Registry records"),
                    Registry.SnapshotOwnedBy(OwnerIds[Index], true).IsEmpty());
                TestFalse(TEXT("Released stress material is collectable"), ActiveMaterials[Index].IsValid());
            }
        }
    }

    TestEqual(TEXT("Stress test exercised all 1/4/8 Widget cycles"), CreatedWidgetCount, ExpectedWidgetCount);
    TestTrue(TEXT("Stress test observed nonempty per-Widget resource trees"), PeakOwnedResourceCount > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiRenderTest, "RmlUiUnreal.Native.HtmlCssEffects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiRenderTest::RunTest(const FString&)
{
    FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
    RmlUiTests::FView View;
    if (!TestNotNull(TEXT("Native DX11 view"), View.Handle)) return false;
    const FString Path = RmlUiTests::ContentPath(TEXT("Tests/capabilities.html"));
    if (!TestTrue(TEXT("Load .html and external .css"), RmlUE_LoadDocument(View.Handle, TCHAR_TO_UTF8(*Path)) != 0)) return false;
    RmlUE_Frame Frame{};
    if (!TestTrue(TEXT("Render real DX11 pixels"), RmlUE_Render(View.Handle, &Frame) != 0)) return false;
    TestEqual(TEXT("Frame width"), Frame.Width, 640);
    TestEqual(TEXT("Frame height"), Frame.Height, 480);
    FColor Marker = RmlUiTests::Pixel(Frame, 32, 32);
    TestTrue(TEXT("External CSS red marker: BGRA order, position, alpha"),
        FMath::Abs(int(Marker.R) - 219) < 8 && FMath::Abs(int(Marker.G) - 54) < 8 && FMath::Abs(int(Marker.B) - 74) < 8 && Marker.A > 250);
    FColor Left = RmlUiTests::Pixel(Frame, 140, 40);
    FColor Right = RmlUiTests::Pixel(Frame, 285, 40);
    TestTrue(TEXT("Gradient varies from blue to yellow"), Left.B > Left.R && Right.R > Right.B);
    RmlUE_Stats Stats{};
    RmlUE_GetStats(View.Handle, &Stats);
    TestTrue(TEXT("Geometry rendered"), Stats.GeometryDraws > 0);
    TestTrue(TEXT("Rounded and transformed clipping used"), Stats.ClipMasks > 0);
    TestTrue(TEXT("Offscreen compositing used"), Stats.Layers > 0);
    TestTrue(TEXT("Filter compiled"), Stats.Filters > 0);
    TestTrue(TEXT("Gradient shader compiled"), Stats.Shaders > 0);
    TestTrue(TEXT("Save native effects screenshot"), RmlUiTests::SaveFrame(Frame, TEXT("native-effects.png")));
    TestTrue(TEXT("Resize with DPI"), RmlUE_Resize(View.Handle, 800, 600, 1.25f) != 0);
    TestTrue(TEXT("Render resized target"), RmlUE_Render(View.Handle, &Frame) != 0);
    TestEqual(TEXT("Resized pixel buffer width"), Frame.Width, 800);
    TestTrue(TEXT("Save resized screenshot"), RmlUiTests::SaveFrame(Frame, TEXT("native-effects-dpi.png")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiInputTest, "RmlUiUnreal.Native.InputAndLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiInputTest::RunTest(const FString&)
{
    FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
    RmlUiTests::FView View;
    if (!TestNotNull(TEXT("Native input context"), View.Handle)) return false;
    const FString Path = RmlUiTests::ContentPath(TEXT("Tests/capabilities.html"));
    if (!TestTrue(TEXT("Load fixture"), RmlUE_LoadDocument(View.Handle, TCHAR_TO_UTF8(*Path)) != 0)) return false;
    RmlUE_Frame Frame{};
    RmlUE_Render(View.Handle, &Frame);
    RmlUE_Rect Rect{};
    TestTrue(TEXT("Button layout exists"), RmlUE_GetElementRect(View.Handle, "action", &Rect) != 0);
    RmlUiTests::Click(View.Handle, Rect);
    bool bClicked = false;
    RmlUE_Event Event{};
    while (RmlUE_PollEvent(View.Handle, &Event))
        bClicked |= FCStringAnsi::Strcmp(Event.Type, "click") == 0 && FCStringAnsi::Strcmp(Event.ElementId, "action") == 0;
    TestTrue(TEXT("Pointer coordinates dispatch click to correct element"), bClicked);
    TestTrue(TEXT("Input layout exists"), RmlUE_GetElementRect(View.Handle, "entry", &Rect) != 0);
    RmlUiTests::Click(View.Handle, Rect);
    RmlUE_Text(View.Handle, "Unreal 58");
    RmlUE_Key(View.Handle, 0x08, 1, 0);
    RmlUE_Key(View.Handle, 0x08, 0, 0);
    char Value[128]{};
    TestTrue(TEXT("Read input value"), RmlUE_GetAttribute(View.Handle, "entry", "value", Value, sizeof(Value)) != 0);
    TestEqual(TEXT("Text input and backspace"), FString(UTF8_TO_TCHAR(Value)), FString(TEXT("Unreal 5")));
    TestTrue(TEXT("DOM property"), RmlUE_SetProperty(View.Handle, "marker", "background-color", "#176b59") != 0);
    RmlUE_Render(View.Handle, &Frame);
    FColor Changed = RmlUiTests::Pixel(Frame, 32, 32);
    TestTrue(TEXT("DOM mutation changes rendered pixels"), Changed.G > Changed.R && Changed.G > Changed.B);
    TestTrue(TEXT("DOM text update"), RmlUE_SetInnerRml(View.Handle, "status", "Updated from Unreal") != 0);
    AddExpectedMessagePlain(TEXT("Element id was not found: missing"), ELogVerbosity::Warning);
    TestTrue(TEXT("Missing id rejected"), RmlUE_SetInnerRml(View.Handle, "missing", "x") == 0);
    RmlUE_Rect Before{}, After{};
    RmlUE_GetElementRect(View.Handle, "scroll-content", &Before);
    RmlUE_GetElementRect(View.Handle, "scroll", &Rect);
    RmlUE_MouseMove(View.Handle, int(Rect.X + 40), int(Rect.Y + 40), 0);
    RmlUE_MouseWheel(View.Handle, -3.0f, 0);
    // Wheel scrolling advances with the system clock, independently of the render-call count.
    const double ScrollDeadline = FPlatformTime::Seconds() + 0.5;
    do
    {
        RmlUE_Render(View.Handle, &Frame);
        RmlUE_GetElementRect(View.Handle, "scroll-content", &After);
        if (After.Y < Before.Y) break;
        FPlatformProcess::Sleep(0.01f);
    } while (FPlatformTime::Seconds() < ScrollDeadline);
    TestTrue(TEXT("Wheel scroll moves content upward"), After.Y < Before.Y);
    RmlUE_FocusLost(View.Handle);
    for (int Index = 0; Index < 3; ++Index)
    {
        RmlUiTests::FView Other(320, 240);
        TestNotNull(TEXT("Independent second context"), Other.Handle);
        if (Other.Handle)
        {
            TestTrue(TEXT("In-memory document"), RmlUE_LoadDocumentFromMemory(Other.Handle,
                "<rml><head><style>body {font-family:LatoLatin;font-size:16px;background-color:#375a7f;width:100%;height:100%;}</style></head><body>Second</body></rml>", "memory.rml") != 0);
            TestTrue(TEXT("Second context renders"), RmlUE_Render(Other.Handle, &Frame) != 0);
        }
        TestTrue(TEXT("Reload original document"), RmlUE_LoadDocument(View.Handle, TCHAR_TO_UTF8(*Path)) != 0);
        TestTrue(TEXT("Original context remains usable"), RmlUE_Render(View.Handle, &Frame) != 0);
    }
    AddExpectedMessagePlain(TEXT("View dimensions must be 1..4096 and DPR finite and positive."), ELogVerbosity::Warning);
    TestTrue(TEXT("Reject zero dimension"), RmlUE_Resize(View.Handle, 0, 480, 1.0f) == 0);
    TestTrue(TEXT("Recover after invalid resize"), RmlUE_Render(View.Handle, &Frame) != 0);
    return true;
}

class FRmlUiSlateCapture : public IAutomationLatentCommand
{
public:
    explicit FRmlUiSlateCapture(FAutomationTestBase* InTest, bool InGrid = false) : Test(InTest), bGrid(InGrid) {}
    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            Widget = SNew(SRmlUiWidget).DocumentPath(RmlUiTests::ContentPath(bGrid ? TEXT("Grid.html") : TEXT("Demo.rml")));
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi render verification")))
                .ClientSize(FVector2D(1280, 800)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[Widget.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Start = FPlatformTime::Seconds();
            return false;
        }
        if (FPlatformTime::Seconds() - Start < 2.0) return false;
        Test->TestTrue(TEXT("Slate widget has rendered frames"), Widget->GetFrameNumber() > 0);
        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        FlushRenderingCommands();
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size);
        Test->TestTrue(TEXT("Actual Slate/RHI screenshot"), bCaptured && Pixels.Num() > 0);
        if (bCaptured && Pixels.Num() > 0)
        {
            Test->TestEqual(TEXT("Capture width matches requested viewport"), Size.X, Stage == 1 ? 800 : bGrid && Stage == 2 ? 390 : 1280);
            Test->TestEqual(TEXT("Capture height matches requested viewport"), Size.Y, Stage == 1 ? 600 : bGrid && Stage == 2 ? 844 : 800);
            int32 DistinctSamples = 0;
            const FColor First = Pixels[0];
            for (int32 Index = 0; Index < Pixels.Num(); Index += 97)
                if (Pixels[Index] != First && Pixels[Index].R + Pixels[Index].G + Pixels[Index].B > 60) ++DistinctSamples;
            Test->TestTrue(TEXT("Final UE output is nonblank"), DistinctSamples > 200);
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            Test->TestTrue(TEXT("Save final Unreal screenshot"), FFileHelper::SaveArrayToFile(Png,
                *RmlUiTests::ArtifactPath(bGrid ? (Stage == 0 ? TEXT("unreal-grid-1280.png") : Stage == 1 ? TEXT("unreal-grid-800.png") : TEXT("unreal-grid-390.png")) :
                    (Stage == 0 ? TEXT("unreal-demo-1280.png") : Stage == 1 ? TEXT("unreal-demo-800.png") : TEXT("unreal-assets.png")))));
        }
        if (bGrid)
        {
            RmlUE_Rect Main{}, Side{}, Resource{}, Metrics{}, Apply{};
            Test->TestTrue(TEXT("Grid main exists"), RmlUE_GetElementRect(Widget->GetNativeView(), "main", &Main) != 0);
            Test->TestTrue(TEXT("Grid sidebar exists"), RmlUE_GetElementRect(Widget->GetNativeView(), "sidebar", &Side) != 0);
            Test->TestTrue(TEXT("Named areas respond to viewport"), Stage == 2 ? Side.Y > Main.Y + Main.Height : Main.X >= Side.X + Side.Width);
            Test->TestTrue(TEXT("Spanning resource exists"), RmlUE_GetElementRect(Widget->GetNativeView(), "featured", &Resource) != 0);
            Test->TestTrue(TEXT("Resource spans two rows"), FMath::IsNearlyEqual(Resource.Height, 312.f, 1.f));
            Test->TestTrue(TEXT("Metrics exists"), RmlUE_GetElementRect(Widget->GetNativeView(), "metrics", &Metrics) != 0);
            Test->TestTrue(TEXT("Main remains inside viewport"), Main.X + Main.Width <= Size.X + 1 && Main.Width > 100);
            RmlUE_Rect Reload{};
            Test->TestTrue(TEXT("Nested flex toolbar retains button height"), RmlUE_GetElementRect(Widget->GetNativeView(), "reload", &Reload) != 0 && Reload.Height >= 30);
            if (Stage == 0 && RmlUE_GetElementRect(Widget->GetNativeView(), "apply", &Apply))
            {
                RmlUiTests::Click(Widget->GetNativeView(), Apply);
                RmlUE_Event Event{}; bool bClicked = false;
                while (RmlUE_PollEvent(Widget->GetNativeView(), &Event)) bClicked |= FCStringAnsi::Strcmp(Event.Type,"click") == 0 && FCStringAnsi::Strcmp(Event.ElementId,"apply") == 0;
                Test->TestTrue(TEXT("Grid hit testing delivers click"), bClicked);
            }
        }
        if (Stage == 0)
        {
            Stage = 1;
            Window->Resize(FVector2D(800, 600));
            Start = FPlatformTime::Seconds();
            return false;
        }
        if (Stage == 1)
        {
            Stage = 2;
            Window->Resize(bGrid ? FVector2D(390, 844) : FVector2D(1280, 800));
            if (!bGrid) {
                Widget->SetElementProperty(TEXT("view-workspace"), TEXT("display"), TEXT("none"));
                Widget->SetElementProperty(TEXT("view-assets"), TEXT("display"), TEXT("block"));
            }
            Start = FPlatformTime::Seconds();
            return false;
        }
        RmlUE_Stats Stats{};
        RmlUE_GetStats(Widget->GetNativeView(), &Stats);
        Test->TestTrue(TEXT("Demo loads real image textures"), Stats.LoadedTextures > 0);
        FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Widget.Reset();
        Window.Reset();
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SRmlUiWidget> Widget;
    double Start = 0;
    int Stage = 0;
    bool bGrid = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiSlateTest, "RmlUiUnreal.Slate.RenderAndResize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiSlateTest::RunTest(const FString&)
{
    AddCommand(new FRmlUiSlateCapture(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiGridSlateTest, "RmlUiUnreal.Slate.Grid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlUiGridSlateTest::RunTest(const FString&)
{
    AddCommand(new FRmlUiSlateCapture(this, true));
    return true;
}

class FRmlUiSlateRhiCapture final : public IAutomationLatentCommand
{
public:
    explicit FRmlUiSlateRhiCapture(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            const FString Document = TEXT(R"RML(
<rml><head><style>
body { width: 100%; height: 100%; margin: 0; background-color: #162231; }
#warm { display: block; position: absolute; left: 36px; top: 32px; width: 220px; height: 136px; background-color: #e84a44; }
#cool { display: block; position: absolute; left: 184px; top: 118px; width: 244px; height: 150px; background-color: #2cb891; }
#alpha { display: block; position: absolute; left: 356px; top: 32px; width: 84px; height: 62px; background-color: rgba(255, 255, 255, 128); }
#mask { display: block; position: absolute; left: 30px; top: 210px; width: 130px; height: 80px; overflow: hidden; border-radius: 24px; }
#mask-inner { display: block; position: relative; left: -10px; top: 10px; width: 130px; height: 60px; overflow: hidden; border-radius: 18px; }
#mask-fill { display: block; width: 140px; height: 80px; background-color: #f5c542; }
</style></head><body><div id="warm"></div><div id="cool"></div><div id="alpha"></div><div id="mask"><div id="mask-inner"><div id="mask-fill"></div></div></div></body></rml>
)RML");
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(true).InlineDocument(Document)
                .SourcePath(TEXT("/slate-rhi-direct.rml")).DesiredSize(FVector2D(480, 320));
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi Slate RHI verification")))
                .ClientSize(FVector2D(480, 320)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[Widget.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            OwnerId = RmlUE_GetViewResourceId(Widget->GetNativeView());
            Start = FPlatformTime::Seconds();
            return false;
        }
        if (FPlatformTime::Seconds() - Start < 2.0) return false;

        FlushRenderingCommands();
        Test->TestTrue(TEXT("Slate RHI fixture rendered frames"), Widget->GetFrameNumber() > 0);
        Test->TestTrue(TEXT("Slate RHI geometry initialized"), Widget->GetReadySlateRhiGeometryCount() > 0);
        RmlUE_Rect WarmRect{}, CoolRect{};
        Test->TestTrue(TEXT("Warm fixture element has layout"),
            RmlUE_GetElementRect(Widget->GetNativeView(), "warm", &WarmRect) != 0 && WarmRect.Width > 100);
        Test->TestTrue(TEXT("Cool fixture element has layout"),
            RmlUE_GetElementRect(Widget->GetNativeView(), "cool", &CoolRect) != 0 && CoolRect.Width > 100);
        const TArray<FRmlUiResourceInfo> OwnedResources =
            FRmlUiResourceRegistry::Get().SnapshotOwnedBy(OwnerId, true);
        Test->TestTrue(TEXT("Direct fixture has live RHI buffers"), OwnedResources.ContainsByPredicate(
            [](const FRmlUiResourceInfo& Info)
            {
                return Info.Backend == ERmlUiResourceBackend::RHI && Info.State == ERmlUiResourceState::Live;
            }));

        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size);
        Test->TestTrue(TEXT("Capture direct Slate RHI output"), bCaptured && Pixels.Num() > 0);
        Test->TestTrue(TEXT("Pure CSS fixture submits persistent RHI draws"), Widget->GetSlateRhiDrawCount() > 0);
        Test->TestTrue(TEXT("Nested rounded clipping submits RHI stencil masks"), Widget->GetSlateRhiMaskCount() >= 2);
        Test->TestTrue(TEXT("Pure CSS fixture needs no transient Slate vertex fallback"),
            Widget->GetSlateFallbackDrawCount() == 0);
        if (bCaptured && Pixels.Num() > 0)
        {
            const auto PixelAt = [&](int32 X, int32 Y) { return Pixels[Y * Size.X + X]; };
            const auto NearRgb = [](FColor Actual, FColor Expected, int32 Tolerance)
            {
                return FMath::Abs(int32(Actual.R) - Expected.R) <= Tolerance &&
                    FMath::Abs(int32(Actual.G) - Expected.G) <= Tolerance &&
                    FMath::Abs(int32(Actual.B) - Expected.B) <= Tolerance;
            };
            Test->TestTrue(TEXT("Direct shader preserves CSS background color space"),
                NearRgb(PixelAt(10, 10), FColor(22, 34, 49), 4));
            Test->TestTrue(TEXT("Direct shader preserves warm CSS color"),
                NearRgb(PixelAt(60, 60), FColor(232, 74, 68), 4));
            Test->TestTrue(TEXT("Direct shader preserves cool CSS color"),
                NearRgb(PixelAt(300, 200), FColor(44, 184, 145), 4));
            Test->TestTrue(TEXT("Premultiplied alpha composites over the CSS background"),
                NearRgb(PixelAt(380, 60), FColor(139, 145, 152), 6));
            Test->TestTrue(TEXT("Outer rounded mask clips inside the nested mask rectangle"),
                NearRgb(PixelAt(32, 222), FColor(22, 34, 49), 4));
            Test->TestTrue(TEXT("Nested rounded mask preserves its intersected content"),
                NearRgb(PixelAt(80, 240), FColor(245, 197, 66), 5));
            Test->TestTrue(TEXT("Inner rounded mask clips inside the outer mask rectangle"),
                NearRgb(PixelAt(148, 222), FColor(22, 34, 49), 4));
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
                TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            Test->TestTrue(TEXT("Save direct Slate RHI screenshot"), FFileHelper::SaveArrayToFile(Png,
                *RmlUiTests::ArtifactPath(TEXT("slate-rhi-direct.png"))));
        }

        Widget->ShutdownNative();
        FlushRenderingCommands();
        Test->TestTrue(TEXT("Direct fixture releases its complete resource tree"),
            FRmlUiResourceRegistry::Get().SnapshotOwnedBy(OwnerId, true).IsEmpty());
        FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Widget.Reset();
        Window.Reset();
        return true;
    }

private:
    FAutomationTestBase* Test;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SRmlUiWidget> Widget;
    uint64 OwnerId = 0;
    double Start = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiSlateRhiTest, "RmlUiUnreal.Slate.RhiDirectRendering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiSlateRhiTest::RunTest(const FString&)
{
    AddCommand(new FRmlUiSlateRhiCapture(this));
    return true;
}

#endif
