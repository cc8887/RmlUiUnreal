#include "RmlUiBridge.h"
#include "RmlUi_Renderer_DX11.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/Texture.h>
#include <RmlUi/Core/StyleSheetContainer.h>
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
#include <unordered_map>

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
uint32_t NextNode = 0;
RmlUE_Stats* ActiveStats = nullptr;
bool CapturingStyleSheetDiagnostics = false;
std::string StyleSheetDiagnostics;
ComPtr<ID3D11Device> Device;
ComPtr<ID3D11DeviceContext> DeviceContext;
std::set<RmlUE_View*> Views;
std::set<RmlUE_StyleSheet*> StyleSheets;
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
        if (CapturingStyleSheetDiagnostics &&
            (Type == Rml::Log::LT_WARNING || Type == Rml::Log::LT_ERROR || Type == Rml::Log::LT_ASSERT) && StyleSheetDiagnostics.empty())
            StyleSheetDiagnostics = Message;
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

class SlateCommandRenderer final : public Rml::RenderInterface {
    struct GeometryData {
        std::vector<RmlUE_SlateVertex> Vertices;
        std::vector<uint32_t> Indices;
    };
    struct TextureData {
        uint64_t Id = 0;
        int Kind = 0;
        int Width = 0, Height = 0;
        std::vector<unsigned char> Pixels;
        std::string Alias;
    };
    uint64_t NextTexture = 0;
    bool ScissorEnabled = false;
    Rml::Rectanglei Scissor{};
public:
    std::vector<RmlUE_SlateDraw> Draws;
    std::unordered_map<uint64_t, TextureData> TextureDataById;
    std::vector<RmlUE_SlateTexture> PublicTextures;
    uint32_t UnsupportedFeatures = 0;

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override
    {
        auto Geometry = std::make_unique<GeometryData>();
        Geometry->Vertices.reserve(Vertices.size());
        for (const Rml::Vertex& Vertex : Vertices)
            Geometry->Vertices.push_back({Vertex.position.x, Vertex.position.y, Vertex.tex_coord.x, Vertex.tex_coord.y,
                Vertex.colour.red, Vertex.colour.green, Vertex.colour.blue, Vertex.colour.alpha});
        Geometry->Indices.reserve(Indices.size());
        for (int Index : Indices) Geometry->Indices.push_back(static_cast<uint32_t>(Index));
        return reinterpret_cast<Rml::CompiledGeometryHandle>(Geometry.release());
    }
    void RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation, Rml::TextureHandle Texture) override
    {
        const auto* Geometry = reinterpret_cast<const GeometryData*>(Handle);
        if (!Geometry) return;
        const Rml::Rectanglei Region = Scissor;
        Draws.push_back({Geometry->Vertices.data(), static_cast<uint32_t>(Geometry->Vertices.size()), Geometry->Indices.data(),
            static_cast<uint32_t>(Geometry->Indices.size()), static_cast<uint64_t>(Texture), Translation.x, Translation.y,
            ScissorEnabled ? 1 : 0, static_cast<float>(Region.Left()), static_cast<float>(Region.Top()),
            static_cast<float>(Region.Width()), static_cast<float>(Region.Height())});
        if (ActiveStats) ++ActiveStats->GeometryDraws;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle Handle) override { delete reinterpret_cast<GeometryData*>(Handle); }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& Dimensions, const Rml::String& Source) override
    {
        static constexpr const char* MaterialPrefix = "ue-material://";
        if (Source.rfind(MaterialPrefix, 0) == 0)
        {
            TextureData Texture;
            Texture.Id = ++NextTexture;
            Texture.Kind = 1;
            Texture.Alias = Source.substr(std::strlen(MaterialPrefix));
            Dimensions = {1, 1};
            TextureDataById.emplace(Texture.Id, std::move(Texture));
            return static_cast<Rml::TextureHandle>(NextTexture);
        }
        unsigned char* Buffer = nullptr;
        int Width = 0, Height = 0;
        if (!Host.LoadImage(Host.User, Source.c_str(), &Buffer, &Width, &Height) || !Buffer || Width <= 0 || Height <= 0)
        {
            if (Buffer) Host.FreeBuffer(Host.User, Buffer);
            Fail("Could not decode image: " + Source);
            return 0;
        }
        std::vector<unsigned char> Pixels(Buffer, Buffer + static_cast<size_t>(Width) * Height * 4);
        Host.FreeBuffer(Host.User, Buffer);
        for (size_t Index = 0; Index < Pixels.size(); Index += 4)
            for (int Channel = 0; Channel < 3; ++Channel)
                Pixels[Index + Channel] = static_cast<unsigned char>((static_cast<unsigned>(Pixels[Index + Channel]) * Pixels[Index + 3] + 127) / 255);
        Dimensions = {Width, Height};
        if (ActiveStats) ++ActiveStats->LoadedTextures;
        return GenerateTexture({Pixels.data(), Pixels.size()}, Dimensions);
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> Source, Rml::Vector2i Dimensions) override
    {
        TextureData Texture;
        Texture.Id = ++NextTexture;
        Texture.Width = Dimensions.x;
        Texture.Height = Dimensions.y;
        Texture.Pixels.assign(Source.begin(), Source.end());
        TextureDataById.emplace(Texture.Id, std::move(Texture));
        return static_cast<Rml::TextureHandle>(NextTexture);
    }
    void ReleaseTexture(Rml::TextureHandle Texture) override { TextureDataById.erase(static_cast<uint64_t>(Texture)); }
    void EnableScissorRegion(bool Enable) override { ScissorEnabled = Enable; }
    void SetScissorRegion(Rml::Rectanglei Region) override { Scissor = Region; }
    void EnableClipMask(bool) override { UnsupportedFeatures |= 1; }
    void RenderToClipMask(Rml::ClipMaskOperation, Rml::CompiledGeometryHandle, Rml::Vector2f) override { UnsupportedFeatures |= 1; }
    void SetTransform(const Rml::Matrix4f* Transform) override { if (Transform) UnsupportedFeatures |= 2; }
    Rml::LayerHandle PushLayer() override { UnsupportedFeatures |= 4; return 0; }
    void CompositeLayers(Rml::LayerHandle, Rml::LayerHandle, Rml::BlendMode, Rml::Span<const Rml::CompiledFilterHandle>) override { UnsupportedFeatures |= 4; }
    void PopLayer() override {}
    Rml::TextureHandle SaveLayerAsTexture() override { UnsupportedFeatures |= 4; return 0; }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override { UnsupportedFeatures |= 4; return 0; }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String&, const Rml::Dictionary&) override { UnsupportedFeatures |= 8; return 0; }
    Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override { UnsupportedFeatures |= 16; return 0; }

    void BeginFrame()
    {
        Draws.clear();
        PublicTextures.clear();
        UnsupportedFeatures = 0;
    }
    void EndFrame()
    {
        PublicTextures.reserve(TextureDataById.size());
        for (const auto& Pair : TextureDataById)
        {
            const TextureData& Texture = Pair.second;
            PublicTextures.push_back({Texture.Id, Texture.Kind, Texture.Pixels.empty() ? nullptr : Texture.Pixels.data(),
                Texture.Width, Texture.Height, Texture.Alias.empty() ? nullptr : Texture.Alias.c_str()});
        }
    }
};

