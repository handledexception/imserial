/* Single include point for the cimgui C API plus a few tiny helpers. */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "cimgui.h"

static inline ImVec2 v2(float x, float y)
{
    ImVec2 r = {x, y};
    return r;
}
static inline ImVec2 v2add(ImVec2 a, ImVec2 b) { return v2(a.x + b.x, a.y + b.y); }
static inline ImVec2 v2sub(ImVec2 a, ImVec2 b) { return v2(a.x - b.x, a.y - b.y); }
static inline ImVec4 v4(float x, float y, float z, float w)
{
    ImVec4 r = {x, y, z, w};
    return r;
}
static inline ImRect_c rect(ImVec2 a, ImVec2 b)
{
    ImRect_c r = {a, b};
    return r;
}
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

#define COL32(r, g, b, a) (((ImU32)(a) << 24) | ((ImU32)(b) << 16) | ((ImU32)(g) << 8) | ((ImU32)(r)))
