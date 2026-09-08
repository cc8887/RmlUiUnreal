#include "RmlUiBridge.h"
#include "RmlUi_Renderer_DX11.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Debugger.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef LoadImage
#undef LoadImage
#endif

using Microsoft::WRL::ComPtr;

namespace {
RmlUE_Host Host{};
std::string LastError;
std::thread::id OwnerThread;
bool Initialized = false;
uint64_t NextView = 0;
RmlUE_Stats* ActiveStats = nullptr;
ComPtr<ID3D11Device> Device;
ComPtr<ID3D11DeviceContext> DeviceContext;
std::set<RmlUE_View*> Views;
RmlUE_View* DebuggerView = nullptr;

int Fail(const std::string& Message)
{
    LastError = Message;
    if (Host.Log) Host.Log(Host.User, 0, Message.c_str());
    return 0;
}

bool OnOwnerThread()
{
    if (OwnerThread != std::this_thread::get_id())
        return Fail("RmlUi bridge called from a thread other than its initialization thread.") != 0;
    return true;
}

bool ValidDimensions(int Width, int Height, float DpRatio)
{
    return Width > 0 && Height > 0 && Width <= 4096 && Height <= 4096 && std::isfinite(DpRatio) && DpRatio > 0;
}

void CopyString(char* Destination, size_t Capacity, const std::string& Source)
{
    if (!Destination || Capacity == 0) return;
    size_t Count = std::min(Capacity - 1, Source.size());
    // Avoid truncating in the middle of a UTF-8 code point.
    if (Count < Source.size())
        while (Count > 0 && (static_cast<unsigned char>(Source[Count]) & 0xc0) == 0x80) --Count;
    std::memcpy(Destination, Source.data(), Count);
    Destination[Count] = 0;
}

class BridgeFileInterface final : public Rml::FileInterface {
    struct File { std::vector<unsigned char> Bytes; size_t Offset = 0; };
public:
    Rml::FileHandle Open(const Rml::String& Path) override
    {
        unsigned char* Buffer = nullptr;
        size_t Size = 0;
        const int Loaded = Host.ReadFile(Host.User, Path.c_str(), &Buffer, &Size);
        std::unique_ptr<File> Result;
        if (Loaded && (Buffer || Size == 0))
        {
            Result = std::make_unique<File>();
            if (Size) Result->Bytes.assign(Buffer, Buffer + Size);
        }
        if (Buffer) Host.FreeBuffer(Host.User, Buffer);
        return reinterpret_cast<Rml::FileHandle>(Result.release());
    }
    void Close(Rml::FileHandle Handle) override { delete reinterpret_cast<File*>(Handle); }
    size_t Read(void* Buffer, size_t Size, Rml::FileHandle Handle) override
    {
        auto* FileData = reinterpret_cast<File*>(Handle);
        if (!FileData || !Buffer) return 0;
        const size_t Count = std::min(Size, FileData->Bytes.size() - FileData->Offset);
        if (Count) std::memcpy(Buffer, FileData->Bytes.data() + FileData->Offset, Count);
        FileData->Offset += Count;
        return Count;
    }
    bool Seek(Rml::FileHandle Handle, long Offset, int Origin) override
    {
        auto* FileData = reinterpret_cast<File*>(Handle);
        if (!FileData || (Origin != SEEK_SET && Origin != SEEK_END && Origin != SEEK_CUR)) return false;
        const int64_t Base = Origin == SEEK_SET ? 0 : static_cast<int64_t>(Origin == SEEK_END ? FileData->Bytes.size() : FileData->Offset);
        const int64_t Target = Base + Offset;
        if (Target < 0 || static_cast<uint64_t>(Target) > FileData->Bytes.size()) return false;
        FileData->Offset = static_cast<size_t>(Target);
        return true;
    }
    size_t Tell(Rml::FileHandle Handle) override { return reinterpret_cast<File*>(Handle)->Offset; }
    size_t Length(Rml::FileHandle Handle) override { return reinterpret_cast<File*>(Handle)->Bytes.size(); }
};

class BridgeSystemInterface final : public Rml::SystemInterface {
    std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
public:
    double GetElapsedTime() override { return std::chrono::duration<double>(std::chrono::steady_clock::now() - Started).count(); }
    bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override
    {
        if (Type == Rml::Log::LT_ERROR || Type == Rml::Log::LT_ASSERT) LastError = Message;
        if (Host.Log) Host.Log(Host.User, (Type == Rml::Log::LT_ERROR || Type == Rml::Log::LT_ASSERT) ? 0 : Type == Rml::Log::LT_WARNING ? 1 : 2, Message.c_str());
        return true;
    }
    void SetClipboardText(const Rml::String& Text) override { if (Host.SetClipboard) Host.SetClipboard(Host.User, Text.c_str()); }
    void GetClipboardText(Rml::String& Text) override
    {
        Text.clear();
        if (!Host.GetClipboard) return;
        unsigned char* Buffer = nullptr;
        size_t Size = 0;
        if (Host.GetClipboard(Host.User, &Buffer, &Size) && Buffer)
            Text.assign(reinterpret_cast<const char*>(Buffer), Size);
        if (Buffer) Host.FreeBuffer(Host.User, Buffer);
    }
};

class BridgeRenderer final : public RenderInterface_DX11 {
public:
    explicit BridgeRenderer(ID3D11Device* InDevice) : RenderInterface_DX11(InDevice) {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& Dimensions, const Rml::String& Source) override
    {
        unsigned char* Buffer = nullptr;
        int Width = 0, Height = 0;
        if (!Host.LoadImage(Host.User, Source.c_str(), &Buffer, &Width, &Height) || !Buffer || Width <= 0 || Height <= 0 || Width > 16384 || Height > 16384)
        {
            if (Buffer) Host.FreeBuffer(Host.User, Buffer);
            Fail("Could not decode image: " + Source);
            return 0;
        }
        const size_t Size = static_cast<size_t>(Width) * Height * 4;
        std::vector<unsigned char> Pixels(Buffer, Buffer + Size);
        Host.FreeBuffer(Host.User, Buffer);
        for (size_t Index = 0; Index < Size; Index += 4)
            for (int Channel = 0; Channel < 3; ++Channel)
                Pixels[Index + Channel] = static_cast<unsigned char>((static_cast<unsigned>(Pixels[Index + Channel]) * Pixels[Index + 3] + 127) / 255);
        Dimensions = {Width, Height};
        const auto Handle = GenerateTexture({Pixels.data(), Pixels.size()}, Dimensions);
        if (Handle && ActiveStats) ++ActiveStats->LoadedTextures;
        return Handle;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture) override
    {
        if (ActiveStats) ++ActiveStats->GeometryDraws;
        RenderInterface_DX11::RenderGeometry(Geometry, Translation, Texture);
    }
    void RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation) override
    {
        if (ActiveStats) ++ActiveStats->ClipMasks;
        RenderInterface_DX11::RenderToClipMask(Operation, Geometry, Translation);
    }
    Rml::LayerHandle PushLayer() override
    {
        if (ActiveStats) ++ActiveStats->Layers;
        return RenderInterface_DX11::PushLayer();
    }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& Name, const Rml::Dictionary& Parameters) override
    {
        auto Result = RenderInterface_DX11::CompileFilter(Name, Parameters);
        if (Result && ActiveStats) ++ActiveStats->Filters;
        return Result;
    }
    Rml::CompiledShaderHandle CompileShader(const Rml::String& Name, const Rml::Dictionary& Parameters) override
    {
        auto Result = RenderInterface_DX11::CompileShader(Name, Parameters);
        if (Result && ActiveStats) ++ActiveStats->Shaders;
        return Result;
    }
};