class UeMaterialDecorator final : public Rml::Decorator {
    int TextureIndex = -1;
public:
    bool Initialise(const Rml::Texture& Texture) { TextureIndex = AddTexture(Texture); return TextureIndex >= 0; }
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element* Element, Rml::BoxArea PaintArea) const override
    {
        Rml::RenderManager* RenderManager = Element->GetRenderManager();
        if (!RenderManager) return INVALID_DECORATORDATAHANDLE;
        Rml::Mesh Mesh;
        Rml::MeshUtilities::GenerateQuad(Mesh, {0.f, 0.f}, Element->GetBox().GetSize(PaintArea), Rml::ColourbPremultiplied(255));
        auto* Geometry = new Rml::Geometry(RenderManager->MakeGeometry(std::move(Mesh)));
        return reinterpret_cast<Rml::DecoratorDataHandle>(Geometry);
    }
    void ReleaseElementData(Rml::DecoratorDataHandle Data) const override { delete reinterpret_cast<Rml::Geometry*>(Data); }
    void RenderElement(Rml::Element* Element, Rml::DecoratorDataHandle Data) const override
    {
        auto* Geometry = reinterpret_cast<Rml::Geometry*>(Data);
        Geometry->Render(Element->GetAbsoluteOffset(Rml::BoxArea::Padding).Round(), GetTexture(TextureIndex));
    }
};

