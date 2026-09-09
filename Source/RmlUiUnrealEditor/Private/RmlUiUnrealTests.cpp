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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
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

    Registry.UpdateUnreal(UnrealId, 8192);
    const TArray<FRmlUiResourceInfo> Updated = Registry.Snapshot();
    const FRmlUiResourceInfo* UpdatedInfo = Updated.FindByPredicate(
        [UnrealId](const FRmlUiResourceInfo& Info) { return Info.Id == UnrealId; });
    TestTrue(TEXT("Resource estimate can be updated"), UpdatedInfo && UpdatedInfo->EstimatedBytes == 8192);
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

#endif
