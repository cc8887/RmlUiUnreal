#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(RMLUE_BRIDGE_EXPORTS)
#define RMLUE_API __declspec(dllexport)
#else
#define RMLUE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RmlUE_View RmlUE_View;
typedef struct RmlUE_StyleSheet RmlUE_StyleSheet;
typedef uint32_t RmlUE_Node;

typedef struct RmlUE_NodeEvent {
    RmlUE_Node Target;
    RmlUE_Node CurrentTarget;
    const char* Type;
    const char* Value;
    int Phase;
    int Key;
    int Button;
    int Modifiers;
    float X, Y;
    int Checked;
    const char* KeyName;
} RmlUE_NodeEvent;

// Synchronous, owner-thread callback. Strings are valid only during the callback.
// Return 1 to stop propagation, 2 to stop immediate propagation. RmlUi has no
// independent DOM preventDefault operation. Do not destroy/reload the view here.
// Bit 4 suppresses the following Enter text character; this is a text-input bridge operation.
typedef int (*RmlUE_NodeEventCallback)(void* User, uint32_t Listener, const RmlUE_NodeEvent* Event);

// Handles are process-unique positive int32 values, never pointers. Destroyed
// nodes, replaced documents and handles belonging to another view are rejected.
RMLUE_API RmlUE_Node RmlUE_GetRootNode(RmlUE_View* View);
RMLUE_API RmlUE_Node RmlUE_FindNode(RmlUE_View* View, const char* Id);
// Kind: 0 = element, 1 = text, 2 = non-rendering fragment anchor.
RMLUE_API RmlUE_Node RmlUE_CreateNode(RmlUE_View* View, int Kind, const char* TagOrText);
RMLUE_API int RmlUE_IsNodeValid(RmlUE_View* View, RmlUE_Node Node);
RMLUE_API int RmlUE_InsertNode(RmlUE_View* View, RmlUE_Node Node, RmlUE_Node Parent, RmlUE_Node Before);
RMLUE_API int RmlUE_RemoveNode(RmlUE_View* View, RmlUE_Node Node);
RMLUE_API RmlUE_Node RmlUE_ParentNode(RmlUE_View* View, RmlUE_Node Node);
RMLUE_API RmlUE_Node RmlUE_NextNode(RmlUE_View* View, RmlUE_Node Node);
RMLUE_API int RmlUE_SetNodeText(RmlUE_View* View, RmlUE_Node Node, const char* Text);
RMLUE_API int RmlUE_GetNodeText(RmlUE_View* View, RmlUE_Node Node, char* Text, size_t Capacity);
// A null Value removes the attribute/property.
RMLUE_API int RmlUE_SetNodeAttribute(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* Value);
RMLUE_API int RmlUE_GetNodeAttribute(RmlUE_View* View, RmlUE_Node Node, const char* Name, char* Value, size_t Capacity);
RMLUE_API int RmlUE_SetNodeProperty(RmlUE_View* View, RmlUE_Node Node, const char* Name, const char* Value);
RMLUE_API int RmlUE_ListenNode(RmlUE_View* View, RmlUE_Node Node, const char* Type, uint32_t Listener, int Capture);
RMLUE_API void RmlUE_UnlistenNode(RmlUE_View* View, uint32_t Listener);
RMLUE_API void RmlUE_SetNodeEventCallback(RmlUE_View* View, RmlUE_NodeEventCallback Callback, void* User);
RMLUE_API int RmlUE_Update(RmlUE_View* View);
RMLUE_API void RmlUE_GetNodeCounts(RmlUE_View* View, int* Nodes, int* Listeners);
RMLUE_API int RmlUE_ScrollNode(RmlUE_View* View, RmlUE_Node Node, float Top);
RMLUE_API float RmlUE_NodeScrollRemaining(RmlUE_View* View, RmlUE_Node Node);
RMLUE_API int RmlUE_FocusNode(RmlUE_View* View, RmlUE_Node Node);