class UeMaterialDecoratorInstancer final : public Rml::DecoratorInstancer {
    Rml::PropertyId AliasId;
public:
    UeMaterialDecoratorInstancer()
    {
        AliasId = RegisterProperty("alias", "").AddParser("string").GetId();
        RegisterShorthand("decorator", "alias", Rml::ShorthandType::FallThrough);
    }
    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& Properties,
        const Rml::DecoratorInstancerInterface& Interface) override
    {
        const Rml::String Alias = Properties.GetProperty(AliasId)->Get<Rml::String>();
        if (Alias.empty()) return nullptr;
        auto Result = Rml::MakeShared<UeMaterialDecorator>();
        return Result->Initialise(Interface.GetTexture("ue-material://" + Alias)) ? Result : nullptr;
    }
};

std::unique_ptr<BridgeFileInterface> FileInterface;
std::unique_ptr<BridgeSystemInterface> SystemInterface;
std::unique_ptr<BridgeRenderer> Renderer;
std::unique_ptr<UeMaterialDecoratorInstancer> MaterialDecoratorInstancer;
}

struct RmlUE_StyleSheet final {
    Rml::SharedPtr<Rml::StyleSheetContainer> Container;
    uint32_t References = 1;
};

struct RmlUE_View final : public Rml::EventListener {
    struct NodeRecord {
        Rml::ObserverPtr<Rml::Element> Element;
        Rml::ElementPtr Detached;
    };
    struct NodeListener final : Rml::EventListener {
        RmlUE_View* View = nullptr;
        Rml::ObserverPtr<Rml::Element> Element;
        std::string Type;
        uint32_t Id = 0;
        bool Capture = false;
        bool Removed = false;
        void ProcessEvent(Rml::Event& Event) override;
        void Detach() {
            if (!Removed && Element) Element->RemoveEventListener(Type, this, Capture);
            Removed = true;
        }
        ~NodeListener() override { Detach(); }
    };
    Rml::Context* Context = nullptr;
    std::unique_ptr<SlateCommandRenderer> SlateRenderer;
    Rml::ElementDocument* Document = nullptr;
    Rml::SharedPtr<Rml::StyleSheetContainer> AuthorStyleSheet;
    Rml::SharedPtr<Rml::StyleSheetContainer> BaseStyleSheet;
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
    std::unordered_map<RmlUE_Node, NodeRecord> Nodes;
    std::unordered_map<Rml::Element*, RmlUE_Node> NodeIds;
    std::vector<std::unique_ptr<NodeListener>> NodeListeners;
    RmlUE_NodeEventCallback NodeCallback = nullptr;
    bool SuppressNextNewline = false;
    void* NodeUser = nullptr;
    int CallbackDepth = 0;

