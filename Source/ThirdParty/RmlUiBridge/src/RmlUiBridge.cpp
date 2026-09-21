#include "RmlUiBridge.h"
#include "RmlUiTextInputBridge.h"
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
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/TextInputHandler.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>
#include "../vendor/RmlUi-ba95ffe8bfb6370efb2cdcca927eaad4710c5413/Source/Core/TransformState.h"
#include <RmlUi/Debugger.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
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

static Rml::TextInputHandler* RmlUE_CreateTextInputHandler(RmlUE_View* View);
static void RmlUE_DestroyTextInputHandler(RmlUE_View* View);
static std::string HostKeyName(int Key, bool Shift);
static std::string HostKeyCode(int Key);
static void CancelPointerCaptures(RmlUE_View* View, Rml::Element* Subtree);
static bool InModalScope(RmlUE_View* View, Rml::Element* Element);
static void FocusModalNext(RmlUE_View* View, bool Backwards);

namespace {
static_assert(static_cast<int>(Rml::ClipMaskOperation::Set) == RMLUE_CLIP_MASK_SET);
static_assert(static_cast<int>(Rml::ClipMaskOperation::SetInverse) == RMLUE_CLIP_MASK_SET_INVERSE);
static_assert(static_cast<int>(Rml::ClipMaskOperation::Intersect) == RMLUE_CLIP_MASK_INTERSECT);

RmlUE_Host Host{};
std::string LastError;
std::thread::id OwnerThread;
bool Initialized = false;
uint64_t NextView = 0;
uint64_t NextResource = 0;
uint64_t NextResourceSequence = 0;
uint32_t NextNode = 0;
uint32_t NextAnimationTargetGeneration = 0;
RmlUE_Stats* ActiveStats = nullptr;
bool CapturingStyleSheetDiagnostics = false;
std::string StyleSheetDiagnostics;
ComPtr<ID3D11Device> Device;
ComPtr<ID3D11DeviceContext> DeviceContext;
std::set<RmlUE_View*> Views;
std::set<RmlUE_StyleSheet*> StyleSheets;
RmlUE_View* DebuggerView = nullptr;
std::unordered_map<uint64_t, RmlUE_ResourceRecord> Resources;
RmlUE_ResourceEventCallback ResourceEventCallback = nullptr;
void* ResourceEventUser = nullptr;
void CopyString(char* Destination, size_t Capacity, const std::string& Source);

uint64_t RegisterResource(int Type, int Backend, uint64_t OwnerId, uint64_t EstimatedBytes, const std::string& Name)
{
    RmlUE_ResourceRecord Record{};
    Record.Id = ++NextResource;
    Record.OwnerId = OwnerId;
    Record.EstimatedBytes = EstimatedBytes;
    Record.CreatedSequence = ++NextResourceSequence;
    Record.Type = Type;
    Record.Backend = Backend;
    CopyString(Record.Name, sizeof(Record.Name), Name);
    Resources.emplace(Record.Id, Record);
    if (ResourceEventCallback) ResourceEventCallback(ResourceEventUser, RMLUE_RESOURCE_CREATED, &Record);
    return Record.Id;
}

void UpdateResource(uint64_t Id, uint64_t EstimatedBytes)
{
    const auto Found = Resources.find(Id);
    if (Found == Resources.end()) return;
    Found->second.EstimatedBytes = EstimatedBytes;
    const RmlUE_ResourceRecord Record = Found->second;
    if (ResourceEventCallback) ResourceEventCallback(ResourceEventUser, RMLUE_RESOURCE_UPDATED, &Record);
}

void UnregisterResource(uint64_t Id)
{
    const auto Found = Resources.find(Id);
    if (Found == Resources.end()) return;
    const RmlUE_ResourceRecord Record = Found->second;
    Resources.erase(Found);
    if (ResourceEventCallback) ResourceEventCallback(ResourceEventUser, RMLUE_RESOURCE_DESTROYED, &Record);
}

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
        uint64_t Id = 0;
        uint64_t ResourceId = 0;
        bool Published = false;
        bool PendingRelease = false;
        std::vector<RmlUE_SlateVertex> Vertices;
        std::vector<uint32_t> Indices;
    };
    struct TextureData {
        uint64_t Id = 0;
        uint64_t ResourceId = 0;
        bool Published = false;
        bool PendingRelease = false;
        int Kind = 0;
        int MaterialSlot = RMLUE_MATERIAL_SLOT_NONE;
        int Width = 0, Height = 0;
        std::vector<unsigned char> Pixels;
        std::string Alias;
    };
    uint64_t NextGeometry = 0;
    uint64_t NextTexture = 0;
    uint64_t OwnerResourceId = 0;
    bool ScissorEnabled = false;
    Rml::Rectanglei Scissor{};
    bool TransformEnabled = false;
    float TransformM00 = 1.f, TransformM01 = 0.f, TransformM10 = 0.f, TransformM11 = 1.f, TransformX = 0.f, TransformY = 0.f;
    bool ClipMaskEnabled = false;
    RmlUE_Node CurrentClipMaskOwnerNode = 0;
    std::vector<RmlUE_SlateClipMask> ActiveClipMasks;
    std::function<RmlUE_Node(Rml::Element*)> ResolveNode;
    struct VisualOpacityState
    {
        float Opacity = 1.f;
        uint64_t TopologyGeneration = 0;
        std::vector<size_t> DrawIndices;
        uint32_t VisualSlot = UINT32_MAX;
        uint32_t BindingRefCount = 0;
        bool Active = false;
    };
    struct VisualTransformState
    {
        float M00 = 1.f, M01 = 0.f, M10 = 0.f, M11 = 1.f, X = 0.f, Y = 0.f;
        bool Enabled = false;
        bool Active = true;
    };
    struct VisualNodeSlot
    {
        RmlUE_Node Node = 0;
        float PendingOpacity = 1.f;
        VisualTransformState PendingTransform;
        bool OpacityPending = false;
        bool TransformPending = false;
        bool RemoveOpacityOverrideAfterReplay = false;
        bool Queued = false;
    };
    std::unordered_map<RmlUE_Node, VisualOpacityState> VisualOpacityOverrides;
    std::unordered_map<RmlUE_Node, VisualTransformState> VisualTransformOverrides;
    struct NodeDrawState
    {
        std::vector<size_t> DrawIndices;
        uint32_t VisualSlot = UINT32_MAX;
    };
    std::unordered_map<RmlUE_Node, NodeDrawState> DrawIndicesByNode;
    std::vector<VisualNodeSlot> VisualNodeSlots;
    std::vector<uint32_t> PendingVisualSlots;
    struct TransformDrawNode
    {
        RmlUE_Node Node = 0;
        Rml::ObserverPtr<Rml::Element> Element;
        std::vector<size_t> DrawIndices;
        uint32_t VisualSlot = UINT32_MAX;
        VisualTransformState State;
    };
    struct TransformMaskNode
    {
        RmlUE_Node Node = 0;
        Rml::ObserverPtr<Rml::Element> Element;
        std::vector<size_t> MaskIndices;
        VisualTransformState State;
    };
    struct PendingClipMaskTransform
    {
        RmlUE_Node Node = 0;
        VisualTransformState State;
    };
    std::vector<PendingClipMaskTransform> PendingClipMaskTransforms;
    std::unordered_map<RmlUE_Node, size_t> PendingClipMaskTransformByNode;
    struct TransformBindingCache
    {
        uint32_t Generation = 0;
        uint64_t ContentRevision = 0;
        uint64_t TopologyGeneration = 0;
        uint32_t RootPropertySlot = UINT32_MAX;
        Rml::ObserverPtr<Rml::Element> Root;
        std::vector<TransformDrawNode> DrawNodes;
        VisualTransformState OverrideState;
        bool OverrideActive = false;
    };
    struct TransformPropertySlot
    {
        Rml::ObserverPtr<Rml::Element> Element;
        RmlUE_Node Node = 0;
        uint32_t Parent = UINT32_MAX;
        uint32_t SubtreeEnd = 0;
        uint8_t SubtreeHasScissorDependency : 1;
        uint8_t SubtreeHasClipMaskDependency : 1;
        uint8_t Dirty : 1;
        uint8_t BatchPublishRoot : 1;
        uint8_t BatchPublished : 1;
        TransformPropertySlot() : SubtreeHasScissorDependency(0), SubtreeHasClipMaskDependency(0), Dirty(0),
            BatchPublishRoot(0), BatchPublished(0) {}
    };
    std::vector<TransformPropertySlot> TransformPropertySlots;
    std::unordered_map<Rml::Element*, uint32_t> TransformPropertySlotByElement;
    std::unordered_map<RmlUE_Node, uint32_t> TransformPropertySlotByNode;
    uint32_t PendingTransformPropertyMin = UINT32_MAX;
    uint32_t PendingTransformPropertyMax = 0;
    uint32_t PendingTransformPropertyLastEnd = 0;
    bool PendingTransformPropertiesOrderedDisjoint = true;
    bool TransformBatchUsesCoverage = false;
    std::vector<Rml::Element*> TransformElementScratch;
    std::vector<TransformDrawNode> TransformDrawScratch;
    std::vector<TransformMaskNode> TransformMaskScratch;
    std::vector<TransformBindingCache> TransformBindingCaches;
    std::unordered_map<uint32_t, std::vector<TransformMaskNode>> TransformMaskBindingsByIndex;
    struct ElementVisualState { RmlUE_Node Node = 0; float Opacity = 1.f; };
    std::vector<ElementVisualState> ElementVisualStack;
    ElementVisualState CurrentElementVisual;
    uint64_t ContentRevision = 1;
    uint64_t VisualRevision = 1;
    uint64_t RecordedContentRevision = 0;
    uint64_t TopologyGeneration = 0;
    uint64_t FullRenderFrames = 0;
    uint64_t ReplayedFrames = 0;
    bool HasRecordedFrame = false;
