#include "RmlUiJSRuntime.h"

#include "RmlUiCssAnimationSession.h"

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
#include "RmlUiPerformance.h"
#include "RmlUiWidget.h"
#include "SRmlUiWidget.h"
#include "Serialization/JsonSerializer.h"
#include "Windows/WindowsHWrapper.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RmlUiCapabilityCatalog.inl"
#include "Misc/AutomationTest.h"
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
    BCRYPT_HASH_HANDLE HashHandle = nullptr;
    uint8 Hash[32]{};
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    ULONG ObjectLength = 0;
    ULONG ResultLength = 0;
    NTSTATUS Status = BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&ObjectLength), sizeof(ObjectLength), &ResultLength, 0);
    TArray<uint8> HashObject;
    if (Status >= 0) {
        HashObject.SetNumUninitialized(ObjectLength);
        Status = BCryptCreateHash(Algorithm, &HashHandle, HashObject.GetData(), ObjectLength, nullptr, 0, 0);
    }
    if (Status >= 0) {
        Status = BCryptHashData(HashHandle, const_cast<PUCHAR>(Bytes.GetData()), static_cast<ULONG>(Bytes.Num()), 0);
    }
    if (Status >= 0) Status = BCryptFinishHash(HashHandle, Hash, sizeof(Hash), 0);
    if (HashHandle) BCryptDestroyHash(HashHandle);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Status >= 0 && BytesToHex(Hash, sizeof(Hash)).Equals(Expected, ESearchCase::IgnoreCase);
}
struct FVueVersion
{
    FString Path, Directory, Version, Entry, Document, MotionManifest;
    TSharedPtr<FJsonObject> Manifest;
    bool bStrictCapabilities = false;
    bool ValidateCapabilities(bool bSlate, FString& Error)
    {
        // Existing ABI-1 bundles predate capability declarations and retain their
        // original behaviour. New bundles must declare an executable profile.
        bStrictCapabilities = false;
        if (!Manifest->HasField(TEXT("capabilities"))) return true;
        const TSharedPtr<FJsonObject>* Capabilities = nullptr;
        auto Reject = [&Error](const FString& Reason) {
            Error = TEXT("UI capability rejection; previous page retained. ") + Reason; return false;
        };
        if (!Manifest->TryGetObjectField(TEXT("capabilities"), Capabilities)) return Reject(TEXT("Invalid capabilities object."));
        const auto& C = **Capabilities;
        double Schema = 0, HostAbi = 0, SlateAbi = 0;
        FString Profile, Compiler, Diagnostics;
        const TArray<TSharedPtr<FJsonValue>> *Required = nullptr, *Degraded = nullptr;
        const TSharedPtr<FJsonObject>* Files = nullptr;
        if (!C.TryGetNumberField(TEXT("schemaVersion"), Schema) || Schema != RmlUiCapabilitySchemaVersion ||
            !C.TryGetStringField(TEXT("compiler"), Compiler) || Compiler != TEXT("rmlui-css/2.0.0") ||
            !C.TryGetNumberField(TEXT("minimumHostAbi"), HostAbi) || HostAbi < RmlUiMinimumHostAbi || HostAbi != FMath::FloorToDouble(HostAbi) || HostAbi > RmlUE_GetHostAbiVersion() ||
            !C.TryGetStringField(TEXT("profile"), Profile) ||
            !C.TryGetArrayField(TEXT("requiredFeatures"), Required) || !C.TryGetArrayField(TEXT("degradedFeatures"), Degraded) ||
            !C.TryGetStringField(TEXT("diagnostics"), Diagnostics) || !RelativeFile(Diagnostics) ||
            !Manifest->TryGetObjectField(TEXT("files"), Files) || !(*Files)->HasField(Diagnostics))
            return Reject(TEXT("Unsupported schema/compiler/Host ABI or missing hashed diagnostics."));
        if (Profile != (bSlate ? TEXT("slate-rhi") : TEXT("dx11-compat")))
            return Reject(TEXT("Renderer profile does not match this View: ") + Profile);
        if (bSlate && (!C.TryGetNumberField(TEXT("minimumSlateAbi"), SlateAbi) || SlateAbi < RmlUiMinimumSlateAbi ||
            SlateAbi != FMath::FloorToDouble(SlateAbi) || SlateAbi > RMLUE_SLATE_ABI_VERSION))
            return Reject(TEXT("Unsupported Slate ABI."));
        TSet<FString> RequiredSet;
        for (const auto& Value : *Required) {
            FString Feature;
            if (!Value->TryGetString(Feature) || !RmlUiProfileHasFeature(Profile, Feature) || RequiredSet.Contains(Feature))
                return Reject(TEXT("Required feature is unavailable: ") + Feature);
            RequiredSet.Add(Feature);
        }
        TSet<FString> DegradedSet;
        for (const auto& Value : *Degraded) {
            FString Feature;
            if (!Value->TryGetString(Feature) || !RmlUiKnownFeature(Feature) || RequiredSet.Contains(Feature) || DegradedSet.Contains(Feature))
                return Reject(TEXT("Invalid or contradictory degraded feature: ") + Feature);
            DegradedSet.Add(Feature);
        }
        bStrictCapabilities = true;
        return true;
    }
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
        if (Manifest->HasField(TEXT("motionManifest")) &&
            (!Manifest->TryGetStringField(TEXT("motionManifest"), MotionManifest) ||
                !RelativeFile(MotionManifest) || !(*Files)->HasField(MotionManifest))) {
            Error = TEXT("UI motion manifest is missing from hashed resources."); return false;
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

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiCapabilityManifestTest, "RmlUiUnreal.JS.CapabilityManifest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlUiCapabilityManifestTest::RunTest(const FString&)
{
    auto Make = []() {
        FVueVersion Version;
        ParseJson(TEXT(R"JSON({"files":{"compile-diagnostics.json":"hash-already-validated-by-Read"},"capabilities":{
            "schemaVersion":1,"compiler":"rmlui-css/2.0.0","profile":"slate-rhi",
            "minimumHostAbi":2,"minimumSlateAbi":5,"requiredFeatures":["css.variables","layout.measure","input.ime"],
            "degradedFeatures":["render.layers"],"diagnostics":"compile-diagnostics.json"}})JSON"), Version.Manifest);
        return Version;
    };
    FString Error;
    auto Version = Make();
    TestTrue(TEXT("A supported profile enables strict native writes"), Version.ValidateCapabilities(true, Error) && Version.bStrictCapabilities);
    TestFalse(TEXT("Profile is checked against the actual View renderer"), Version.ValidateCapabilities(false, Error));
    Version = Make(); Version.Manifest->RemoveField(TEXT("capabilities"));
    TestTrue(TEXT("Legacy bundles remain readable without a new capability promise"), Version.ValidateCapabilities(true, Error) && !Version.bStrictCapabilities);
    for (double Abi : {0.0, 1.0, 2.5, 999.0}) {
        Version = Make(); Version.Manifest->GetObjectField(TEXT("capabilities"))->SetNumberField(TEXT("minimumHostAbi"), Abi);
        TestFalse(TEXT("Invalid or newer host ABI is rejected"), Version.ValidateCapabilities(true, Error));
    }
    for (const TCHAR* Feature : {TEXT("browser.dom"), TEXT("render.filters")}) {
        Version = Make();
        Version.Manifest->GetObjectField(TEXT("capabilities"))->SetArrayField(TEXT("requiredFeatures"), {MakeShared<FJsonValueString>(Feature)});
        TestFalse(TEXT("Unknown and unavailable required features are rejected"), Version.ValidateCapabilities(true, Error));
    }
    Version = Make(); Version.Manifest->GetObjectField(TEXT("capabilities"))->SetArrayField(TEXT("degradedFeatures"), {MakeShared<FJsonValueString>(TEXT("css.variables"))});
    TestFalse(TEXT("A feature cannot be both required and degraded"), Version.ValidateCapabilities(true, Error));
    Version = Make(); Version.Manifest->GetObjectField(TEXT("files"))->RemoveField(TEXT("compile-diagnostics.json"));
    TestFalse(TEXT("Diagnostics must be included in hashed resources"), Version.ValidateCapabilities(true, Error));
    Version = Make(); Version.Manifest->GetObjectField(TEXT("capabilities"))->SetNumberField(TEXT("minimumSlateAbi"), 999);
    TestFalse(TEXT("Newer Slate packet ABI is rejected"), Version.ValidateCapabilities(true, Error));
    Version = Make(); Version.Manifest->GetObjectField(TEXT("capabilities"))->SetStringField(TEXT("profile"), TEXT("dx11-compat"));
    TestTrue(TEXT("DX11 compatibility renderer accepts its own declared profile"), Version.ValidateCapabilities(false, Error));
    return true;
}
#endif

FString URmlUiJSRuntime::DefaultManifestPath()
{
    return FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(), TEXT("Vue/current.json"));
}
bool URmlUiJSRuntime::Fail(const FString& Message)
{
    LastError = Message;
    UE_LOG(LogTemp, Warning, TEXT("RmlUiJS update: %s"), *Message);
    OnStatus.Broadcast(Message);
    RefreshWidgetWake();
    return false;
}
bool URmlUiJSRuntime::Start(URmlUiWidget* Widget, const FString& ManifestPath, bool bWatch)
{
    check(IsInGameThread());
    Stop();
    JsonHostRequestCount = 0;
    AdvanceCount = 0;
    AdvanceSkipCount = 0;
    if (!Widget) return Fail(TEXT("A RmlUi widget is required."));
    Target = Widget;
    Widget->TakeWidget();
    SlateWidget = Widget->GetSlateRmlWidget();
    WatchedManifest = ManifestPath.IsEmpty() ? DefaultManifestPath() : ManifestPath;
    FFileHelper::LoadFileToString(WatchedText, *WatchedManifest);
    bWatchFiles = bWatch;
    NextWatchTime = bWatchFiles ? FPlatformTime::Seconds() + 0.25 : 0.0;
    auto Slate = SlateWidget.Pin();
    if (!Slate || !Activate(WatchedManifest)) { Target = nullptr; SlateWidget.Reset(); return false; }
    FrameHandle = Slate->OnBeforeRender.AddUObject(this, &URmlUiJSRuntime::Advance);
    ShutdownHandle = Slate->OnNativeShutdown.AddUObject(this, &URmlUiJSRuntime::Stop);
    LastAdvanceTime = FPlatformTime::Seconds();
    RefreshWidgetWake();
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
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_JS_Activate);
    FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::JsActivate);
    auto Slate = SlateWidget.Pin();
    if (!Slate) return Fail(TEXT("RmlUi widget has been destroyed."));
    FVueVersion Version;
    FString Error;
    if (!Version.Read(Path, Error) || !Version.ValidateCapabilities(Slate->IsUsingSlateRenderer(), Error)) return Fail(Error);
    FString State = Active ? Active->CaptureState() : TEXT("{}");
    RmlUE_View* NewView = Slate->IsUsingSlateRenderer()
        ? RmlUE_CreateSlateView(1280, 800, 1)
        : RmlUE_CreateView(1280, 800, 1);
    if (!NewView) return Fail(UTF8_TO_TCHAR(RmlUE_GetLastError()));
    RmlUE_SetStrictCapabilities(NewView, Version.bStrictCapabilities);
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
    Candidate->SetWakeCallback([this]() { RefreshWidgetWake(); });
    TArray<TPair<FString, UObject*>> ServiceArguments;
    ServiceArguments.Reserve(Services.Num());
    for (const auto& Pair : Services) {
        if (!IsValid(Pair.Value)) { RmlUE_DestroyView(NewView); Candidate = nullptr; return Fail(TEXT("A registered JavaScript service is no longer valid: ") + Pair.Key); }
        ServiceArguments.Emplace(Pair.Key, Pair.Value.Get());
    }
    IRmlUiCssAnimationSession* CandidateCssAnimationSession = nullptr;
    if (!Version.MotionManifest.IsEmpty())
    {
        FString MotionJson;
        CandidateCssAnimationSession = CreateRmlUiCssAnimationSession();
        CandidateCssAnimationSession->SetWakeCallback([this]()
        {
            if (const TSharedPtr<SRmlUiWidget> Pinned = SlateWidget.Pin())
                Pinned->SetExternalWakeDeadline(this, FPlatformTime::Seconds());
        });
        CandidateCssAnimationSession->SetLifecycleEventBatchCallback(
            [WeakContext = TWeakObjectPtr<URmlUiJSContext>(Candidate)](const FString& Json)
            {
                if (URmlUiJSContext* Context = WeakContext.Get()) Context->QueueHostEvent(Json);
            });
        const bool bLoadedMotionManifest = FFileHelper::LoadFileToString(
            MotionJson, *FPaths::Combine(Version.Directory, Version.MotionManifest));
        if (!bLoadedMotionManifest) Error = TEXT("Cannot read the hashed motion manifest.");
        if (!bLoadedMotionManifest || !CandidateCssAnimationSession->Install(MotionJson, NewView, Error))
        {
            delete CandidateCssAnimationSession;
            Candidate = nullptr; RmlUE_DestroyView(NewView);
            return Fail(TEXT("Candidate UI motion manifest rejected; previous page retained. ") + Error);
        }
        Candidate->SetCssAnimationRestartCallback([CandidateCssAnimationSession](int32 Node)
        {
            return CandidateCssAnimationSession->RequestRestart(static_cast<RmlUE_Node>(Node));
        });
    }
    // Keep the debugger on a unique port while both old and candidate VMs exist.
    const int32 CandidatePort = DebugPort < 0 ? -1 : DebugPort + ((ReloadCount + 1) % 2);
    if (!Candidate->Initialize(NewView, Version.Directory, Version.Entry, Version.Version, State, CandidatePort, ServiceArguments)) {
        Error = Candidate->LastError;
        delete CandidateCssAnimationSession;
        Candidate->Dispose(); Candidate = nullptr; RmlUE_DestroyView(NewView);
        return Fail(TEXT("Candidate UI rejected; previous page retained. ") + Error);
    }
    if (!RmlUE_Update(NewView) || !Candidate->LastError.IsEmpty()) {
        Error = Candidate->LastError.IsEmpty() ? UTF8_TO_TCHAR(RmlUE_GetLastError()) : Candidate->LastError;
        delete CandidateCssAnimationSession;
        Candidate->Dispose(); Candidate = nullptr; RmlUE_DestroyView(NewView);
        return Fail(TEXT("Candidate UI rejected after initial layout; previous page retained. ") + Error);
    }
    if (CandidateCssAnimationSession &&
        (!CandidateCssAnimationSession->FlushActivationChanges(Error) ||
         !CandidateCssAnimationSession->ValidateInitialBindings(Error)))
    {
        delete CandidateCssAnimationSession;
        Candidate->Dispose(); Candidate = nullptr; RmlUE_DestroyView(NewView);
        return Fail(TEXT("Candidate UI motion activation rejected; previous page retained. ") + Error);
    }
    RmlUE_View* OldView = Slate->ExchangeNativeView(NewView);
    if (Active) Active->Dispose();
    delete CssAnimationSession;
    CssAnimationSession = CandidateCssAnimationSession;
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
    LastAdvanceTime = FPlatformTime::Seconds();
    RefreshWidgetWake();
    UE_LOG(LogTemp, Display, TEXT("RmlUiJS activated %s"), *ActiveVersion);
    if (CssAnimationSession)
        UE_LOG(LogTemp, Display, TEXT("RmlUiJS CSS activation: %s"), *CssAnimationSession->GetActivationDiagnostics());
    OnStatus.Broadcast(TEXT("Activated ") + ActiveVersion);
    return true;
}
void URmlUiJSRuntime::Advance(float DeltaSeconds)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_JS_RuntimeAdvance);
    FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::JsAdvance);
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
            WatchedText = Text; QueueManifest(WatchedManifest);
        }
    }
    if (!PendingManifest.IsEmpty()) {
        FString Path = MoveTemp(PendingManifest); PendingManifest.Reset(); Activate(Path);
    }
    bool bAdvanced = false;
    if (Active && Active->NeedsAdvance(Now)) {
        if (Active->HasDispatchWake())
            FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsDispatchWakes);
        if (Active->HasAnimationFrameWake())
            FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsFrameWakes);
        if (Active->HasTimerWake(Now))
            FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsTimerWakes);
        const double Elapsed = LastAdvanceTime > 0.0 ? Now - LastAdvanceTime : static_cast<double>(DeltaSeconds);
        Active->Advance(static_cast<float>(FMath::Max(0.0, Elapsed)));
        LastAdvanceTime = Now;
        ++AdvanceCount;
        bAdvanced = true;
    }
    if (!bAdvanced) {
        ++AdvanceSkipCount;
        FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsAdvanceSkips);
    }
    if (CssAnimationSession)
    {
        FString ActivationError;
        if (!CssAnimationSession->FlushActivationChanges(ActivationError))
        {
            LastError = TEXT("Native CSS activation failed: ") + ActivationError;
            UE_LOG(LogTemp, Error, TEXT("RmlUiJS: %s"), *LastError);
            OnStatus.Broadcast(LastError);
        }
        CssAnimationSession->FlushLifecycleEvents();
    }
    RefreshWidgetWake();
}
void URmlUiJSRuntime::RefreshWidgetWake()
{
    auto Slate = SlateWidget.Pin();
    if (!Slate) return;
    const double Now = FPlatformTime::Seconds();
    double Deadline = Active ? Active->GetNextWakeTimeSeconds(Now) : TNumericLimits<double>::Max();
    if (bWatchFiles) Deadline = FMath::Min(Deadline, NextWatchTime > 0.0 ? NextWatchTime : Now);
    if (!bDownloading && !Requests.IsEmpty()) Deadline = Now;
    if (!PendingManifest.IsEmpty()) Deadline = Now;
    Slate->SetExternalWakeDeadline(this, Deadline);
}
void URmlUiJSRuntime::QueueManifest(const FString& Path)
{
    PendingManifest = Path;
    RefreshWidgetWake();
}
void URmlUiJSRuntime::Reload() { QueueManifest(WatchedManifest); }
FString URmlUiJSRuntime::GetCssAnimationDiagnostics() const
{
    return CssAnimationSession ? CssAnimationSession->GetActivationDiagnostics() : TEXT("{\"rules\":[]}");
}
void URmlUiJSRuntime::LoadVersion(const FString& Path) { QueueManifest(Path); }
void URmlUiJSRuntime::Rollback() { if (!PreviousManifest.IsEmpty()) QueueManifest(PreviousManifest); }
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
        Slate->ClearExternalWakeDeadline(this);
        Slate->OnBeforeRender.Remove(FrameHandle);
        Slate->OnNativeShutdown.Remove(ShutdownHandle);
    }
    delete CssAnimationSession;
    CssAnimationSession = nullptr;
    if (Active) Active->Dispose();
    if (Candidate) Candidate->Dispose();
    Active = nullptr; Candidate = nullptr; Target = nullptr;
    SlateWidget.Reset(); PendingManifest.Reset();
    NextWatchTime = 0.0;
    LastAdvanceTime = 0.0;
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
                    QueueManifest(Manifest);
                }
            });
            if (!FileRequest->ProcessRequest()) { State->Failed = true; bDownloading = false; Fail(TEXT("Cannot start resource download.")); return; }
        }
    });
    if (!Request->ProcessRequest()) { bDownloading = false; Fail(TEXT("Cannot start manifest download.")); }
}