std::unique_ptr<BridgeFileInterface> FileInterface;
std::unique_ptr<BridgeSystemInterface> SystemInterface;
std::unique_ptr<BridgeRenderer> Renderer;
}

struct RmlUE_View final : public Rml::EventListener {
    Rml::Context* Context = nullptr;
    Rml::ElementDocument* Document = nullptr;
    std::string Name;
    int Width = 0, Height = 0;
    float DpRatio = 1;
    uint64_t FrameNumber = 0;
    RmlUE_Stats Stats{};
    ComPtr<ID3D11Texture2D> Target;
    ComPtr<ID3D11Texture2D> Staging;
    ComPtr<ID3D11RenderTargetView> TargetView;
    std::vector<unsigned char> Pixels;
    std::deque<RmlUE_Event> Events;
    std::set<int> PressedButtons;
    std::set<int> PressedKeys;
    std::set<int> Touches;

    void ProcessEvent(Rml::Event& Event) override
    {
        auto* Element = Event.GetTargetElement();
        if (!Element || !Document || Element->GetOwnerDocument() != Document) return;
        RmlUE_Event Output{};
        CopyString(Output.Type, sizeof(Output.Type), Event.GetType());
        auto* IdElement = Element;
        while (IdElement && IdElement->GetId().empty() && IdElement != Document) IdElement = IdElement->GetParentNode();
        if (IdElement) CopyString(Output.ElementId, sizeof(Output.ElementId), IdElement->GetId());
        const auto* FormControl = rmlui_dynamic_cast<Rml::ElementFormControl*>(Element);
        auto Value = FormControl ? FormControl->GetValue() : Event.GetParameter<Rml::String>("value", Element->GetAttribute<Rml::String>("value", ""));
        const auto InputType = Element->GetAttribute<Rml::String>("type", "");
        if (Element->GetTagName() == "input" && (InputType == "checkbox" || InputType == "radio"))
            Value = Element->HasAttribute("checked") ? "true" : "false";
        CopyString(Output.Value, sizeof(Output.Value), Value);
        if (Events.size() == 1024) Events.pop_front();
        Events.push_back(Output);
    }
};

