#include "RmlUiJSContext.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "JsEnv.h"
#include "JSModuleLoader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RmlUiBridge.h"
#include "Serialization/JsonSerializer.h"

namespace {
class FVueModuleLoader final : public puerts::DefaultJSModuleLoader
{
public:
    explicit FVueModuleLoader(const FString& Root) : DefaultJSModuleLoader(Root)
    {
        Bootstrap = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("Puerts"))->GetContentDir(), TEXT("JavaScript"));
    }
    bool Search(const FString& RequiredDir, const FString& Module, FString& Path, FString& Absolute) override
    {
        return SearchModuleInDir(RequiredDir.IsEmpty() ? ScriptRoot : RequiredDir, Module, Path, Absolute) ||
            SearchModuleInDir(ScriptRoot, Module, Path, Absolute) || SearchModuleInDir(Bootstrap, Module, Path, Absolute);
    }
private:
    FString Bootstrap;
};
class FVueLogger final : public puerts::ILogger
{
public:
    explicit FVueLogger(URmlUiJSContext* Context) : Owner(Context) {}
    void Log(const FString& Message) const override { UE_LOG(LogTemp, Display, TEXT("RmlUiJS: %s"), *Message); }
    void Info(const FString& Message) const override { Log(Message); }
    void Warn(const FString& Message) const override { UE_LOG(LogTemp, Warning, TEXT("RmlUiJS: %s"), *Message); }
    void Error(const FString& Message) const override { if (Owner.IsValid()) Owner->ReportError(Message); }
private:
    TWeakObjectPtr<URmlUiJSContext> Owner;
};
FString JsonString(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
    return Result;
}
}

URmlUiJSContext::URmlUiJSContext() = default;
URmlUiJSContext::~URmlUiJSContext() = default;

bool URmlUiJSContext::Initialize(RmlUE_View* InView, const FString& Directory, const FString& Entry,
    const FString& InVersion, const FString& State, int32 DebugPort, const TArray<TPair<FString, UObject*>>& Services)
{
    check(IsInGameThread());
    View = InView;
    StateJson = State;
    Version = InVersion;
    RmlUE_SetNodeEventCallback(View, &URmlUiJSContext::NativeEvent, this);
    Environment = MakeShared<puerts::FJsEnv>(std::make_shared<FVueModuleLoader>(Directory), std::make_shared<FVueLogger>(this), DebugPort);
    TArray<TPair<FString, UObject*>> Arguments;
    Arguments.Reserve(Services.Num() + 1);
    Arguments.Emplace(TEXT("bridge"), this);
    Arguments.Append(Services);
    Environment->Start(Entry, Arguments);
    if (!bReady && LastError.IsEmpty()) LastError = TEXT("The JS entry did not call ReportReady synchronously.");
    return bReady && LastError.IsEmpty();
}