// All calls and callbacks execute on the thread that initializes the bridge.
// Returned buffers are copied synchronously, then released through FreeBuffer.
typedef struct RmlUE_Host {
    void* User;
    int (*ReadFile)(void* User, const char* Utf8Path, unsigned char** Data, size_t* Size);
    int (*LoadImage)(void* User, const char* Utf8Path, unsigned char** StraightRGBA, int* Width, int* Height);
    void (*FreeBuffer)(void* User, void* Data);
    void (*Log)(void* User, int Level, const char* Message);
    void (*SetClipboard)(void* User, const char* Utf8Text);
    int (*GetClipboard)(void* User, unsigned char** Utf8Text, size_t* Size);
} RmlUE_Host;

typedef struct RmlUE_Frame {
    // Top-left origin, tightly packed, straight-alpha sRGB BGRA8. Valid until next Render/Destroy.
    const unsigned char* Pixels;
    int Width;
    int Height;
    uint64_t Number;
} RmlUE_Frame;

typedef struct RmlUE_SlateVertex {
    float X, Y;
    float U, V;
    // Nonlinear sRGB byte channels, premultiplied by A in encoded space by RmlUi.
    uint8_t R, G, B, A;
} RmlUE_SlateVertex;

typedef struct RmlUE_SlateDraw {
    uint64_t GeometryId;
    uint64_t Texture;
    float TranslateX, TranslateY;
    // Row-major 2D affine transform applied after geometry translation.
    int TransformEnabled;
    float TransformM00, TransformM01, TransformM10, TransformM11, TransformX, TransformY;
    int ScissorEnabled;
    float ScissorX, ScissorY, ScissorWidth, ScissorHeight;
    // Range in RmlUE_SlateFrame::ClipMasks which must be rebuilt before this draw.
    uint32_t ClipMaskStart, ClipMaskCount;
} RmlUE_SlateDraw;

typedef struct RmlUE_SlateClipMask {
    uint64_t GeometryId;
    // 0 = Set, 1 = SetInverse, 2 = Intersect.
    int Operation;
    float TranslateX, TranslateY;
    int TransformEnabled;
    float TransformM00, TransformM01, TransformM10, TransformM11, TransformX, TransformY;
    int ScissorEnabled;
    float ScissorX, ScissorY, ScissorWidth, ScissorHeight;
} RmlUE_SlateClipMask;

typedef struct RmlUE_SlateGeometryDelta {
    uint64_t Id;
    int Action;
    const RmlUE_SlateVertex* Vertices;
    uint32_t VertexCount;
    const uint32_t* Indices;
    uint32_t IndexCount;
} RmlUE_SlateGeometryDelta;

typedef struct RmlUE_SlateTexture {
    uint64_t Id;
    int Action;
    // 0 = generated/loaded RGBA texture, 1 = host-registered UE material alias.
    int Kind;
    // -1 = not a material, 0 = background, 1 = border, 2 = reserved foreground.
    int MaterialSlot;
    // Top-left, tightly packed nonlinear sRGB RGBA8. RGB is premultiplied by A in encoded space.
    const unsigned char* PremultipliedRGBA;
    int Width, Height;
    const char* MaterialAlias;
} RmlUE_SlateTexture;

typedef struct RmlUE_SlateFrame {
    uint32_t AbiVersion;
    const RmlUE_SlateDraw* Draws;
    uint32_t DrawCount;
    // Clip-mask snapshots referenced by draw ranges. Geometry uses the same incremental cache as ordinary draws.
    const RmlUE_SlateClipMask* ClipMasks;
    uint32_t ClipMaskCount;
    // Create/destroy deltas since the previous frame. Create payload pointers remain valid until the next render.
    const RmlUE_SlateGeometryDelta* GeometryDeltas;
    uint32_t GeometryDeltaCount;
    const RmlUE_SlateTexture* Textures;
    // Texture entries are create/destroy deltas, not a complete live-resource snapshot.
    uint32_t TextureCount;
    uint64_t Number;
    // Nonzero when the document requested effects which need the legacy renderer.
    uint32_t UnsupportedFeatures;
} RmlUE_SlateFrame;