public:
    std::vector<RmlUE_SlateDraw> Draws;
    std::vector<RmlUE_SlateVisualDelta> PublicVisualDeltas;
    std::vector<RmlUE_SlateClipMask> PublicClipMasks;
    std::unordered_map<uint64_t, GeometryData*> GeometryDataById;
    std::unordered_map<uint64_t, TextureData> TextureDataById;
    std::vector<uint64_t> ReleasedGeometryIds;
    std::vector<uint64_t> ReleasedTextureIds;
    std::vector<RmlUE_SlateGeometryDelta> PublicGeometryDeltas;
    std::vector<RmlUE_SlateTexture> PublicTextures;
    uint32_t UnsupportedFeatures = 0;

    ~SlateCommandRenderer() override
    {
        for (const auto& Pair : GeometryDataById)
        {
            UnregisterResource(Pair.second->ResourceId);
            delete Pair.second;
        }
        for (const auto& Pair : TextureDataById) UnregisterResource(Pair.second.ResourceId);
    }

    void SetOwnerResourceId(uint64_t InOwnerResourceId) { OwnerResourceId = InOwnerResourceId; }
    void SetNodeResolver(std::function<RmlUE_Node(Rml::Element*)> InResolveNode) { ResolveNode = std::move(InResolveNode); }
    void MarkContentDirty()
    {
        if (++ContentRevision == 0) ContentRevision = 1;
    }
    void OnElementRenderDirty(Rml::Element*) override { MarkContentDirty(); }
    uint32_t EnsureVisualNodeSlot(RmlUE_Node Node)
    {
        if (!Node) return UINT32_MAX;
        auto [StateIt, Inserted] = DrawIndicesByNode.try_emplace(Node);
        if (Inserted || StateIt->second.VisualSlot >= VisualNodeSlots.size())
        {
            StateIt->second.VisualSlot = static_cast<uint32_t>(VisualNodeSlots.size());
            VisualNodeSlots.push_back({Node});
        }
        return StateIt->second.VisualSlot;
    }
    void QueueClipMaskTransform(RmlUE_Node Node, const VisualTransformState& State)
    {
        auto [Found, Inserted] = PendingClipMaskTransformByNode.try_emplace(Node, PendingClipMaskTransforms.size());
        if (Inserted) PendingClipMaskTransforms.push_back({Node, State});
        else PendingClipMaskTransforms[Found->second].State = State;
    }
    void QueueVisualSlot(uint32_t SlotIndex)
    {
        if (SlotIndex >= VisualNodeSlots.size()) return;
        VisualNodeSlot& Slot = VisualNodeSlots[SlotIndex];
        if (Slot.Queued) return;
        Slot.Queued = true;
        PendingVisualSlots.push_back(SlotIndex);
    }
    void RefreshVisualOpacityTopology(RmlUE_Node Node, VisualOpacityState& State)
    {
        if (State.TopologyGeneration == TopologyGeneration) return;
        State.TopologyGeneration = TopologyGeneration;
        State.DrawIndices.clear();
        State.VisualSlot = UINT32_MAX;
        const auto DrawState = DrawIndicesByNode.find(Node);
        if (DrawState != DrawIndicesByNode.end())
        {
            State.DrawIndices = DrawState->second.DrawIndices;
            State.VisualSlot = DrawState->second.VisualSlot;
        }
    }
    void* RetainVisualOpacityBinding(RmlUE_Node Node)
    {
        if (!Node) return nullptr;
        auto Existing = VisualOpacityOverrides.try_emplace(Node).first;
        VisualOpacityState& State = Existing->second;
        ++State.BindingRefCount;
        RefreshVisualOpacityTopology(Node, State);
        return &State;
    }
    void ReleaseVisualOpacityBinding(RmlUE_Node Node)
    {
        const auto Existing = VisualOpacityOverrides.find(Node);
        if (Existing == VisualOpacityOverrides.end()) return;
        VisualOpacityState& State = Existing->second;
        if (State.BindingRefCount) --State.BindingRefCount;
        if (!State.BindingRefCount && !State.Active) VisualOpacityOverrides.erase(Existing);
    }
    bool SetVisualOpacity(RmlUE_Node Node, float Opacity, float BaseOpacity,
        void* PreparedState = nullptr)
    {
        if (!Node || !std::isfinite(Opacity) || Opacity < 0.f || Opacity > 1.f) return false;
        bool Inserted = false;
        VisualOpacityState* StatePtr = static_cast<VisualOpacityState*>(PreparedState);
        if (!StatePtr)
        {
            auto Result = VisualOpacityOverrides.try_emplace(Node);
            StatePtr = &Result.first->second;
            Inserted = Result.second;
        }
        VisualOpacityState& State = *StatePtr;
        if (!Inserted && State.Active && State.Opacity == Opacity) return true;
        State.Opacity = Opacity;
        State.Active = true;
        if (++VisualRevision == 0) VisualRevision = 1;
        RefreshVisualOpacityTopology(Node, State);
        if (BaseOpacity > 1.f / 255.f && !State.DrawIndices.empty())
        {
            const float Multiplier = std::max(0.f, Opacity / BaseOpacity);
            for (size_t Index : State.DrawIndices)
                if (Index < Draws.size()) Draws[Index].VisualOpacity = Multiplier;
            if (State.VisualSlot < VisualNodeSlots.size())
            {
                VisualNodeSlot& Slot = VisualNodeSlots[State.VisualSlot];
                Slot.PendingOpacity = Multiplier;
                Slot.OpacityPending = true;
                Slot.RemoveOpacityOverrideAfterReplay = false;
                QueueVisualSlot(State.VisualSlot);
            }
        }
        return true;
    }
    bool ClearVisualOpacity(RmlUE_Node Node)
    {
        const auto Existing = VisualOpacityOverrides.find(Node);
        if (Existing == VisualOpacityOverrides.end() || !Existing->second.Active) return false;
        VisualOpacityState& State = Existing->second;
        State.Active = false;
        if (++VisualRevision == 0) VisualRevision = 1;
        RefreshVisualOpacityTopology(Node, State);
        if (!State.DrawIndices.empty())
        {
            for (size_t Index : State.DrawIndices)
                if (Index < Draws.size()) Draws[Index].VisualOpacity = 1.f;
            if (State.VisualSlot < VisualNodeSlots.size())
            {
                VisualNodeSlot& Slot = VisualNodeSlots[State.VisualSlot];
                Slot.PendingOpacity = 1.f;
                Slot.OpacityPending = true;
                Slot.RemoveOpacityOverrideAfterReplay = true;
                QueueVisualSlot(State.VisualSlot);
            }
        }
        else if (!State.BindingRefCount) VisualOpacityOverrides.erase(Existing);
        return true;
    }
    static bool ReadElementTransform(Rml::Element* Element, VisualTransformState& State)
    {
        State.Enabled = false;
        State.M00 = State.M11 = 1.f;
        State.M01 = State.M10 = State.X = State.Y = 0.f;
        const Rml::TransformState* TransformState = Element ? Element->GetTransformState() : nullptr;
        const Rml::Matrix4f* Transform = TransformState ? TransformState->GetTransform() : nullptr;
        if (!Transform) return true;
        const auto Row0 = Transform->GetRow(0);
        const auto Row1 = Transform->GetRow(1);
        const auto Row2 = Transform->GetRow(2);
        const auto Row3 = Transform->GetRow(3);
        const auto Near = [](float A, float B) { return std::abs(A - B) <= 0.0001f; };
        if (!Near(Row0[2], 0.f) || !Near(Row1[2], 0.f) ||
            !Near(Row2[0], 0.f) || !Near(Row2[1], 0.f) || !Near(Row2[2], 1.f) || !Near(Row2[3], 0.f) ||
            !Near(Row3[0], 0.f) || !Near(Row3[1], 0.f) || !Near(Row3[2], 0.f) || !Near(Row3[3], 1.f))
            return false;
        State.Enabled = true;
        State.M00 = Row0[0]; State.M01 = Row0[1]; State.M10 = Row1[0]; State.M11 = Row1[1];
        State.X = Row0[3]; State.Y = Row1[3];
        return true;
    }
    static bool DecodeAnimationTarget(RmlUE_AnimationTarget Target, uint32_t& Index, uint32_t& Generation)
    {
        const uint32_t PackedIndex = static_cast<uint32_t>(Target);
        Generation = static_cast<uint32_t>(Target >> 32);
        if (!PackedIndex || !Generation) return false;
        Index = PackedIndex - 1;
        return true;
    }
    TransformBindingCache* FindTransformBindingCache(RmlUE_AnimationTarget Target)
    {
        uint32_t Index = 0, Generation = 0;
        if (!DecodeAnimationTarget(Target, Index, Generation) || Index >= TransformBindingCaches.size()) return nullptr;
        TransformBindingCache& Cache = TransformBindingCaches[Index];
        return Cache.Generation == Generation ? &Cache : nullptr;
    }
    std::vector<TransformMaskNode>* FindTransformMaskBinding(RmlUE_AnimationTarget Target)
    {
        uint32_t Index = 0, Generation = 0;
        if (!DecodeAnimationTarget(Target, Index, Generation) || Index >= TransformBindingCaches.size() ||
            TransformBindingCaches[Index].Generation != Generation) return nullptr;
        const auto Found = TransformMaskBindingsByIndex.find(Index);
        return Found != TransformMaskBindingsByIndex.end() ? &Found->second : nullptr;
    }
    void BuildTransformPropertyTree(Rml::Element* Root)
    {
        TransformPropertySlots.clear();
        TransformPropertySlotByElement.clear();
        TransformPropertySlotByNode.clear();
        PendingTransformPropertyMin = UINT32_MAX;
        PendingTransformPropertyMax = 0;
        PendingTransformPropertyLastEnd = 0;
        PendingTransformPropertiesOrderedDisjoint = true;
        TransformBatchUsesCoverage = false;
        if (!Root || !ResolveNode) return;

        struct VisitFrame
        {
            Rml::Element* Element = nullptr;
            uint32_t Parent = UINT32_MAX;
            uint32_t Slot = UINT32_MAX;
            int NextChild = 0;
        };
        std::vector<VisitFrame> Stack;
        Stack.push_back({Root});
        while (!Stack.empty())
        {
            VisitFrame& Frame = Stack.back();
            if (Frame.Slot == UINT32_MAX)
            {
                Frame.Slot = static_cast<uint32_t>(TransformPropertySlots.size());
                TransformPropertySlot& Slot = TransformPropertySlots.emplace_back();
                Slot.Element = Frame.Element->GetObserverPtr();
                Slot.Node = ResolveNode(Frame.Element);
                Slot.Parent = Frame.Parent;
                TransformPropertySlotByElement.emplace(Frame.Element, Frame.Slot);
                if (Slot.Node) TransformPropertySlotByNode.emplace(Slot.Node, Frame.Slot);
            }
            if (Frame.NextChild < Frame.Element->GetNumChildren(true))
            {
                Rml::Element* Child = Frame.Element->GetChild(Frame.NextChild++);
                if (Child) Stack.push_back({Child, Frame.Slot});
                continue;
            }
            TransformPropertySlots[Frame.Slot].SubtreeEnd =
                static_cast<uint32_t>(TransformPropertySlots.size());
            Stack.pop_back();
        }

        for (TransformPropertySlot& Slot : TransformPropertySlots)
        {
            const auto DrawState = DrawIndicesByNode.find(Slot.Node);
            if (DrawState == DrawIndicesByNode.end()) continue;
            for (size_t DrawIndex : DrawState->second.DrawIndices)
                if (DrawIndex >= Draws.size())
                {
                    Slot.SubtreeHasClipMaskDependency = true;
                    break;
                }
                else
                {
                    Slot.SubtreeHasScissorDependency |= Draws[DrawIndex].ScissorEnabled != 0;
                    Slot.SubtreeHasClipMaskDependency |= Draws[DrawIndex].ClipMaskCount != 0;
                }
        }
        for (size_t Index = TransformPropertySlots.size(); Index-- > 0;)
        {
            TransformPropertySlot& Slot = TransformPropertySlots[Index];
            if (Slot.Parent < TransformPropertySlots.size())
            {
                TransformPropertySlot& Parent = TransformPropertySlots[Slot.Parent];
                Parent.SubtreeHasScissorDependency |= Slot.SubtreeHasScissorDependency;
                Parent.SubtreeHasClipMaskDependency |= Slot.SubtreeHasClipMaskDependency;
            }
        }
    }
    bool QueueTransformPropertyRoot(Rml::Element* Root, RmlUE_AnimationTarget Target)
    {
        uint32_t SlotIndex = UINT32_MAX;
        if (TransformBindingCache* Cache = FindTransformBindingCache(Target))
            SlotIndex = Cache->RootPropertySlot;
        else
        {
            const auto Found = TransformPropertySlotByElement.find(Root);
            if (Found != TransformPropertySlotByElement.end()) SlotIndex = Found->second;
        }
        if (SlotIndex >= TransformPropertySlots.size()) return false;
        TransformPropertySlot& Slot = TransformPropertySlots[SlotIndex];
        if (Slot.Dirty)
        {
            PendingTransformPropertiesOrderedDisjoint = false;
        }
        else
        {
            Slot.Dirty = true;
            if (PendingTransformPropertyMin != UINT32_MAX && SlotIndex < PendingTransformPropertyLastEnd)
                PendingTransformPropertiesOrderedDisjoint = false;
            PendingTransformPropertyMin = std::min(PendingTransformPropertyMin, SlotIndex);
            PendingTransformPropertyMax = std::max(PendingTransformPropertyMax, SlotIndex);
            PendingTransformPropertyLastEnd = Slot.SubtreeEnd;
        }
        return true;
    }
    void BeginVisualTransformBatch()
    {
        if (TransformBatchUsesCoverage)
        {
            const uint32_t Last = std::min(PendingTransformPropertyMax,
                static_cast<uint32_t>(TransformPropertySlots.size() - 1));
            for (uint32_t SlotIndex = PendingTransformPropertyMin; SlotIndex <= Last; ++SlotIndex)
            {
                TransformPropertySlot& Slot = TransformPropertySlots[SlotIndex];
                Slot.Dirty = false;
                Slot.BatchPublishRoot = false;
                Slot.BatchPublished = false;
            }
        }
        PendingTransformPropertyMin = UINT32_MAX;
        PendingTransformPropertyMax = 0;
        PendingTransformPropertyLastEnd = 0;
        PendingTransformPropertiesOrderedDisjoint = true;
        TransformBatchUsesCoverage = false;
    }
    void SynchronizeVisualTransformBatch()
    {
        if (PendingTransformPropertyMin >= TransformPropertySlots.size() ||
            PendingTransformPropertiesOrderedDisjoint) return;
        TransformBatchUsesCoverage = true;
        const uint32_t Last = std::min(PendingTransformPropertyMax,
            static_cast<uint32_t>(TransformPropertySlots.size() - 1));
        uint32_t CoveredUntil = 0;
        for (uint32_t Index = PendingTransformPropertyMin; Index <= Last; ++Index)
        {
            TransformPropertySlot& Slot = TransformPropertySlots[Index];
            if (!Slot.Dirty) continue;
            Slot.Dirty = false;
            Slot.BatchPublishRoot = Index >= CoveredUntil;
            Slot.BatchPublished = false;
            if (!Slot.BatchPublishRoot) continue;
            if (Rml::Element* Element = Slot.Element.get())
            {
                Element->SynchronizeAnimationTransformStateTree();
                CoveredUntil = Slot.SubtreeEnd;
            }
        }
    }
    bool CollectTransformDrawNodes(Rml::Element* Root, std::vector<TransformDrawNode>& OutDrawNodes,
        std::vector<TransformMaskNode>& OutMaskNodes)
    {
        TransformElementScratch.clear();
        OutDrawNodes.clear();
        OutMaskNodes.clear();
        if (!Root || !ResolveNode) return false;
        const auto PropertyRoot = TransformPropertySlotByElement.find(Root);
        if (PropertyRoot != TransformPropertySlotByElement.end())
        {
            const uint32_t RootSlot = PropertyRoot->second;
            if (RootSlot >= TransformPropertySlots.size()) return false;
            const uint32_t End = TransformPropertySlots[RootSlot].SubtreeEnd;
            std::unordered_map<RmlUE_Node, size_t> MaskBindingByNode;
            for (uint32_t SlotIndex = RootSlot; SlotIndex < End; ++SlotIndex)
            {
                const TransformPropertySlot& PropertySlot = TransformPropertySlots[SlotIndex];
                const auto Indices = DrawIndicesByNode.find(PropertySlot.Node);
                if (!PropertySlot.Node || Indices == DrawIndicesByNode.end() || Indices->second.DrawIndices.empty()) continue;
                TransformDrawNode& Binding = OutDrawNodes.emplace_back();
                Binding.Node = PropertySlot.Node;
                Binding.Element = PropertySlot.Element;
                Binding.DrawIndices = Indices->second.DrawIndices;
                Binding.VisualSlot = Indices->second.VisualSlot;
                if (!Binding.Element || Binding.VisualSlot >= VisualNodeSlots.size()) return false;
                for (size_t DrawIndex : Binding.DrawIndices)
                {
                    if (DrawIndex >= Draws.size()) return false;
                    const RmlUE_SlateDraw& Draw = Draws[DrawIndex];
                    if (Draw.ClipMaskStart > PublicClipMasks.size() ||
                        Draw.ClipMaskCount > PublicClipMasks.size() - Draw.ClipMaskStart) return false;
                    for (uint32_t Offset = 0; Offset < Draw.ClipMaskCount; ++Offset)
                    {
                        const size_t MaskIndex = static_cast<size_t>(Draw.ClipMaskStart) + Offset;
                        const RmlUE_Node OwnerNode = PublicClipMasks[MaskIndex].OwnerNode;
                        const auto OwnerSlotIt = TransformPropertySlotByNode.find(OwnerNode);
                        if (!OwnerNode || OwnerSlotIt == TransformPropertySlotByNode.end()) return false;
                        const uint32_t OwnerSlotIndex = OwnerSlotIt->second;
                        if (OwnerSlotIndex < RootSlot || OwnerSlotIndex >= End) continue;
                        auto [MaskIt, Inserted] = MaskBindingByNode.try_emplace(OwnerNode, OutMaskNodes.size());
                        if (Inserted)
                        {
                            const TransformPropertySlot& OwnerSlot = TransformPropertySlots[OwnerSlotIndex];
                            if (!OwnerSlot.Element) return false;
                            TransformMaskNode& MaskNode = OutMaskNodes.emplace_back();
                            MaskNode.Node = OwnerNode;
                            MaskNode.Element = OwnerSlot.Element;
                        }
                        OutMaskNodes[MaskIt->second].MaskIndices.push_back(MaskIndex);
                    }
                }
            }
            return !OutDrawNodes.empty();
        }
        TransformElementScratch.push_back(Root);
        for (size_t ElementIndex = 0; ElementIndex < TransformElementScratch.size(); ++ElementIndex)
        {
            Rml::Element* Element = TransformElementScratch[ElementIndex];
            for (int ChildIndex = 0; ChildIndex < Element->GetNumChildren(true); ++ChildIndex)
                TransformElementScratch.push_back(Element->GetChild(ChildIndex));
            const RmlUE_Node DrawNode = ResolveNode(Element);
            const auto Indices = DrawIndicesByNode.find(DrawNode);
            if (!DrawNode || Indices == DrawIndicesByNode.end() || Indices->second.DrawIndices.empty()) continue;
            for (size_t DrawIndex : Indices->second.DrawIndices)
                if (DrawIndex >= Draws.size() || Draws[DrawIndex].ClipMaskCount != 0)
                    return false;
            TransformDrawNode& Binding = OutDrawNodes.emplace_back();
            Binding.Node = DrawNode;
            Binding.Element = Element->GetObserverPtr();
            Binding.DrawIndices = Indices->second.DrawIndices;
            Binding.VisualSlot = Indices->second.VisualSlot;
            if (Binding.VisualSlot >= VisualNodeSlots.size()) return false;
        }
        return !OutDrawNodes.empty();
    }
    std::vector<TransformDrawNode>* ResolveTransformDrawNodes(
        Rml::Element* Root, RmlUE_AnimationTarget Target)
    {
        uint32_t Index = 0, Generation = 0;
        if (DecodeAnimationTarget(Target, Index, Generation))
        {
            if (Index >= TransformBindingCaches.size()) TransformBindingCaches.resize(static_cast<size_t>(Index) + 1);
            TransformBindingCache& Cache = TransformBindingCaches[Index];
            if (Cache.Generation == Generation && Cache.ContentRevision == ContentRevision &&
                Cache.TopologyGeneration == TopologyGeneration && Cache.Root.get() == Root && !Cache.DrawNodes.empty())
                return &Cache.DrawNodes;
            const bool OverrideActive = Cache.Generation == Generation && Cache.OverrideActive;
            const VisualTransformState OverrideState = Cache.OverrideState;
            Cache = {};
            TransformMaskBindingsByIndex.erase(Index);
            std::vector<TransformMaskNode> MaskNodes;
            if (!CollectTransformDrawNodes(Root, Cache.DrawNodes, MaskNodes)) return nullptr;
            if (!MaskNodes.empty())
                TransformMaskBindingsByIndex.emplace(Index, std::move(MaskNodes));
            Cache.Generation = Generation;
            Cache.ContentRevision = ContentRevision;
            Cache.TopologyGeneration = TopologyGeneration;
            const auto PropertySlot = TransformPropertySlotByElement.find(Root);
            Cache.RootPropertySlot = PropertySlot != TransformPropertySlotByElement.end()
                ? PropertySlot->second : UINT32_MAX;
            Cache.Root = Root->GetObserverPtr();
            Cache.OverrideActive = OverrideActive;
            Cache.OverrideState = OverrideState;
            return &Cache.DrawNodes;
        }
        return CollectTransformDrawNodes(Root, TransformDrawScratch, TransformMaskScratch) ? &TransformDrawScratch : nullptr;
    }
    bool UpdateCollectedTransformDraws(std::vector<TransformDrawNode>& DrawNodes,
        std::vector<TransformMaskNode>* MaskNodes)
    {
        for (TransformDrawNode& DrawNode : DrawNodes)
            if (!DrawNode.Element || !ReadElementTransform(DrawNode.Element.get(), DrawNode.State)) return false;
        if (MaskNodes)
            for (TransformMaskNode& MaskNode : *MaskNodes)
                if (!MaskNode.Element || !ReadElementTransform(MaskNode.Element.get(), MaskNode.State)) return false;
        for (const TransformDrawNode& DrawNode : DrawNodes)
        {
            for (size_t DrawIndex : DrawNode.DrawIndices)
            {
                if (DrawIndex >= Draws.size()) return false;
                RmlUE_SlateDraw& Draw = Draws[DrawIndex];
                Draw.TransformEnabled = DrawNode.State.Enabled ? 1 : 0;
                Draw.TransformM00 = DrawNode.State.M00; Draw.TransformM01 = DrawNode.State.M01;
                Draw.TransformM10 = DrawNode.State.M10; Draw.TransformM11 = DrawNode.State.M11;
                Draw.TransformX = DrawNode.State.X; Draw.TransformY = DrawNode.State.Y;
            }
            if (DrawNode.VisualSlot >= VisualNodeSlots.size()) return false;
            VisualNodeSlot& Slot = VisualNodeSlots[DrawNode.VisualSlot];
            Slot.PendingTransform = DrawNode.State;
            Slot.TransformPending = true;
            QueueVisualSlot(DrawNode.VisualSlot);
        }
        if (MaskNodes) for (const TransformMaskNode& MaskNode : *MaskNodes)
        {
            for (size_t MaskIndex : MaskNode.MaskIndices)
            {
                if (MaskIndex >= PublicClipMasks.size()) return false;
                RmlUE_SlateClipMask& Mask = PublicClipMasks[MaskIndex];
                Mask.TransformEnabled = MaskNode.State.Enabled ? 1 : 0;
                Mask.TransformM00 = MaskNode.State.M00; Mask.TransformM01 = MaskNode.State.M01;
                Mask.TransformM10 = MaskNode.State.M10; Mask.TransformM11 = MaskNode.State.M11;
                Mask.TransformX = MaskNode.State.X; Mask.TransformY = MaskNode.State.Y;
            }
            QueueClipMaskTransform(MaskNode.Node, MaskNode.State);
        }
        return true;
    }
    bool PrepareVisualTransform(RmlUE_Node Node, Rml::Element* Element, const float* Values,
        RmlUE_AnimationTarget Target)
    {
        if (!Node || !Element || !Values) return false;
        const auto RejectActiveOverride = [this, Node, Element, Target]()
        {
            TransformBindingCache* Cache = FindTransformBindingCache(Target);
            const auto Existing = VisualTransformOverrides.find(Node);
            if ((Cache && Cache->OverrideActive) ||
                (Existing != VisualTransformOverrides.end() && Existing->second.Active))
            {
                Element->ClearAnimationTransform2D();
                Element->SynchronizeAnimationTransformStateTree();
                if (Cache) Cache->OverrideActive = false;
                if (Existing != VisualTransformOverrides.end()) VisualTransformOverrides.erase(Existing);
                MarkContentDirty();
            }
            return false;
        };
        if (!HasRecordedFrame || RecordedContentRevision != ContentRevision ||
            !Element->GetComputedValues().transform() || !ResolveTransformDrawNodes(Element, Target))
            return RejectActiveOverride();

        Element->SetAnimationTransform2D(Values[0], Values[1], Values[2], Values[3], Values[4]);
        if (TransformBindingCache* Cache = FindTransformBindingCache(Target))
        {
            Cache->OverrideActive = true;
            Cache->OverrideState.Active = true;
        }
        else
        {
            auto Existing = VisualTransformOverrides.try_emplace(Node).first;
            Existing->second.Active = true;
        }
        if (!QueueTransformPropertyRoot(Element, Target)) return RejectActiveOverride();
        if (++VisualRevision == 0) VisualRevision = 1;
        return true;
    }
    bool PublishVisualTransform(RmlUE_Node Node, Rml::Element* Element, RmlUE_AnimationTarget Target)
    {
        TransformBindingCache* Cache = FindTransformBindingCache(Target);
        std::vector<TransformDrawNode>* DrawNodes = Cache ? &Cache->DrawNodes :
            (Element ? ResolveTransformDrawNodes(Element, Target) : nullptr);
        std::vector<TransformMaskNode>* MaskNodes = !Cache ? &TransformMaskScratch :
            (!TransformMaskBindingsByIndex.empty() ? FindTransformMaskBinding(Target) : nullptr);
        uint32_t PropertySlotIndex = Cache ? Cache->RootPropertySlot : UINT32_MAX;
        if (!Cache && Element)
        {
            const auto PropertySlot = TransformPropertySlotByElement.find(Element);
            if (PropertySlot != TransformPropertySlotByElement.end()) PropertySlotIndex = PropertySlot->second;
        }
        TransformPropertySlot* PropertySlot = PropertySlotIndex < TransformPropertySlots.size()
            ? &TransformPropertySlots[PropertySlotIndex] : nullptr;
        auto Existing = VisualTransformOverrides.find(Node);
        VisualTransformState* State = Cache && Cache->OverrideActive ? &Cache->OverrideState :
            (Existing != VisualTransformOverrides.end() && Existing->second.Active ? &Existing->second : nullptr);
        if (TransformBatchUsesCoverage)
        {
            if (PropertySlot && !PropertySlot->BatchPublishRoot) return State && Element && DrawNodes;
            if (PropertySlot && PropertySlot->BatchPublished) return State && Element && DrawNodes;
        }
        else if (PropertySlot && PropertySlot->Dirty)
        {
            PropertySlot->Dirty = false;
            if (Element) Element->SynchronizeAnimationTransformStateTree();
        }
        if (!State || !Element || !DrawNodes || !ReadElementTransform(Element, *State) ||
            !UpdateCollectedTransformDraws(*DrawNodes, MaskNodes))
        {
            if (Element)
            {
                Element->ClearAnimationTransform2D();
                Element->SynchronizeAnimationTransformStateTree();
            }
            if (Cache) Cache->OverrideActive = false;
            else VisualTransformOverrides.erase(Node);
            MarkContentDirty();
            return false;
        }
        if (TransformBatchUsesCoverage && PropertySlot) PropertySlot->BatchPublished = true;
        return true;
    }
    void ReleaseTransformBinding(RmlUE_AnimationTarget Target)
    {
        uint32_t Index = 0, Generation = 0;
        if (!DecodeAnimationTarget(Target, Index, Generation) || Index >= TransformBindingCaches.size()) return;
        TransformBindingCache& Cache = TransformBindingCaches[Index];
        if (Cache.Generation != Generation) return;
        if (Cache.OverrideActive)
        {
            if (Rml::Element* Element = Cache.Root.get())
            {
                Element->ClearAnimationTransform2D();
                Element->SynchronizeAnimationTransformStateTree();
            }
            MarkContentDirty();
        }
        Cache = {};
        TransformMaskBindingsByIndex.erase(Index);
    }
    void ClearTransformBindings()
    {
        TransformBindingCaches.clear();
        TransformMaskBindingsByIndex.clear();
        TransformPropertySlots.clear();
        TransformPropertySlotByElement.clear();
        TransformPropertySlotByNode.clear();
        PendingTransformPropertyMin = UINT32_MAX;
        PendingTransformPropertyMax = 0;
        PendingTransformPropertyLastEnd = 0;
        PendingTransformPropertiesOrderedDisjoint = true;
        TransformBatchUsesCoverage = false;
    }
    bool ClearVisualTransform(RmlUE_Node Node, Rml::Element* Element, RmlUE_AnimationTarget Target = 0)
    {
        TransformBindingCache* Cache = FindTransformBindingCache(Target);
        const auto Existing = VisualTransformOverrides.find(Node);
        if ((!Cache || !Cache->OverrideActive) &&
            (Existing == VisualTransformOverrides.end() || !Existing->second.Active)) return false;
        if (!Element) return false;
        std::vector<TransformDrawNode>* DrawNodes = HasRecordedFrame && RecordedContentRevision == ContentRevision
            ? ResolveTransformDrawNodes(Element, Target) : nullptr;
        std::vector<TransformMaskNode>* MaskNodes = !Cache ? &TransformMaskScratch :
            (!TransformMaskBindingsByIndex.empty() ? FindTransformMaskBinding(Target) : nullptr);
        const bool CanUpdateRetainedDraws = DrawNodes != nullptr;
        Element->ClearAnimationTransform2D();
        Element->SynchronizeAnimationTransformStateTree();
        if (Cache) Cache->OverrideActive = false;
        if (Existing != VisualTransformOverrides.end()) VisualTransformOverrides.erase(Existing);
        if (!CanUpdateRetainedDraws || !UpdateCollectedTransformDraws(*DrawNodes, MaskNodes))
        {
            MarkContentDirty();
        }
        if (++VisualRevision == 0) VisualRevision = 1;
        return true;
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override
    {
        auto Geometry = std::make_unique<GeometryData>();
        Geometry->Id = ++NextGeometry;
        Geometry->Vertices.reserve(Vertices.size());
        for (const Rml::Vertex& Vertex : Vertices)
            Geometry->Vertices.push_back({Vertex.position.x, Vertex.position.y, Vertex.tex_coord.x, Vertex.tex_coord.y,
                Vertex.colour.red, Vertex.colour.green, Vertex.colour.blue, Vertex.colour.alpha});
        Geometry->Indices.reserve(Indices.size());
        for (int Index : Indices) Geometry->Indices.push_back(static_cast<uint32_t>(Index));
        Geometry->ResourceId = RegisterResource(RMLUE_RESOURCE_GEOMETRY, RMLUE_RESOURCE_BACKEND_SLATE, OwnerResourceId,
            Geometry->Vertices.size() * sizeof(RmlUE_SlateVertex) + Geometry->Indices.size() * sizeof(uint32_t), "Slate compiled geometry");
        GeometryData* Result = Geometry.release();
        GeometryDataById.emplace(Result->Id, Result);
        return reinterpret_cast<Rml::CompiledGeometryHandle>(Result);
    }
    void RenderGeometry(Rml::CompiledGeometryHandle Handle, Rml::Vector2f Translation, Rml::TextureHandle Texture) override
    {
        const auto* Geometry = reinterpret_cast<const GeometryData*>(Handle);
        if (!Geometry) return;
        const Rml::Rectanglei Region = Scissor;
        const uint32_t ClipMaskStart = static_cast<uint32_t>(PublicClipMasks.size());
        if (ClipMaskEnabled)
            PublicClipMasks.insert(PublicClipMasks.end(), ActiveClipMasks.begin(), ActiveClipMasks.end());
        const uint32_t ClipMaskCount = static_cast<uint32_t>(PublicClipMasks.size()) - ClipMaskStart;
        const size_t DrawIndex = Draws.size();
        Draws.push_back({Geometry->Id, static_cast<uint64_t>(Texture), Translation.x, Translation.y,
            TransformEnabled ? 1 : 0, TransformM00, TransformM01, TransformM10, TransformM11, TransformX, TransformY,
            ScissorEnabled ? 1 : 0, static_cast<float>(Region.Left()), static_cast<float>(Region.Top()),
            static_cast<float>(Region.Width()), static_cast<float>(Region.Height()), ClipMaskStart, ClipMaskCount,
            CurrentElementVisual.Node, CurrentElementVisual.Opacity});
        if (CurrentElementVisual.Node)
        {
            EnsureVisualNodeSlot(CurrentElementVisual.Node);
            auto StateIt = DrawIndicesByNode.find(CurrentElementVisual.Node);
            StateIt->second.DrawIndices.push_back(DrawIndex);
        }
        if (ActiveStats) ++ActiveStats->GeometryDraws;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle Handle) override
    {
        auto* Geometry = reinterpret_cast<GeometryData*>(Handle);
        if (Geometry) Geometry->PendingRelease = true;
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& Dimensions, const Rml::String& Source) override
    {
        static constexpr const char* BackgroundPrefix = "ue-material://background/";
        static constexpr const char* BorderPrefix = "ue-material://border/";
        const char* MaterialPrefix = nullptr;
        int MaterialSlot = RMLUE_MATERIAL_SLOT_NONE;
        if (Source.rfind(BackgroundPrefix, 0) == 0)
        {
            MaterialPrefix = BackgroundPrefix;
            MaterialSlot = RMLUE_MATERIAL_SLOT_BACKGROUND;
        }
        else if (Source.rfind(BorderPrefix, 0) == 0)
        {
            MaterialPrefix = BorderPrefix;
            MaterialSlot = RMLUE_MATERIAL_SLOT_BORDER;
        }
        if (MaterialPrefix)
        {
            TextureData Texture;
            Texture.Id = ++NextTexture;
            Texture.Kind = 1;
            Texture.MaterialSlot = MaterialSlot;
            Texture.Alias = Source.substr(std::strlen(MaterialPrefix));
            if (Texture.Alias.empty())
            {
                Fail("UE material alias cannot be empty.");
                return 0;
            }
            Dimensions = {1, 1};
            Texture.ResourceId = RegisterResource(RMLUE_RESOURCE_MATERIAL_BINDING, RMLUE_RESOURCE_BACKEND_SLATE,
                OwnerResourceId, 0, Texture.Alias);
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
        Texture.ResourceId = RegisterResource(RMLUE_RESOURCE_TEXTURE, RMLUE_RESOURCE_BACKEND_SLATE, OwnerResourceId,
            Texture.Pixels.size(), "Slate texture");
        TextureDataById.emplace(Texture.Id, std::move(Texture));
        return static_cast<Rml::TextureHandle>(NextTexture);
    }
    void ReleaseTexture(Rml::TextureHandle Texture) override
    {
        const auto Found = TextureDataById.find(static_cast<uint64_t>(Texture));
        if (Found == TextureDataById.end()) return;
        Found->second.PendingRelease = true;
    }
    void EnableScissorRegion(bool Enable) override { ScissorEnabled = Enable; }
    void SetScissorRegion(Rml::Rectanglei Region) override { Scissor = Region; }
    void EnableClipMask(bool Enable) override
    {
        ClipMaskEnabled = Enable;
        if (!Enable) ActiveClipMasks.clear();
    }
    void SetClipMaskOwner(Rml::Element* Element) override
    {
        CurrentClipMaskOwnerNode = Element && ResolveNode ? ResolveNode(Element) : 0;
    }
    void RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Handle,
        Rml::Vector2f Translation) override
    {
        const auto* Geometry = reinterpret_cast<const GeometryData*>(Handle);
        if (!Geometry) return;
        if (Operation == Rml::ClipMaskOperation::Set || Operation == Rml::ClipMaskOperation::SetInverse)
            ActiveClipMasks.clear();
        if (ActiveClipMasks.size() >= 254)
        {
            UnsupportedFeatures |= RMLUE_UNSUPPORTED_CLIP_MASK;
            return;
        }
        const Rml::Rectanglei Region = Scissor;
        ActiveClipMasks.push_back({Geometry->Id, CurrentClipMaskOwnerNode, static_cast<int>(Operation), Translation.x, Translation.y,
            TransformEnabled ? 1 : 0, TransformM00, TransformM01, TransformM10, TransformM11, TransformX, TransformY,
            ScissorEnabled ? 1 : 0, static_cast<float>(Region.Left()), static_cast<float>(Region.Top()),
            static_cast<float>(Region.Width()), static_cast<float>(Region.Height())});
        if (ActiveStats) ++ActiveStats->ClipMasks;
    }
    void SetTransform(const Rml::Matrix4f* Transform) override
    {
        TransformEnabled = false;
        TransformM00 = TransformM11 = 1.f;
        TransformM01 = TransformM10 = TransformX = TransformY = 0.f;
        if (!Transform) return;
        const auto Row0 = Transform->GetRow(0);
        const auto Row1 = Transform->GetRow(1);
        const auto Row2 = Transform->GetRow(2);
        const auto Row3 = Transform->GetRow(3);
        const auto Near = [](float A, float B) { return std::abs(A - B) <= 0.0001f; };
        const bool Is2DAffine = Near(Row0[2], 0.f) && Near(Row1[2], 0.f) &&
            Near(Row2[0], 0.f) && Near(Row2[1], 0.f) && Near(Row2[2], 1.f) && Near(Row2[3], 0.f) &&
            Near(Row3[0], 0.f) && Near(Row3[1], 0.f) && Near(Row3[2], 0.f) && Near(Row3[3], 1.f);
        if (!Is2DAffine)
        {
            UnsupportedFeatures |= RMLUE_UNSUPPORTED_TRANSFORM_3D;
            return;
        }
        TransformEnabled = true;
        TransformM00 = Row0[0];
        TransformM01 = Row0[1];
        TransformM10 = Row1[0];
        TransformM11 = Row1[1];
        TransformX = Row0[3];
        TransformY = Row1[3];
    }
    void BeginElement(Rml::Element* Element) override
    {
        ElementVisualStack.push_back(CurrentElementVisual);
        CurrentElementVisual = {};
        if (!Element || !ResolveNode) return;
        const RmlUE_Node Node = ResolveNode(Element);
        if (!Node) return;
        CurrentElementVisual.Node = Node;
        const float BaseOpacity = Element->GetComputedValues().opacity();
        const auto Override = VisualOpacityOverrides.find(Node);
        if (Override == VisualOpacityOverrides.end() || !Override->second.Active) return;
        if (BaseOpacity <= 1.f / 255.f) return;
        CurrentElementVisual.Opacity = std::max(0.f, Override->second.Opacity / BaseOpacity);
    }
    void EndElement(Rml::Element*) override
    {
        if (ElementVisualStack.empty()) { CurrentElementVisual = {}; return; }
        CurrentElementVisual = ElementVisualStack.back();
        ElementVisualStack.pop_back();
    }
    Rml::LayerHandle PushLayer() override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_LAYER; return 0; }
    void CompositeLayers(Rml::LayerHandle, Rml::LayerHandle, Rml::BlendMode, Rml::Span<const Rml::CompiledFilterHandle>) override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_LAYER; }
    void PopLayer() override {}
    Rml::TextureHandle SaveLayerAsTexture() override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_LAYER; return 0; }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_LAYER; return 0; }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String&, const Rml::Dictionary&) override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_FILTER; return 0; }
    Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override { UnsupportedFeatures |= RMLUE_UNSUPPORTED_SHADER; return 0; }

    void BeginFrame()
    {
        Draws.clear();
        DrawIndicesByNode.clear();
        VisualNodeSlots.clear();
        PendingVisualSlots.clear();
        PendingClipMaskTransforms.clear();
        PendingClipMaskTransformByNode.clear();
        PublicClipMasks.clear();
        PublicVisualDeltas.clear();
        ClipMaskEnabled = false;
        CurrentClipMaskOwnerNode = 0;
        ActiveClipMasks.clear();
        PublicGeometryDeltas.clear();
        PublicTextures.clear();
        UnsupportedFeatures = 0;
        ElementVisualStack.clear();
        CurrentElementVisual = {};
        for (auto It = GeometryDataById.begin(); It != GeometryDataById.end();)
        {
            GeometryData* Geometry = It->second;
            if (!Geometry->PendingRelease) { ++It; continue; }
            if (Geometry->Published) ReleasedGeometryIds.push_back(Geometry->Id);
            UnregisterResource(Geometry->ResourceId);
            delete Geometry;
            It = GeometryDataById.erase(It);
        }
        for (auto It = TextureDataById.begin(); It != TextureDataById.end();)
        {
            TextureData& Texture = It->second;
            if (!Texture.PendingRelease) { ++It; continue; }
            if (Texture.Published) ReleasedTextureIds.push_back(Texture.Id);
            UnregisterResource(Texture.ResourceId);
            It = TextureDataById.erase(It);
        }
    }
    void EndFrame()
    {
        std::sort(ReleasedGeometryIds.begin(), ReleasedGeometryIds.end());
        for (uint64_t Id : ReleasedGeometryIds)
            PublicGeometryDeltas.push_back({Id, RMLUE_SLATE_RESOURCE_DESTROY, nullptr, 0, nullptr, 0});
        ReleasedGeometryIds.clear();
        std::vector<uint64_t> GeometryIds;
        GeometryIds.reserve(GeometryDataById.size());
        for (const auto& Pair : GeometryDataById) if (!Pair.second->Published) GeometryIds.push_back(Pair.first);
        std::sort(GeometryIds.begin(), GeometryIds.end());
        for (uint64_t Id : GeometryIds)
        {
            GeometryData& Geometry = *GeometryDataById.at(Id);
            PublicGeometryDeltas.push_back({Geometry.Id, RMLUE_SLATE_RESOURCE_CREATE, Geometry.Vertices.data(),
                static_cast<uint32_t>(Geometry.Vertices.size()), Geometry.Indices.data(), static_cast<uint32_t>(Geometry.Indices.size())});
            Geometry.Published = true;
        }

        std::sort(ReleasedTextureIds.begin(), ReleasedTextureIds.end());
        for (uint64_t Id : ReleasedTextureIds)
            PublicTextures.push_back({Id, RMLUE_SLATE_RESOURCE_DESTROY, 0, RMLUE_MATERIAL_SLOT_NONE, nullptr, 0, 0, nullptr});
        ReleasedTextureIds.clear();
        std::vector<uint64_t> TextureIds;
        TextureIds.reserve(TextureDataById.size());
        for (const auto& Pair : TextureDataById) if (!Pair.second.Published) TextureIds.push_back(Pair.first);
        std::sort(TextureIds.begin(), TextureIds.end());
        for (uint64_t Id : TextureIds)
        {
            TextureData& Texture = TextureDataById.at(Id);
            PublicTextures.push_back({Texture.Id, RMLUE_SLATE_RESOURCE_CREATE, Texture.Kind, Texture.MaterialSlot,
                Texture.Pixels.empty() ? nullptr : Texture.Pixels.data(),
                Texture.Width, Texture.Height, Texture.Alias.empty() ? nullptr : Texture.Alias.c_str()});
            Texture.Published = true;
        }
    }

    bool CanReplay(double NextUpdateDelay) const
    {
        return HasRecordedFrame && RecordedContentRevision == ContentRevision && !std::isfinite(NextUpdateDelay);
    }
    void BeginReplayFrame()
    {
        PublicVisualDeltas.clear();
        std::sort(PendingVisualSlots.begin(), PendingVisualSlots.end(), [this](uint32_t A, uint32_t B)
        {
            return VisualNodeSlots[A].Node < VisualNodeSlots[B].Node;
        });
        PublicVisualDeltas.reserve(PendingVisualSlots.size() + PendingClipMaskTransforms.size());
        for (uint32_t SlotIndex : PendingVisualSlots)
        {
            if (SlotIndex >= VisualNodeSlots.size()) continue;
            VisualNodeSlot& Slot = VisualNodeSlots[SlotIndex];
            RmlUE_SlateVisualDelta Delta{};
            Delta.Node = Slot.Node;
            bool Changed = false;
            if (Slot.OpacityPending)
            {
                Delta.VisualOpacity = Slot.PendingOpacity;
                Delta.OpacityChanged = 1;
                Slot.OpacityPending = false;
                if (Slot.RemoveOpacityOverrideAfterReplay) VisualOpacityOverrides.erase(Slot.Node);
                Slot.RemoveOpacityOverrideAfterReplay = false;
                Changed = true;
            }
            if (Slot.TransformPending)
            {
                const VisualTransformState& State = Slot.PendingTransform;
                Delta.TransformChanged = 1; Delta.TransformEnabled = State.Enabled ? 1 : 0;
                Delta.TransformM00 = State.M00; Delta.TransformM01 = State.M01;
                Delta.TransformM10 = State.M10; Delta.TransformM11 = State.M11;
                Delta.TransformX = State.X; Delta.TransformY = State.Y;
                Slot.TransformPending = false;
                Changed = true;
            }
            Slot.Queued = false;
            if (Changed) PublicVisualDeltas.push_back(Delta);
        }
        for (const PendingClipMaskTransform& Pending : PendingClipMaskTransforms)
        {
            const VisualTransformState& State = Pending.State;
            RmlUE_SlateVisualDelta Delta{};
            Delta.Node = Pending.Node;
            Delta.ClipMaskTransformChanged = 1;
            Delta.TransformEnabled = State.Enabled ? 1 : 0;
            Delta.TransformM00 = State.M00; Delta.TransformM01 = State.M01;
            Delta.TransformM10 = State.M10; Delta.TransformM11 = State.M11;
            Delta.TransformX = State.X; Delta.TransformY = State.Y;
            PublicVisualDeltas.push_back(Delta);
        }
        PendingVisualSlots.clear();
        PendingClipMaskTransforms.clear();
        PendingClipMaskTransformByNode.clear();
        PublicGeometryDeltas.clear();
        PublicTextures.clear();
        ++ReplayedFrames;
    }
    void FinishFullFrame(Rml::Element* Root)
    {
        RecordedContentRevision = ContentRevision;
        HasRecordedFrame = true;
        if (++TopologyGeneration == 0) TopologyGeneration = 1;
        BuildTransformPropertyTree(Root);
        PendingVisualSlots.clear();
        PendingClipMaskTransforms.clear();
        PendingClipMaskTransformByNode.clear();
        for (auto It = VisualOpacityOverrides.begin(); It != VisualOpacityOverrides.end();)
        {
            if (!It->second.Active && !It->second.BindingRefCount)
            {
                It = VisualOpacityOverrides.erase(It);
            }
            else
            {
                RefreshVisualOpacityTopology(It->first, It->second);
                ++It;
            }
        }
        for (auto It = VisualTransformOverrides.begin(); It != VisualTransformOverrides.end();)
        {
            if (!It->second.Active) It = VisualTransformOverrides.erase(It);
            else ++It;
        }
        ++FullRenderFrames;
    }
    void GetReplayStats(RmlUE_SlateReplayStats& OutStats) const
    {
        OutStats = {ContentRevision, FullRenderFrames, ReplayedFrames};
    }
    void GetScheduleState(double NextUpdateDelay, RmlUE_SlateScheduleState& OutState) const
    {
        OutState = {ContentRevision, VisualRevision, NextUpdateDelay,
            HasRecordedFrame ? 1 : 0, CanReplay(NextUpdateDelay) ? 1 : 0};
    }
};

