#include "RmlUiCommunicationBenchmark.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace RmlUiCommunication
{
FString JsonString(const TSharedRef<FJsonObject>& Value)
{
    FString Json;
    FJsonSerializer::Serialize(Value, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}
}

float URmlUiCommunicationBenchmark::GetFloat() { ++ScalarReads; return Scalar; }
float URmlUiCommunicationBenchmark::GetObjectValue() { ++PropertyReads; return Object->Value; }
URmlUiCommunicationValue* URmlUiCommunicationBenchmark::GetObject() { ++ObjectResolves; return Object; }
void URmlUiCommunicationBenchmark::Ready() { bReady = true; }
void URmlUiCommunicationBenchmark::Report(const FString& Json) { LastReport = Json; }
void URmlUiCommunicationBenchmark::BeginBatch() { BatchStart = FPlatformTime::Cycles64(); }
double URmlUiCommunicationBenchmark::EndBatch()
{
    const uint64 End = FPlatformTime::Cycles64();
    return FPlatformTime::ToMilliseconds64(End - BatchStart);
}
void URmlUiCommunicationBenchmark::ResetBatch(float Value)
{
    Scalar = Value;
    Object->Value = Value;
    ScalarReads = PropertyReads = ObjectResolves = Requests = RequestBytes = ResponseBytes = 0;
    ParseCycles = ReadCycles = SerializeCycles = EnqueueCycles = 0;
    LastReport.Empty(); Error.Empty();
}
void URmlUiCommunicationBenchmark::Request(const FString& Json)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_WebEventRequest);
    ++Requests;
    RequestBytes += FTCHARToUTF8(*Json).Length();
    TSharedPtr<FJsonObject> RequestObject;
    double Id = 0;
    FString Kind;
    bool bValid = false;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_WebParseRequest);
        const uint64 Start = FPlatformTime::Cycles64();
        bValid = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), RequestObject) &&
            RequestObject.IsValid() && RequestObject->TryGetNumberField(TEXT("id"), Id) &&
            RequestObject->TryGetStringField(TEXT("kind"), Kind) && (Kind == TEXT("float") || Kind == TEXT("property"));
        ParseCycles += FPlatformTime::Cycles64() - Start;
    }
    if (!bValid) { Error = TEXT("Invalid JSON event request"); return; }
    float Value;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_WebReadValue);
        const uint64 Start = FPlatformTime::Cycles64();
        Value = Kind == TEXT("float") ? GetFloat() : GetObjectValue();
        ReadCycles += FPlatformTime::Cycles64() - Start;
    }
    FString Javascript;
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_WebSerializeResponse);
        const uint64 Start = FPlatformTime::Cycles64();
        TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetNumberField(TEXT("id"), Id);
        Response->SetNumberField(TEXT("value"), Value);
        const FString ResponseJson = RmlUiCommunication::JsonString(Response);
        ResponseBytes += FTCHARToUTF8(*ResponseJson).Length();
        FString Quoted;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Quoted);
        Writer->WriteValue(ResponseJson);
        Writer->Close();
        Javascript = TEXT("window.dispatchEvent(new CustomEvent('ue-response',{detail:") + Quoted + TEXT("}));");
        SerializeCycles += FPlatformTime::Cycles64() - Start;
    }
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_WebEnqueueResponse);
        const uint64 Start = FPlatformTime::Cycles64();
        if (SendJavascript) SendJavascript(Javascript);
        else Error = TEXT("Missing event response transport");
        EnqueueCycles += FPlatformTime::Cycles64() - Start;
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "RmlUiJSContext.h"
#include "SRmlUiWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"