#define RMLUE_MATERIAL_SLOT_NONE (-1)
#define RMLUE_MATERIAL_SLOT_BACKGROUND 0
#define RMLUE_MATERIAL_SLOT_BORDER 1
#define RMLUE_MATERIAL_SLOT_FOREGROUND 2
#define RMLUE_SLATE_RESOURCE_CREATE 1
#define RMLUE_SLATE_RESOURCE_DESTROY 2
#define RMLUE_CLIP_MASK_SET 0
#define RMLUE_CLIP_MASK_SET_INVERSE 1
#define RMLUE_CLIP_MASK_INTERSECT 2
#define RMLUE_UNSUPPORTED_CLIP_MASK (1u << 0)
#define RMLUE_UNSUPPORTED_TRANSFORM_3D (1u << 1)
#define RMLUE_UNSUPPORTED_LAYER (1u << 2)
#define RMLUE_UNSUPPORTED_FILTER (1u << 3)
#define RMLUE_UNSUPPORTED_SHADER (1u << 4)
#define RMLUE_SLATE_ABI_VERSION 5u

typedef struct RmlUE_Event {
    char Type[32];
    char ElementId[256];
    char Value[2048];
} RmlUE_Event;

typedef struct RmlUE_Rect { float X, Y, Width, Height; } RmlUE_Rect;
typedef struct RmlUE_Stats {
    uint64_t GeometryDraws;
    uint64_t ClipMasks;
    uint64_t Layers;
    uint64_t Filters;
    uint64_t Shaders;
    uint64_t LoadedTextures;
} RmlUE_Stats;

typedef struct RmlUE_ResourceRecord {
    uint64_t Id;
    uint64_t OwnerId;
    uint64_t EstimatedBytes;
    uint64_t CreatedSequence;
    int Type;
    int Backend;
    char Name[96];
} RmlUE_ResourceRecord;

#define RMLUE_RESOURCE_VIEW 1
#define RMLUE_RESOURCE_STYLE_SHEET 2
#define RMLUE_RESOURCE_GEOMETRY 3
#define RMLUE_RESOURCE_TEXTURE 4
#define RMLUE_RESOURCE_FRAME_BUFFER 5
#define RMLUE_RESOURCE_MATERIAL_BINDING 6
#define RMLUE_RESOURCE_BACKEND_SHARED 0
#define RMLUE_RESOURCE_BACKEND_DX11 1
#define RMLUE_RESOURCE_BACKEND_SLATE 2
#define RMLUE_RESOURCE_CREATED 1
#define RMLUE_RESOURCE_UPDATED 2
#define RMLUE_RESOURCE_DESTROYED 3
typedef void (*RmlUE_ResourceEventCallback)(void* User, int Action, const RmlUE_ResourceRecord* Record);

