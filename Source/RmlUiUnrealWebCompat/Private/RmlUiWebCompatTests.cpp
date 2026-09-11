#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RmlUiBridge.h"
#include "RmlUiResourceRegistry.h"
#include "RmlUiWebCompatModule.h"
#include "RmlUiWebWidget.h"
#include "SRmlUiWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

namespace
{
FString WebCompatArtifactPath(const TCHAR* Name)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiTests"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    return FPaths::Combine(Directory, Name);
}

UMaterial* CreateNativeUiMaterial(const FLinearColor& Color, float Opacity)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->MaterialDomain = MD_UI;
    Material->BlendMode = BLEND_Translucent;
    UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();
    EditorOnly->EmissiveColor.UseConstant = true;
    EditorOnly->EmissiveColor.Constant = Color;
    EditorOnly->Opacity.UseConstant = true;
    EditorOnly->Opacity.Constant = Opacity;
    Material->PostEditChange();
    return Material;
}

TWeakObjectPtr<UObject> FindOwnedMaterialObject(uint64 OwnerId)
{
    for (const FRmlUiResourceInfo& Info : FRmlUiResourceRegistry::Get().SnapshotOwnedBy(OwnerId, true))
    {
        if (Info.Type == ERmlUiResourceType::SlateMaterialBrush) return Info.Object;
    }
    return nullptr;
}

