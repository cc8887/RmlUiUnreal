#include "RmlUiJSRuntime.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RmlUiBridge.h"
#include "RmlUiWidget.h"
#include "SRmlUiWidget.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"
#include <bcrypt.h>

namespace {
bool ParseJson(const FString& Text, TSharedPtr<FJsonObject>& Json)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid();
}
bool RelativeFile(const FString& Path)
{
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":"))) return false;
    TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
    for (const FString& Part : Parts) {
        if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..")) return false;
        for (TCHAR C : Part) if (!FChar::IsAlnum(C) && C != '_' && C != '-' && C != '.') return false;
    }
    return true;
}
bool DigestMatches(const TArray<uint8>& Bytes, const FString& Expected)
{
    if (Expected.Len() != 64) return false;
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    uint8 Hash[32]{};
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    const NTSTATUS Status = BCryptHash(Algorithm, nullptr, 0, const_cast<uint8*>(Bytes.GetData()), Bytes.Num(), Hash, sizeof(Hash));
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Status >= 0 && BytesToHex(Hash, sizeof(Hash)).Equals(Expected, ESearchCase::IgnoreCase);
}
struct FVueVersion
{
    FString Path, Directory, Version, Entry, Document;
    TSharedPtr<FJsonObject> Manifest;
    bool Read(const FString& Input, FString& Error)
    {
        Path = FPaths::ConvertRelativePathToFull(Input);
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path) || !ParseJson(Text, Manifest)) { Error = TEXT("Cannot read UI manifest: ") + Path; return false; }
        FString Pointer;
        if (Manifest->TryGetStringField(TEXT("manifest"), Pointer)) {
            if (!RelativeFile(Pointer)) { Error = TEXT("Invalid UI manifest pointer."); return false; }
            Path = FPaths::Combine(FPaths::GetPath(Path), Pointer);
            if (!FFileHelper::LoadFileToString(Text, *Path) || !ParseJson(Text, Manifest)) { Error = TEXT("Cannot read version manifest."); return false; }
        }
        Directory = FPaths::GetPath(Path);
        int32 Format = 0, Abi = 0, Schema = 0;
        const TSharedPtr<FJsonObject>* Files = nullptr;
        if (!Manifest->TryGetNumberField(TEXT("format"), Format) || Format != 1 ||
            !Manifest->TryGetNumberField(TEXT("abi"), Abi) || Abi != 1 ||
            !Manifest->TryGetNumberField(TEXT("stateSchema"), Schema) || Schema != 1 ||
            !Manifest->TryGetStringField(TEXT("version"), Version) || !RelativeFile(Version) || Version.Contains(TEXT("/")) ||
            !Manifest->TryGetStringField(TEXT("entry"), Entry) || !RelativeFile(Entry) ||
            !Manifest->TryGetStringField(TEXT("document"), Document) || !RelativeFile(Document) ||
            !Manifest->TryGetObjectField(TEXT("files"), Files) || !(*Files)->HasField(Entry) || !(*Files)->HasField(Document)) {
            Error = TEXT("Unsupported or incomplete UI manifest (format/ABI/state schema must be 1)."); return false;
        }
        int64 Total = 0;
        for (const auto& Pair : (*Files)->Values) {
            const FString Name(Pair.Key);
            FString Expected; TArray<uint8> Bytes;
            if (!RelativeFile(Name) || !Pair.Value->TryGetString(Expected) ||
                !FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, Name)) ||
                Bytes.Num() > 16 * 1024 * 1024 || !DigestMatches(Bytes, Expected)) {
                Error = TEXT("UI resource is missing, too large or has a SHA-256 mismatch: ") + Name; return false;
            }
            Total += Bytes.Num();
            if (Total > 64 * 1024 * 1024) { Error = TEXT("UI version exceeds 64 MiB."); return false; }
        }
        return true;
    }
};
}

