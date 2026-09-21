#pragma once
#include "RmlUiBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// Version 1. Positions are Unicode scalar indices, never UTF-8 bytes or UTF-16 ACP.
// All calls execute on the bridge owner thread. Geometry is in RmlUi viewport pixels.
typedef struct RmlUE_TextInputState {
    uint32_t Version;
    RmlUE_Node Node;
    int Active;
    int ReadOnly;
    int Composing;
    int Length;
    int SelectionStart;
    int SelectionEnd;
    int Caret;
    int CompositionStart;
    int CompositionEnd;
} RmlUE_TextInputState;

RMLUE_API int RmlUE_TextInputGetState(RmlUE_View* View, RmlUE_TextInputState* State);
// Returns required buffer capacity including the terminator, or 0 without an active input.
RMLUE_API int RmlUE_TextInputGetText(RmlUE_View* View, char* Text, size_t Capacity);
RMLUE_API int RmlUE_TextInputSetSelection(RmlUE_View* View, int Anchor, int Caret);
RMLUE_API int RmlUE_TextInputReplace(RmlUE_View* View, int Start, int End, const char* Text);
RMLUE_API int RmlUE_TextInputBeginComposition(RmlUE_View* View);
RMLUE_API int RmlUE_TextInputUpdateComposition(RmlUE_View* View, int Start, int End);
// Cancel restores the pre-composition text and selection. Commit respects maxlength.
RMLUE_API int RmlUE_TextInputEndComposition(RmlUE_View* View, int Cancel);
RMLUE_API int RmlUE_TextInputIsComposing(RmlUE_View* View);
RMLUE_API int RmlUE_TextInputGetBounds(RmlUE_View* View, int Start, int End, RmlUE_Rect* Bounds, int* Clipped);
RMLUE_API int RmlUE_TextInputGetScreenBounds(RmlUE_View* View, RmlUE_Rect* Bounds);
RMLUE_API int RmlUE_TextInputHitTest(RmlUE_View* View, float X, float Y);

#ifdef __cplusplus
}
#endif
