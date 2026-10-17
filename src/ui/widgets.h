/* Custom-drawn widgets in the VS Code look: toolbar icon buttons, toggle
 * switches, segmented controls, splitters with a grip, cards. */
#pragma once
#include "app/app.h"
#include "ui/icons.h"

#define S(x) ((x) * g_app->ui_scale)

/* Smoothly animates a per-id value toward target; keeps frames coming while moving. */
float ui_anim(ImGuiID id, float target, float speed);

void ui_card(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding, ImDrawFlags flags);
void ui_draw_icon(ImDrawList *dl, ImVec2 center, const char *icon, ImU32 col, float size);
void ui_draw_icon_rotated(ImDrawList *dl, ImVec2 center, const char *icon, ImU32 col, float size, float angle);
ImVec2 ui_text_size(ImFont *font, float size, const char *text, const char *end);
void ui_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, const char *end);
/* Draws text clipped with an ellipsis to max_w. */
void ui_text_ellipsis(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float max_w);

bool ui_icon_button(const char *id, const char *icon, const char *tooltip, bool toggled, float size);
bool ui_toggle(const char *id, bool *v);
bool ui_segmented(const char *id, const char *const *labels, int n, int *value, float width);
bool ui_section_header(const char *label, bool *open);
bool ui_button(const char *label, const char *icon, ImVec2 size, bool primary);
void ui_tooltip(const char *text);

/* Combo box (chevron, no arrow button) filling the remaining width. */
bool ui_combo(const char *id, int *v, const char *const *items, int n);

/* Shared look for popup menus (context menus, menu bar): call ui_menu_push()
 * right after Begin*Popup* succeeds and ui_menu_pop() before EndPopup. */
void ui_menu_push(void);
void ui_menu_pop(void);

/* Vertical splitter occupying [x, x+w] x [y0, y1]. Dragging adjusts *value by
 * the horizontal mouse delta. Double-click resets to def. */
bool ui_splitter_v(const char *id, float x, float y0, float y1, float w, float *value, float vmin, float vmax,
                   float def);

/* Labelled row helper for the sidebar forms: label on the left, widget on the right. */
void ui_form_label(const char *label, float label_w);

/* Registers a title bar region that must receive clicks instead of dragging the window. */
void ui_titlebar_hole(ImVec2 a, ImVec2 b);

void ui_format_bytes(uint64_t n, char *out, size_t outlen);

/* The app icon (appicon.c): rendered into an n x n RGBA buffer for the window
 * icon, or drawn as the title bar logo, about `size` px square. */
void app_icon_render(uint8_t *rgba, int pitch, int n);
void ui_draw_app_logo(ImDrawList *dl, ImVec2 center, float size);