RMLUE_API int RmlUE_Initialize(const RmlUE_Host* Host);
RMLUE_API void RmlUE_Shutdown(void);
RMLUE_API const char* RmlUE_GetLastError(void);
RMLUE_API const char* RmlUE_GetVersion(void);
RMLUE_API int RmlUE_LoadFont(const char* Utf8Path, int Fallback);
// Parsed style sheets are immutable, reference counted, and may be shared by views.
// The base sheet has lower cascade priority than the document's author styles.
RMLUE_API RmlUE_StyleSheet* RmlUE_CreateStyleSheet(const char* Rcss);
RMLUE_API void RmlUE_RetainStyleSheet(RmlUE_StyleSheet* StyleSheet);
RMLUE_API void RmlUE_ReleaseStyleSheet(RmlUE_StyleSheet* StyleSheet);
RMLUE_API RmlUE_View* RmlUE_CreateView(int Width, int Height, float DpRatio);
// Creates a command-recording view for direct Slate replay. It allocates no full-view render target.
RMLUE_API RmlUE_View* RmlUE_CreateSlateView(int Width, int Height, float DpRatio);
RMLUE_API void RmlUE_DestroyView(RmlUE_View* View);
// Applies to the current document and all documents subsequently loaded by the view.
// Pass null to restore the document's unmodified author style sheet.
RMLUE_API int RmlUE_SetBaseStyleSheet(RmlUE_View* View, RmlUE_StyleSheet* StyleSheet);
RMLUE_API int RmlUE_LoadDocument(RmlUE_View* View, const char* Utf8Path);
RMLUE_API int RmlUE_LoadDocumentFromMemory(RmlUE_View* View, const char* Rml, const char* SourcePath);
RMLUE_API int RmlUE_Resize(RmlUE_View* View, int Width, int Height, float DpRatio);
RMLUE_API int RmlUE_Render(RmlUE_View* View, RmlUE_Frame* Frame);
// Pointers remain valid until the next render or destruction of this view.
RMLUE_API int RmlUE_RenderSlate(RmlUE_View* View, RmlUE_SlateFrame* Frame);
RMLUE_API int RmlUE_PollEvent(RmlUE_View* View, RmlUE_Event* Event);
RMLUE_API int RmlUE_SetInnerRml(RmlUE_View* View, const char* Id, const char* Rml);
RMLUE_API int RmlUE_SetProperty(RmlUE_View* View, const char* Id, const char* Property, const char* Value);
// checked/disabled/selected getters return true/false; setting false or 0 removes the boolean attribute.
RMLUE_API int RmlUE_SetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, const char* Value);
RMLUE_API int RmlUE_GetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, char* Value, size_t Capacity);
RMLUE_API int RmlUE_GetElementRect(RmlUE_View* View, const char* Id, RmlUE_Rect* Rect);
RMLUE_API void RmlUE_GetStats(RmlUE_View* View, RmlUE_Stats* Stats);
// Owner-thread diagnostic API. Returns the total record count and copies at most Capacity records.
// Records contain stable IDs and metadata, never native object pointers.
RMLUE_API size_t RmlUE_GetResourceSnapshot(RmlUE_ResourceRecord* Records, size_t Capacity);
RMLUE_API void RmlUE_SetResourceEventCallback(RmlUE_ResourceEventCallback Callback, void* User);
RMLUE_API uint64_t RmlUE_GetViewResourceId(RmlUE_View* View);
RMLUE_API void RmlUE_SetDebuggerVisible(RmlUE_View* View, int Visible);

// Modifiers use Win32-like flags: shift=1, ctrl=2, alt=4, caps=8, num=16, meta=32.
// Keyboard keys are Windows virtual-key codes; text uses UTF-8 separately.
RMLUE_API void RmlUE_MouseMove(RmlUE_View* View, int X, int Y, int Modifiers);
RMLUE_API void RmlUE_MouseButton(RmlUE_View* View, int Button, int Down, int Modifiers);
// Wheel delta follows Unreal: positive = up, negative = down.
RMLUE_API void RmlUE_MouseWheel(RmlUE_View* View, float Delta, int Modifiers);
RMLUE_API void RmlUE_MouseLeave(RmlUE_View* View);
RMLUE_API void RmlUE_Key(RmlUE_View* View, int VirtualKey, int Down, int Modifiers);
RMLUE_API void RmlUE_Text(RmlUE_View* View, const char* Utf8Text);
RMLUE_API void RmlUE_FocusLost(RmlUE_View* View);
// Touch phase: 0 = start, 1 = move, 2 = end, 3 = cancel.
RMLUE_API void RmlUE_Touch(RmlUE_View* View, int Id, float X, float Y, int Phase);

#ifdef __cplusplus
}
#endif
