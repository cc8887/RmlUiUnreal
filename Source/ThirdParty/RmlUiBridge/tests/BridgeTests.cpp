#include "RmlUiBridge.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

struct ResourceEvents
{
    int Created = 0;
    int Updated = 0;
    int Destroyed = 0;
};

static void ResourceEvent(void* User, int Action, const RmlUE_ResourceRecord*)
{
    auto& Events = *static_cast<ResourceEvents*>(User);
    if (Action == RMLUE_RESOURCE_CREATED) ++Events.Created;
    else if (Action == RMLUE_RESOURCE_UPDATED) ++Events.Updated;
    else if (Action == RMLUE_RESOURCE_DESTROYED) ++Events.Destroyed;
}

static int ReadFile(void*, const char* Path, unsigned char** Buffer, size_t* Size)
{
    std::ifstream Input(std::filesystem::u8path(Path), std::ios::binary);
    if (!Input) return 0;
    std::vector<unsigned char> Bytes((std::istreambuf_iterator<char>(Input)), {});
    *Size = Bytes.size();
    *Buffer = static_cast<unsigned char*>(std::malloc(Bytes.size() + 1));
    std::memcpy(*Buffer, Bytes.data(), Bytes.size());
    return 1;
}
static int LoadImage(void*, const char*, unsigned char** Buffer, int* Width, int* Height)
{
    *Width = 2; *Height = 2;
    const unsigned char Image[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    *Buffer = static_cast<unsigned char*>(std::malloc(16));
    std::memcpy(*Buffer, Image, 16);
    return 1;
}
static void FreeBuffer(void*, void* Buffer) { std::free(Buffer); }
static void Log(void*, int Level, const char* Message) { std::printf("RML[%d] %s\n", Level, Message); }
static void Require(bool Condition, const char* Message)
{
    if (!Condition) { std::fprintf(stderr, "FAIL: %s; %s\n", Message, RmlUE_GetLastError()); std::exit(1); }
}
static void Click(RmlUE_View* View, const char* Id)
{
    RmlUE_Rect Rectangle{};
    Require(RmlUE_GetElementRect(View, Id, &Rectangle) != 0, "element rectangle");
    RmlUE_MouseMove(View, static_cast<int>(Rectangle.X + Rectangle.Width / 2), static_cast<int>(Rectangle.Y + Rectangle.Height / 2), 0);
    RmlUE_MouseButton(View, 0, 1, 0);
    RmlUE_MouseButton(View, 0, 0, 0);
}
static int Brightness(const RmlUE_Frame& Frame, int X, int Y)
{
    const auto* Pixel = Frame.Pixels + (Y * Frame.Width + X) * 4;
    return int(Pixel[0]) + int(Pixel[1]) + int(Pixel[2]);
}
static const RmlUE_SlateGeometryDelta* FindCreatedGeometry(const RmlUE_SlateFrame& Frame, uint64_t Id)
{
    for (uint32_t Index = 0; Index < Frame.GeometryDeltaCount; ++Index)
        if (Frame.GeometryDeltas[Index].Id == Id && Frame.GeometryDeltas[Index].Action == RMLUE_SLATE_RESOURCE_CREATE)
            return &Frame.GeometryDeltas[Index];
    return nullptr;
}
int main(int Count, char** Arguments)
{
    Require(Count >= 2, "font path argument");
    RmlUE_Host Host{};
    Host.ReadFile = ReadFile; Host.LoadImage = LoadImage; Host.FreeBuffer = FreeBuffer; Host.Log = Log;
    Require(RmlUE_Initialize(&Host) != 0, "initialize");
    ResourceEvents Events;
    RmlUE_SetResourceEventCallback(ResourceEvent, &Events);
    Require(RmlUE_GetResourceSnapshot(nullptr, 0) == 0, "resource registry starts empty");
    Require(RmlUE_LoadFont(Arguments[1], 0) != 0, "font");
    Require(RmlUE_CreateView(0, 256, 1) == nullptr, "reject zero dimensions");
    Require(RmlUE_CreateView(256, 256, NAN) == nullptr, "reject NaN DPR");
    auto* View = RmlUE_CreateView(256, 256, 1);
    Require(View != nullptr, "view");
    const uint64_t ViewResourceId = RmlUE_GetViewResourceId(View);
    Require(ViewResourceId != 0, "view exposes a stable diagnostic resource id");
    {
        const size_t Count = RmlUE_GetResourceSnapshot(nullptr, 0);
        Require(Count == 2, "legacy view registers view and frame buffer resources");
        std::vector<RmlUE_ResourceRecord> Records(Count);
        Require(RmlUE_GetResourceSnapshot(Records.data(), Records.size()) == Count, "resource snapshot two-pass query");
        bool FoundView = false, FoundFrameBuffer = false;
        for (const auto& Record : Records)
        {
            FoundView |= Record.Id == ViewResourceId && Record.Type == RMLUE_RESOURCE_VIEW && Record.Backend == RMLUE_RESOURCE_BACKEND_DX11;
            FoundFrameBuffer |= Record.Type == RMLUE_RESOURCE_FRAME_BUFFER && Record.OwnerId == ViewResourceId && Record.EstimatedBytes == 256ull * 256 * 12;
        }
        Require(FoundView && FoundFrameBuffer, "resource snapshot preserves type, backend, owner and estimated bytes");
    }
    const char* Markup = R"(<rml><head><style>
body { margin: 0; font-family: LatoLatin; font-size: 18px; }
#alpha { position: absolute; left: 0; top: 0; width: 32px; height: 32px; background-color: #ff000080; }
#gradient { position: absolute; left: 40px; top: 0; width: 96px; height: 48px; decorator: linear-gradient(90deg, #f00, #00f); }
#clip { position: absolute; left: 150px; top: 0; width: 50px; height: 50px; border-radius: 20px; overflow: hidden; }
#clip div { width: 80px; height: 80px; background-color: #0f0; }
#effect { position: absolute; left: 20px; top: 60px; width: 80px; height: 30px; background-color: #ff0; filter: blur(2px); transform: rotate(5deg); }
#button { position: absolute; left: 20px; top: 110px; width: 90px; height: 30px; background-color: #eee; color: #111; }
#field { position: absolute; left: 20px; top: 160px; width: 150px; height: 30px; background-color: #eee; color: #111; }
img { position: absolute; left: 180px; top: 70px; width: 40px; height: 40px; }
</style></head><body><div id="alpha"/><div id="gradient"/><div id="clip"><div/></div>
<div id="effect"/><button id="button">Click</button><input id="field" type="text" value="old"/><img src="image.png"/></body></rml>)";
    Require(RmlUE_LoadDocumentFromMemory(View, Markup, "memory.rml") != 0, "document");
    RmlUE_Frame Frame{};
    Require(RmlUE_Render(View, &Frame) != 0, "render");
    const auto* Pixel = Frame.Pixels + (8 * Frame.Width + 8) * 4;
    std::printf("alpha pixel BGRA=%u %u %u %u\n", Pixel[0], Pixel[1], Pixel[2], Pixel[3]);
    Require(Pixel[0] < 5 && Pixel[1] < 5 && Pixel[2] > 245 && Pixel[3] > 120 && Pixel[3] < 136, "straight alpha and channel order");
    Pixel = Frame.Pixels + (250 * Frame.Width + 250) * 4;
    Require(Pixel[3] == 0, "transparent background");
    int GlyphPixels = 0;
    for (int Y = 112; Y < 138; ++Y)
        for (int X = 22; X < 108; ++X)
        {
            const auto* TextPixel = Frame.Pixels + (Y * Frame.Width + X) * 4;
            if (TextPixel[0] < 80 && TextPixel[1] < 80 && TextPixel[2] < 80 && TextPixel[3] > 240) ++GlyphPixels;
        }
    Require(GlyphPixels > 20, "FreeType glyph pixels");
    const auto* GradientLeft = Frame.Pixels + (20 * Frame.Width + 45) * 4;
    const auto* GradientRight = Frame.Pixels + (20 * Frame.Width + 130) * 4;
    Require(GradientLeft[2] > GradientLeft[0] && GradientRight[0] > GradientRight[2], "gradient color pixels");
    Require(Frame.Pixels[(1 * Frame.Width + 151) * 4 + 3] == 0, "rounded clipping corner");
    RmlUE_Stats Stats{};
    RmlUE_GetStats(View, &Stats);
    std::printf("draws=%llu masks=%llu layers=%llu filters=%llu shaders=%llu textures=%llu\n", Stats.GeometryDraws, Stats.ClipMasks, Stats.Layers, Stats.Filters, Stats.Shaders, Stats.LoadedTextures);
    Require(Stats.GeometryDraws && Stats.ClipMasks && Stats.Layers && Stats.Filters && Stats.Shaders && Stats.LoadedTextures, "advanced backend callbacks");
    Click(View, "button");
    RmlUE_Event Event{};
    bool Clicked = false;
    while (RmlUE_PollEvent(View, &Event)) Clicked |= std::strcmp(Event.Type, "click") == 0 && std::strcmp(Event.ElementId, "button") == 0;
    Require(Clicked, "click event round trip");
    RmlUE_MouseButton(View, 0, 1, 0);
    RmlUE_FocusLost(View);
    Require(RmlUE_PollEvent(View, &Event) == 0, "focus loss must not click a pressed button");
    Click(View, "field");
    RmlUE_Key(View, 'A', 1, 2); RmlUE_Key(View, 'A', 0, 2); RmlUE_Text(View, "abc");
    char Value[64]{};
    Require(RmlUE_GetAttribute(View, "field", "value", Value, sizeof(Value)) != 0 && std::strcmp(Value, "abc") == 0, "text and keyboard input");
    Require(RmlUE_SetAttribute(View, "field", "disabled", "") != 0, "set empty boolean attribute");
    Require(RmlUE_GetAttribute(View, "field", "disabled", Value, sizeof(Value)) != 0 && std::strcmp(Value, "true") == 0, "boolean attribute presence");
    Require(RmlUE_SetAttribute(View, "field", "disabled", "false") != 0, "clear boolean attribute");
    Require(RmlUE_GetAttribute(View, "field", "disabled", Value, sizeof(Value)) != 0 && std::strcmp(Value, "false") == 0, "boolean attribute absence");
    Require(RmlUE_LoadDocument(View, "this-file-does-not-exist.rml") == 0, "failed reload");
    RmlUE_Rect PreservedRect{};
    Require(RmlUE_GetElementRect(View, "button", &PreservedRect) != 0, "preserve old document after failed reload");
    Require(RmlUE_Resize(View, 320, 240, 1.25f) != 0, "resize");
    Require(Events.Updated > 0, "frame buffer resize emits a resource update");
    Require(RmlUE_Render(View, &Frame) != 0 && Frame.Width == 320 && Frame.Height == 240, "resized frame");
    auto* Second = RmlUE_CreateView(64, 64, 1);
    Require(Second != nullptr, "second context");
    Require(RmlUE_LoadDocumentFromMemory(Second, "<html><head><style>body { width: 100%; height: 100%; margin: 0; background-color: #00f; }</style></head><body/></html>", "probe.html") != 0, "html document root");
    Require(RmlUE_Render(Second, &Frame) != 0, "second view render");
    Require(Frame.Pixels[0] > 245 && Frame.Pixels[1] < 5 && Frame.Pixels[2] < 5 && Frame.Pixels[3] > 245, "html root renders its styled body");
    Require(RmlUE_Render(View, &Frame) != 0, "switch back to first view");
    RmlUE_FocusLost(View);
    RmlUE_DestroyView(Second);

    auto* SlateView = RmlUE_CreateSlateView(128, 64, 1);
    const uint64_t SlateViewResourceId = RmlUE_GetViewResourceId(SlateView);
    const char* SlateMarkup = R"(<rml><head><style>
body { margin: 0; } #panel, #frame { display: inline-block; width: 48px; height: 40px; }
#panel { background-color: #234; decorator: ue-material(panel.energy); }
#frame { border: 6px #fff; border-radius: 10px; decorator: ue-material-border(panel.frame); }
#transformed { position: absolute; left: 58px; top: 28px; width: 28px; height: 14px; background-color: #f00; transform-origin: 0px 0px; transform: translate(8px, 3px) rotate(12deg); }
</style></head><body><div id="panel"></div><div id="frame"></div><div id="transformed"></div></body></rml>)";
    Require(SlateView != nullptr && RmlUE_LoadDocumentFromMemory(SlateView, SlateMarkup, "slate.rml") != 0, "Slate command document");
    RmlUE_SlateFrame SlateFrame{};
    Require(RmlUE_RenderSlate(SlateView, &SlateFrame) != 0 && SlateFrame.AbiVersion == RMLUE_SLATE_ABI_VERSION && SlateFrame.DrawCount > 0, "Slate command render");
    bool FoundBackgroundMaterial = false;
    bool FoundBorderMaterial = false;
    bool FoundTransformedDraw = false;
    uint64_t BorderTexture = 0;
    for (uint32_t Index = 0; Index < SlateFrame.TextureCount; ++Index)
    {
        const RmlUE_SlateTexture& Texture = SlateFrame.Textures[Index];
        FoundBackgroundMaterial |= Texture.Action == RMLUE_SLATE_RESOURCE_CREATE && Texture.Kind == 1 &&
            Texture.MaterialSlot == RMLUE_MATERIAL_SLOT_BACKGROUND && Texture.MaterialAlias &&
            std::strcmp(Texture.MaterialAlias, "panel.energy") == 0;
        if (Texture.Action == RMLUE_SLATE_RESOURCE_CREATE && Texture.Kind == 1 &&
            Texture.MaterialSlot == RMLUE_MATERIAL_SLOT_BORDER && Texture.MaterialAlias &&
            std::strcmp(Texture.MaterialAlias, "panel.frame") == 0)
        {
            FoundBorderMaterial = true;
            BorderTexture = Texture.Id;
        }
    }
    Require(FoundBackgroundMaterial, "Slate frame carries the background material slot and alias");
    Require(FoundBorderMaterial, "Slate frame carries the border material slot and alias");
    bool FoundBorderRing = false;
    for (uint32_t Index = 0; Index < SlateFrame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = SlateFrame.Draws[Index];
        const RmlUE_SlateGeometryDelta* Geometry = FindCreatedGeometry(SlateFrame, Draw.GeometryId);
        Require(Geometry != nullptr, "first Slate frame creates every referenced geometry cache entry");
        if (Draw.Texture == BorderTexture)
        {
            FoundBorderRing = Geometry->VertexCount > 4 && Geometry->IndexCount >= 24;
            bool FoundNonZeroUv = false;
            for (uint32_t VertexIndex = 0; VertexIndex < Geometry->VertexCount; ++VertexIndex)
                FoundNonZeroUv |= Geometry->Vertices[VertexIndex].U > 0.01f || Geometry->Vertices[VertexIndex].V > 0.01f;
            FoundBorderRing &= FoundNonZeroUv;
        }
        if (Geometry->VertexCount > 0 && Geometry->Vertices[0].R > 240 && Geometry->Vertices[0].G < 10 && Geometry->Vertices[0].B < 10)
            FoundTransformedDraw = Draw.TransformEnabled != 0 && std::fabs(Draw.TransformM01) > 0.01f;
    }
    Require(FoundBorderRing, "Border material uses textured ring geometry rather than a content-covering quad");
    Require(FoundTransformedDraw, "Slate draw carries a 2D affine transform");
    Require((SlateFrame.UnsupportedFeatures & RMLUE_UNSUPPORTED_TRANSFORM_3D) == 0, "Supported 2D transforms do not set the unsupported feature bit");
    {
        const size_t Count = RmlUE_GetResourceSnapshot(nullptr, 0);
        std::vector<RmlUE_ResourceRecord> Records(Count);
        RmlUE_GetResourceSnapshot(Records.data(), Records.size());
        bool FoundGeometry = false, FoundMaterialBinding = false;
        for (const auto& Record : Records)
        {
            FoundGeometry |= Record.Type == RMLUE_RESOURCE_GEOMETRY && Record.OwnerId == SlateViewResourceId && Record.EstimatedBytes > 0;
            FoundMaterialBinding |= Record.Type == RMLUE_RESOURCE_MATERIAL_BINDING && Record.OwnerId == SlateViewResourceId;
            Require(!(Record.Type == RMLUE_RESOURCE_FRAME_BUFFER && Record.OwnerId == SlateViewResourceId),
                "Slate view does not allocate a legacy DX11 frame buffer");
        }
        Require(FoundGeometry && FoundMaterialBinding, "Slate resources are registered under their owning view");
    }
    RmlUE_SlateFrame CachedSlateFrame{};
    Require(RmlUE_RenderSlate(SlateView, &CachedSlateFrame) != 0 && CachedSlateFrame.DrawCount > 0 &&
        CachedSlateFrame.GeometryDeltaCount == 0 && CachedSlateFrame.TextureCount == 0,
        "unchanged Slate frame reuses host geometry and texture caches without resending resources");
    Require(RmlUE_LoadDocumentFromMemory(SlateView,
        "<rml><head><style>body{margin:0}#replacement{display:block;width:20px;height:20px;background-color:#0f0}</style></head><body><div id='replacement'></div></body></rml>",
        "slate-replacement.rml") != 0, "replace cached Slate document");
    RmlUE_SlateFrame ReplacedSlateFrame{};
    Require(RmlUE_RenderSlate(SlateView, &ReplacedSlateFrame) != 0, "render replaced cached Slate document");
    bool FoundGeometryDestroy = false, FoundGeometryCreate = false, FoundTextureDestroy = false;
    for (uint32_t Index = 0; Index < ReplacedSlateFrame.GeometryDeltaCount; ++Index)
    {
        FoundGeometryDestroy |= ReplacedSlateFrame.GeometryDeltas[Index].Action == RMLUE_SLATE_RESOURCE_DESTROY;
        FoundGeometryCreate |= ReplacedSlateFrame.GeometryDeltas[Index].Action == RMLUE_SLATE_RESOURCE_CREATE;
    }
    for (uint32_t Index = 0; Index < ReplacedSlateFrame.TextureCount; ++Index)
        FoundTextureDestroy |= ReplacedSlateFrame.Textures[Index].Action == RMLUE_SLATE_RESOURCE_DESTROY;
    std::printf("Slate replacement deltas draws=%u geometry=%u destroy=%d create=%d texture=%u destroy=%d\n",
        ReplacedSlateFrame.DrawCount,
        ReplacedSlateFrame.GeometryDeltaCount, FoundGeometryDestroy, FoundGeometryCreate,
        ReplacedSlateFrame.TextureCount, FoundTextureDestroy);
    std::fflush(stdout);
    Require(FoundGeometryDestroy && FoundGeometryCreate && FoundTextureDestroy,
        "document replacement sends cache destroys before new geometry is drawn");
    Require(RmlUE_Render(SlateView, &Frame) == 0, "Slate view rejects legacy frame readback");
    RmlUE_DestroyView(SlateView);

    auto* ClipView = RmlUE_CreateSlateView(128, 64, 1);
    const char* ClipMarkup = R"(<rml><head><style>