class FWebCompatVisualCapture final : public IAutomationLatentCommand
{
public:
    FWebCompatVisualCapture(FAutomationTestBase* InTest, bool bInMaterial) : Test(InTest), bMaterial(bInMaterial) {}

    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            Widget.Reset(NewObject<URmlUiWebWidget>());
            Widget->DesiredSize = FVector2D(720, 420);
            Widget->bUseSlateRenderer = bMaterial;
            if (bMaterial)
            {
                Widget->DocumentPath.Empty();
                Widget->InlineSourcePath = TEXT("F://RmlUiTests/material-e2e.html");
                Widget->InlineDocument = TEXT(R"(<html><head><style>
html, body { width: 100%; height: 100%; margin: 0; background: #18202a; font-family: LatoLatin; color: white; }
.stage { width: 100%; height: 100%; padding: 52px; box-sizing: border-box; }
.material-frame { display: block; width: 444px; height: 244px; border: 12px solid #fff; border-radius: 24px; box-sizing: border-box; -rmlui-material: engine.border-ui; -rmlui-material-slot: border; }
.material-panel { display: block; width: 420px; height: 220px; padding: 28px; box-sizing: border-box; background: #24415c; -rmlui-material: engine.default-ui; -rmlui-material-slot: background; }
.material-panel h1 { margin: 0 0 18px 0; font-size: 28px; }
.material-panel p { margin: 0; font-size: 17px; }
.material-clip { display: block; position: absolute; left: 540px; top: 52px; width: 120px; height: 100px; overflow: hidden; border-radius: 30px; }
.material-clip-panel { display: block; width: 160px; height: 100px; -rmlui-material: engine.default-ui; -rmlui-material-slot: background; }
</style></head><body><main class="stage"><section class="material-frame" id="material-frame"><div class="material-panel" id="material-panel"><h1>UE UI Material</h1><p>Background and rounded border are native Slate material draws.</p></div></section><aside class="material-clip" id="material-clip"><div class="material-clip-panel" id="material-clip-panel"></div></aside></main></body></html>)");
                UMaterial* BackgroundMaterial = CreateNativeUiMaterial(FLinearColor(0.08f, 0.42f, 0.62f, 1.0f), 0.82f);
                UMaterial* BorderMaterial = CreateNativeUiMaterial(FLinearColor(0.82f, 0.04f, 0.28f, 1.0f), 1.0f);
                Test->TestNotNull(TEXT("Create native UE UI-domain background material"), BackgroundMaterial);
                Test->TestNotNull(TEXT("Create native UE UI-domain border material"), BorderMaterial);
                if (BackgroundMaterial)
                    Test->TestTrue(TEXT("Register native UE UI background material alias"), Widget->RegisterMaterial(TEXT("engine.default-ui"), BackgroundMaterial));
                if (BorderMaterial)
                    Test->TestTrue(TEXT("Register native UE UI border material alias"), Widget->RegisterMaterial(TEXT("engine.border-ui"), BorderMaterial));
            }
            else
            {
                const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"));
                Test->TestTrue(TEXT("Find WebCompat plugin"), Plugin.IsValid());
                if (!Plugin.IsValid()) return true;
                Widget->DocumentPath = FPaths::Combine(Plugin->GetContentDir(), TEXT("RmlUi/Tests/bulma-card.html"));
            }

            SlateRoot = Widget->TakeWidget();
            Window = SNew(SWindow).Title(FText::FromString(bMaterial ? TEXT("RmlUi UE material E2E") : TEXT("RmlUi Bulma card E2E")))
                .ClientSize(FVector2D(720, 420)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[SlateRoot.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Started = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Started < 2.0) return false;
        const TSharedPtr<SRmlUiWidget> SlateWidget = Widget->GetSlateRmlWidget();
        Test->TestTrue(TEXT("Slate widget rendered frames"), SlateWidget.IsValid() && SlateWidget->GetFrameNumber() > 0);
        Test->TestTrue(TEXT("Document rendered without errors"), SlateWidget.IsValid() && SlateWidget->GetLastError().IsEmpty());
        if (SlateWidget.IsValid() && bMaterial)
        {
            Test->TestTrue(TEXT("Native Slate renderer is active"), SlateWidget->IsUsingSlateRenderer());
            Test->TestTrue(TEXT("Registered UE material was resolved by an actual draw"), SlateWidget->GetResolvedMaterialDrawCount() > 0);
            Test->TestTrue(TEXT("Background material slot was resolved by an actual draw"),
                SlateWidget->GetResolvedMaterialDrawCount(RMLUE_MATERIAL_SLOT_BACKGROUND) > 0);
            Test->TestTrue(TEXT("Border material slot was resolved by an actual draw"),
                SlateWidget->GetResolvedMaterialDrawCount(RMLUE_MATERIAL_SLOT_BORDER) > 0);
            Test->TestTrue(TEXT("Rounded CSS overflow clips an actual UE material draw"),
                SlateWidget->GetSlateMaterialClipDrawCount() > 0);
            Test->TestEqual(TEXT("Supported convex material clipping has no unsupported feature bit"),
                SlateWidget->GetUnsupportedSlateFeatures() & RMLUE_UNSUPPORTED_CLIP_MASK, 0u);
        }
        if (SlateWidget.IsValid() && !bMaterial)
        {
            RmlUE_Rect Header{}, Content{}, Footer{}, Details{}, Approve{};
            Test->TestTrue(TEXT("Bulma card regions have layout"),
                RmlUE_GetElementRect(SlateWidget->GetNativeView(), "card-header", &Header) &&
                RmlUE_GetElementRect(SlateWidget->GetNativeView(), "card-content", &Content) &&
                RmlUE_GetElementRect(SlateWidget->GetNativeView(), "card-footer", &Footer));
            Test->TestTrue(TEXT("Bulma card keeps header-content-footer order"), Header.Y < Content.Y && Content.Y < Footer.Y);
            Test->TestTrue(TEXT("Bulma footer uses horizontal flex layout"),
                RmlUE_GetElementRect(SlateWidget->GetNativeView(), "details", &Details) &&
                RmlUE_GetElementRect(SlateWidget->GetNativeView(), "approve", &Approve) &&
                FMath::IsNearlyEqual(Details.Y, Approve.Y, 1.0f) && Approve.X > Details.X + Details.Width - 2.0f);
        }

        FlushRenderingCommands();
        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(SlateRoot.ToSharedRef(), Pixels, Size);
        Test->TestTrue(TEXT("Capture end-to-end Slate output"), bCaptured && Size.X == 720 && Size.Y == 420 && Pixels.Num() == Size.X * Size.Y);
        if (bCaptured && Pixels.Num() > 0)
        {
            int32 DistinctSamples = 0;
            const FColor First = Pixels[0];
            for (int32 Index = 0; Index < Pixels.Num(); Index += 67)
                if (Pixels[Index] != First && Pixels[Index].R + Pixels[Index].G + Pixels[Index].B > 40) ++DistinctSamples;
            Test->TestTrue(TEXT("Captured UI is nonblank and visually varied"), DistinctSamples > 100);
            if (bMaterial)
            {
                int32 BorderMaterialPixels = 0;
                for (const FColor& Pixel : Pixels)
                    if (Pixel.R > 130 && Pixel.R > Pixel.G * 2 && Pixel.R > Pixel.B * 1.5f) ++BorderMaterialPixels;
                Test->TestTrue(TEXT("Native border material is visible in the captured output"), BorderMaterialPixels > 500);
                const auto NearRgb = [](FColor Actual, FColor Expected, int32 Tolerance)
                {
                    return FMath::Abs(int32(Actual.R) - Expected.R) <= Tolerance &&
                        FMath::Abs(int32(Actual.G) - Expected.G) <= Tolerance &&
                        FMath::Abs(int32(Actual.B) - Expected.B) <= Tolerance;
                };
                const FColor RoundedOutside = Pixels[54 * Size.X + 542];
                const FColor RoundedInside = Pixels[92 * Size.X + 580];
                Test->TestTrue(TEXT("UE material is absent outside the rounded CSS clip"),
                    NearRgb(RoundedOutside, FColor(24, 32, 42), 5));
                Test->TestTrue(TEXT("UE material remains visible inside the rounded CSS clip"),
                    RoundedInside.B > RoundedOutside.B + 35 && RoundedInside.G > RoundedOutside.G + 25);
            }
            if (!bMaterial)
            {
                int32 VisibleShadowPixels = 0;
                for (int32 Y = 90; Y < 350; ++Y)
                    for (int32 X = 574; X < 650; ++X)
                    {
                        const FColor Pixel = Pixels[Y * Size.X + X];
                        if (int32(Pixel.R) + int32(Pixel.G) + int32(Pixel.B) < 690) ++VisibleShadowPixels;
                    }
                Test->TestTrue(TEXT("Large non-zero CSS box-shadow is visible outside the card"), VisibleShadowPixels > 500);
            }
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            Test->TestTrue(TEXT("Save end-to-end screenshot"), FFileHelper::SaveArrayToFile(Png,
                *WebCompatArtifactPath(bMaterial ? TEXT("slate-native-engine-material.png") : TEXT("webcompat-bulma-card.png"))));
        }

        FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        SlateRoot.Reset();
        Window.Reset();
        Widget.Reset();
        return true;
    }

private:
    FAutomationTestBase* Test = nullptr;
    TStrongObjectPtr<URmlUiWebWidget> Widget;
    TSharedPtr<SWidget> SlateRoot;
    TSharedPtr<SWindow> Window;
    double Started = 0;
    bool bMaterial = false;
};

class FPixelParityCapture final : public IAutomationLatentCommand
{
public:
    FPixelParityCapture(FAutomationTestBase* InTest, bool bInSlate) : Test(InTest), bSlate(bInSlate) {}

    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"));
            Test->TestTrue(TEXT("Find WebCompat plugin for pixel parity"), Plugin.IsValid());
            if (!Plugin.IsValid()) return true;
            Widget.Reset(NewObject<URmlUiWebWidget>());
            Widget->DesiredSize = FVector2D(720, 420);
            Widget->bUseSlateRenderer = bSlate;
            Widget->DocumentPath = FPaths::Combine(Plugin->GetContentDir(), TEXT("RmlUi/Tests/pixel-parity.html"));
            SlateRoot = Widget->TakeWidget();
            Window = SNew(SWindow).Title(FText::FromString(bSlate ? TEXT("RmlUi Slate parity") : TEXT("RmlUi legacy parity")))
                .ClientSize(FVector2D(720, 420)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[SlateRoot.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Started = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Started < 2.0) return false;
        const TSharedPtr<SRmlUiWidget> SlateWidget = Widget->GetSlateRmlWidget();
        Test->TestTrue(TEXT("Pixel parity widget rendered frames"), SlateWidget.IsValid() && SlateWidget->GetFrameNumber() > 0);
        Test->TestTrue(TEXT("Pixel parity document rendered without errors"), SlateWidget.IsValid() && SlateWidget->GetLastError().IsEmpty());
        Test->TestEqual(TEXT("Pixel parity renderer selection"), SlateWidget.IsValid() && SlateWidget->IsUsingSlateRenderer(), bSlate);

        TSharedRef<FJsonObject> Layout = MakeShared<FJsonObject>();
        Layout->SetStringField(TEXT("renderer"), bSlate ? TEXT("slate") : TEXT("legacy"));
        TSharedRef<FJsonObject> Viewport = MakeShared<FJsonObject>();
        Viewport->SetNumberField(TEXT("width"), 720);
        Viewport->SetNumberField(TEXT("height"), 420);
        Layout->SetObjectField(TEXT("viewport"), Viewport);
        TSharedRef<FJsonObject> Elements = MakeShared<FJsonObject>();
        static const TCHAR* ElementIds[] = { TEXT("stage"), TEXT("panel"), TEXT("header"), TEXT("grid"), TEXT("tile-a"),
            TEXT("tile-b"), TEXT("tile-c"), TEXT("clip-inner"), TEXT("footer"), TEXT("status"), TEXT("action") };
        for (const TCHAR* Id : ElementIds)
        {
            RmlUE_Rect Rect{};
            const bool bFound = SlateWidget.IsValid() && RmlUE_GetElementRect(SlateWidget->GetNativeView(), TCHAR_TO_UTF8(Id), &Rect) != 0;
            Test->TestTrue(FString::Printf(TEXT("Pixel parity layout contains %s"), Id), bFound);
            if (!bFound) continue;
            TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
            Value->SetNumberField(TEXT("x"), Rect.X);
            Value->SetNumberField(TEXT("y"), Rect.Y);
            Value->SetNumberField(TEXT("width"), Rect.Width);
            Value->SetNumberField(TEXT("height"), Rect.Height);
            Elements->SetObjectField(Id, Value);
        }
        Layout->SetObjectField(TEXT("elements"), Elements);
        FString LayoutJson;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&LayoutJson);
        Test->TestTrue(TEXT("Serialize pixel parity layout"), FJsonSerializer::Serialize(Layout, Writer));
        Test->TestTrue(TEXT("Save pixel parity layout"), FFileHelper::SaveStringToFile(LayoutJson,
            *WebCompatArtifactPath(bSlate ? TEXT("pixel-parity-slate-layout.json") : TEXT("pixel-parity-legacy-layout.json"))));

        FlushRenderingCommands();
        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(SlateRoot.ToSharedRef(), Pixels, Size);
        Test->TestTrue(TEXT("Capture pixel parity output"), bCaptured && Size.X == 720 && Size.Y == 420 && Pixels.Num() == Size.X * Size.Y);
        if (bCaptured)
        {
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            Test->TestTrue(TEXT("Save pixel parity screenshot"), FFileHelper::SaveArrayToFile(Png,
                *WebCompatArtifactPath(bSlate ? TEXT("pixel-parity-slate.png") : TEXT("pixel-parity-legacy.png"))));
        }

        FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        SlateRoot.Reset();
        Window.Reset();
        Widget.Reset();
        return true;
    }

private:
    FAutomationTestBase* Test = nullptr;
    TStrongObjectPtr<URmlUiWebWidget> Widget;
    TSharedPtr<SWidget> SlateRoot;
    TSharedPtr<SWindow> Window;
    double Started = 0;
    bool bSlate = false;
};

class FMultiWidgetMaterialLifecycleCapture final : public IAutomationLatentCommand
{
public:
    explicit FMultiWidgetMaterialLifecycleCapture(FAutomationTestBase* InTest) : Test(InTest) {}

    virtual bool Update() override
    {
        if (!Window.IsValid())
        {
            const auto ConfigureWidget = [](URmlUiWebWidget* Widget, const TCHAR* Owner)
            {
                Widget->DesiredSize = FVector2D(320, 200);
                Widget->bUseSlateRenderer = true;
                Widget->InlineSourcePath = FString::Printf(TEXT("F://RmlUiTests/multi-%s.html"), Owner);
                Widget->InlineDocument = FString::Printf(TEXT(R"HTML(<html><head><style>
html, body { width: 100%%; height: 100%%; margin: 0; background: #101820; }
.panel { display: block; width: 100%%; height: 100%%; background: #202830; -rmlui-material: shared.panel; -rmlui-material-slot: background; }
</style></head><body><div class="panel" id="panel" data-owner="%s"></div></body></html>)HTML"), Owner);
            };
            FirstWidget.Reset(NewObject<URmlUiWebWidget>());
            SecondWidget.Reset(NewObject<URmlUiWebWidget>());
            ConfigureWidget(FirstWidget.Get(), TEXT("first"));
            ConfigureWidget(SecondWidget.Get(), TEXT("second"));
            UMaterial* RedMaterial = CreateNativeUiMaterial(FLinearColor(0.85f, 0.04f, 0.03f, 1.0f), 1.0f);
            UMaterial* GreenMaterial = CreateNativeUiMaterial(FLinearColor(0.03f, 0.72f, 0.12f, 1.0f), 1.0f);
            Test->TestTrue(TEXT("First widget registers the shared alias"),
                RedMaterial && FirstWidget->RegisterMaterial(TEXT("shared.panel"), RedMaterial));
            Test->TestTrue(TEXT("Second widget registers the shared alias"),
                GreenMaterial && SecondWidget->RegisterMaterial(TEXT("shared.panel"), GreenMaterial));
            FirstRoot = FirstWidget->TakeWidget();
            SecondRoot = SecondWidget->TakeWidget();
            SlateRoot = SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)[FirstRoot.ToSharedRef()]
                + SHorizontalBox::Slot().FillWidth(1.0f)[SecondRoot.ToSharedRef()];
            Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi multi-widget material lifecycle")))
                .ClientSize(FVector2D(640, 200)).UseOSWindowBorder(false).CreateTitleBar(false)
                .AutoCenter(EAutoCenter::None).ScreenPosition(FVector2D(0, 0))
                .AdjustInitialSizeAndPositionForDPIScale(false).SaneWindowPlacement(false)
                .SizingRule(ESizingRule::FixedSize).SupportsMaximize(false).SupportsMinimize(false)[SlateRoot.ToSharedRef()];
            FSlateApplication::Get().AddWindow(Window.ToSharedRef());
            Started = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Started < 2.0) return false;
        const TSharedPtr<SRmlUiWidget> FirstSlate = FirstWidget->GetSlateRmlWidget();
        const TSharedPtr<SRmlUiWidget> SecondSlate = SecondWidget->GetSlateRmlWidget();
        if (!Test->TestTrue(TEXT("Both native Slate widgets remain valid"), FirstSlate.IsValid() && SecondSlate.IsValid()))
            return Finish();
        FlushRenderingCommands();

        if (Stage == 0)
        {
            FirstOwnerId = RmlUE_GetViewResourceId(FirstSlate->GetNativeView());
            SecondOwnerId = RmlUE_GetViewResourceId(SecondSlate->GetNativeView());
            Test->TestTrue(TEXT("Material widgets use distinct View owners"),
                FirstOwnerId != 0 && SecondOwnerId != 0 && FirstOwnerId != SecondOwnerId);
            Test->TestTrue(TEXT("Both widgets resolve their local material alias"),
                FirstSlate->GetResolvedMaterialDrawCount() > 0 && SecondSlate->GetResolvedMaterialDrawCount() > 0);
            FString FirstOwner, SecondOwner;
            Test->TestTrue(TEXT("First material widget keeps independent DOM state"),
                FirstSlate->GetElementAttribute(TEXT("panel"), TEXT("data-owner"), FirstOwner) && FirstOwner == TEXT("first"));
            Test->TestTrue(TEXT("Second material widget keeps independent DOM state"),
                SecondSlate->GetElementAttribute(TEXT("panel"), TEXT("data-owner"), SecondOwner) && SecondOwner == TEXT("second"));
            OldFirstMaterial = FindOwnedMaterialObject(FirstOwnerId);
            SecondMaterial = FindOwnedMaterialObject(SecondOwnerId);
            Test->TestTrue(TEXT("Each material brush is owned by its Widget View"),
                OldFirstMaterial.IsValid() && SecondMaterial.IsValid() && OldFirstMaterial.Get() != SecondMaterial.Get());
            CaptureAndCheck(TEXT("slate-multi-widget-materials.png"), 0);

            UMaterial* BlueMaterial = CreateNativeUiMaterial(FLinearColor(0.03f, 0.16f, 0.90f, 1.0f), 1.0f);
            Test->TestTrue(TEXT("First widget hot-replaces only its local alias"),
                BlueMaterial && FirstWidget->RegisterMaterial(TEXT("shared.panel"), BlueMaterial));
            Stage = 1;
            Started = FPlatformTime::Seconds();
            return false;
        }

        if (Stage == 1)
        {
            FirstMaterial = FindOwnedMaterialObject(FirstOwnerId);
            Test->TestTrue(TEXT("Replacement installs a distinct MID under the same View owner"),
                FirstMaterial.IsValid() && FirstMaterial.Get() != OldFirstMaterial.Get());
            Test->TestTrue(TEXT("Peer Widget retains its original MID"),
                SecondMaterial.IsValid() && FindOwnedMaterialObject(SecondOwnerId).Get() == SecondMaterial.Get());
            CaptureAndCheck(TEXT("slate-multi-widget-material-replaced.png"), 1);
            CollectGarbage(RF_NoFlags, true);
            Test->TestFalse(TEXT("Replaced MID is collectable after render-thread release"), OldFirstMaterial.IsValid());

            FirstWidget->UnregisterMaterial(TEXT("shared.panel"));
            FirstSlate->ShutdownNative();
            FlushRenderingCommands();
            CollectGarbage(RF_NoFlags, true);
            Test->TestTrue(TEXT("First Widget releases its complete resource tree"),
                FRmlUiResourceRegistry::Get().SnapshotOwnedBy(FirstOwnerId, true).IsEmpty());
            Test->TestFalse(TEXT("Unregistered first MID is collectable"), FirstMaterial.IsValid());
            SecondFrameBeforePeerRelease = SecondSlate->GetFrameNumber();
            Stage = 2;
            Started = FPlatformTime::Seconds();
            return false;
        }

        Test->TestTrue(TEXT("Peer Widget keeps rendering after first Widget shutdown"),
            SecondSlate->GetFrameNumber() > SecondFrameBeforePeerRelease &&
            !FRmlUiResourceRegistry::Get().SnapshotOwnedBy(SecondOwnerId, true).IsEmpty());
        SecondWidget->UnregisterMaterial(TEXT("shared.panel"));
        SecondSlate->ShutdownNative();
        FlushRenderingCommands();
        CollectGarbage(RF_NoFlags, true);
        Test->TestTrue(TEXT("Second Widget releases its complete resource tree"),
            FRmlUiResourceRegistry::Get().SnapshotOwnedBy(SecondOwnerId, true).IsEmpty());
        Test->TestFalse(TEXT("Final MID is collectable after its Widget releases it"), SecondMaterial.IsValid());
        return Finish();
    }

private:
    void CaptureAndCheck(const TCHAR* Name, int32 ExpectedStage)
    {
        TArray<FColor> Pixels;
        FIntVector Size = FIntVector::ZeroValue;
        const bool bCaptured = FSlateApplication::Get().TakeScreenshot(SlateRoot.ToSharedRef(), Pixels, Size);
        Test->TestTrue(TEXT("Capture both material Widgets"),
            bCaptured && Size.X == 640 && Size.Y == 200 && Pixels.Num() == Size.X * Size.Y);
        if (!bCaptured || Pixels.Num() != Size.X * Size.Y) return;
        const FColor FirstPixel = Pixels[100 * Size.X + 160];
        const FColor SecondPixel = Pixels[100 * Size.X + 480];
        Test->TestTrue(ExpectedStage == 0 ? TEXT("First Widget uses its red material") : TEXT("First Widget uses its replacement blue material"),
            ExpectedStage == 0 ? FirstPixel.R > FirstPixel.G * 2 && FirstPixel.R > FirstPixel.B * 2
                               : FirstPixel.B > FirstPixel.R * 2 && FirstPixel.B > FirstPixel.G * 2);
        Test->TestTrue(TEXT("Second Widget keeps its green material"),
            SecondPixel.G > SecondPixel.R * 2 && SecondPixel.G > SecondPixel.B * 2);
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
            TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
        Test->TestTrue(TEXT("Save multi-Widget material screenshot"),
            FFileHelper::SaveArrayToFile(Png, *WebCompatArtifactPath(Name)));
    }

    bool Finish()
    {
        if (Window.IsValid()) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        SlateRoot.Reset();
        FirstRoot.Reset();
        SecondRoot.Reset();
        Window.Reset();
        FirstWidget.Reset();
        SecondWidget.Reset();
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    TStrongObjectPtr<URmlUiWebWidget> FirstWidget;
    TStrongObjectPtr<URmlUiWebWidget> SecondWidget;
    TSharedPtr<SWidget> FirstRoot;
    TSharedPtr<SWidget> SecondRoot;
    TSharedPtr<SWidget> SlateRoot;
    TSharedPtr<SWindow> Window;
    TWeakObjectPtr<UObject> OldFirstMaterial;
    TWeakObjectPtr<UObject> FirstMaterial;
    TWeakObjectPtr<UObject> SecondMaterial;
    uint64 FirstOwnerId = 0;
    uint64 SecondOwnerId = 0;
    uint64 SecondFrameBeforePeerRelease = 0;
    double Started = 0;
    int32 Stage = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiWebCompatCascadeTest, "RmlUiUnreal.WebCompat.ProfileCascade",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiWebCompatCascadeTest::RunTest(const FString& Parameters)
{
    FRmlUiWebCompatModule& Module = FRmlUiWebCompatModule::Get();
    RmlUE_StyleSheet* Profile = Module.FindProfile(TEXT("WebModernV1"));
    TestNotNull(TEXT("WebModernV1 is parsed during plugin startup"), Profile);
    if (!Profile) return false;

    RmlUE_View* View = RmlUE_CreateView(200, 120, 1.0f);
    TestNotNull(TEXT("Create compatibility test view"), View);
    if (!View) return false;
    TestTrue(TEXT("Attach cached profile"), RmlUE_SetBaseStyleSheet(View, Profile) != 0);

    const char* Markup =
        "<html><head><style>body { margin: 0; font-family: LatoLatin; } #a, #b { width: 40px; height: 20px; }</style></head>"
        "<body><div id='a'>a</div><div id='b'>b</div></body></html>";
    TestTrue(TEXT("Load compatible document"), RmlUE_LoadDocumentFromMemory(View, Markup, "compat-test.html") != 0);
    RmlUE_Rect A{}, B{};
    TestTrue(TEXT("Read compatible rectangles"),
        RmlUE_GetElementRect(View, "a", &A) != 0 && RmlUE_GetElementRect(View, "b", &B) != 0);
    TestTrue(TEXT("Profile supplies browser-like block flow"), B.Y > A.Y + 10.0f);

    const char* OverrideMarkup =
        "<html><head><style>body { margin: 0; font-family: LatoLatin; } div { display: inline; }</style></head>"
        "<body><div id='a'>a</div><div id='b'>b</div></body></html>";
    TestTrue(TEXT("Load author override document"),
        RmlUE_LoadDocumentFromMemory(View, OverrideMarkup, "compat-override.html") != 0);
    TestTrue(TEXT("Read overridden rectangles"),
        RmlUE_GetElementRect(View, "a", &A) != 0 && RmlUE_GetElementRect(View, "b", &B) != 0);
    TestTrue(TEXT("Author CSS overrides the compatibility profile"), FMath::Abs(B.Y - A.Y) < 1.0f);

    TestTrue(TEXT("Raw mode detaches the profile"), RmlUE_SetBaseStyleSheet(View, nullptr) != 0);
    RmlUE_DestroyView(View);

    const FName CompilerId = Module.GetDocumentCompilerId();
    TestTrue(TEXT("A dynamic compiler is registered without coupling WebCompat to its provider"),
        CompilerId == TEXT("PuertsRuntimeV1") || CompilerId == TEXT("PostCssEditorV1"));
    Module.ClearCompiledDocumentCache();
    const int32 CompileCountBefore = Module.GetDynamicCompileCount();
    const int32 CacheHitsBefore = Module.GetDynamicCacheHitCount();
    const FString DynamicMarkup = TEXT("<html><head><style>.magictime { animation-duration: 1s; } .puffIn { animation-name: puffIn; } @keyframes puffIn { from { opacity: 0; } to { opacity: 1; } }</style></head><body><div class='magictime puffIn'/></body></html>");
    FString CompiledMarkup;
    FString Diagnostics;
    bool bCacheHit = true;
    TestTrue(TEXT("Compile LLM-style inline document once"),
        Module.CompileDynamicDocument(DynamicMarkup, TEXT("llm-response.html"), CompiledMarkup, Diagnostics, bCacheHit));
    TestFalse(TEXT("First dynamic compilation is not cached"), bCacheHit);
    TestTrue(TEXT("Animation longhands became an RmlUi shorthand"), CompiledMarkup.Contains(TEXT("animation: 1s cubic-out puffIn")));
    TestEqual(TEXT("Compiler invocation count increments once"), Module.GetDynamicCompileCount(), CompileCountBefore + 1);

    FString CachedMarkup;
    FString CachedDiagnostics;
    TestTrue(TEXT("Compile identical document from cache"),
        Module.CompileDynamicDocument(DynamicMarkup, TEXT("llm-response.html"), CachedMarkup, CachedDiagnostics, bCacheHit));
    TestTrue(TEXT("Second dynamic compilation is a cache hit"), bCacheHit);
    TestEqual(TEXT("Cached document matches compiler output"), CachedMarkup, CompiledMarkup);
    TestEqual(TEXT("Cache hit count increments once"), Module.GetDynamicCacheHitCount(), CacheHitsBefore + 1);
    TestEqual(TEXT("Cache hit does not invoke compiler"), Module.GetDynamicCompileCount(), CompileCountBefore + 1);

    RmlUE_View* DynamicView = RmlUE_CreateView(200, 120, 1.0f);
    FTCHARToUTF8 CompiledUtf8(*CompiledMarkup);
    TestTrue(TEXT("RmlUi parses dynamically compiled markup"), DynamicView &&
        RmlUE_LoadDocumentFromMemory(DynamicView, CompiledUtf8.Get(), "dynamic-compat.html") != 0);
    if (DynamicView) RmlUE_DestroyView(DynamicView);

    const FString UnsupportedMarkup = TEXT("<html><head><style>.x { transition-property: transform; transition-timing-function: cubic-bezier(0, 1, 1, 0); }</style></head><body/></html>");
    AddExpectedError(TEXT("Dynamic document compilation failed: WebCompat compilation failed:"),
        EAutomationExpectedErrorFlags::Contains, 1);
    TestFalse(TEXT("Unsupported browser CSS fails atomically"),
        Module.CompileDynamicDocument(UnsupportedMarkup, TEXT("unsupported.html"), CompiledMarkup, Diagnostics, bCacheHit));
    TestTrue(TEXT("Unsupported CSS returns a useful diagnostic"), Diagnostics.Contains(TEXT("unsupported-timing-function")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiEngineMaterialE2ETest, "RmlUiUnreal.WebCompat.EngineMaterialE2E",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiEngineMaterialE2ETest::RunTest(const FString&)
{
    AddCommand(new FWebCompatVisualCapture(this, true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiMultiWidgetMaterialLifecycleE2ETest,
    "RmlUiUnreal.WebCompat.MultiWidgetMaterialLifecycleE2E",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiMultiWidgetMaterialLifecycleE2ETest::RunTest(const FString&)
{
    AddCommand(new FMultiWidgetMaterialLifecycleCapture(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiBulmaCardE2ETest, "RmlUiUnreal.WebCompat.BulmaCardE2E",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiBulmaCardE2ETest::RunTest(const FString&)
{
    AddCommand(new FWebCompatVisualCapture(this, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiPixelParityLegacyE2ETest, "RmlUiUnreal.WebCompat.PixelParityLegacyE2E",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiPixelParityLegacyE2ETest::RunTest(const FString&)
{
    AddCommand(new FPixelParityCapture(this, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiPixelParitySlateE2ETest, "RmlUiUnreal.WebCompat.PixelParitySlateE2E",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiPixelParitySlateE2ETest::RunTest(const FString&)
{
    AddCommand(new FPixelParityCapture(this, true));
    return true;
}

#endif