class UeMaterialDecorator final : public Rml::Decorator {
    int TextureIndex = -1;
    int MaterialSlot = RMLUE_MATERIAL_SLOT_BACKGROUND;
public:
    bool Initialise(const Rml::Texture& Texture, int InMaterialSlot)
    {
        MaterialSlot = InMaterialSlot;
        TextureIndex = AddTexture(Texture);
        return TextureIndex >= 0;
    }
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element* Element, Rml::BoxArea PaintArea) const override
    {
        Rml::RenderManager* RenderManager = Element->GetRenderManager();
        if (!RenderManager) return INVALID_DECORATORDATAHANDLE;
        const Rml::ColourbPremultiplied Tint =
            Rml::Colourb(255).ToPremultiplied(Element->GetComputedValues().opacity());
        Rml::Mesh Mesh;
        for (int Index = 0; Index < Element->GetNumBoxes(); ++Index)
        {
            const Rml::RenderBox RenderBox = Element->GetRenderBox(PaintArea, Index);
            if (MaterialSlot == RMLUE_MATERIAL_SLOT_BORDER)
            {
                const Rml::ColourbPremultiplied Transparent(0, 0, 0, 0);
                const Rml::ColourbPremultiplied BorderColors[4] = {Tint, Tint, Tint, Tint};
                Rml::MeshUtilities::GenerateBackgroundBorder(Mesh, RenderBox, Transparent, BorderColors);
            }
            else
            {
                Rml::MeshUtilities::GenerateBackground(Mesh, RenderBox, Tint);
            }
        }
        if (Mesh.vertices.empty()) return INVALID_DECORATORDATAHANDLE;
        Rml::Vector2f Minimum = Mesh.vertices.front().position;
        Rml::Vector2f Maximum = Minimum;
        for (const Rml::Vertex& Vertex : Mesh.vertices)
        {
            Minimum.x = std::min(Minimum.x, Vertex.position.x);
            Minimum.y = std::min(Minimum.y, Vertex.position.y);
            Maximum.x = std::max(Maximum.x, Vertex.position.x);
            Maximum.y = std::max(Maximum.y, Vertex.position.y);
        }
        const Rml::Vector2f Extent = Maximum - Minimum;
        for (Rml::Vertex& Vertex : Mesh.vertices)
            Vertex.tex_coord = {(Vertex.position.x - Minimum.x) / std::max(Extent.x, 1.f),
                (Vertex.position.y - Minimum.y) / std::max(Extent.y, 1.f)};
        auto* Geometry = new Rml::Geometry(RenderManager->MakeGeometry(std::move(Mesh)));
        return reinterpret_cast<Rml::DecoratorDataHandle>(Geometry);
    }
    void ReleaseElementData(Rml::DecoratorDataHandle Data) const override { delete reinterpret_cast<Rml::Geometry*>(Data); }
    void RenderElement(Rml::Element* Element, Rml::DecoratorDataHandle Data) const override
    {
        auto* Geometry = reinterpret_cast<Rml::Geometry*>(Data);
        Geometry->Render(Element->GetAbsoluteOffset(Rml::BoxArea::Border).Round(), GetTexture(TextureIndex));
    }
};