body { margin: 0; }
#outer { display: block; position: absolute; left: 4px; top: 4px; width: 38px; height: 24px; overflow: hidden; border-radius: 8px; }
#inner { display: block; position: relative; left: 6px; top: 5px; width: 30px; height: 18px; overflow: hidden; border-radius: 6px; }
#clipped { display: block; width: 50px; height: 30px; background-color: #0f0; }
#unclipped { display: block; position: absolute; left: 110px; top: 0; width: 10px; height: 10px; background-color: #00f; }
</style></head><body><div id="outer"><div id="inner"><div id="clipped"></div></div></div><div id="unclipped"></div></body></rml>)";
    Require(ClipView != nullptr && RmlUE_LoadDocumentFromMemory(ClipView, ClipMarkup, "slate-clip.rml") != 0, "Slate clipping document");
    RmlUE_SlateFrame ClipFrame{};
    Require(RmlUE_RenderSlate(ClipView, &ClipFrame) != 0, "Slate clipping render");
    bool FoundNestedClipDraw = false;
    bool FoundUnclippedSiblingDraw = false;
    bool FoundMaskSet = false;
    bool FoundMaskIntersect = false;
    for (uint32_t Index = 0; Index < ClipFrame.ClipMaskCount; ++Index)
    {
        FoundMaskSet |= ClipFrame.ClipMasks[Index].Operation == RMLUE_CLIP_MASK_SET;
        FoundMaskIntersect |= ClipFrame.ClipMasks[Index].Operation == RMLUE_CLIP_MASK_INTERSECT;
        Require(FindCreatedGeometry(ClipFrame, ClipFrame.ClipMasks[Index].GeometryId) != nullptr,
            "clipping frame creates every referenced mask geometry cache entry");
    }
    for (uint32_t Index = 0; Index < ClipFrame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = ClipFrame.Draws[Index];
        const RmlUE_SlateGeometryDelta* Geometry = FindCreatedGeometry(ClipFrame, Draw.GeometryId);
        Require(Geometry != nullptr, "clipping frame creates every referenced geometry cache entry");
        if (Geometry->VertexCount > 0 && Geometry->Vertices[0].G > 240 && Geometry->Vertices[0].R < 10 && Geometry->Vertices[0].B < 10)
            FoundNestedClipDraw = Draw.ScissorEnabled != 0 && Draw.ScissorWidth <= 30.f && Draw.ScissorHeight <= 18.f &&
                Draw.ClipMaskCount >= 2;
        if (Geometry->VertexCount > 0 && Geometry->Vertices[0].B > 240 && Geometry->Vertices[0].R < 10 && Geometry->Vertices[0].G < 10)
            FoundUnclippedSiblingDraw = Draw.ScissorEnabled == 0;
    }
    Require(FoundNestedClipDraw, "Slate draw carries the nested rectangular clip intersection");
    Require(FoundMaskSet && FoundMaskIntersect, "Slate frame carries nested non-rectangular clip-mask operations");
    Require((ClipFrame.UnsupportedFeatures & RMLUE_UNSUPPORTED_CLIP_MASK) == 0,
        "Plain geometry clip masks are supported by the Slate command path");
    Require(FoundUnclippedSiblingDraw, "Slate rectangular clip state is restored after leaving the clipped subtree");
    RmlUE_DestroyView(ClipView);

    auto* MaskedMaterialView = RmlUE_CreateSlateView(64, 64, 1);
    const char* MaskedMaterialMarkup = R"(<rml><head><style>