    RmlUE_Node Track(Rml::Element* Element) {
        if (!Element) return 0;
        auto Existing = NodeIds.find(Element);
        if (Existing != NodeIds.end()) {
            auto Record = Nodes.find(Existing->second);
            if (Record != Nodes.end() && Record->second.Element.get() == Element) return Existing->second;
        }
        if (NextNode == INT32_MAX) { Fail("Node handle space exhausted."); return 0; }
        const auto Id = ++NextNode;
        Nodes[Id].Element = Element->GetObserverPtr();
        NodeIds[Element] = Id;
        return Id;
    }
    void PruneNodes() {
        if (CallbackDepth) return;
        NodeListeners.erase(std::remove_if(NodeListeners.begin(), NodeListeners.end(),
            [](const auto& L) { return L->Removed || !L->Element; }), NodeListeners.end());
        for (auto It = Nodes.begin(); It != Nodes.end();) {
            if (!It->second.Element) It = Nodes.erase(It); else ++It;
        }
        for (auto It = NodeIds.begin(); It != NodeIds.end();) {
            auto Record = Nodes.find(It->second);
            if (Record == Nodes.end() || Record->second.Element.get() != It->first) It = NodeIds.erase(It); else ++It;
        }
    }
    void ClearNodes() {
        NodeCallback = nullptr;
        NodeUser = nullptr;
        NodeListeners.clear();
        NodeIds.clear();
        Nodes.clear();
    }

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

void RmlUE_View::NodeListener::ProcessEvent(Rml::Event& Event)
{
    if (Removed || !View->NodeCallback) return;
    auto* Target = Event.GetTargetElement();
    auto* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Target);
    const std::string Value = Control ? Control->GetValue() : Event.GetParameter<Rml::String>("value", "");
    const int Flags = (Event.GetParameter<int>("shift_key", 0) ? 1 : 0) |
        (Event.GetParameter<int>("ctrl_key", 0) ? 2 : 0) | (Event.GetParameter<int>("alt_key", 0) ? 4 : 0) |
        (Event.GetParameter<int>("meta_key", 0) ? 32 : 0);
    RmlUE_NodeEvent Output{View->Track(Target), View->Track(Event.GetCurrentElement()), Event.GetType().c_str(),
        Value.c_str(), static_cast<int>(Event.GetPhase()), Event.GetParameter<int>("key_identifier", 0),
        Event.GetParameter<int>("button", 0), Flags, Event.GetParameter<float>("mouse_x", 0),
        Event.GetParameter<float>("mouse_y", 0), Target && Target->HasAttribute("checked") ? 1 : 0};
    Output.KeyName = Output.Key == Rml::Input::KI_RETURN ? "Enter" : (Output.Key == Rml::Input::KI_ESCAPE ? "Escape" : "");
    ++View->CallbackDepth;
    const int Result = View->NodeCallback(View->NodeUser, Id, &Output);
    --View->CallbackDepth;
    if ((Result & 4) && Output.Key == Rml::Input::KI_RETURN) View->SuppressNextNewline = true;
    if (Result & 2) Event.StopImmediatePropagation();
    else if (Result & 1) Event.StopPropagation();
}

