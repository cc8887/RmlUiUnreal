#include "RmlUiUnrealModule.h"

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RmlUiBridge.h"
#include "SRmlUiWidget.h"

DEFINE_LOG_CATEGORY(LogRmlUiUnreal);
IMPLEMENT_MODULE(FRmlUiUnrealModule, RmlUiUnreal)

namespace
{
int ReadFile(void*, const char* Path, unsigned char** Data, size_t* Size)
{
    TArray<uint8> Bytes;
    if (!Path || !Data || !Size || !FFileHelper::LoadFileToArray(Bytes, UTF8_TO_TCHAR(Path)))
    {
        return 0;
    }
    *Size = static_cast<size_t>(Bytes.Num());
    *Data = static_cast<unsigned char*>(FMemory::Malloc(FMath::Max<size_t>(*Size, 1)));
    FMemory::Memcpy(*Data, Bytes.GetData(), *Size);
    return 1;
}

int LoadImage(void*, const char* Path, unsigned char** Data, int* Width, int* Height)
{
    TArray<uint8> Compressed;
    if (!Path || !Data || !Width || !Height || !FFileHelper::LoadFileToArray(Compressed, UTF8_TO_TCHAR(Path)))
    {
        return 0;
    }
    IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const EImageFormat Format = ImageModule.DetectImageFormat(Compressed.GetData(), Compressed.Num());
    if (Format == EImageFormat::Invalid)
    {
        return 0;
    }
    TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(Format);
    TArray64<uint8> Pixels;
    if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()) ||
        Wrapper->GetWidth() <= 0 || Wrapper->GetHeight() <= 0 ||
        Wrapper->GetWidth() > 16384 || Wrapper->GetHeight() > 16384 || !Wrapper->GetRaw(ERGBFormat::RGBA, 8, Pixels))
    {
        return 0;
    }
    *Width = Wrapper->GetWidth();
    *Height = Wrapper->GetHeight();
    *Data = static_cast<unsigned char*>(FMemory::Malloc(Pixels.Num()));
    FMemory::Memcpy(*Data, Pixels.GetData(), Pixels.Num());
    return 1;
}

void FreeBuffer(void*, void* Data)
{
    FMemory::Free(Data);
}

void NativeLog(void*, int Level, const char* Message)
{
    if (Level <= 1)
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("%s"), UTF8_TO_TCHAR(Message ? Message : ""));
    }
    else
    {
        UE_LOG(LogRmlUiUnreal, Log, TEXT("%s"), UTF8_TO_TCHAR(Message ? Message : ""));
    }
}

void SetClipboard(void*, const char* Text)
{
    FPlatformApplicationMisc::ClipboardCopy(UTF8_TO_TCHAR(Text ? Text : ""));
}

int GetClipboard(void*, unsigned char** Data, size_t* Size)
{
    FString Clipboard;
    FPlatformApplicationMisc::ClipboardPaste(Clipboard);
    FTCHARToUTF8 Utf8(*Clipboard);
    *Size = Utf8.Length();
    *Data = static_cast<unsigned char*>(FMemory::Malloc(*Size + 1));
    FMemory::Memcpy(*Data, Utf8.Get(), *Size + 1);
    return 1;
}
}

FRmlUiUnrealModule& FRmlUiUnrealModule::Get()
{
    return FModuleManager::LoadModuleChecked<FRmlUiUnrealModule>(TEXT("RmlUiUnreal"));
}

void FRmlUiUnrealModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"));
    if (!Plugin)
    {
        InitializationError = TEXT("RmlUiUnreal plugin directory was not found.");
        return;
    }
    PluginRoot = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
    const FString DllPath = FPaths::Combine(PluginRoot, TEXT("Binaries/ThirdParty/Win64/RmlUiBridge.dll"));
    BridgeDll = FPlatformProcess::GetDllHandle(*DllPath);
    if (!BridgeDll)
    {
        InitializationError = FString::Printf(TEXT("Unable to load %s. Run the project's Build.ps1 or Source/ThirdParty/RmlUiBridge/BuildBridge.ps1 first."), *DllPath);
        UE_LOG(LogRmlUiUnreal, Error, TEXT("%s"), *InitializationError);
        return;
    }
    RmlUE_Host Host{};
    Host.ReadFile = ReadFile;
    Host.LoadImage = LoadImage;
    Host.FreeBuffer = FreeBuffer;
    Host.Log = NativeLog;
    Host.SetClipboard = SetClipboard;
    Host.GetClipboard = GetClipboard;
    bInitialized = RmlUE_Initialize(&Host) != 0;
    if (!bInitialized)
    {
        InitializationError = UTF8_TO_TCHAR(RmlUE_GetLastError());
        UE_LOG(LogRmlUiUnreal, Error, TEXT("RmlUi initialization failed: %s"), *InitializationError);
        return;
    }
    for (const TCHAR* Font : { TEXT("LatoLatin-Regular.ttf"), TEXT("LatoLatin-Bold.ttf"), TEXT("RobotoMono-Regular.ttf") })
    {
        const FString FontPath = FPaths::Combine(GetContentRoot(), TEXT("Fonts"), Font);
        if (!RmlUE_LoadFont(TCHAR_TO_UTF8(*FontPath), 0))
        {
            UE_LOG(LogRmlUiUnreal, Warning, TEXT("Could not load font %s: %s"), *FontPath, UTF8_TO_TCHAR(RmlUE_GetLastError()));
        }
    }
    UE_LOG(LogRmlUiUnreal, Display, TEXT("Initialized RmlUi %s"), UTF8_TO_TCHAR(RmlUE_GetVersion()));
}

void FRmlUiUnrealModule::ShutdownModule()
{
    for (const TWeakPtr<SRmlUiWidget>& WeakWidget : Widgets)
    {
        if (const TSharedPtr<SRmlUiWidget> Widget = WeakWidget.Pin())
        {
            Widget->ShutdownNative();
        }
    }
    Widgets.Empty();
    if (bInitialized)
    {
        RmlUE_Shutdown();
        bInitialized = false;
    }
    if (BridgeDll)
    {
        FPlatformProcess::FreeDllHandle(BridgeDll);
        BridgeDll = nullptr;
    }
}

FString FRmlUiUnrealModule::GetContentRoot() const
{
    return FPaths::Combine(PluginRoot, TEXT("Content/RmlUi"));
}

FString FRmlUiUnrealModule::GetDefaultDocumentPath() const
{
    return FPaths::Combine(GetContentRoot(), TEXT("Demo.rml"));
}

FString FRmlUiUnrealModule::ResolveDocumentPath(const FString& Path) const
{
    if (Path.IsEmpty())
    {
        return GetDefaultDocumentPath();
    }
    if (!FPaths::IsRelative(Path))
    {
        return Path;
    }
    const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("RmlUi"), Path));
    return FPaths::FileExists(ProjectPath) ? ProjectPath : FPaths::Combine(GetContentRoot(), Path);
}

void FRmlUiUnrealModule::RegisterWidget(const TSharedRef<SRmlUiWidget>& Widget)
{
    Widgets.RemoveAll([](const TWeakPtr<SRmlUiWidget>& Item) { return !Item.IsValid(); });
    Widgets.Add(Widget);
}