body { margin: 0; }
#clip { display: block; width: 40px; height: 32px; overflow: hidden; border-radius: 8px; opacity: 0.5; }
#material { display: block; width: 52px; height: 32px; decorator: ue-material(masked.panel); }
</style></head><body><div id="clip"><div id="material"></div></div></body></rml>)";
    Require(MaskedMaterialView != nullptr &&
        RmlUE_LoadDocumentFromMemory(MaskedMaterialView, MaskedMaterialMarkup, "slate-masked-material.rml") != 0,
        "Slate masked material document");
    RmlUE_SlateFrame MaskedMaterialFrame{};
    Require(RmlUE_RenderSlate(MaskedMaterialView, &MaskedMaterialFrame) != 0 &&
        (MaskedMaterialFrame.UnsupportedFeatures & RMLUE_UNSUPPORTED_CLIP_MASK) == 0,
        "Slate command ABI transports material clip masks for host capability validation");
    bool FoundMaskedMaterialDraw = false;
    for (uint32_t Index = 0; Index < MaskedMaterialFrame.DrawCount; ++Index)
    {
        const RmlUE_SlateDraw& Draw = MaskedMaterialFrame.Draws[Index];
        if (Draw.Texture == 0 || Draw.ClipMaskCount == 0) continue;
        const RmlUE_SlateGeometryDelta* Geometry = FindCreatedGeometry(MaskedMaterialFrame, Draw.GeometryId);
        FoundMaskedMaterialDraw |= Geometry && Geometry->VertexCount > 0 &&
            Geometry->Vertices[0].A >= 126 && Geometry->Vertices[0].A <= 128 &&
            Geometry->Vertices[0].R == Geometry->Vertices[0].A;
    }
    Require(FoundMaskedMaterialDraw,
        "Slate material draw retains its clip-mask snapshot and inherited premultiplied opacity");
    RmlUE_DestroyView(MaskedMaterialView);

    auto* Transform3DView = RmlUE_CreateSlateView(64, 64, 1);
    const char* Transform3DMarkup = R"(<rml><head><style>