namespace {
bool ValidView(RmlUE_View* View)
{
    if (!Initialized || !OnOwnerThread()) return false;
    if (!View || Views.find(View) == Views.end()) return Fail("Invalid or destroyed RmlUi view.") != 0;
    ActiveStats = &View->Stats;
    return true;
}

bool CreateTargets(RmlUE_View& View, int Width, int Height)
{
    D3D11_TEXTURE2D_DESC Description{};
    Description.Width = Width;
    Description.Height = Height;
    Description.MipLevels = 1;
    Description.ArraySize = 1;
    Description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Description.SampleDesc.Count = 1;
    Description.Usage = D3D11_USAGE_DEFAULT;
    Description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> NewTarget, NewStaging;
    ComPtr<ID3D11RenderTargetView> NewView;
    if (FAILED(Device->CreateTexture2D(&Description, nullptr, &NewTarget)) || FAILED(Device->CreateRenderTargetView(NewTarget.Get(), nullptr, &NewView)))
        return Fail("Failed to create RmlUi offscreen DX11 render target.") != 0;
    Description.Usage = D3D11_USAGE_STAGING;
    Description.BindFlags = 0;
    Description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(Device->CreateTexture2D(&Description, nullptr, &NewStaging)))
        return Fail("Failed to create RmlUi DX11 readback texture.") != 0;
    View.Target = std::move(NewTarget);
    View.Staging = std::move(NewStaging);
    View.TargetView = std::move(NewView);
    View.Width = Width;
    View.Height = Height;
    View.Pixels.resize(static_cast<size_t>(Width) * Height * 4);
    return true;
}

int Modifiers(int Flags)
{
    using namespace Rml::Input;
    return ((Flags & 1) ? KM_SHIFT : 0) | ((Flags & 2) ? KM_CTRL : 0) | ((Flags & 4) ? KM_ALT : 0) |
        ((Flags & 8) ? KM_CAPSLOCK : 0) | ((Flags & 16) ? KM_NUMLOCK : 0) | ((Flags & 32) ? KM_META : 0);
}