class UeMaterialDecoratorInstancer final : public Rml::DecoratorInstancer {
    Rml::PropertyId AliasId;
    int MaterialSlot;
public:
    explicit UeMaterialDecoratorInstancer(int InMaterialSlot) : MaterialSlot(InMaterialSlot)
    {
        AliasId = RegisterProperty("alias", "").AddParser("string").GetId();
        RegisterShorthand("decorator", "alias", Rml::ShorthandType::FallThrough);
    }
    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& Properties,
        const Rml::DecoratorInstancerInterface& Interface) override
    {
        Rml::String Alias = Properties.GetProperty(AliasId)->Get<Rml::String>();
        if (Alias.empty()) return nullptr;
        auto Result = Rml::MakeShared<UeMaterialDecorator>();
        const Rml::String SlotName = MaterialSlot == RMLUE_MATERIAL_SLOT_BORDER ? "border" : "background";
        return Result->Initialise(Interface.GetTexture("ue-material://" + SlotName + "/" + Alias), MaterialSlot) ? Result : nullptr;
    }
};

std::unique_ptr<BridgeFileInterface> FileInterface;
std::unique_ptr<BridgeSystemInterface> SystemInterface;
std::unique_ptr<BridgeRenderer> Renderer;
std::unique_ptr<UeMaterialDecoratorInstancer> MaterialDecoratorInstancer;
std::unique_ptr<UeMaterialDecoratorInstancer> MaterialBorderDecoratorInstancer;
}