body { margin: 0; } #probe { display: block; width: 20px; height: 20px; background-color: #fff; transform: translateZ(4px); }
</style></head><body><div id="probe"></div></body></rml>)";
    Require(Transform3DView != nullptr && RmlUE_LoadDocumentFromMemory(Transform3DView, Transform3DMarkup, "slate-transform-3d.rml") != 0,
        "Slate unsupported 3D transform document");
    RmlUE_SlateFrame Transform3DFrame{};
    Require(RmlUE_RenderSlate(Transform3DView, &Transform3DFrame) != 0 &&
        (Transform3DFrame.UnsupportedFeatures & RMLUE_UNSUPPORTED_TRANSFORM_3D) != 0,
        "Slate reports unsupported 3D transforms instead of silently treating them as 2D");
    RmlUE_DestroyView(Transform3DView);

    const char* FlowMarkup = R"(<html><head><style>
body { margin: 0; font-family: LatoLatin; font-size: 16px; }
#first, #second { width: 40px; height: 20px; }
</style></head><body><div id="first">one</div><div id="second">two</div></body></html>)";
    auto* FlowView = RmlUE_CreateView(160, 120, 1);
    Require(FlowView != nullptr && RmlUE_LoadDocumentFromMemory(FlowView, FlowMarkup, "flow.html") != 0, "raw flow document");
    RmlUE_Rect First{}, RawSecond{}, CompatibleSecond{}, RestoredSecond{};
    Require(RmlUE_GetElementRect(FlowView, "first", &First) != 0 && RmlUE_GetElementRect(FlowView, "second", &RawSecond) != 0,
        "raw inline element rectangles");
    Require(std::fabs(RawSecond.Y - First.Y) < 1.0f, "raw RmlUi keeps generic div elements inline");
    auto* BaseStyle = RmlUE_CreateStyleSheet("div { display: block; } * { box-sizing: border-box; }");
    Require(BaseStyle != nullptr, "parse reusable base style sheet");
    Require(RmlUE_CreateStyleSheet("button { color: inherit; }") == nullptr,
        "strict base style parsing rejects unsupported declarations");
    RmlUE_RetainStyleSheet(BaseStyle);
    RmlUE_ReleaseStyleSheet(BaseStyle);
    Require(RmlUE_SetBaseStyleSheet(FlowView, BaseStyle) != 0, "attach reusable base style sheet");
    Require(RmlUE_GetElementRect(FlowView, "second", &CompatibleSecond) != 0 && CompatibleSecond.Y > First.Y + 10.0f,
        "base style sheet supplies block flow without reparsing the document");
    Require(RmlUE_SetBaseStyleSheet(FlowView, nullptr) != 0, "detach base style sheet");
    Require(RmlUE_GetElementRect(FlowView, "second", &RestoredSecond) != 0 && std::fabs(RestoredSecond.Y - RawSecond.Y) < 1.0f,
        "detaching base style restores raw author styles");
    RmlUE_ReleaseStyleSheet(BaseStyle);
    RmlUE_DestroyView(FlowView);
    RmlUE_DestroyView(View);
    if (Count >= 3)
    {
        auto* WheelView = RmlUE_CreateView(640, 480, 1);
        Require(WheelView != nullptr && RmlUE_LoadDocument(WheelView, Arguments[2]) != 0, "wheel fixture");
        Require(RmlUE_Render(WheelView, &Frame) != 0, "wheel fixture render");
        Click(WheelView, "action");
        Click(WheelView, "entry");
        RmlUE_Text(WheelView, "Unreal 58");
        RmlUE_Key(WheelView, 8, 1, 0);
        RmlUE_Key(WheelView, 8, 0, 0);
        RmlUE_SetInnerRml(WheelView, "status", "Updated from Unreal");
        RmlUE_Rect Before{}, Scroll{}, After{};
        RmlUE_GetElementRect(WheelView, "scroll-content", &Before);
        RmlUE_GetElementRect(WheelView, "scroll", &Scroll);
        std::printf("wheel before=%.2f container=(%.2f,%.2f,%.2f,%.2f)\n", Before.Y, Scroll.X, Scroll.Y, Scroll.Width, Scroll.Height);
        RmlUE_MouseMove(WheelView, int(Scroll.X + 40), int(Scroll.Y + 40), 0);
        RmlUE_MouseWheel(WheelView, -3, 0);
        for (int Index = 0; Index < 32; ++Index)
        {
            RmlUE_Render(WheelView, &Frame);
            RmlUE_GetElementRect(WheelView, "scroll-content", &After);
            if (Index < 4 || Index == 31) std::printf("wheel frame=%d y=%.2f\n", Index, After.Y);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Require(After.Y < Before.Y, "wheel fixture content moves");
        RmlUE_DestroyView(WheelView);

        const auto MotionPath = Count >= 4
            ? std::filesystem::u8path(Arguments[3]).u8string()
            : (std::filesystem::u8path(Arguments[2]).parent_path() / "web-motion-libraries.html").u8string();
        auto* MotionView = RmlUE_CreateView(800, 480, 1);
        Require(MotionView != nullptr && RmlUE_LoadDocument(MotionView, MotionPath.c_str()) != 0, "web motion library fixture");
        Require(RmlUE_Render(MotionView, &Frame) != 0, "web motion first frame");
        const int PopupEarly = Brightness(Frame, 215, 205);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Require(RmlUE_Render(MotionView, &Frame) != 0, "web motion settled frame");
        const int PopupSettled = Brightness(Frame, 215, 205);
        std::printf("Magic.css popup brightness early=%d settled=%d source=%s\n", PopupEarly, PopupSettled, MotionPath.c_str());
        std::fflush(stdout);
        Require(PopupSettled > PopupEarly + 120, "Magic.css puffIn popup animates across rendered frames");

        // The upstream classes use browser longhands that RmlUi 6.3 does not register.
        Require(RmlUE_SetProperty(MotionView, "magic-popup", "animation-name", "magic-puff-in") == 0,
            "Magic.css animation-name requires RCSS shorthand adaptation");
        Require(RmlUE_SetProperty(MotionView, "magic-popup", "animation-duration", "0.6s") == 0,
            "Magic.css animation-duration requires RCSS shorthand adaptation");
        Require(RmlUE_SetProperty(MotionView, "hover-grow", "transition-duration", "0.3s") == 0,
            "Hover.css transition-duration requires RCSS shorthand adaptation");
        Require(RmlUE_SetProperty(MotionView, "hover-grow", "transition-property", "transform") == 0,
            "Hover.css transition-property requires RCSS shorthand adaptation");
        Require(RmlUE_SetProperty(MotionView, "hover-grow", "transition", "transform 0.3s cubic-out") != 0,
            "RmlUi accepts adapted Hover.css transition shorthand");

        RmlUE_FocusLost(MotionView);
        RmlUE_MouseMove(MotionView, 790, 470, 0);
        Require(RmlUE_Render(MotionView, &Frame) != 0, "Hover.css reset frame");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        Require(RmlUE_Render(MotionView, &Frame) != 0, "Hover.css settled reset frame");
        RmlUE_Rect Button{};
        Require(RmlUE_GetElementRect(MotionView, "hover-grow", &Button) != 0, "Hover.css button layout");
        const int ProbeX = int(Button.X) - 5;
        const int ProbeY = int(Button.Y + Button.Height * 0.5f);
        const int ButtonBefore = Brightness(Frame, ProbeX, ProbeY);
        RmlUE_MouseMove(MotionView, int(Button.X + Button.Width * 0.5f), ProbeY, 0);
        Require(RmlUE_Render(MotionView, &Frame) != 0, "Hover.css transition start frame");
        const int ButtonStart = Brightness(Frame, ProbeX, ProbeY);
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        Require(RmlUE_Render(MotionView, &Frame) != 0, "Hover.css transition middle frame");
        const int ButtonMiddle = Brightness(Frame, ProbeX, ProbeY);
        std::printf("Hover.css edge brightness before=%d start=%d middle=%d\n", ButtonBefore, ButtonStart, ButtonMiddle);
        std::fflush(stdout);
        Require(std::abs(ButtonStart - ButtonBefore) < 24, "Hover.css adapted Grow does not jump on hover");
        Require(ButtonMiddle > ButtonStart + 80, "Hover.css adapted Grow expands smoothly across rendered frames");
        RmlUE_DestroyView(MotionView);
    }
    Require(RmlUE_GetResourceSnapshot(nullptr, 0) == 0, "all explicitly released resources leave the registry");
    Require(Events.Created == Events.Destroyed, "resource create and destroy events are balanced");
    RmlUE_SetResourceEventCallback(nullptr, nullptr);
    RmlUE_Shutdown();
    Require(RmlUE_Initialize(&Host) != 0, "reinitialize");
    RmlUE_Shutdown();
    std::puts("PASS: RmlUi native bridge rendering, web motion libraries, alpha, effects, input, reload, resize, multiple contexts and reinitialization.");
    return 0;
}