bool URmlUiJSContext::Result(int Value)
{
    if (!Value) ReportError(UTF8_TO_TCHAR(RmlUE_GetLastError()));
    return Value != 0;
}
int32 URmlUiJSContext::RootNode() { return View ? RmlUE_GetRootNode(View) : 0; }
int32 URmlUiJSContext::FindNode(const FString& Id) { return View ? RmlUE_FindNode(View, TCHAR_TO_UTF8(*Id)) : 0; }
int32 URmlUiJSContext::CreateNode(int32 Kind, const FString& Text) { return View ? RmlUE_CreateNode(View, Kind, TCHAR_TO_UTF8(*Text)) : 0; }
bool URmlUiJSContext::IsNodeValid(int32 Node) { return View && RmlUE_IsNodeValid(View, Node); }
bool URmlUiJSContext::InsertNode(int32 Node, int32 Parent, int32 Before) { return View && Result(RmlUE_InsertNode(View, Node, Parent, Before)); }
bool URmlUiJSContext::RemoveNode(int32 Node) { return View && Result(RmlUE_RemoveNode(View, Node)); }
int32 URmlUiJSContext::ParentNode(int32 Node) { return View ? RmlUE_ParentNode(View, Node) : 0; }
int32 URmlUiJSContext::NextNode(int32 Node) { return View ? RmlUE_NextNode(View, Node) : 0; }
bool URmlUiJSContext::SetText(int32 Node, const FString& Text) { return View && Result(RmlUE_SetNodeText(View, Node, TCHAR_TO_UTF8(*Text))); }
FString URmlUiJSContext::GetText(int32 Node)
{
    TArray<char> Buffer; Buffer.SetNumZeroed(1024 * 1024);
    return View && Result(RmlUE_GetNodeText(View, Node, Buffer.GetData(), Buffer.Num())) ? UTF8_TO_TCHAR(Buffer.GetData()) : FString();
}
bool URmlUiJSContext::SetAttribute(int32 Node, const FString& Name, const FString& Value, bool bRemove)
{
    return View && Result(RmlUE_SetNodeAttribute(View, Node, TCHAR_TO_UTF8(*Name), bRemove ? nullptr : TCHAR_TO_UTF8(*Value)));
}
FString URmlUiJSContext::GetAttribute(int32 Node, const FString& Name)
{
    TArray<char> Buffer; Buffer.SetNumZeroed(1024 * 1024);
    return View && Result(RmlUE_GetNodeAttribute(View, Node, TCHAR_TO_UTF8(*Name), Buffer.GetData(), Buffer.Num())) ? UTF8_TO_TCHAR(Buffer.GetData()) : FString();
}
bool URmlUiJSContext::SetProperty(int32 Node, const FString& Name, const FString& Value, bool bRemove)
{
    return View && Result(RmlUE_SetNodeProperty(View, Node, TCHAR_TO_UTF8(*Name), bRemove ? nullptr : TCHAR_TO_UTF8(*Value)));
}
bool URmlUiJSContext::Listen(int32 Node, const FString& Type, int32 Listener, bool bCapture)
{
    return View && Result(RmlUE_ListenNode(View, Node, TCHAR_TO_UTF8(*Type), Listener, bCapture ? 1 : 0));
}
void URmlUiJSContext::Unlisten(int32 Listener) { if (View) RmlUE_UnlistenNode(View, Listener); }
void URmlUiJSContext::SetEventResult(int32 Value) { if (!EventResults.IsEmpty()) EventResults.Last() |= Value; }
void URmlUiJSContext::SaveState(const FString& Json) { StateJson = Json; }
void URmlUiJSContext::ReportReady() { bReady = true; }
void URmlUiJSContext::ReportError(const FString& Error)
{
    LastError = Error;
    UE_LOG(LogTemp, Error, TEXT("RmlUiJS: %s"), *Error);
}
void URmlUiJSContext::ReportDebugState(const FString& Json) { DebugStateJson = Json; }
void URmlUiJSContext::RequestHost(int32 RequestId, const FString& Method, const FString& Json)
{
    if (bDisposed) return;
    if (!bHostActive) { PendingHostRequests.Add({RequestId, Method, Json}); return; }
    if (!OnHostRequest.IsBound()) { ResolveHostRequest(RequestId, TEXT("No UE host handler is registered"), false); return; }
    OnHostRequest.Broadcast(RequestId, Method, Json);
}
void URmlUiJSContext::ActivateHostRequests()
{
    bHostActive = true;
    auto Queued = MoveTemp(PendingHostRequests);
    PendingHostRequests.Reset();
    for (const auto& Request : Queued) RequestHost(Request.Id, Request.Method, Request.Json);
}
void URmlUiJSContext::ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess)
{
    if (bDisposed) return;
    auto Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), RequestId);
    Response->SetBoolField(TEXT("success"), bSuccess);
    Response->SetStringField(TEXT("payload"), Json);
    PendingResponses.Add(JsonString(Response));
}
int URmlUiJSContext::NativeEvent(void* User, uint32 Listener, const RmlUE_NodeEvent* Event)
{
    auto* Self = static_cast<URmlUiJSContext*>(User);
    if (Self->bDisposed) return 0;
    auto Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("listener"), Listener);
    Json->SetNumberField(TEXT("target"), Event->Target);
    Json->SetNumberField(TEXT("currentTarget"), Event->CurrentTarget);
    Json->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Event->Type));
    Json->SetStringField(TEXT("value"), UTF8_TO_TCHAR(Event->Value));
    Json->SetBoolField(TEXT("checked"), Event->Checked != 0);
    Json->SetNumberField(TEXT("phase"), Event->Phase);
    Json->SetNumberField(TEXT("key"), Event->Key);
    Json->SetStringField(TEXT("keyName"), UTF8_TO_TCHAR(Event->KeyName ? Event->KeyName : ""));
    Json->SetNumberField(TEXT("button"), Event->Button);
    Json->SetNumberField(TEXT("modifiers"), Event->Modifiers);
    Json->SetNumberField(TEXT("x"), Event->X);
    Json->SetNumberField(TEXT("y"), Event->Y);
    Self->EventResults.Add(0);
    Self->OnNativeEvent.Broadcast(JsonString(Json));
    return Self->EventResults.Pop();
}
void URmlUiJSContext::Advance(float DeltaSeconds)
{
    if (bDisposed) return;
    TArray<FString> Responses = MoveTemp(PendingResponses);
    for (const FString& Json : Responses) OnHostResponse.Broadcast(Json);
    auto Events = MoveTemp(PendingHostEvents); PendingHostEvents.Reset();
    for (const FString& Json : Events) OnHostEvent.Broadcast(Json);
    OnFrame.Broadcast(DeltaSeconds);
}
bool URmlUiJSContext::ScrollNode(int32 Node, float Top) { return View && Result(RmlUE_ScrollNode(View, Node, Top)); }
float URmlUiJSContext::ScrollRemaining(int32 Node) { return View ? RmlUE_NodeScrollRemaining(View, Node) : 0; }
bool URmlUiJSContext::FocusNode(int32 Node) { return View && RmlUE_FocusNode(View, Node); }
void URmlUiJSContext::QueueHostEvent(const FString& Json) { if (!bDisposed) PendingHostEvents.Add(Json); }
FString URmlUiJSContext::CaptureState()
{
    if (!bDisposed) OnLifecycle.Broadcast(TEXT("serialize"));
    return StateJson;
}
void URmlUiJSContext::Dispose()
{
    if (bDisposed) return;
    OnLifecycle.Broadcast(TEXT("dispose"));
    bDisposed = true;
    if (View) RmlUE_SetNodeEventCallback(View, nullptr, nullptr);
    OnNativeEvent.Clear(); OnFrame.Clear(); OnLifecycle.Clear(); OnHostResponse.Clear(); OnHostRequest.Clear();
    PendingResponses.Empty();
    PendingHostRequests.Empty();
    OnHostEvent.Clear(); PendingHostEvents.Empty();
    Environment.Reset();
    View = nullptr;
}
void URmlUiJSContext::BeginDestroy() { Dispose(); Super::BeginDestroy(); }
