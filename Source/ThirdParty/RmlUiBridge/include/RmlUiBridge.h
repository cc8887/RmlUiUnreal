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

RMLUE_API int RmlUE_Initialize(const RmlUE_Host* Host);
RMLUE_API void RmlUE_Shutdown(void);
RMLUE_API const char* RmlUE_GetLastError(void);
RMLUE_API const char* RmlUE_GetVersion(void);
RMLUE_API int RmlUE_LoadFont(const char* Utf8Path, int Fallback);
RMLUE_API RmlUE_View* RmlUE_CreateView(int Width, int Height, float DpRatio);
RMLUE_API void RmlUE_DestroyView(RmlUE_View* View);
RMLUE_API int RmlUE_LoadDocument(RmlUE_View* View, const char* Utf8Path);
RMLUE_API int RmlUE_LoadDocumentFromMemory(RmlUE_View* View, const char* Rml, const char* SourcePath);
RMLUE_API int RmlUE_Resize(RmlUE_View* View, int Width, int Height, float DpRatio);
RMLUE_API int RmlUE_Render(RmlUE_View* View, RmlUE_Frame* Frame);
RMLUE_API int RmlUE_PollEvent(RmlUE_View* View, RmlUE_Event* Event);
RMLUE_API int RmlUE_SetInnerRml(RmlUE_View* View, const char* Id, const char* Rml);
RMLUE_API int RmlUE_SetProperty(RmlUE_View* View, const char* Id, const char* Property, const char* Value);
// checked/disabled/selected getters return true/false; setting false or 0 removes the boolean attribute.
RMLUE_API int RmlUE_SetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, const char* Value);
RMLUE_API int RmlUE_GetAttribute(RmlUE_View* View, const char* Id, const char* Attribute, char* Value, size_t Capacity);
RMLUE_API int RmlUE_GetElementRect(RmlUE_View* View, const char* Id, RmlUE_Rect* Rect);
RMLUE_API void RmlUE_GetStats(RmlUE_View* View, RmlUE_Stats* Stats);
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