namespace {
bool ValidView(RmlUE_View* View)
{
    if (!Initialized || !OnOwnerThread()) return false;
    if (!View || Views.find(View) == Views.end()) return Fail("Invalid or destroyed RmlUi view.") != 0;
    ActiveStats = &View->Stats;
    return true;
}

bool ValidStyleSheet(RmlUE_StyleSheet* StyleSheet)
{
    if (!Initialized || !OnOwnerThread()) return false;
    if (!StyleSheet || StyleSheets.find(StyleSheet) == StyleSheets.end())
        return Fail("Invalid or released RmlUi style sheet handle.") != 0;
    return true;
}

void ApplyDocumentStyleSheet(RmlUE_View& View)
{
    if (!View.Document || !View.AuthorStyleSheet) return;
    auto Combined = View.BaseStyleSheet
        ? View.BaseStyleSheet->CombineStyleSheetContainer(*View.AuthorStyleSheet)
        : View.AuthorStyleSheet->CombineStyleSheetContainer(Rml::StyleSheetContainer());
    View.Document->SetStyleSheetContainer(std::move(Combined));
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
    Rml::ElementDocument* PreviousDocument = View.Document;
    const Rml::StyleSheetContainer* LoadedStyleSheet = NewDocument->GetStyleSheetContainer();
    View.AuthorStyleSheet = LoadedStyleSheet
        ? LoadedStyleSheet->CombineStyleSheetContainer(Rml::StyleSheetContainer())
        : Rml::MakeShared<Rml::StyleSheetContainer>();
    View.Document = NewDocument;
    ApplyDocumentStyleSheet(View);
    View.ClearNodes();
    if (PreviousDocument)
    {
        PreviousDocument->RemoveEventListener("click", &View);
        PreviousDocument->RemoveEventListener("change", &View);
        PreviousDocument->RemoveEventListener("submit", &View);
        PreviousDocument->Close();
    }
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
        MaterialDecoratorInstancer = std::make_unique<UeMaterialDecoratorInstancer>();
        Rml::Factory::RegisterDecoratorInstancer("ue-material", MaterialDecoratorInstancer.get());
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
    while (!StyleSheets.empty())
    {
        auto* StyleSheet = *StyleSheets.begin();
        StyleSheets.erase(StyleSheets.begin());
        delete StyleSheet;
    }
    ActiveStats = nullptr;
    if (Initialized) Rml::Shutdown();
    Initialized = false;
    MaterialDecoratorInstancer.reset();
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
const char* RmlUE_GetVersion() { return "RmlUi 6.3 / FreeType 2.14.3 / Slate command + DX11 compatibility backends"; }
int RmlUE_LoadFont(const char* Path, int Fallback)
{
    return Path && Initialized && OnOwnerThread() && Rml::LoadFontFace(Path, Fallback != 0) ? 1 : Fail("Could not load RmlUi font.");
}

RmlUE_StyleSheet* RmlUE_CreateStyleSheet(const char* Rcss)
{
    if (!Initialized || !OnOwnerThread() || !Rcss) { Fail("A style sheet can only be created on the initialized owner thread."); return nullptr; }
    LastError.clear();
    StyleSheetDiagnostics.clear();
    CapturingStyleSheetDiagnostics = true;
    auto Container = Rml::Factory::InstanceStyleSheetString(Rcss);
    CapturingStyleSheetDiagnostics = false;
    if (!Container || !StyleSheetDiagnostics.empty())
    {
        const std::string Diagnostic = !StyleSheetDiagnostics.empty() ? StyleSheetDiagnostics : LastError;
        Fail("Could not strictly parse the RmlUi style sheet. " + Diagnostic);
        return nullptr;
    }
    auto* StyleSheet = new RmlUE_StyleSheet{std::move(Container), 1};
    StyleSheets.insert(StyleSheet);
    return StyleSheet;
}

void RmlUE_RetainStyleSheet(RmlUE_StyleSheet* StyleSheet)
{
    if (!ValidStyleSheet(StyleSheet)) return;
    if (StyleSheet->References == UINT32_MAX) { Fail("Style sheet reference count overflow."); return; }
    ++StyleSheet->References;
}

void RmlUE_ReleaseStyleSheet(RmlUE_StyleSheet* StyleSheet)
{
    if (!ValidStyleSheet(StyleSheet)) return;
    if (--StyleSheet->References == 0)
    {
        StyleSheets.erase(StyleSheet);
        delete StyleSheet;
    }
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

RmlUE_View* RmlUE_CreateSlateView(int Width, int Height, float DpRatio)
{
    if (!Initialized || !OnOwnerThread() || !ValidDimensions(Width, Height, DpRatio))
    {
        Fail("View dimensions must be 1..4096 and DPR finite and positive.");
        return nullptr;
    }
    auto View = std::make_unique<RmlUE_View>();
    View->Name = "unreal-slate-" + std::to_string(++NextView);
    View->Width = Width;
    View->Height = Height;
    View->DpRatio = DpRatio;
    View->SlateRenderer = std::make_unique<SlateCommandRenderer>();
    View->Context = Rml::CreateContext(View->Name, {Width, Height}, View->SlateRenderer.get());
    if (!View->Context) { Fail("Could not create RmlUi Slate command context."); return nullptr; }
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    Views.insert(View.get());
    return View.release();
}

void RmlUE_DestroyView(RmlUE_View* View)
{
    if (!ValidView(View)) return;
    if (View->CallbackDepth) { Fail("Cannot destroy a view inside its synchronous event callback."); return; }
    if (DebuggerView == View) { Rml::Debugger::Shutdown(); DebuggerView = nullptr; }
    View->ClearNodes();
    Rml::RemoveContext(View->Name);
    if (View->SlateRenderer) Rml::ReleaseRenderManagers();
    Views.erase(View);
    ActiveStats = nullptr;
    delete View;
}

int RmlUE_SetBaseStyleSheet(RmlUE_View* View, RmlUE_StyleSheet* StyleSheet)
{
    if (!ValidView(View) || (StyleSheet && !ValidStyleSheet(StyleSheet))) return 0;
    View->BaseStyleSheet = StyleSheet ? StyleSheet->Container : nullptr;
    ApplyDocumentStyleSheet(*View);
    if (View->Document) View->Context->Update();
    return 1;
}

int RmlUE_LoadDocument(RmlUE_View* View, const char* Path)
{
    if (!ValidView(View) || !Path) return 0;
    if (View->CallbackDepth) return Fail("Defer document replacement until event dispatch completes.");
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    return AdoptDocument(*View, View->Context->LoadDocument(Path));
}

int RmlUE_LoadDocumentFromMemory(RmlUE_View* View, const char* Markup, const char* Source)
{
    if (!ValidView(View) || !Markup) return 0;
    if (View->CallbackDepth) return Fail("Defer document replacement until event dispatch completes.");
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    return AdoptDocument(*View, View->Context->LoadDocumentFromMemory(Markup, Source ? Source : "memory.rml"));
}

int RmlUE_Resize(RmlUE_View* View, int Width, int Height, float DpRatio)
{
    if (!ValidView(View)) return 0;
    if (!ValidDimensions(Width, Height, DpRatio)) return Fail("View dimensions must be 1..4096 and DPR finite and positive.");
    if (Width != View->Width || Height != View->Height)
    {
        if (View->SlateRenderer) { View->Width = Width; View->Height = Height; }
        else if (!CreateTargets(*View, Width, Height)) return 0;
    }
    View->DpRatio = DpRatio;
    View->Context->SetDimensions({Width, Height});
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    return 1;
}

int RmlUE_Render(RmlUE_View* View, RmlUE_Frame* Frame)
{
    if (Frame) *Frame = {};
    if (!ValidView(View) || !Frame) return 0;
    if (View->SlateRenderer) return Fail("A Slate command view must be rendered with RmlUE_RenderSlate.");
    Renderer->SetViewport(View->Width, View->Height);
    RmlUE_Update(View);
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

int RmlUE_RenderSlate(RmlUE_View* View, RmlUE_SlateFrame* Frame)
{
    if (Frame) *Frame = {};
    if (!ValidView(View) || !Frame) return 0;
    if (!View->SlateRenderer) return Fail("A DX11 compatibility view must be rendered with RmlUE_Render.");
    if (!RmlUE_Update(View)) return 0;
    View->SlateRenderer->BeginFrame();
    View->Context->Render();
    View->SlateRenderer->EndFrame();
    ++View->FrameNumber;
    *Frame = {RMLUE_SLATE_ABI_VERSION, View->SlateRenderer->Draws.data(), static_cast<uint32_t>(View->SlateRenderer->Draws.size()),
        View->SlateRenderer->PublicTextures.data(), static_cast<uint32_t>(View->SlateRenderer->PublicTextures.size()),
        View->FrameNumber, View->SlateRenderer->UnsupportedFeatures};
    return 1;
}

namespace {
Rml::Element* GetNode(RmlUE_View* View, RmlUE_Node Node)
{
    if (!ValidView(View)) return nullptr;
    auto Found = View->Nodes.find(Node);
    if (Found == View->Nodes.end() || !Found->second.Element) {
        Fail("Invalid, expired or foreign node handle.");
        return nullptr;
    }
    return Found->second.Element.get();
}
void AppendNodeText(Rml::Element* Element, std::string& Text)
{
    if (auto* TextNode = rmlui_dynamic_cast<Rml::ElementText*>(Element)) Text += TextNode->GetText();
    else for (int Index = 0; Index < Element->GetNumChildren(); ++Index) AppendNodeText(Element->GetChild(Index), Text);
}
}

RmlUE_Node RmlUE_GetRootNode(RmlUE_View* View) { return ValidView(View) ? View->Track(View->Document) : 0; }
RmlUE_Node RmlUE_FindNode(RmlUE_View* View, const char* Id)
{
    if (!ValidView(View) || !View->Document || !Id) return 0;
    return View->Track(View->Document->GetElementById(Id));
}
int RmlUE_IsNodeValid(RmlUE_View* View, RmlUE_Node Node)
{
    if (!ValidView(View)) return 0;
    auto Found = View->Nodes.find(Node);
    return Found != View->Nodes.end() && Found->second.Element ? 1 : 0;
}
RmlUE_Node RmlUE_CreateNode(RmlUE_View* View, int Kind, const char* TagOrText)
{
    if (!ValidView(View) || !View->Document || !TagOrText || Kind < 0 || Kind > 2) return 0;
    Rml::ElementPtr Element = Kind == 0 ? View->Document->CreateElement(TagOrText) :
        View->Document->CreateTextNode(Kind == 1 ? TagOrText : "");
    if (!Element) { Fail("Cannot create RmlUi node."); return 0; }
    const auto Id = View->Track(Element.get());
    if (Id) View->Nodes.at(Id).Detached = std::move(Element);
    return Id;
}
int RmlUE_InsertNode(RmlUE_View* View, RmlUE_Node Node, RmlUE_Node Parent, RmlUE_Node Before)
{
    auto* Element = GetNode(View, Node);
    auto* ParentElement = GetNode(View, Parent);
    auto* Anchor = Before ? GetNode(View, Before) : nullptr;
    if (!Element || !ParentElement || (Before && !Anchor)) return 0;
    if (Element == View->Document || (Anchor && Anchor->GetParentNode() != ParentElement)) return Fail("Invalid parent or insertion anchor.");
    if (Element == Anchor) return 1;
    for (auto* Ancestor = ParentElement; Ancestor; Ancestor = Ancestor->GetParentNode())
        if (Ancestor == Element) return Fail("A node cannot be inserted into its own subtree.");
    Rml::ElementPtr Owned;
    if (auto* OldParent = Element->GetParentNode()) Owned = OldParent->RemoveChild(Element);
    else Owned = std::move(View->Nodes.at(Node).Detached);
    if (!Owned) return Fail("Node has no transferable owner.");
    if (Anchor) ParentElement->InsertBefore(std::move(Owned), Anchor);
    else ParentElement->AppendChild(std::move(Owned));
    return 1;
}
int RmlUE_RemoveNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node);
    if (!Element) return 0;
    if (Element == View->Document) return Fail("Cannot remove the document root.");
    if (auto* Parent = Element->GetParentNode()) { auto Removed = Parent->RemoveChild(Element); }
    else View->Nodes.at(Node).Detached.reset();
    View->PruneNodes();
    return 1;
}
RmlUE_Node RmlUE_ParentNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node); return Element ? View->Track(Element->GetParentNode()) : 0;
}
RmlUE_Node RmlUE_NextNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node); return Element ? View->Track(Element->GetNextSibling()) : 0;
}
int RmlUE_SetNodeText(RmlUE_View* View, RmlUE_Node Node, const char* Text)
{
    auto* Element = GetNode(View, Node); if (!Element || !Text) return 0;
    if (auto* TextNode = rmlui_dynamic_cast<Rml::ElementText*>(Element)) TextNode->SetText(Text);
    else {
        Element->SetInnerRML("");
        if (*Text) Element->AppendChild(View->Document->CreateTextNode(Text));
    }
    return 1;
}
int RmlUE_GetNodeText(RmlUE_View* View, RmlUE_Node Node, char* Text, size_t Capacity)
{
    auto* Element = GetNode(View, Node); if (!Element || !Text || !Capacity) return 0;
    std::string Value; AppendNodeText(Element, Value);
    if (Value.size() >= Capacity) return Fail("Text output buffer is too small.");
    CopyString(Text, Capacity, Value); return 1;
}
int RmlUE_SetNodeAttribute(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* Value)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name) return 0;
    const bool Boolean = std::strcmp(Name, "checked") == 0 || std::strcmp(Name, "selected") == 0 || std::strcmp(Name, "disabled") == 0;
    if (!Value || (Boolean && (std::strcmp(Value, "false") == 0 || std::strcmp(Value, "0") == 0))) Element->RemoveAttribute(Name);
    else Element->SetAttribute(Name, Rml::String(Value));
    return 1;
}
int RmlUE_GetNodeAttribute(RmlUE_View* View, RmlUE_Node Node, const char* Name, char* Value, size_t Capacity)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name || !Value || !Capacity) return 0;
    const bool Boolean = std::strcmp(Name, "checked") == 0 || std::strcmp(Name, "selected") == 0 || std::strcmp(Name, "disabled") == 0;
    auto* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Element);
    std::string Result = Boolean ? (Element->HasAttribute(Name) ? "true" : "false") :
        (Control && std::strcmp(Name, "value") == 0 ? Control->GetValue() : Element->GetAttribute<Rml::String>(Name, ""));
    if (Result.size() >= Capacity) return Fail("Attribute output buffer is too small.");
    CopyString(Value, Capacity, Result); return 1;
}
int RmlUE_SetNodeProperty(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* Value)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name) return 0;
    if (!Value) { Element->RemoveProperty(Name); return 1; }
    return Element->SetProperty(Name, Value) ? 1 : Fail(std::string("Unsupported RmlUi property/value: ") + Name + ": " + Value);
}
int RmlUE_ListenNode(RmlUE_View* View, RmlUE_Node Node, const char* Type, uint32_t Listener, int Capture)
{
    auto* Element = GetNode(View, Node); if (!Element || !Type || !Listener) return 0;
    RmlUE_UnlistenNode(View, Listener);
    auto Entry = std::make_unique<RmlUE_View::NodeListener>();
    Entry->View = View; Entry->Element = Element->GetObserverPtr(); Entry->Type = Type;
    Entry->Id = Listener; Entry->Capture = Capture != 0;
    Element->AddEventListener(Type, Entry.get(), Entry->Capture);
    View->NodeListeners.push_back(std::move(Entry));
    return 1;
}
void RmlUE_UnlistenNode(RmlUE_View* View, uint32_t Listener)
{
    if (!ValidView(View)) return;
    for (auto& Entry : View->NodeListeners) if (Entry->Id == Listener) Entry->Detach();
}
void RmlUE_SetNodeEventCallback(RmlUE_View* View, RmlUE_NodeEventCallback Callback, void* User)
{
    if (ValidView(View)) { View->NodeCallback = Callback; View->NodeUser = User; }
}
int RmlUE_Update(RmlUE_View* View)
{
    if (!ValidView(View)) return 0;
    View->PruneNodes();
    return View->Context->Update() ? 1 : 0;
}
void RmlUE_GetNodeCounts(RmlUE_View* View, int* Nodes, int* Listeners)
{
    if (!ValidView(View)) return;
    View->PruneNodes();
    if (Nodes) *Nodes = static_cast<int>(View->Nodes.size());
    if (Listeners) *Listeners = static_cast<int>(View->NodeListeners.size());
}

