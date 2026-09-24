#include "RmlUiChatTransport.h"
#include "RmlUiJSRuntime.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"

struct FRmlChatStream
{
    FCriticalSection Mutex;
    TArray<uint8> Bytes;
    int64 Total = 0;
    bool Exceeded = false, Complete = false, Success = false;
    int32 Status = 0;
};
namespace {
FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Text; FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text)); return Text;
}
bool Decode(const FString& Text, TSharedPtr<FJsonObject>& Object)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}
bool ValidEndpoint(const FString& Url)
{
    return Url.StartsWith(TEXT("https://")) || Url.StartsWith(TEXT("http://127.0.0.1:")) || Url.StartsWith(TEXT("http://localhost:"));
}
FString HistoryPath() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RmlUiChat/session.json")); }
}
void URmlUiChatTransport::Attach(URmlUiJSRuntime* InRuntime, const FString& InEndpoint, const FString& InModel, const FString& InProtocol)
{
    Stop(); Runtime = InRuntime; Endpoint = InEndpoint; Model = InModel; Protocol = InProtocol;
    if (!Runtime) return;
    Runtime->OnHostRequest.AddDynamic(this, &URmlUiChatTransport::HandleRequest);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &URmlUiChatTransport::Tick), 0.04f);
}
void URmlUiChatTransport::Cancel()
{
    if (bStreaming) ++StreamCancels;
    bStreaming = false;
    if (Request) Request->CancelRequest();
    Request.Reset(); Stream.Reset(); StreamContext.Reset(); PendingBytes.Reset(); EventData.Reset(); bTerminal = false;
}
void URmlUiChatTransport::Stop()
{
    Cancel();
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle); TickHandle.Reset();
    if (Runtime) Runtime->OnHostRequest.RemoveDynamic(this, &URmlUiChatTransport::HandleRequest);
    Runtime = nullptr;
}
void URmlUiChatTransport::BeginDestroy() { Stop(); Super::BeginDestroy(); }
void URmlUiChatTransport::HandleRequest(int32 Id, const FString& Method, const FString& Text)
{
    if (!Method.StartsWith(TEXT("chat.")) || !Runtime) return;
    TSharedPtr<FJsonObject> Json;
    if (!Decode(Text, Json)) { Runtime->ResolveHostRequest(Id, TEXT("Invalid request JSON"), false); return; }
    FString Error;
    auto Result = MakeShared<FJsonObject>();
    if (Method == TEXT("chat.config")) {
        Result->SetStringField(TEXT("endpoint"), Endpoint); Result->SetStringField(TEXT("model"), Model); Result->SetStringField(TEXT("protocol"), Protocol);
    } else if (Method == TEXT("chat.configure")) {
        FString NewEndpoint, NewModel, NewProtocol;
        if (!Json->TryGetStringField(TEXT("endpoint"), NewEndpoint) || !ValidEndpoint(NewEndpoint) ||
            !Json->TryGetStringField(TEXT("model"), NewModel) || NewModel.Len() > 128 ||
            !Json->TryGetStringField(TEXT("protocol"), NewProtocol) || (NewProtocol != TEXT("openai") && NewProtocol != TEXT("nanochat"))) Error = TEXT("Use an HTTPS or localhost endpoint and a supported protocol.");
        else { Cancel(); Endpoint = NewEndpoint; Model = NewModel; Protocol = NewProtocol; }
    } else if (Method == TEXT("chat.start")) {
        StartStream(Json, Error);
    } else if (Method == TEXT("chat.stop")) { Cancel();
    } else if (Method == TEXT("chat.copy")) {
        FString Value;
        if (Json->TryGetStringField(TEXT("text"), Value) && Value.Len() <= 131072) FPlatformApplicationMisc::ClipboardCopy(*Value);
        else Error = TEXT("Copy text is too large.");
    } else if (Method == TEXT("chat.openLink")) {
        FString Url;
        if (Json->TryGetStringField(TEXT("url"), Url) && (Url.StartsWith(TEXT("https://")) || Url.StartsWith(TEXT("http://")))) FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
        else Error = TEXT("Unsupported link protocol.");
    } else if (Method == TEXT("chat.load")) {
        FString Saved; TSharedPtr<FJsonObject> History;
        if (bPersistenceEnabled && IFileManager::Get().FileSize(*HistoryPath()) <= 2 * 1024 * 1024 && FFileHelper::LoadFileToString(Saved, *HistoryPath()) && Decode(Saved, History)) Result = History.ToSharedRef();
    } else if (Method == TEXT("chat.persist")) {
        if (!bPersistenceEnabled) {}
        else if (Text.Len() > 512 * 1024) Error = TEXT("Conversation history exceeds the local limit.");
        else {
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(HistoryPath()), true);
            const FString Temporary = HistoryPath() + TEXT(".tmp");
            if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || IFileManager::Get().Move(*HistoryPath(), *Temporary, true) != COPY_OK) Error = TEXT("Cannot save conversation history.");
        }
    } else Error = TEXT("Unknown chat command.");
    Runtime->ResolveHostRequest(Id, Error.IsEmpty() ? Encode(Result) : Error, Error.IsEmpty());
}
bool URmlUiChatTransport::StartStream(const TSharedPtr<FJsonObject>& Json, FString& Error)
{
    const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
    FString Id;
    if (!ValidEndpoint(Endpoint) || !Json->TryGetStringField(TEXT("streamId"), Id) || Id.IsEmpty() || Id.Len() > 80 ||
        !Json->TryGetArrayField(TEXT("messages"), Messages) || Messages->IsEmpty() || Messages->Num() > 100) { Error = TEXT("Invalid endpoint or conversation."); return false; }
    int32 Total = 0;
    for (const auto& Value : *Messages) {
        const TSharedPtr<FJsonObject>* Message = nullptr; FString Role, Content;
        if (!Value->TryGetObject(Message) || !(*Message)->TryGetStringField(TEXT("role"), Role) ||
            !(*Message)->TryGetStringField(TEXT("content"), Content) || (Role != TEXT("user") && Role != TEXT("assistant")) || Content.Len() > 65536) {
            Error = TEXT("Invalid message role or size."); return false;
        }
        Total += Content.Len();
    }
    if (Total > 262144) { Error = TEXT("Conversation exceeds the request limit."); return false; }
    Cancel(); StreamId = Id; StreamContext = Runtime->GetContext();
    auto Body = MakeShared<FJsonObject>(); Body->SetArrayField(TEXT("messages"), *Messages);
    double Temperature = 0.7; Json->TryGetNumberField(TEXT("temperature"), Temperature);
    Body->SetNumberField(TEXT("temperature"), FMath::Clamp(Temperature, 0.0, 2.0));
    Body->SetNumberField(TEXT("max_tokens"), 2048);
    if (Protocol == TEXT("nanochat")) Body->SetNumberField(TEXT("top_k"), 50);
    else { Body->SetStringField(TEXT("model"), Model); Body->SetBoolField(TEXT("stream"), true); }
    Stream = MakeShared<FRmlChatStream, ESPMode::ThreadSafe>();
    const auto State = Stream;
    Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Endpoint); Request->SetVerb(TEXT("POST")); Request->SetTimeout(90);
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json")); Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
    if (!ApiKey.IsEmpty()) Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
    Request->SetContentAsString(Encode(Body));
    Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    // HTTP thread only touches this bounded byte buffer; all JSON, UObject and JS work is on the game thread.
    Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([State](void* Data, int64& Length) {
        FScopeLock Lock(&State->Mutex);
        State->Total += Length;
        if (State->Total > 4 * 1024 * 1024) { State->Exceeded = true; Length = 0; return; }
        State->Bytes.Append(static_cast<uint8*>(Data), static_cast<int32>(Length));
    }));
    Request->OnProcessRequestComplete().BindLambda([State](FHttpRequestPtr, FHttpResponsePtr Response, bool Success) {
        State->Complete = true; State->Success = Success; State->Status = Response ? Response->GetResponseCode() : 0;
    });
    bStreaming = Request->ProcessRequest();
    if (!bStreaming) { Error = TEXT("Cannot start chat request."); Cancel(); return false; }
    ++StreamStarts; return true;
}
void URmlUiChatTransport::Emit(const FString& Delta, const FString& Status, const FString& Error)
{
    if (!StreamContext.IsValid() || !Runtime || StreamContext.Get() != Runtime->GetContext()) return;
    auto Event = MakeShared<FJsonObject>();
    Event->SetStringField(TEXT("type"), TEXT("chat.stream")); Event->SetStringField(TEXT("streamId"), StreamId);
    Event->SetStringField(TEXT("delta"), Delta); Event->SetStringField(TEXT("status"), Status); Event->SetStringField(TEXT("error"), Error);
    StreamContext->QueueHostEvent(Encode(Event));
    if (!Delta.IsEmpty()) ++StreamDeltas;
}
bool URmlUiChatTransport::ParseEvent(const FString& Data, FString& Delta, FString& Error)
{
    if (Data == TEXT("[DONE]")) { bTerminal = true; return true; }
    TSharedPtr<FJsonObject> Json;
    if (!Decode(Data, Json)) { Error = TEXT("Invalid JSON in the event stream."); return false; }
    if (Json->HasField(TEXT("error"))) { Error = TEXT("The model server returned a stream error."); return false; }
    if (Protocol == TEXT("nanochat")) {
        FString Token; if (Json->TryGetStringField(TEXT("token"), Token)) Delta += Token;
        bool Done = false; if (Json->TryGetBoolField(TEXT("done"), Done) && Done) bTerminal = true;
    } else {
        const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
        if (Json->TryGetArrayField(TEXT("choices"), Choices) && !Choices->IsEmpty()) {
            const TSharedPtr<FJsonObject>* Choice = nullptr;
            const TSharedPtr<FJsonObject>* Chunk = nullptr;
            FString Text;
            if ((*Choices)[0]->TryGetObject(Choice) && (*Choice)->TryGetObjectField(TEXT("delta"), Chunk) && (*Chunk)->TryGetStringField(TEXT("content"), Text)) Delta += Text;
        }
    }
    return true;
}
bool URmlUiChatTransport::Tick(float)
{
    if (!bStreaming || !Stream) return true;
    if (!Runtime || StreamContext.Get() != Runtime->GetContext()) { Cancel(); return true; }
    FString Error, Delta;
    {
        FScopeLock Lock(&Stream->Mutex);
        PendingBytes.Append(Stream->Bytes); Stream->Bytes.Reset();
        if (Stream->Exceeded) Error = TEXT("Response exceeds the 4 MiB stream limit.");
    }
    int32 Start = 0;
    for (int32 Index = 0; Index < PendingBytes.Num() && Error.IsEmpty() && !bTerminal; ++Index) {
        if (PendingBytes[Index] != '\n') continue;
        int32 Length = Index - Start;
        if (Length && PendingBytes[Index - 1] == '\r') --Length;
        // Decode only complete SSE lines, so split multi-byte UTF-8 sequences remain intact.
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(PendingBytes.GetData() + Start), Length);
        const FString Line(Converted.Length(), Converted.Get()); Start = Index + 1;
        if (Line.IsEmpty()) {
            if (!EventData.IsEmpty()) { ParseEvent(EventData, Delta, Error); EventData.Reset(); }
        } else if (Line.StartsWith(TEXT("data:"))) {
            FString Value = Line.Mid(5); if (Value.StartsWith(TEXT(" "))) Value.RightChopInline(1);
            if (!EventData.IsEmpty()) EventData += TEXT("\n"); EventData += Value;
        }
    }
    if (Start) PendingBytes.RemoveAt(0, Start, EAllowShrinking::No);
    if (Stream->Complete && !bTerminal && Error.IsEmpty()) {
        Error = Stream->Status != 200 ? FString::Printf(TEXT("HTTP %d: chat request failed."), Stream->Status) : TEXT("The stream ended before its completion marker.");
    }
    if (!Error.IsEmpty()) { Emit(Delta, TEXT("error"), Error); Cancel(); }
    else if (bTerminal) { Emit(Delta, TEXT("complete")); bStreaming = false; Cancel(); }
    else if (!Delta.IsEmpty()) Emit(Delta, TEXT("streaming"));
    return true;
}