FString URmlUiJSRuntime::DefaultManifestPath()
{
    return FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(), TEXT("Vue/current.json"));
}
bool URmlUiJSRuntime::Fail(const FString& Message)
{
    LastError = Message;
    UE_LOG(LogTemp, Warning, TEXT("RmlUiJS update: %s"), *Message);
    OnStatus.Broadcast(Message);
    return false;
}
bool URmlUiJSRuntime::Start(URmlUiWidget* Widget, const FString& ManifestPath, bool bWatch)
{
    check(IsInGameThread());
    Stop();
    JsonHostRequestCount = 0;
    if (!Widget) return Fail(TEXT("A RmlUi widget is required."));
    Target = Widget;
    Widget->TakeWidget();
    SlateWidget = Widget->GetSlateRmlWidget();
    WatchedManifest = ManifestPath.IsEmpty() ? DefaultManifestPath() : ManifestPath;
    FFileHelper::LoadFileToString(WatchedText, *WatchedManifest);
    bWatchFiles = bWatch;
    auto Slate = SlateWidget.Pin();
    if (!Slate || !Activate(WatchedManifest)) { Target = nullptr; SlateWidget.Reset(); return false; }
    FrameHandle = Slate->OnBeforeRender.AddUObject(this, &URmlUiJSRuntime::Advance);
    ShutdownHandle = Slate->OnNativeShutdown.AddUObject(this, &URmlUiJSRuntime::Stop);
    return true;
}
bool URmlUiJSRuntime::RegisterService(const FString& Name, UObject* Service)
{
    if (Active || Candidate) return Fail(TEXT("Services must be registered before Start."));
    if (!Service || Name.IsEmpty() || Name == TEXT("bridge") || (!FChar::IsAlpha(Name[0]) && Name[0] != '_'))
        return Fail(TEXT("A service requires a valid UObject and a non-reserved JavaScript identifier."));
    for (const TCHAR Character : Name)
        if (!FChar::IsAlnum(Character) && Character != '_')
            return Fail(TEXT("Service names may contain only letters, digits and underscores."));
    Services.Add(Name, Service);
    return true;
}
bool URmlUiJSRuntime::UnregisterService(const FString& Name)
{
    if (Active || Candidate) return Fail(TEXT("Services cannot change while the runtime is active."));
    return Services.Remove(Name) > 0;
}
void URmlUiJSRuntime::ClearServices()
{
    if (Active || Candidate) { Fail(TEXT("Services cannot change while the runtime is active.")); return; }
    Services.Empty();
}
bool URmlUiJSRuntime::Activate(const FString& Path)
{
    auto Slate = SlateWidget.Pin();
    if (!Slate) return Fail(TEXT("RmlUi widget has been destroyed."));
    FVueVersion Version;
    FString Error;
    if (!Version.Read(Path, Error)) return Fail(Error);
    FString State = Active ? Active->CaptureState() : TEXT("{}");
    RmlUE_View* NewView = RmlUE_CreateView(1280, 800, 1);
    if (!NewView) return Fail(UTF8_TO_TCHAR(RmlUE_GetLastError()));
    const TArray<TSharedPtr<FJsonValue>>* Fonts = nullptr;
    if (Version.Manifest->TryGetArrayField(TEXT("fonts"), Fonts)) {
        static TSet<FString> LoadedFontHashes;
        const auto Files = Version.Manifest->GetObjectField(TEXT("files"));
        for (const auto& FontValue : *Fonts) {
            FString Font, Hash;
            if (!FontValue->TryGetString(Font) || !RelativeFile(Font) || !Files->TryGetStringField(Font, Hash)) { RmlUE_DestroyView(NewView); return Fail(TEXT("Invalid manifest font.")); }
            if (!LoadedFontHashes.Contains(Hash)) {
                if (!RmlUE_LoadFont(TCHAR_TO_UTF8(*FPaths::Combine(Version.Directory, Font)), 1)) { RmlUE_DestroyView(NewView); return Fail(TEXT("Cannot load manifest font: ") + Font); }
                LoadedFontHashes.Add(Hash);
            }
        }
    }
    if (!RmlUE_LoadDocument(NewView, TCHAR_TO_UTF8(*FPaths::Combine(Version.Directory, Version.Document)))) {
        Error = UTF8_TO_TCHAR(RmlUE_GetLastError());
        RmlUE_DestroyView(NewView); return Fail(Error);
    }
    Candidate = NewObject<URmlUiJSContext>(this);
    TArray<TPair<FString, UObject*>> ServiceArguments;
    ServiceArguments.Reserve(Services.Num());
    for (const auto& Pair : Services) {
        if (!IsValid(Pair.Value)) { RmlUE_DestroyView(NewView); Candidate = nullptr; return Fail(TEXT("A registered JavaScript service is no longer valid: ") + Pair.Key); }
        ServiceArguments.Emplace(Pair.Key, Pair.Value.Get());
    }
    // Keep the debugger on a unique port while both old and candidate VMs exist.
    const int32 CandidatePort = DebugPort < 0 ? -1 : DebugPort + ((ReloadCount + 1) % 2);
    if (!Candidate->Initialize(NewView, Version.Directory, Version.Entry, Version.Version, State, CandidatePort, ServiceArguments)) {
        Error = Candidate->LastError;
        Candidate->Dispose(); Candidate = nullptr; RmlUE_DestroyView(NewView);
        return Fail(TEXT("Candidate UI rejected; previous page retained. ") + Error);
    }
    RmlUE_Update(NewView);
    RmlUE_View* OldView = Slate->ExchangeNativeView(NewView);
    if (Active) Active->Dispose();
    if (OldView) RmlUE_DestroyView(OldView);
    Active = Candidate; Candidate = nullptr;
    HostRequests.Empty();
    Active->OnHostRequest.AddDynamic(this, &URmlUiJSRuntime::ForwardHostRequest);
    PreviousManifest = ActiveManifest;
    ActiveManifest = Version.Path;
    ActiveVersion = Version.Version;
    ++ReloadCount;
    LastError.Reset();
    Active->ActivateHostRequests();
    UE_LOG(LogTemp, Display, TEXT("RmlUiJS activated %s"), *ActiveVersion);
    OnStatus.Broadcast(TEXT("Activated ") + ActiveVersion);
    return true;
}
void URmlUiJSRuntime::Advance(float DeltaSeconds)
{
    if (bAdvancing) return;
    TGuardValue<bool> Guard(bAdvancing, true);
    if (!bDownloading && !Requests.IsEmpty()) {
        // Cancel remaining siblings after a failed download, then release completed requests.
        ++DownloadGeneration;
        auto FinishedRequests = MoveTemp(Requests);
        Requests.Reset();
        for (auto& Request : FinishedRequests) if (Request) Request->CancelRequest();
    }
    const double Now = FPlatformTime::Seconds();
    if (bWatchFiles && Now >= NextWatchTime) {
        NextWatchTime = Now + 0.25;
        FString Text;
        if (FFileHelper::LoadFileToString(Text, *WatchedManifest) && Text != WatchedText) {
            WatchedText = Text; PendingManifest = WatchedManifest;
        }
    }
    if (!PendingManifest.IsEmpty()) {
        FString Path = MoveTemp(PendingManifest); PendingManifest.Reset(); Activate(Path);
    }
    if (Active) Active->Advance(DeltaSeconds);
}
void URmlUiJSRuntime::Reload() { PendingManifest = WatchedManifest; }
void URmlUiJSRuntime::LoadVersion(const FString& Path) { PendingManifest = Path; }
void URmlUiJSRuntime::Rollback() { if (!PreviousManifest.IsEmpty()) PendingManifest = PreviousManifest; }
void URmlUiJSRuntime::ForwardHostRequest(int32 RequestId, const FString& Method, const FString& Json)
{
    if (!Active) return;
    ++JsonHostRequestCount;
    const int32 GlobalId = ++NextHostRequest;
    HostRequests.Add(GlobalId, {Active, RequestId});
    if (OnHostRequest.IsBound()) OnHostRequest.Broadcast(GlobalId, Method, Json);
    else ResolveHostRequest(GlobalId, TEXT("No host handler registered"), false);
}
void URmlUiJSRuntime::ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess)
{
    FHostRequest Request;
    if (HostRequests.RemoveAndCopyValue(RequestId, Request) && Request.Context.IsValid())
        Request.Context->ResolveHostRequest(Request.LocalId, Json, bSuccess);
}
void URmlUiJSRuntime::Stop()
{
    ++DownloadGeneration;
    for (auto& Request : Requests) if (Request) Request->CancelRequest();
    Requests.Empty(); bDownloading = false;
    HostRequests.Empty();
    if (auto Slate = SlateWidget.Pin()) {
        Slate->OnBeforeRender.Remove(FrameHandle);
        Slate->OnNativeShutdown.Remove(ShutdownHandle);
    }
    if (Active) Active->Dispose();
    if (Candidate) Candidate->Dispose();
    Active = nullptr; Candidate = nullptr; Target = nullptr;
    SlateWidget.Reset(); PendingManifest.Reset();
}
void URmlUiJSRuntime::BeginDestroy() { Stop(); Super::BeginDestroy(); }