namespace RmlUiCommunication
{
struct FScenario { FString Mode; int32 Concurrency; bool bWeb; };
double Percentile(TArray<double> Values, double Fraction)
{
    if (Values.IsEmpty()) return 0.0;
    Values.Sort();
    return Values[FMath::Clamp(FMath::CeilToInt(Values.Num() * Fraction) - 1, 0, Values.Num() - 1)];
}
class FCommunicationCommand final : public IAutomationLatentCommand
{
public:
    explicit FCommunicationCommand(FAutomationTestBase* InTest) : Test(InTest)
    {
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiCommTrials="), Trials);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiCommRequests="), Requests);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiCommReads="), Reads);
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiCommWarmup="), Warmup);
        Trials = FMath::Clamp(Trials, 1, 30); Requests = FMath::Clamp(Requests, 1, 4096);
        Reads = FMath::Clamp(Reads, 1000, 10000000); Warmup = FMath::Clamp(Warmup, 1, 10);
        Output = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Performance")));
        FParse::Value(FCommandLine::Get(), TEXT("RmlUiPerfOutput="), Output);
        IFileManager::Get().MakeDirectory(*Output, true);
        Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            IPluginManager::Get().FindPlugin(TEXT("RmlUiUnreal"))->GetContentDir(), TEXT("Performance")));
        Service.Reset(NewObject<URmlUiCommunicationBenchmark>());
        Service->Object = NewObject<URmlUiCommunicationValue>(Service.Get());
        for (const TCHAR* Mode : { TEXT("event_float"), TEXT("event_property"), TEXT("promise_float"), TEXT("promise_property") })
        {
            Scenarios.Add({ Mode, 1, true });
            Scenarios.Add({ Mode, 32, true });
        }
        for (const TCHAR* Mode : { TEXT("typed_float"), TEXT("typed_property_getter"), TEXT("typed_property_cached"), TEXT("typed_property_resolve") })
            Scenarios.Add({ Mode, 1, false });
    }
    virtual ~FCommunicationCommand() override { Cleanup(); }
    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (!Window.IsValid()) { StartRuntime(); return false; }
        if (!Service->Error.IsEmpty()) return Fail(Service->Error);
        if (Context.IsValid() && !Context->LastError.IsEmpty()) return Fail(Context->LastError);
        if (!Service->bReady)
        {
            if (Now - Started > 45.0) return Fail(TEXT("Communication runtime readiness timeout"));
            return false;
        }
        if (bFinishPending)
        {
            if (Now < NextStart) return false;
            bFinishPending = false;
            FinishScenario();
            ++Scenario;
            Batch = 0;
            if (Scenario == Scenarios.Num()) { SaveReport(); return true; }
            if (Scenarios[Scenario].bWeb != Scenarios[Scenario - 1].bWeb)
            {
                Capture(TEXT("WebBrowser")); Cleanup();
            }
            return false;
        }
        if (bWaiting)
        {
            if (Service->LastReport.IsEmpty())
            {
                if (Now - Started > 120.0) return Fail(TEXT("Communication batch timeout"));
                return false;
            }
            if (!AcceptBatch()) return true;
            bWaiting = false;
            ++Batch;
            // Allow response acknowledgements and the visible result to drain before the next epoch.
            NextStart = Now + 0.12;
            if (Batch == Warmup + Trials)
            {
                bFinishPending = true;
                NextStart = Now + 0.5;
            }
            return false;
        }
        if (Now < NextStart) return false;
        BeginBatch();
        return false;
    }