Rml::Input::KeyIdentifier KeyIdentifier(int Key)
{
    using namespace Rml::Input;
    if (Key >= 'A' && Key <= 'Z') return static_cast<Rml::Input::KeyIdentifier>(KI_A + Key - 'A');
    if (Key >= '0' && Key <= '9') return static_cast<Rml::Input::KeyIdentifier>(KI_0 + Key - '0');
    if (Key >= VK_F1 && Key <= VK_F24) return static_cast<Rml::Input::KeyIdentifier>(KI_F1 + Key - VK_F1);
    if (Key >= VK_NUMPAD0 && Key <= VK_NUMPAD9) return static_cast<Rml::Input::KeyIdentifier>(KI_NUMPAD0 + Key - VK_NUMPAD0);
    switch (Key)
    {
        case VK_BACK: return KI_BACK; case VK_TAB: return KI_TAB; case VK_CLEAR: return KI_CLEAR;
        case VK_RETURN: return KI_RETURN; case VK_PAUSE: return KI_PAUSE; case VK_CAPITAL: return KI_CAPITAL;
        case VK_ESCAPE: return KI_ESCAPE; case VK_SPACE: return KI_SPACE; case VK_PRIOR: return KI_PRIOR;
        case VK_NEXT: return KI_NEXT; case VK_END: return KI_END; case VK_HOME: return KI_HOME;
        case VK_LEFT: return KI_LEFT; case VK_UP: return KI_UP; case VK_RIGHT: return KI_RIGHT; case VK_DOWN: return KI_DOWN;
        case VK_INSERT: return KI_INSERT; case VK_DELETE: return KI_DELETE; case VK_LWIN: return KI_LWIN; case VK_RWIN: return KI_RWIN;
        case VK_MULTIPLY: return KI_MULTIPLY; case VK_ADD: return KI_ADD; case VK_SUBTRACT: return KI_SUBTRACT;
        case VK_DECIMAL: return KI_DECIMAL; case VK_DIVIDE: return KI_DIVIDE;
        case VK_NUMLOCK: return KI_NUMLOCK; case VK_SCROLL: return KI_SCROLL;
        case VK_SHIFT: case VK_LSHIFT: return KI_LSHIFT; case VK_RSHIFT: return KI_RSHIFT;
        case VK_CONTROL: case VK_LCONTROL: return KI_LCONTROL; case VK_RCONTROL: return KI_RCONTROL;
        case VK_MENU: case VK_LMENU: return KI_LMENU; case VK_RMENU: return KI_RMENU;
        case VK_OEM_1: return KI_OEM_1; case VK_OEM_PLUS: return KI_OEM_PLUS; case VK_OEM_COMMA: return KI_OEM_COMMA;
        case VK_OEM_MINUS: return KI_OEM_MINUS; case VK_OEM_PERIOD: return KI_OEM_PERIOD; case VK_OEM_2: return KI_OEM_2;
        case VK_OEM_3: return KI_OEM_3; case VK_OEM_4: return KI_OEM_4; case VK_OEM_5: return KI_OEM_5;
        case VK_OEM_6: return KI_OEM_6; case VK_OEM_7: return KI_OEM_7; case VK_OEM_102: return KI_OEM_102;
        default: return KI_UNKNOWN;
    }
}

int AdoptDocument(RmlUE_View& View, Rml::ElementDocument* NewDocument)
{
    if (!NewDocument) return Fail("RmlUi could not load the document; the existing document was preserved. " + LastError);
    if (View.Document)
    {
        View.Document->RemoveEventListener("click", &View);
        View.Document->RemoveEventListener("change", &View);
        View.Document->RemoveEventListener("submit", &View);
        View.Document->Close();
    }
    View.Document = NewDocument;
    View.Events.clear();
    NewDocument->AddEventListener("click", &View);
    NewDocument->AddEventListener("change", &View);
    NewDocument->AddEventListener("submit", &View);
    Rml::ReleaseTextures();
    NewDocument->Show();
    View.Context->Update();
    return 1;
}

Rml::Element* FindElement(RmlUE_View* View, const char* Id)
{
    if (!ValidView(View) || !View->Document || !Id) return nullptr;
    auto* Result = View->Document->GetElementById(Id);
    if (!Result) Fail(std::string("Element id was not found: ") + Id);
    return Result;
}
}