void URmlUiJSRuntime::FetchUpdate(const FString& Url)
{
    if (!Url.StartsWith(TEXT("https://")) && !Url.StartsWith(TEXT("http://127.0.0.1:")) && !Url.StartsWith(TEXT("http://localhost:"))) {
        Fail(TEXT("Update URLs require HTTPS or a localhost development server.")); return;
    }
    if (bDownloading) { Fail(TEXT("An update is already downloading.")); return; }
    bDownloading = true;
    const uint32 Generation = ++DownloadGeneration;
    auto Request = FHttpModule::Get().CreateRequest();
    Requests.Add(Request);
    Request->SetURL(Url); Request->SetVerb(TEXT("GET")); Request->SetTimeout(30);
    Request->OnProcessRequestComplete().BindWeakLambda(this,
        [this, Url, Generation](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess) {
        if (Generation != DownloadGeneration) return;
        TSharedPtr<FJsonObject> Json;
        const TSharedPtr<FJsonObject>* Files = nullptr;
        FString Version;
        if (!bSuccess || !Response || Response->GetResponseCode() != 200 || Response->GetContentLength() > 1024 * 1024 ||
            !ParseJson(Response->GetContentAsString(), Json) || !Json->TryGetObjectField(TEXT("files"), Files) ||
            !Json->TryGetStringField(TEXT("version"), Version) || !RelativeFile(Version) || Version.Contains(TEXT("/")) ||
            (*Files)->Values.IsEmpty() || (*Files)->Values.Num() > 128) {
            bDownloading = false; Fail(TEXT("Cannot download a valid version manifest.")); return;
        }
        struct FDownloadState {
            FString Directory, ManifestText;
            int32 Remaining = 0;
            int64 TotalBytes = 0;
            bool Failed = false;
        };
        auto State = MakeShared<FDownloadState>();
        // A unique directory keeps downloads isolated from active/previous versions.
        State->Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiVersions"), Version + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        State->ManifestText = Response->GetContentAsString();
        State->Remaining = (*Files)->Values.Num();
        const FString BaseUrl = Url.Left(Url.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
        for (const auto& Pair : (*Files)->Values) {
            const FString Name(Pair.Key);
            FString Hash;
            if (!RelativeFile(Name) || !Pair.Value->TryGetString(Hash) || Hash.Len() != 64) {
                bDownloading = false; State->Failed = true; Fail(TEXT("Invalid update file path or hash.")); return;
            }
            auto FileRequest = FHttpModule::Get().CreateRequest(); Requests.Add(FileRequest);
            FileRequest->SetURL(BaseUrl + Name); FileRequest->SetVerb(TEXT("GET")); FileRequest->SetTimeout(30);
            FileRequest->OnProcessRequestComplete().BindWeakLambda(this,
                [this, State, Name, Hash, Generation](FHttpRequestPtr, FHttpResponsePtr FileResponse, bool bOk) {
                if (Generation != DownloadGeneration || State->Failed) return;
                if (!bOk || !FileResponse || FileResponse->GetResponseCode() != 200 || FileResponse->GetContent().Num() > 16 * 1024 * 1024 ||
                    !DigestMatches(FileResponse->GetContent(), Hash)) {
                    State->Failed = true; bDownloading = false; Fail(TEXT("Downloaded UI resource failed validation: ") + Name); return;
                }
                State->TotalBytes += FileResponse->GetContent().Num();
                const FString Output = FPaths::Combine(State->Directory, Name);
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
                if (State->TotalBytes > 64 * 1024 * 1024 || !FFileHelper::SaveArrayToFile(FileResponse->GetContent(), *Output)) {
                    State->Failed = true; bDownloading = false; Fail(TEXT("Cannot stage downloaded UI resources.")); return;
                }
                if (--State->Remaining == 0) {
                    const FString Manifest = FPaths::Combine(State->Directory, TEXT("manifest.json"));
                    bDownloading = false;
                    if (!FFileHelper::SaveStringToFile(State->ManifestText, *Manifest, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) {
                        Fail(TEXT("Cannot publish downloaded UI manifest.")); return;
                    }
                    PendingManifest = Manifest;
                }
            });
            if (!FileRequest->ProcessRequest()) { State->Failed = true; bDownloading = false; Fail(TEXT("Cannot start resource download.")); return; }
        }
    });
    if (!Request->ProcessRequest()) { bDownloading = false; Fail(TEXT("Cannot start manifest download.")); }
}