struct RmlUE_StyleSheet final {
    Rml::SharedPtr<Rml::StyleSheetContainer> Container;
    uint32_t References = 1;
    uint64_t ResourceId = 0;
};

struct RmlUE_View final : public Rml::EventListener {
    struct InputGuard final : Rml::EventListener {
        RmlUE_View* View = nullptr;
        void ProcessEvent(Rml::Event& Event) override;
    } Guard;
    struct NodeRecord {
        Rml::ObserverPtr<Rml::Element> Element;
        Rml::ElementPtr Detached;
    };
    struct AnimationTargetRecord {
        Rml::ObserverPtr<Rml::Element> Element;
        void* PreparedOpacityState = nullptr;
        RmlUE_Node Node = 0;
        uint32_t Generation = 1;
        uint32_t PreparedVisualProperties = 0;
        bool Active = false;
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
    uint64_t ResourceId = 0;
    uint64_t FrameBufferResourceId = 0;
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
    std::unordered_map<int, Rml::Vector2f> TouchPositions;
    std::unordered_map<RmlUE_Node, NodeRecord> Nodes;
    std::unordered_map<Rml::Element*, RmlUE_Node> NodeIds;
    std::vector<AnimationTargetRecord> AnimationTargets;
    std::vector<uint32_t> FreeAnimationTargets;
    std::vector<std::unique_ptr<NodeListener>> NodeListeners;
    RmlUE_NodeEventCallback NodeCallback = nullptr;
    bool SuppressNextNewline = false;
    bool SuppressNextText = false;
    void* NodeUser = nullptr;
    int CallbackDepth = 0;
    bool Updating = false;
    bool StrictCapabilities = false;
    uint64_t LayoutRevision = 0;
    RmlUE_LayoutCallback LayoutCallback = nullptr;
    void* LayoutUser = nullptr;
    Rml::ObserverPtr<Rml::Element> ModalRoot;
    std::unordered_map<int, Rml::ObserverPtr<Rml::Element>> PointerCaptures;
    std::unordered_map<int, Rml::ObserverPtr<Rml::Element>> PointerDownTargets;
    bool CancellingPointers = false;
    float MouseX = 0, MouseY = 0;
    int InputPointerId = 0;
    int InputFlags = 0;
    bool InputRepeat = false;
    float WheelX = 0, WheelY = 0;

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
    static RmlUE_AnimationTarget PackAnimationTarget(uint32_t Index, uint32_t Generation) {
        return (static_cast<uint64_t>(Generation) << 32) | (static_cast<uint64_t>(Index) + 1);
    }
    static bool UnpackAnimationTarget(RmlUE_AnimationTarget Target, uint32_t& Index, uint32_t& Generation) {
        const uint32_t PackedIndex = static_cast<uint32_t>(Target);
        Generation = static_cast<uint32_t>(Target >> 32);
        if (!PackedIndex || !Generation) return false;
        Index = PackedIndex - 1;
        return true;
    }
    AnimationTargetRecord* FindAnimationTarget(RmlUE_AnimationTarget Target) {
        uint32_t Index = 0, Generation = 0;
        if (!UnpackAnimationTarget(Target, Index, Generation) || Index >= AnimationTargets.size()) return nullptr;
        AnimationTargetRecord& Record = AnimationTargets[Index];
        return Record.Active && Record.Generation == Generation && Record.Element ? &Record : nullptr;
    }
    RmlUE_AnimationTarget ResolveAnimationTarget(RmlUE_Node Node, Rml::Element* Element) {
        if (NextAnimationTargetGeneration == UINT32_MAX) {
            Fail("Animation target generation space exhausted.");
            return 0;
        }
        uint32_t Index = 0;
        if (!FreeAnimationTargets.empty()) {
            Index = FreeAnimationTargets.back();
            FreeAnimationTargets.pop_back();
        }
        else {
            Index = static_cast<uint32_t>(AnimationTargets.size());
            AnimationTargets.emplace_back();
        }
        AnimationTargetRecord& Record = AnimationTargets[Index];
        Record.Element = Element->GetObserverPtr();
        Record.PreparedOpacityState = nullptr;
        Record.Node = Node;
        Record.Generation = ++NextAnimationTargetGeneration;
        Record.PreparedVisualProperties = 0;
        Record.Active = true;
        return PackAnimationTarget(Index, Record.Generation);
    }
    bool ReleaseAnimationTarget(RmlUE_AnimationTarget Target) {
        uint32_t Index = 0, Generation = 0;
        if (!UnpackAnimationTarget(Target, Index, Generation) || Index >= AnimationTargets.size()) return false;
        AnimationTargetRecord& Record = AnimationTargets[Index];
        if (!Record.Active || Record.Generation != Generation) return false;
        if (SlateRenderer)
        {
            if (Record.PreparedVisualProperties & RMLUE_ANIMATED_PROPERTY_OPACITY)
                SlateRenderer->ReleaseVisualOpacityBinding(Record.Node);
            SlateRenderer->ReleaseTransformBinding(Target);
        }
        Record.Element = nullptr;
        Record.PreparedOpacityState = nullptr;
        Record.Node = 0;
        Record.PreparedVisualProperties = 0;
        Record.Active = false;
        FreeAnimationTargets.push_back(Index);
        return true;
    }
    void ClearAnimationTargets() {
        if (SlateRenderer) SlateRenderer->ClearTransformBindings();
        FreeAnimationTargets.clear();
        FreeAnimationTargets.reserve(AnimationTargets.size());
        for (uint32_t Index = 0; Index < AnimationTargets.size(); ++Index) {
            AnimationTargetRecord& Record = AnimationTargets[Index];
            if (SlateRenderer && Record.Active &&
                (Record.PreparedVisualProperties & RMLUE_ANIMATED_PROPERTY_OPACITY))
                SlateRenderer->ReleaseVisualOpacityBinding(Record.Node);
            Record.Element = nullptr;
            Record.PreparedOpacityState = nullptr;
            Record.Node = 0;
            Record.PreparedVisualProperties = 0;
            Record.Active = false;
            FreeAnimationTargets.push_back(Index);
        }
    }
    void PruneNodes() {
        if (CallbackDepth) return;
        for (auto It = PointerDownTargets.begin(); It != PointerDownTargets.end();) {
            if (!It->second) It = PointerDownTargets.erase(It); else ++It;
        }
        for (auto It = PointerCaptures.begin(); It != PointerCaptures.end();) {
            if (!It->second) It = PointerCaptures.erase(It); else ++It;
        }
        NodeListeners.erase(std::remove_if(NodeListeners.begin(), NodeListeners.end(),
            [](const auto& L) { return L->Removed || !L->Element; }), NodeListeners.end());
        for (auto It = Nodes.begin(); It != Nodes.end();) {
            if (!It->second.Element) {
                if (SlateRenderer) SlateRenderer->ClearVisualOpacity(It->first);
                It = Nodes.erase(It);
            }
            else ++It;
        }
        for (auto It = NodeIds.begin(); It != NodeIds.end();) {
            auto Record = Nodes.find(It->second);
            if (Record == Nodes.end() || Record->second.Element.get() != It->first) It = NodeIds.erase(It); else ++It;
        }
    }
    void ClearNodes() {
        ClearAnimationTargets();
        ModalRoot = nullptr;
        PointerCaptures.clear();
        PointerDownTargets.clear();
        NodeCallback = nullptr;
        NodeUser = nullptr;
        NodeListeners.clear();
        if (SlateRenderer)
            for (const auto& Pair : Nodes) SlateRenderer->ClearVisualOpacity(Pair.first);
        NodeIds.clear();
        Nodes.clear();
    }
    void MarkContentDirty() {
        if (SlateRenderer) SlateRenderer->MarkContentDirty();
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
    const std::string KeyName = HostKeyName(Output.Key, (Flags & 1) != 0);
    const std::string Code = HostKeyCode(Output.Key);
    const std::string Data = Event.GetParameter<Rml::String>("data", "");
    Output.KeyName = KeyName.c_str();
    Output.Code = Code.c_str();
    Output.Data = Data.c_str();
    Output.PointerId = Event.GetParameter<int>("pointer_id", View->InputPointerId);
    Output.PointerType = Output.PointerId == 0 ? "mouse" : "touch";
    Output.Repeat = View->InputRepeat ? 1 : 0;
    Output.IsComposing = RmlUE_TextInputIsComposing(View);
    auto* Related = static_cast<Rml::Element*>(Event.GetParameter<void*>("related_target", nullptr));
    Output.RelatedTarget = Related && Related->GetOwnerDocument() == View->Document ? View->Track(Related) : 0;
    Output.Cancelable = Event.IsInterruptible() ? 1 : 0;
    Output.DefaultPrevented = Event.IsDefaultPrevented() ? 1 : 0;
    if (Output.PointerId == 0) {
        for (int Button : View->PressedButtons) Output.Buttons |= (Button == 0 ? 1 : Button == 1 ? 2 : Button == 2 ? 4 : 1 << Button);
    } else Output.Buttons = View->Touches.count(Output.PointerId - 1) ? 1 : 0;
    Output.Buttons = Event.GetParameter<int>("buttons", Output.Buttons);
    const auto Screen = Event.GetUnprojectedMouseScreenPos();
    Output.X = Screen.x; Output.Y = Screen.y;
    auto Local = Screen;
    Event.GetCurrentElement()->Project(Local);
    auto Offset = Event.GetCurrentElement()->GetAbsoluteOffset(Rml::BoxArea::Border);
    Output.LocalX = Local.x - Offset.x;
    Output.LocalY = Local.y - Offset.y;
    Output.WheelX = View->WheelX;
    Output.WheelY = View->WheelY;
    Output.Timestamp = Rml::GetSystemInterface()->GetElapsedTime() * 1000.0;
    Output.AbiVersion = RMLUE_HOST_ABI_VERSION;
    Output.StructSize = sizeof(Output);
    ++View->CallbackDepth;
    const int Result = View->NodeCallback(View->NodeUser, Id, &Output);
    --View->CallbackDepth;
    if ((Result & 4) && Output.Key == Rml::Input::KI_RETURN) View->SuppressNextNewline = true;
    if (Result & 8) {
        Event.PreventDefault();
        if (Event.GetType() == "keydown" && Event.IsDefaultPrevented()) View->SuppressNextText = true;
    }
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
    // RmlUi treats unknown pseudo-classes as explicit states. Keep :root's
    // selector specificity while making theme tokens target the document only.
    NewDocument->SetPseudoClass("root", true);
    ApplyDocumentStyleSheet(View);
    View.ClearNodes();
    View.Guard.View = &View;
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
    for (const char* Type : {"keydown", "keyup", "mousedown", "mouseup", "click", "mousewheel", "touchstart", "touchmove", "focus"})
        NewDocument->AddEventListener(Type, &View.Guard, true);
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
        MaterialDecoratorInstancer = std::make_unique<UeMaterialDecoratorInstancer>(RMLUE_MATERIAL_SLOT_BACKGROUND);
        MaterialBorderDecoratorInstancer = std::make_unique<UeMaterialDecoratorInstancer>(RMLUE_MATERIAL_SLOT_BORDER);
        Rml::Factory::RegisterDecoratorInstancer("ue-material", MaterialDecoratorInstancer.get());
        Rml::Factory::RegisterDecoratorInstancer("ue-material-border", MaterialBorderDecoratorInstancer.get());
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
        UnregisterResource(StyleSheet->ResourceId);
        delete StyleSheet;
    }
    ActiveStats = nullptr;
    if (Initialized) Rml::Shutdown();
    Initialized = false;
    MaterialDecoratorInstancer.reset();
    MaterialBorderDecoratorInstancer.reset();
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
    while (!Resources.empty()) UnregisterResource(Resources.begin()->first);
    ResourceEventCallback = nullptr;
    ResourceEventUser = nullptr;
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
    StyleSheet->ResourceId = RegisterResource(RMLUE_RESOURCE_STYLE_SHEET, RMLUE_RESOURCE_BACKEND_SHARED, 0, 0, "Shared style sheet");
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
        UnregisterResource(StyleSheet->ResourceId);
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
    View->Context = Rml::CreateContext(View->Name, {Width, Height}, nullptr, RmlUE_CreateTextInputHandler(View.get()));
    if (!View->Context) { RmlUE_DestroyTextInputHandler(View.get()); Fail("Could not create RmlUi context."); return nullptr; }
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    View->ResourceId = RegisterResource(RMLUE_RESOURCE_VIEW, RMLUE_RESOURCE_BACKEND_DX11, 0, sizeof(RmlUE_View), View->Name);
    View->FrameBufferResourceId = RegisterResource(RMLUE_RESOURCE_FRAME_BUFFER, RMLUE_RESOURCE_BACKEND_DX11,
        View->ResourceId, static_cast<uint64_t>(Width) * Height * 12, "DX11 target, staging and CPU frame");
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
    View->SlateRenderer->SetNodeResolver([RawView = View.get()](Rml::Element* Element) { return RawView->Track(Element); });
    View->Context = Rml::CreateContext(View->Name, {Width, Height}, View->SlateRenderer.get(), RmlUE_CreateTextInputHandler(View.get()));
    if (!View->Context) { RmlUE_DestroyTextInputHandler(View.get()); Fail("Could not create RmlUi Slate command context."); return nullptr; }
    View->Context->SetDensityIndependentPixelRatio(DpRatio);
    View->ResourceId = RegisterResource(RMLUE_RESOURCE_VIEW, RMLUE_RESOURCE_BACKEND_SLATE, 0, sizeof(RmlUE_View), View->Name);
    View->SlateRenderer->SetOwnerResourceId(View->ResourceId);
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
    RmlUE_DestroyTextInputHandler(View);
    if (View->SlateRenderer)
    {
        Rml::ReleaseRenderManagers();
        View->SlateRenderer.reset();
    }
    UnregisterResource(View->FrameBufferResourceId);
    UnregisterResource(View->ResourceId);
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
    View->MarkContentDirty();
    return 1;
}

int RmlUE_LoadDocument(RmlUE_View* View, const char* Path)
{
    if (!ValidView(View) || !Path) return 0;
    if (View->CallbackDepth) return Fail("Defer document replacement until event dispatch completes.");
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    const int Result = AdoptDocument(*View, View->Context->LoadDocument(Path));
    if (Result) View->MarkContentDirty();
    return Result;
}

int RmlUE_LoadDocumentFromMemory(RmlUE_View* View, const char* Markup, const char* Source)
{
    if (!ValidView(View) || !Markup) return 0;
    if (View->CallbackDepth) return Fail("Defer document replacement until event dispatch completes.");
    LastError.clear();
    Rml::Factory::ClearStyleSheetCache();
    const int Result = AdoptDocument(*View, View->Context->LoadDocumentFromMemory(Markup, Source ? Source : "memory.rml"));
    if (Result) View->MarkContentDirty();
    return Result;
}

int RmlUE_Resize(RmlUE_View* View, int Width, int Height, float DpRatio)
{
    if (!ValidView(View)) return 0;
    if (!ValidDimensions(Width, Height, DpRatio)) return Fail("View dimensions must be 1..4096 and DPR finite and positive.");
    if (Width != View->Width || Height != View->Height)
    {
        if (View->SlateRenderer) { View->Width = Width; View->Height = Height; }
        else if (!CreateTargets(*View, Width, Height)) return 0;
        if (View->FrameBufferResourceId)
            UpdateResource(View->FrameBufferResourceId, static_cast<uint64_t>(Width) * Height * 12);
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
    const bool Replayed = View->SlateRenderer->CanReplay(View->Context->GetNextUpdateDelay());
    if (Replayed)
    {
        View->SlateRenderer->BeginReplayFrame();
    }
    else
    {
        View->SlateRenderer->BeginFrame();
        View->Context->Render();
        View->SlateRenderer->EndFrame();
        View->SlateRenderer->FinishFullFrame(View->Document);
    }
    ++View->FrameNumber;
    *Frame = {RMLUE_SLATE_ABI_VERSION, View->SlateRenderer->Draws.data(), static_cast<uint32_t>(View->SlateRenderer->Draws.size()),
        View->SlateRenderer->PublicVisualDeltas.data(), static_cast<uint32_t>(View->SlateRenderer->PublicVisualDeltas.size()), Replayed ? 1 : 0,
        View->SlateRenderer->PublicClipMasks.data(), static_cast<uint32_t>(View->SlateRenderer->PublicClipMasks.size()),
        View->SlateRenderer->PublicGeometryDeltas.data(), static_cast<uint32_t>(View->SlateRenderer->PublicGeometryDeltas.size()),
        View->SlateRenderer->PublicTextures.data(), static_cast<uint32_t>(View->SlateRenderer->PublicTextures.size()),
        View->FrameNumber, View->SlateRenderer->UnsupportedFeatures};
    return 1;
}

void RmlUE_GetSlateReplayStats(RmlUE_View* View, RmlUE_SlateReplayStats* Stats)
{
    if (!Stats) return;
    *Stats = {};
    if (ValidView(View) && View->SlateRenderer) View->SlateRenderer->GetReplayStats(*Stats);
}

int RmlUE_GetSlateScheduleState(RmlUE_View* View, RmlUE_SlateScheduleState* State)
{
    if (State) *State = {};
    if (!ValidView(View) || !State || !View->SlateRenderer)
        return Fail("Slate schedule state requires a valid Slate command view and output.");
    View->SlateRenderer->GetScheduleState(View->Context->GetNextUpdateDelay(), *State);
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

#include "RmlUiBridgeTextInput.inl"
#include "RmlUiBridgeHost.inl"

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
    View->MarkContentDirty();
    return 1;
}
int RmlUE_RemoveNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node);
    if (!Element) return 0;
    if (Element == View->Document) return Fail("Cannot remove the document root.");
    CancelPointerCaptures(View, Element);
    if (View->ModalRoot && Element->Contains(View->ModalRoot.get())) View->ModalRoot = nullptr;
    if (auto* Parent = Element->GetParentNode()) { auto Removed = Parent->RemoveChild(Element); }
    else View->Nodes.at(Node).Detached.reset();
    View->PruneNodes();
    View->MarkContentDirty();
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
    View->MarkContentDirty();
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
    if (Value && View->StrictCapabilities && _stricmp(Name, "style") == 0)
        return Fail("Strict capability mode requires SetNodeProperty for dynamic styles, not a style attribute.");
    const bool Boolean = std::strcmp(Name, "checked") == 0 || std::strcmp(Name, "selected") == 0 || std::strcmp(Name, "disabled") == 0;
    if (!Value || (Boolean && (std::strcmp(Value, "false") == 0 || std::strcmp(Value, "0") == 0))) Element->RemoveAttribute(Name);
    else Element->SetAttribute(Name, Rml::String(Value));
    View->MarkContentDirty();
    return 1;
}
int RmlUE_GetNodeAttribute(RmlUE_View* View, RmlUE_Node Node, const char* Name, char* Value, size_t Capacity)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name || !Value || !Capacity) return 0;
    const bool Boolean = std::strcmp(Name, "checked") == 0 || std::strcmp(Name, "selected") == 0 || std::strcmp(Name, "disabled") == 0;
    auto* Control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Element);
    std::string Result = Boolean ? (Element->HasAttribute(Name) ? "true" : "false") :
        (std::strcmp(Name, "class") == 0 ? Element->GetClassNames() :
        (Control && std::strcmp(Name, "value") == 0 ? Control->GetValue() : Element->GetAttribute<Rml::String>(Name, "")));
    if (Result.size() >= Capacity) return Fail("Attribute output buffer is too small.");
    CopyString(Value, Capacity, Result); return 1;
}
int RmlUE_SetNodeProperty(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* Value)
{
    auto* Element = GetNode(View, Node); if (!Element || !Name) return 0;
    if (!Value) { Element->RemoveProperty(Name); View->MarkContentDirty(); return 1; }
    if (!HostPropertyAllowed(View, Name, Value)) return 0;
    if (!Element->SetProperty(Name, Value)) return Fail(std::string("Unsupported RmlUi property/value: ") + Name + ": " + Value);
    View->MarkContentDirty();
    return 1;
}
int RmlUE_SetNodeInnerRml(RmlUE_View* View, RmlUE_Node Node, const char* Markup)
{
    auto* Element = GetNode(View, Node);
    if (!Element || !Markup) return 0;
    Element->SetInnerRML(Markup);
    View->PruneNodes();
    View->MarkContentDirty();
    return 1;
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
    if (View->Updating || View->CallbackDepth) return Fail("Layout cannot be re-entered during a layout or input callback.");
    View->Updating = true;
    View->PruneNodes();
    const bool Updated = View->Context->Update();
    if (Updated) {
        View->Context->GetRootElement()->SynchronizeTransformStateTree();
        ++View->LayoutRevision;
        if (View->ModalRoot && !InModalScope(View, View->Context->GetFocusElement())) FocusModalNext(View, false);
        if (View->LayoutCallback) {
            ++View->CallbackDepth;
            View->LayoutCallback(View->LayoutUser, View->LayoutRevision);
            --View->CallbackDepth;
        }
    }
    View->Updating = false;
    return Updated ? 1 : 0;
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
    View->MarkContentDirty();
    return 1;
}
float RmlUE_NodeScrollRemaining(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node); if (!Element) return 0;
    return std::max(0.f, Element->GetScrollHeight() - Element->GetClientHeight() - Element->GetScrollTop());
}
int RmlUE_FocusNode(RmlUE_View* View, RmlUE_Node Node)
{
    auto* Element = GetNode(View, Node);
    const bool Result = Element && InModalScope(View, Element) && Element->Focus();
    if (Result) View->MarkContentDirty();
    return Result ? 1 : 0;
}