int RmlUE_Initialize(const RmlUE_Host* InHost)
{
    if (Initialized) return OnOwnerThread() ? 1 : 0;
    if (!InHost || !InHost->ReadFile || !InHost->LoadImage || !InHost->FreeBuffer) return Fail("Required RmlUi host callbacks are missing.");
    Host = *InHost;
    OwnerThread = std::this_thread::get_id();
    LastError.clear();
    const auto InitializationFailure = [](const std::string& Message)
    {
        Fail(Message);
        RmlUE_Shutdown();
        return 0;
    };
    try
    {
        const D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL Selected{};
        HRESULT Result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Levels, 1, D3D11_SDK_VERSION, &Device, &Selected, &DeviceContext);
        if (FAILED(Result))
        {
            Result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                Levels, 1, D3D11_SDK_VERSION, &Device, &Selected, &DeviceContext);
            if (SUCCEEDED(Result) && Host.Log) Host.Log(Host.User, 1, "RmlUi reference renderer is using DX11 WARP software fallback.");
        }
        if (FAILED(Result)) return InitializationFailure("D3D11 hardware and WARP device creation both failed: " + std::to_string(static_cast<unsigned>(Result)));
        ComPtr<ID3D11Device1> Device1;
        if (FAILED(Device.As(&Device1))) return InitializationFailure("RmlUi renderer requires Direct3D 11.1 interfaces (Windows 8 or later).");
        FileInterface = std::make_unique<BridgeFileInterface>();
        SystemInterface = std::make_unique<BridgeSystemInterface>();
        Rml::SetFileInterface(FileInterface.get());
        Rml::SetSystemInterface(SystemInterface.get());
        Renderer = std::make_unique<BridgeRenderer>(Device.Get());
        Rml::SetRenderInterface(Renderer.get());
        if (!Rml::Initialise()) return InitializationFailure("RmlUi initialization failed: " + LastError);
        Initialized = true;
        if (Host.Log) Host.Log(Host.User, 2, "RmlUi 6.3 initialized with the full upstream DX11 renderer and FreeType 2.14.3.");
        return 1;
    }
    catch (const std::exception& Error) { return InitializationFailure(std::string("RmlUi initialization exception: ") + Error.what()); }
}

void RmlUE_Shutdown()
{
    if (!OnOwnerThread()) return;
    while (!Views.empty()) RmlUE_DestroyView(*Views.begin());
    ActiveStats = nullptr;
    if (Initialized) Rml::Shutdown();
    Initialized = false;
    Renderer.reset();
    Rml::SetRenderInterface(nullptr);
    Rml::SetFileInterface(nullptr);
    Rml::SetSystemInterface(nullptr);
    FileInterface.reset();
    SystemInterface.reset();
    if (DeviceContext) DeviceContext->ClearState();
    DeviceContext.Reset();
    Device.Reset();
    Host = {};
}

const char* RmlUE_GetLastError() { return LastError.c_str(); }
const char* RmlUE_GetVersion() { return "RmlUi 6.3 / FreeType 2.14.3 / full DX11 offscreen reference backend"; }
int RmlUE_LoadFont(const char* Path, int Fallback)
{
    return Path && Initialized && OnOwnerThread() && Rml::LoadFontFace(Path, Fallback != 0) ? 1 : Fail("Could not load RmlUi font.");
}

RmlUE_View* RmlUE_CreateView(int Width, int Height, float DpRatio)
{
    if (!Initialized || !OnOwnerThread() || !ValidDimensions(Width, Height, DpRatio)) { Fail("View dimensions must be 1..4096 and DPR finite and positive."); return nullptr; }
    auto View = std::make_unique<RmlUE_View>();
    View->Name = "unreal-" + std::to_string(++NextView);
    View->DpRatio = DpRatio;
    if (!CreateTargets(*View, Width, Height)) return nullptr;
    View->Context = Rml::CreateContext(View->Name, {Width, Height});
    if (!View->Context) { Fail("Could not create RmlUi context."); return nullptr; }
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    Views.insert(View.get());
    return View.release();
}

void RmlUE_DestroyView(RmlUE_View* View)
{
    if (!ValidView(View)) return;
    if (DebuggerView == View) { Rml::Debugger::Shutdown(); DebuggerView = nullptr; }
    Rml::RemoveContext(View->Name);
    Views.erase(View);
    ActiveStats = nullptr;
    delete View;
}

int RmlUE_LoadDocument(RmlUE_View* View, const char* Path)
{
    if (!ValidView(View) || !Path) return 0;
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    return AdoptDocument(*View, View->Context->LoadDocument(Path));
}

int RmlUE_LoadDocumentFromMemory(RmlUE_View* View, const char* Markup, const char* Source)
{
    if (!ValidView(View) || !Markup) return 0;
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    return AdoptDocument(*View, View->Context->LoadDocumentFromMemory(Markup, Source ? Source : "memory.rml"));
}

int RmlUE_Resize(RmlUE_View* View, int Width, int Height, float DpRatio)
{
    if (!ValidView(View)) return 0;
    if (!ValidDimensions(Width, Height, DpRatio)) return Fail("View dimensions must be 1..4096 and DPR finite and positive.");
    if ((Width != View->Width || Height != View->Height) && !CreateTargets(*View, Width, Height)) return 0;
    View->DpRatio = DpRatio;
    View->Context->SetDimensions({Width, Height});
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    return 1;
}