private:
    void StartRuntime()
    {
        Service->bReady = false;
        Window = SNew(SWindow).Title(FText::FromString(TEXT("RmlUi communication benchmark")))
            .ClientSize(FVector2D(900, 320)).SizingRule(ESizingRule::FixedSize).AutoCenter(EAutoCenter::PreferredWorkArea);
        if (Scenarios[Scenario].bWeb)
        {
            FModuleManager::LoadModuleChecked<IWebBrowserModule>(TEXT("WebBrowser"));
            Browser = SNew(SWebBrowser).InitialURL(TEXT("about:blank")).ShowControls(false).ShowAddressBar(false).BrowserFrameRate(60);
            Browser->BindUObject(TEXT("bench"), Service.Get(), true);
            Service->SendJavascript = [this](const FString& Script) { Browser->ExecuteJavascript(Script); };
            Browser->LoadURL(TEXT("file:///") + FPaths::Combine(Directory, TEXT("communication.html")));
            Window->SetContent(Browser.ToSharedRef());
        }
        else
        {
            Widget = SNew(SRmlUiWidget).UseSlateRenderer(true).DesiredSize(FVector2D(900, 320))
                .SourcePath(TEXT("F:///communication.rml"))
                .InlineDocument(TEXT("<rml><head><style>body{margin:0;padding:24px;width:100%;height:100%;box-sizing:border-box;background-color:#18232d;color:#eef2f4;font-family:LatoLatin;font-size:18px}h1,p,div{display:block;margin:0 0 20px}h1{color:#8fd3c2;font-size:24px}#result{font-size:16px}</style></head><body><h1>RmlUi / Puerts native UObject reads</h1><p>C++ changes the value before every batch; the cached proxy must observe it.</p><div id='result'>Waiting...</div></body></rml>"));
            Window->SetContent(Widget.ToSharedRef());
            Context.Reset(NewObject<URmlUiJSContext>());
            if (!Widget->GetNativeView() || !Context->Initialize(Widget->GetNativeView(), Directory,
                TEXT("communication.js"), TEXT("benchmark"), TEXT("{}"), -1, { { TEXT("bench"), Service.Get() } }))
                Service->Error = TEXT("Puerts initialization failed: ") + Context->LastError;
        }
        FSlateApplication::Get().AddWindow(Window.ToSharedRef());
        Started = FPlatformTime::Seconds();
        NextStart = Started + 1.0;
    }
    void BeginBatch()
    {
        const FScenario& Spec = Scenarios[Scenario];
        Expected = 17.25f + Scenario * 2 + Batch * 0.25f;
        Service->ResetBatch(Expected);
        TSharedRef<FJsonObject> Configuration = MakeShared<FJsonObject>();
        Configuration->SetStringField(TEXT("mode"), Spec.Mode);
        Configuration->SetNumberField(TEXT("count"), Spec.bWeb ? Requests : Reads);
        Configuration->SetNumberField(TEXT("concurrency"), Spec.Concurrency);
        Configuration->SetNumberField(TEXT("expected"), Expected);
        const FString Json = JsonString(Configuration);
        bWaiting = true; Started = FPlatformTime::Seconds();
        UE_LOG(LogTemp, Display, TEXT("COMM_PHASE mode=%s concurrency=%d batch=%d warmup=%d expected=%.2f"),
            *Spec.Mode, Spec.Concurrency, Batch, Batch < Warmup, Expected);
        if (Spec.bWeb) Browser->ExecuteJavascript(TEXT("window.runCommunicationBenchmark(") + Json + TEXT(");"));
        else { TRACE_CPUPROFILER_EVENT_SCOPE(RmlUiBenchmark_PuertsBatch); Service->OnRun.Broadcast(Json); }
    }
    bool AcceptBatch()
    {
        TSharedPtr<FJsonObject> Result;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Service->LastReport), Result) || !Result.IsValid())
        { Fail(TEXT("Invalid communication result JSON")); return false; }
        FString Error;
        if (Result->TryGetStringField(TEXT("error"), Error)) { Fail(Error); return false; }
        const FScenario& Spec = Scenarios[Scenario];
        const int32 Count = Spec.bWeb ? Requests : Reads;
        const double Elapsed = Result->GetNumberField(TEXT("elapsed_ms"));
        bool bValid = Result->GetStringField(TEXT("mode")) == Spec.Mode &&
            Result->GetNumberField(TEXT("count")) == Count && Result->GetNumberField(TEXT("last")) == Expected &&
            Result->GetNumberField(TEXT("sum")) == static_cast<double>(Expected) * Count && Result->GetNumberField(TEXT("mismatches")) == 0 && Elapsed > 0;
        const bool bProperty = Spec.Mode.Contains(TEXT("property"));
        const bool bDirectProperty = Spec.Mode == TEXT("typed_property_cached") || Spec.Mode == TEXT("typed_property_resolve");
        bValid &= Service->ScalarReads == (!bProperty ? Count : 0);
        bValid &= Service->PropertyReads == (bProperty && !bDirectProperty ? Count : 0);
        bValid &= Service->ObjectResolves == (Spec.Mode == TEXT("typed_property_resolve") ? Count : 0);
        bValid &= Service->Requests == (Spec.Mode.StartsWith(TEXT("event_")) ? Count : 0);
        const FString Display = Result->GetStringField(TEXT("display_text"));
        bValid &= Display.Contains(Spec.Mode) && Display.Contains(TEXT("value="));
        if (!Spec.bWeb) bValid &= Context->GetText(Context->FindNode(TEXT("result"))) == Display;
        if (!Test->TestTrue(FString::Printf(TEXT("Verified %s c%d batch%d values, counters and rendered text"), *Spec.Mode, Spec.Concurrency, Batch), bValid))
        {
            Fail(FString::Printf(TEXT("Bad batch: %s scalar=%lld property=%lld object=%lld requests=%lld"),
                *Service->LastReport, Service->ScalarReads, Service->PropertyReads, Service->ObjectResolves, Service->Requests));
            return false;
        }
        ++VerifiedBatches;
        if (Batch < Warmup) return true;
        Result->SetNumberField(TEXT("expected"), Expected);
        Result->SetNumberField(TEXT("batch_mean_ns_per_read"), Elapsed * 1000000.0 / Count);
        Result->SetNumberField(TEXT("completed_reads_per_second"), Count * 1000.0 / Elapsed);
        Result->SetNumberField(TEXT("scalar_reads"), Service->ScalarReads);
        Result->SetNumberField(TEXT("property_getter_reads"), Service->PropertyReads);
        Result->SetNumberField(TEXT("object_resolves"), Service->ObjectResolves);
        Result->SetNumberField(TEXT("event_requests"), Service->Requests);
        Result->SetNumberField(TEXT("request_json_utf8_bytes"), Service->RequestBytes);
        Result->SetNumberField(TEXT("response_json_utf8_bytes"), Service->ResponseBytes);
        Result->SetNumberField(TEXT("native_parse_ms"), FPlatformTime::ToMilliseconds64(Service->ParseCycles));
        Result->SetNumberField(TEXT("native_read_ms"), FPlatformTime::ToMilliseconds64(Service->ReadCycles));
        Result->SetNumberField(TEXT("native_serialize_ms"), FPlatformTime::ToMilliseconds64(Service->SerializeCycles));
        Result->SetNumberField(TEXT("native_enqueue_ms"), FPlatformTime::ToMilliseconds64(Service->EnqueueCycles));
        TrialResults.Add(MakeShared<FJsonValueObject>(Result));
        Means.Add(Elapsed * 1000000.0 / Count);
        Rates.Add(Count * 1000.0 / Elapsed);
        for (const auto& Value : Result->GetArrayField(TEXT("latencies_ms"))) Latencies.Add(Value->AsNumber());
        return true;
    }
    void FinishScenario()
    {
        const FScenario& Spec = Scenarios[Scenario];
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("mode"), Spec.Mode);
        Result->SetStringField(TEXT("runtime"), Spec.bWeb ? TEXT("WebBrowser_CEF") : TEXT("RmlUi_Puerts"));
        Result->SetNumberField(TEXT("concurrency"), Spec.Concurrency);
        Result->SetNumberField(TEXT("reads_per_trial"), Spec.bWeb ? Requests : Reads);
        Result->SetNumberField(TEXT("batch_mean_ns_per_read_p50"), Percentile(Means, 0.5));
        Result->SetNumberField(TEXT("batch_mean_ns_per_read_p95"), Percentile(Means, 0.95));
        Result->SetNumberField(TEXT("completed_reads_per_second_p50"), Percentile(Rates, 0.5));
        if (Spec.bWeb)
        {
            Result->SetNumberField(TEXT("round_trip_ms_p50"), Percentile(Latencies, 0.5));
            Result->SetNumberField(TEXT("round_trip_ms_p95"), Percentile(Latencies, 0.95));
            Result->SetNumberField(TEXT("round_trip_ms_p99"), Percentile(Latencies, 0.99));
        }
        Result->SetArrayField(TEXT("trials"), TrialResults);
        Results.Add(MakeShared<FJsonValueObject>(Result));
        UE_LOG(LogTemp, Display, TEXT("COMM_RESULT %s c%d mean_p50=%.2f ns/read rate_p50=%.2f reads/s"),
            *Spec.Mode, Spec.Concurrency, Percentile(Means, 0.5), Percentile(Rates, 0.5));
        TrialResults.Reset(); Means.Reset(); Rates.Reset(); Latencies.Reset();
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels; FIntVector Size;
        if (Test->TestTrue(TEXT("Communication result screenshot"),
            FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size)))
        {
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(Size.X, Size.Y,
                TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
            Test->TestTrue(TEXT("Save communication screenshot"), FFileHelper::SaveArrayToFile(Png,
                *FPaths::Combine(Output, FString::Printf(TEXT("Communication-%s.png"), Name))));
        }
    }
    void SaveReport()
    {
        Capture(TEXT("Puerts"));
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("schema"), 1);
        Root->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
        Root->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("Unknown"));
        Root->SetStringField(TEXT("gpu_adapter"), GRHIAdapterName);
        Root->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
        Root->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
        Root->SetNumberField(TEXT("trials_per_scenario"), Trials);
        Root->SetNumberField(TEXT("warmup_batches_per_scenario"), Warmup);
        Root->SetNumberField(TEXT("verified_batches"), VerifiedBatches);
        Root->SetBoolField(TEXT("verified"), VerifiedBatches == Scenarios.Num() * (Trials + Warmup));
        Root->SetStringField(TEXT("timing_scope"), TEXT("Web: JS request-to-response RTT includes CEF/frame queues; Puerts: warmed synchronous loop batch, two clock boundary calls, no per-read clock. Both include loop bookkeeping; UI text update/report excluded."));
        Root->SetStringField(TEXT("event_transport"), TEXT("JSON.stringify -> bound Request(FString) -> UE parse/read/serialize -> ExecuteJavascript CustomEvent(JSON string) -> JSON.parse. Bound void Request additionally produces an unused official Promise ACK."));
        Root->SetStringField(TEXT("property_scope"), TEXT("Same native UObject and changed float epochs. Web uses native GetObjectValue (CEF exposes methods); Puerts measures the same getter plus cached UObject.Value and GetObject().Value. Direct property validation uses changed epochs and checksums, not a property getter counter."));
        Root->SetArrayField(TEXT("scenarios"), Results);
        const FString Path = FPaths::Combine(Output, TEXT("CommunicationComparison.json"));
        Test->TestTrue(TEXT("Save communication comparison"), FFileHelper::SaveStringToFile(JsonString(Root), *Path));
        Test->AddInfo(TEXT("Communication comparison: ") + Path);
    }
    bool Fail(const FString& Error) { Test->AddError(Error); Cleanup(); return true; }
    void Cleanup()
    {
        Service->SendJavascript = nullptr;
        Service->OnRun.Clear();
        if (Context.IsValid()) { Context->Dispose(); Context.Reset(); }
        if (Widget.IsValid()) Widget->ShutdownNative();
        if (Browser.IsValid()) Browser->UnbindUObject(TEXT("bench"), Service.Get(), true);
        if (Window.IsValid() && FSlateApplication::IsInitialized()) FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
        Widget.Reset(); Browser.Reset(); Window.Reset();
    }
    FAutomationTestBase* Test;
    TStrongObjectPtr<URmlUiCommunicationBenchmark> Service;
    TStrongObjectPtr<URmlUiJSContext> Context;
    TSharedPtr<SWindow> Window;
    TSharedPtr<SWebBrowser> Browser;
    TSharedPtr<SRmlUiWidget> Widget;
    TArray<FScenario> Scenarios;
    TArray<TSharedPtr<FJsonValue>> Results, TrialResults;
    TArray<double> Means, Rates, Latencies;
    FString Directory, Output;
    int32 Scenario = 0, Batch = 0, Trials = 3, Requests = 32, Reads = 20000, Warmup = 1, VerifiedBatches = 0;
    float Expected = 0;
    double Started = 0, NextStart = 0;
    bool bWaiting = false, bFinishPending = false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRmlUiCommunicationTest, "RmlUiUnreal.Performance.Communication",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRmlUiCommunicationTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized() || !GDynamicRHI) { AddError(TEXT("Requires a windowed UE editor with RHI")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(RmlUiCommunication::FCommunicationCommand(this));
    return true;
}
#endif