int RmlUE_ScrollNode(RmlUE_View* View, RmlUE_Node Node, float Top)
{
    auto* Element = GetNode(View, Node); if (!Element) return 0;
    View->Context->Update();
    Element->SetScrollTop(Top < 0 ? Element->GetScrollHeight() : Top);
    return 1;
}
float RmlUE_NodeScrollRemaining(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node); if (!Element) return 0;
    return std::max(0.f, Element->GetScrollHeight() - Element->GetClientHeight() - Element->GetScrollTop());
}
int RmlUE_FocusNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node); return Element && Element->Focus();
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
    if (Down) { View->SuppressNextNewline = false; View->PressedKeys.insert(Key); View->Context->ProcessKeyDown(Identifier, Modifiers(Flags)); }
    else { View->PressedKeys.erase(Key); View->Context->ProcessKeyUp(Identifier, Modifiers(Flags)); }
}
void RmlUE_Text(RmlUE_View* View, const char* Text)
{
    if (!ValidView(View) || !Text) return;
    const bool Suppress = View->SuppressNextNewline; View->SuppressNextNewline = false;
    if (Suppress && (std::strcmp(Text, "\n") == 0 || std::strcmp(Text, "\r") == 0)) return;
    View->Context->ProcessTextInput(Rml::String(Text));
}
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