int RmlUE_PollEvent(RmlUE_View* View, RmlUE_Event* Event)
{
    if (!ValidView(View) || !Event || View->Events.empty()) return 0;
    *Event = View->Events.front(); View->Events.pop_front(); return 1;
}
int RmlUE_SetInnerRml(RmlUE_View* View, const char* Id, const char* Markup)
{
    auto* Element = FindElement(View, Id); if (!Element || !Markup) return 0;
    Element->SetInnerRML(Markup); View->MarkContentDirty(); return 1;
}
int RmlUE_SetProperty(RmlUE_View* View, const char* Id, const char* Property, const char* Value)
{
    auto* Element = FindElement(View, Id);
    if (!Element || !Property || !Value || !HostPropertyAllowed(View, Property, Value) || !Element->SetProperty(Property, Value)) return 0;
    View->MarkContentDirty(); return 1;
}
int RmlUE_SetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, const char* Value)
{
    auto* Element = FindElement(View, Id); if (!Element || !Attribute || !Value) return 0;
    if (View->StrictCapabilities && _stricmp(Attribute, "style") == 0)
        return Fail("Strict capability mode requires SetNodeProperty for dynamic styles, not a style attribute.");
    const bool BooleanAttribute = std::strcmp(Attribute, "checked") == 0 || std::strcmp(Attribute, "disabled") == 0 || std::strcmp(Attribute, "selected") == 0;
    if (BooleanAttribute && (std::strcmp(Value, "false") == 0 || std::strcmp(Value, "0") == 0)) Element->RemoveAttribute(Attribute);
    else Element->SetAttribute(Attribute, Rml::String(Value));
    View->MarkContentDirty();
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
size_t RmlUE_GetResourceSnapshot(RmlUE_ResourceRecord* Records, size_t Capacity)
{
    if (!Initialized || !OnOwnerThread()) return 0;
    std::vector<RmlUE_ResourceRecord> Snapshot;
    Snapshot.reserve(Resources.size());
    for (const auto& Pair : Resources) Snapshot.push_back(Pair.second);
    std::sort(Snapshot.begin(), Snapshot.end(), [](const auto& A, const auto& B) { return A.Id < B.Id; });
    const size_t CopyCount = std::min(Capacity, Snapshot.size());
    if (Records && CopyCount) std::copy_n(Snapshot.begin(), CopyCount, Records);
    return Snapshot.size();
}

void RmlUE_SetResourceEventCallback(RmlUE_ResourceEventCallback Callback, void* User)
{
    if (OwnerThread != std::this_thread::get_id())
    {
        Fail("Resource event callback must be changed on the bridge owner thread.");
        return;
    }
    ResourceEventCallback = Callback;
    ResourceEventUser = Callback ? User : nullptr;
}

uint64_t RmlUE_GetViewResourceId(RmlUE_View* View)
{
    return ValidView(View) ? View->ResourceId : 0;
}

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
void RmlUE_MouseMove(RmlUE_View* View, int X, int Y, int Flags)
{
    if (!ValidView(View)) return;
    View->MouseX = static_cast<float>(X); View->MouseY = static_cast<float>(Y); View->InputPointerId = 0; View->InputFlags = Flags;
    View->Context->ProcessMouseMove(X, Y, Modifiers(Flags));
    DispatchPointer(View, "pointermove", 0, View->MouseX, View->MouseY, -1, Flags);
}
void RmlUE_MouseButton(RmlUE_View* View, int Button, int Down, int Flags)
{
    if (!ValidView(View) || Button < 0 || Button > 4) return;
    View->InputPointerId = 0; View->InputFlags = Flags;
    if (Down) {
        View->PressedButtons.insert(Button);
        if (InModalScope(View, View->Context->GetHoverElement()) &&
            DispatchPointer(View, "pointerdown", 0, View->MouseX, View->MouseY, Button, Flags))
            View->Context->ProcessMouseButtonDown(Button, Modifiers(Flags));
    } else {
        View->PressedButtons.erase(Button);
        DispatchPointer(View, "pointerup", 0, View->MouseX, View->MouseY, Button, Flags);
        View->Context->ProcessMouseButtonUp(Button, Modifiers(Flags));
        auto Capture = View->PointerCaptures.find(0);
        if (Capture != View->PointerCaptures.end() && View->PressedButtons.empty()) {
            auto Element = Capture->second; View->PointerCaptures.erase(Capture);
            if (Element) Element->DispatchEvent("lostpointercapture", CaptureParameters(View, 0));
        }
    }
}
void RmlUE_MouseWheel(RmlUE_View* View, float Delta, int Flags)
{
    if (!ValidView(View) || !std::isfinite(Delta) || !InModalScope(View, View->Context->GetHoverElement())) return;
    View->WheelY = -Delta; View->InputPointerId = 0; View->InputFlags = Flags;
    View->Context->ProcessMouseWheel({0, -Delta}, Modifiers(Flags));
    View->WheelY = 0;
}
void RmlUE_MouseLeave(RmlUE_View* View) { if (ValidView(View)) View->Context->ProcessMouseLeave(); }
void RmlUE_Key(RmlUE_View* View, int Key, int Down, int Flags)
{
    if (!ValidView(View)) return;
    const auto Identifier = KeyIdentifier(Key);
    if (Identifier == Rml::Input::KI_UNKNOWN) return;
    View->InputRepeat = Down && View->PressedKeys.count(Key);
    if (View->ModalRoot && !InModalScope(View, View->Context->GetFocusElement())) FocusModalNext(View, false);
    if (Down) {
        View->SuppressNextNewline = false; View->SuppressNextText = false;
        View->PressedKeys.insert(Key); View->Context->ProcessKeyDown(Identifier, Modifiers(Flags));
    } else { View->PressedKeys.erase(Key); View->Context->ProcessKeyUp(Identifier, Modifiers(Flags)); }
    View->InputRepeat = false;
}
void RmlUE_Text(RmlUE_View* View, const char* Text)
{
    if (!ValidView(View) || !Text) return;
    if (View->SuppressNextText) { View->SuppressNextText = false; return; }
    if (!InModalScope(View, View->Context->GetFocusElement())) return;
    const bool Suppress = View->SuppressNextNewline; View->SuppressNextNewline = false;
    if (Suppress && (std::strcmp(Text, "\n") == 0 || std::strcmp(Text, "\r") == 0)) return;
    View->Context->ProcessTextInput(Rml::String(Text));
}
void RmlUE_FocusLost(RmlUE_View* View)
{
    if (!ValidView(View)) return;
    CancelPointerCaptures(View, nullptr);
    View->Context->ProcessMouseLeave();
    for (int Key : View->PressedKeys) View->Context->ProcessKeyUp(KeyIdentifier(Key), 0);
    for (int Button : View->PressedButtons) View->Context->ProcessMouseButtonUp(Button, 0);
    Rml::TouchList Cancelled;
    for (int Id : View->Touches) Cancelled.push_back({static_cast<Rml::TouchId>(Id), View->TouchPositions[Id]});
    if (!Cancelled.empty()) View->Context->ProcessTouchCancel(Cancelled);
    View->PressedKeys.clear(); View->PressedButtons.clear(); View->Touches.clear();
    View->TouchPositions.clear();
    View->Context->ProcessMouseLeave();
    if (auto* Focused = View->Context->GetFocusElement()) Focused->Blur();
}
void RmlUE_Touch(RmlUE_View* View, int Id, float X, float Y, int Phase)
{
    if (!ValidView(View) || Id < 0 || Id == INT32_MAX || Phase < 0 || Phase > 3 || !std::isfinite(X) || !std::isfinite(Y)) return;
    if (Phase != 0 && !View->Touches.count(Id)) return;
    const Rml::TouchList Touches = {{static_cast<Rml::TouchId>(Id), {X, Y}}};
    View->InputPointerId = Id + 1;
    if (Phase == 0 && !InModalScope(View, View->Context->GetElementAtPoint({X, Y}))) { View->InputPointerId = 0; return; }
    View->TouchPositions[Id] = {X, Y};
    switch (Phase)
    {
        case 0:
            View->Touches.insert(Id);
            if (DispatchPointer(View, "pointerdown", Id+1, X, Y, 0, 0)) View->Context->ProcessTouchStart(Touches, 0);
            break;
        case 1: DispatchPointer(View, "pointermove", Id+1, X, Y, -1, 0); View->Context->ProcessTouchMove(Touches, 0); break;
        case 2: View->Touches.erase(Id); DispatchPointer(View, "pointerup", Id+1, X, Y, 0, 0); View->Context->ProcessTouchEnd(Touches, 0); break;
        case 3: View->Touches.erase(Id); DispatchPointer(View, "pointercancel", Id+1, X, Y, -1, 0); View->Context->ProcessTouchCancel(Touches); break;
        default: break;
    }
    if (Phase == 2 || Phase == 3) {
        auto Capture = View->PointerCaptures.find(Id + 1);
        if (Capture != View->PointerCaptures.end()) {
            auto Element = Capture->second; View->PointerCaptures.erase(Capture);
            if (Element) Element->DispatchEvent("lostpointercapture", CaptureParameters(View, Id + 1));
        }
        View->TouchPositions.erase(Id);
    }
    View->InputPointerId = 0;
}
