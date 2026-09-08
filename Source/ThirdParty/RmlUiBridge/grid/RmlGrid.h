#pragma once
#include <stddef.h>
#include <stdint.h>

// Length kinds: auto = 0, pixels = 1, percentage (0..100) = 2.
struct RmlGridLength { float value; int32_t kind; };
struct RmlGridStyle {
    RmlGridLength size[2], min_size[2], max_size[2];
    RmlGridLength margin[4], padding[4]; // top, right, bottom, left
    float border[4];
    int32_t border_box, overflow_x, overflow_y;
    const char* columns;
    const char* rows;
    const char* auto_columns;
    const char* auto_rows;
    const char* areas;
    const char* flow;
    const char* column_start;
    const char* column_end;
    const char* row_start;
    const char* row_end;
    const char* justify_items;
    const char* align_items;
    const char* justify_self;
    const char* align_self;
    const char* justify_content;
    const char* align_content;
    RmlGridLength gap[2];
    float units[5]; // em, rem, dp, vw, vh, resolved by the RmlUi element
};
struct RmlGridRect { float x, y, width, height; float padding[4], margin[4]; };
// Negative available sizes encode min-content (-1) and max-content (-2).
typedef void (*RmlGridMeasure)(void* user, size_t index, float known_width, float known_height,
    float available_width, float available_height, float* width, float* height);
extern "C" {
int RmlGrid_Validate(const char* property, const char* value);
int RmlGrid_Layout(const RmlGridStyle* container, const RmlGridStyle* items, size_t count,
    RmlGridMeasure measure, void* user, RmlGridRect* results, float* width, float* height, char* error, size_t capacity);
}