int RmlUE_Render(RmlUE_View* View, RmlUE_Frame* Frame)
{
    if (Frame) *Frame = {};
    if (!ValidView(View) || !Frame) return 0;
    Renderer->SetViewport(View->Width, View->Height);
    View->Context->Update();
    const float ClearColor[4] = {};
    DeviceContext->ClearRenderTargetView(View->TargetView.Get(), ClearColor);
    Renderer->BeginFrame();
    View->Context->Render();
    Renderer->EndFrame(View->TargetView.Get());
    DeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    DeviceContext->CopyResource(View->Staging.Get(), View->Target.Get());
    D3D11_MAPPED_SUBRESOURCE Mapped{};
    HRESULT Result = DeviceContext->Map(View->Staging.Get(), 0, D3D11_MAP_READ, 0, &Mapped);
    if (FAILED(Result)) return Fail("RmlUi DX11 readback failed: " + std::to_string(static_cast<unsigned>(Result)));
    for (int Y = 0; Y < View->Height; ++Y)
    {
        const auto* Source = static_cast<const unsigned char*>(Mapped.pData) + static_cast<size_t>(Y) * Mapped.RowPitch;
        auto* Destination = View->Pixels.data() + static_cast<size_t>(Y) * View->Width * 4;
        for (int X = 0; X < View->Width; ++X)
        {
            const unsigned Alpha = Source[X * 4 + 3];
            for (int Channel = 0; Channel < 3; ++Channel)
                Destination[X * 4 + Channel] = Alpha ? static_cast<unsigned char>(std::min(255u, (Source[X * 4 + Channel] * 255u + Alpha / 2) / Alpha)) : 0;
            Destination[X * 4 + 3] = static_cast<unsigned char>(Alpha);
        }
    }
    DeviceContext->Unmap(View->Staging.Get(), 0);
    *Frame = {View->Pixels.data(), View->Width, View->Height, ++View->FrameNumber};
    return 1;
}

int RmlUE_PollEvent(RmlUE_View* View, RmlUE_Event* Event)
{
    if (!ValidView(View) || !Event || View->Events.empty()) return 0;
    *Event = View->Events.front(); View->Events.pop_front(); return 1;
}
int RmlUE_SetInnerRml(RmlUE_View* View, const char* Id, const char* Markup)
{
    auto* Element = FindElement(View, Id); if (!Element || !Markup) return 0; Element->SetInnerRML(Markup); return 1;
}
int RmlUE_SetProperty(RmlUE_View* View, const char* Id, const char* Property, const char* Value)
{
    auto* Element = FindElement(View, Id); return Element && Property && Value && Element->SetProperty(Property, Value) ? 1 : 0;
}
int RmlUE_SetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, const char* Value)
{
    auto* Element = FindElement(View, Id); if (!Element || !Attribute || !Value) return 0;
    const bool BooleanAttribute = std::strcmp(Attribute, "checked") == 0 || std::strcmp(Attribute, "disabled") == 0 || std::strcmp(Attribute, "selected") == 0;
    if (BooleanAttribute && (std::strcmp(Value, "false") == 0 || std::strcmp(Value, "0") == 0)) Element->RemoveAttribute(Attribute);
    else Element->SetAttribute(Attribute, Rml::String(Value));
    return 1;
}
int RmlUE_GetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, char* Value, size_t Capacity)
{
    if (Value && Capacity) Value[0] = 0;
    auto* Element = FindElement(View, Id); if (!Element || !Attribute || !Value || !Capacity) return 0;
    if (std::strcmp(Attribute, "checked") == 0 || std::strcmp(Attribute, "disabled") == 0 || std::strcmp(Attribute, "selected") == 0)
    {
        CopyString(Value, Capacity, Element->HasAttribute(Attribute) ? "true" : "false");
        return 1;
    }
    auto* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Element);
    CopyString(Value, Capacity, std::strcmp(Attribute, "value") == 0 && Control ? Control->GetValue() : Element->GetAttribute<Rml::String>(Attribute, ""));
    return 1;
}
int RmlUE_GetElementRect(RmlUE_View* View, const char* Id, RmlUE_Rect* Rect)
{
    auto* Element = FindElement(View, Id); if (!Element || !Rect) return 0;
    View->Context->Update();
    auto Offset = Element->GetAbsoluteOffset(Rml::BoxArea::Border);
    auto Size = Element->GetBox().GetSize(Rml::BoxArea::Border);
    *Rect = {Offset.x, Offset.y, Size.x, Size.y}; return 1;
}
void RmlUE_GetStats(RmlUE_View* View, RmlUE_Stats* Stats) { if (Stats) *Stats = ValidView(View) ? View->Stats : RmlUE_Stats{}; }
void RmlUE_SetDebuggerVisible(RmlUE_View* View, int Visible)
{
    if (!ValidView(View)) return;
    if (Visible && DebuggerView != View)
    {
        if (DebuggerView) Rml::Debugger::Shutdown();
        DebuggerView = Rml::Debugger::Initialise(View->Context) ? View : nullptr;
    }
    if (DebuggerView == View) Rml::Debugger::SetVisible(Visible != 0);
}
void RmlUE_MouseMove(RmlUE_View* View, int X, int Y, int Flags) { if (ValidView(View)) View->Context->ProcessMouseMove(X, Y, Modifiers(Flags)); }
void RmlUE_MouseButton(RmlUE_View* View, int Button, int Down, int Flags)
{
    if (!ValidView(View) || Button < 0 || Button > 4) return;
    if (Down) { View->PressedButtons.insert(Button); View->Context->ProcessMouseButtonDown(Button, Modifiers(Flags)); }
    else { View->PressedButtons.erase(Button); View->Context->ProcessMouseButtonUp(Button, Modifiers(Flags)); }
}
void RmlUE_MouseWheel(RmlUE_View* View, float Delta, int Flags) { if (ValidView(View) && std::isfinite(Delta)) View->Context->ProcessMouseWheel({0, -Delta}, Modifiers(Flags)); }
void RmlUE_MouseLeave(RmlUE_View* View) { if (ValidView(View)) View->Context->ProcessMouseLeave(); }
void RmlUE_Key(RmlUE_View* View, int Key, int Down, int Flags)
{
    if (!ValidView(View)) return;
    const auto Identifier = KeyIdentifier(Key);
    if (Identifier == Rml::Input::KI_UNKNOWN) return;
    if (Down) { View->PressedKeys.insert(Key); View->Context->ProcessKeyDown(Identifier, Modifiers(Flags)); }
    else { View->PressedKeys.erase(Key); View->Context->ProcessKeyUp(Identifier, Modifiers(Flags)); }
}
void RmlUE_Text(RmlUE_View* View, const char* Text) { if (ValidView(View) && Text) View->Context->ProcessTextInput(Rml::String(Text)); }
void RmlUE_FocusLost(RmlUE_View* View)
{
    if (!ValidView(View)) return;
    View->Context->ProcessMouseLeave();
    for (int Key : View->PressedKeys) View->Context->ProcessKeyUp(KeyIdentifier(Key), 0);
    for (int Button : View->PressedButtons) View->Context->ProcessMouseButtonUp(Button, 0);
    Rml::TouchList Cancelled;
    for (int Id : View->Touches) Cancelled.push_back({static_cast<Rml::TouchId>(Id), {0, 0}});
    if (!Cancelled.empty()) View->Context->ProcessTouchCancel(Cancelled);
    View->PressedKeys.clear(); View->PressedButtons.clear(); View->Touches.clear();
    View->Context->ProcessMouseLeave();
    if (auto* Focused = View->Context->GetFocusElement()) Focused->Blur();
}
void RmlUE_Touch(RmlUE_View* View, int Id, float X, float Y, int Phase)
{
    if (!ValidView(View) || Id < 0 || !std::isfinite(X) || !std::isfinite(Y)) return;
    const Rml::TouchList Touches = {{static_cast<Rml::TouchId>(Id), {X, Y}}};
    switch (Phase)
    {
        case 0: View->Touches.insert(Id); View->Context->ProcessTouchStart(Touches, 0); break;
        case 1: View->Context->ProcessTouchMove(Touches, 0); break;
        case 2: View->Touches.erase(Id); View->Context->ProcessTouchEnd(Touches, 0); break;
        case 3: View->Touches.erase(Id); View->Context->ProcessTouchCancel(Touches); break;
        default: break;
    }
}
