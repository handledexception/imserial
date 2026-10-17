#include "ui/widgets.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

float ui_anim(ImGuiID id, float target, float speed)
{
    ImGuiStorage *st = igGetStateStorage();
    float *v = ImGuiStorage_GetFloatRef(st, id, target);
    float dt = igGetIO_Nil()->DeltaTime;
    float k = 1.0f - expf(-dt * speed);
    *v += (target - *v) * k;
    if (fabsf(target - *v) < 0.002f)
        *v = target;
    else
        g_app->animating = true;
    return *v;
}

void ui_card(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border, float rounding, ImDrawFlags flags)
{
    if (fill >> 24)
        ImDrawList_AddRectFilled(dl, a, b, fill, rounding, flags);
    if (border >> 24)
        ImDrawList_AddRect(dl, v2(a.x + 0.5f, a.y + 0.5f), v2(b.x - 0.5f, b.y - 0.5f), border, rounding, 1.0f, flags);
}

static unsigned decode_utf8(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80)
        return u[0];
    if ((u[0] & 0xE0) == 0xC0)
        return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
    if ((u[0] & 0xF0) == 0xE0)
        return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
    return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
}

/* Centers an icon glyph on its ink box so icons line up regardless of metrics. */
static ImVec2 icon_origin(ImVec2 center, const char *icon, float size)
{
    ImFont *f = g_app->font_ui;
    ImFontBaked *baked = ImFont_GetFontBaked(f, size, -1.0f);
    ImFontGlyph *g = baked ? ImFontBaked_FindGlyph(baked, (ImWchar)decode_utf8(icon)) : NULL;
    if (!g)
        return v2(center.x - size * 0.5f, center.y - size * 0.5f);
    return v2(floorf(center.x - (g->X0 + g->X1) * 0.5f + 0.5f), floorf(center.y - (g->Y0 + g->Y1) * 0.5f + 0.5f));
}

void ui_draw_icon(ImDrawList *dl, ImVec2 center, const char *icon, ImU32 col, float size)
{
    ImDrawList_AddText_FontPtr(dl, g_app->font_ui, size, icon_origin(center, icon, size), col, icon, NULL, 0, NULL);
}

void ui_draw_icon_rotated(ImDrawList *dl, ImVec2 center, const char *icon, ImU32 col, float size, float angle)
{
    int start = dl->VtxBuffer.Size;
    ImVec2 o = icon_origin(center, icon, size);
    /* Don't pixel-snap a rotating glyph. */
    ImDrawList_AddText_FontPtr(dl, g_app->font_ui, size, o, col, icon, NULL, 0, NULL);
    float c = cosf(angle), s = sinf(angle);
    for (int i = start; i < dl->VtxBuffer.Size; i++) {
        ImVec2 *p = &dl->VtxBuffer.Data[i].pos;
        float x = p->x - center.x, y = p->y - center.y;
        p->x = center.x + x * c - y * s;
        p->y = center.y + x * s + y * c;
    }
}

ImVec2 ui_text_size(ImFont *font, float size, const char *text, const char *end)
{
    return ImFont_CalcTextSizeA(font, size, FLT_MAX, 0, text, end, NULL);
}

void ui_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, const char *end)
{
    ImDrawList_AddText_FontPtr(dl, font, size, v2(floorf(pos.x), floorf(pos.y)), col, text, end, 0, NULL);
}

void ui_text_ellipsis(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float max_w)
{
    ImVec2 ts = ui_text_size(font, size, text, NULL);
    if (ts.x <= max_w) {
        ui_text(dl, font, size, pos, col, text, NULL);
        return;
    }
    const char *ell = "\xE2\x80\xA6";
    float ew = ui_text_size(font, size, ell, NULL).x;
    const char *end = text;
    const char *rem = NULL;
    ImFont_CalcTextSizeA(font, size, max_w - ew, 0, text, NULL, &rem);
    end = rem ? rem : text;
    ui_text(dl, font, size, pos, col, text, end);
    float w = ui_text_size(font, size, text, end).x;
    ui_text(dl, font, size, v2(pos.x + w, pos.y), col, ell, NULL);
}

void ui_menu_push(void)
{
    igPushFont(g_app->font_ui, g_app->font_size + 1.5f);
    igPushStyleColor_U32(ImGuiCol_HeaderHovered, g_app->theme.menu_sel);
    /* VS Code menu rows are ~2em tall; the hover highlight spans the gap. */
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(igGetStyle()->ItemSpacing.x, S(9)));
}

void ui_menu_pop(void)
{
    igPopStyleVar(1);
    igPopStyleColor(1);
    igPopFont();
}

static void draw_chevron_in_last_item(void)
{
    ImVec2 mn = igGetItemRectMin(), mx = igGetItemRectMax();
    float h = mx.y - mn.y;
    ui_draw_icon(igGetWindowDrawList(), v2(mx.x - h * 0.5f - S(1), mn.y + h * 0.5f), ICON_CHEVRON_DOWN,
                 igIsItemHovered(0) ? g_app->theme.fg : g_app->theme.fg_dim, S(14));
}

bool ui_combo(const char *id, int *v, const char *const *items, int n)
{
    bool changed = false;
    igSetNextItemWidth(igGetContentRegionAvail().x);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(4), S(4)));
    bool open = igBeginCombo(id, items[*v], ImGuiComboFlags_NoArrowButton);
    igPopStyleVar(1);
    if (!open)
        draw_chevron_in_last_item();
    if (open) {
        igPushStyleColor_U32(ImGuiCol_HeaderHovered, g_app->theme.menu_sel);
        for (int i = 0; i < n; i++) {
            if (igSelectable_Bool(items[i], i == *v, 0, v2(0, 0)) && *v != i) {
                *v = i;
                changed = true;
            }
        }
        igPopStyleColor(1);
        igEndCombo();
        draw_chevron_in_last_item();
    }
    return changed;
}

void ui_tooltip(const char *text)
{
    if (text && igIsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(8), S(5)));
        igSetTooltip("%s", text);
        igPopStyleVar(1);
    }
}

bool ui_icon_button(const char *id, const char *icon, const char *tooltip, bool toggled, float size)
{
    const Theme *t = &g_app->theme;
    ImVec2 pos = igGetCursorScreenPos();
    bool pressed = igInvisibleButton(id, v2(size, size), 0);
    bool hovered = igIsItemHovered(0), held = igIsItemActive();
    ImDrawList *dl = igGetWindowDrawList();
    float h = ui_anim(igGetItemID(), hovered || held ? 1.0f : 0.0f, 18.0f);
    if (toggled)
        ImDrawList_AddRectFilled(dl, pos, v2(pos.x + size, pos.y + size), t->list_active, S(5), 0);
    if (h > 0)
        ImDrawList_AddRectFilled(dl, pos, v2(pos.x + size, pos.y + size), col_alpha(t->toolbar_hover, h), S(5), 0);
    ImU32 col = toggled || hovered ? t->fg : t->icon;
    ui_draw_icon(dl, v2(pos.x + size * 0.5f, pos.y + size * 0.5f), icon, col, S(16));
    ui_tooltip(tooltip);
    return pressed;
}

bool ui_toggle(const char *id, bool *v)
{
    const Theme *t = &g_app->theme;
    float h = S(18), w = S(32);
    ImVec2 pos = igGetCursorScreenPos();
    bool pressed = igInvisibleButton(id, v2(w, h), 0);
    if (pressed)
        *v = !*v;
    bool hovered = igIsItemHovered(0);
    float k = ui_anim(igGetItemID(), *v ? 1.0f : 0.0f, 16.0f);
    ImDrawList *dl = igGetWindowDrawList();
    ImU32 off_bg = t->dark ? COL32(0x3A, 0x3B, 0x3C, 255) : COL32(0xD4, 0xD4, 0xD8, 255);
    ImU32 bg = col_lerp(off_bg, t->accent, k);
    if (hovered)
        bg = col_lerp(bg, t->dark ? 0xFFFFFFFFu : 0xFF000000u, 0.08f);
    ImDrawList_AddRectFilled(dl, pos, v2(pos.x + w, pos.y + h), bg, h * 0.5f, 0);
    float r = h * 0.5f - S(3);
    float cx = pos.x + h * 0.5f + (w - h) * k;
    ImDrawList_AddCircleFilled(dl, v2(cx, pos.y + h * 0.5f), r, COL32(255, 255, 255, 255), 0);
    return pressed;
}

bool ui_segmented(const char *id, const char *const *labels, int n, int *value, float width)
{
    const Theme *t = &g_app->theme;
    float h = igGetFrameHeight();
    ImVec2 pos = igGetCursorScreenPos();
    ImDrawList *dl = igGetWindowDrawList();
    igPushID_Str(id);
    ImU32 track = t->dark ? COL32(0x22, 0x23, 0x24, 255) : COL32(0xEC, 0xEC, 0xEF, 255);
    ImDrawList_AddRectFilled(dl, pos, v2(pos.x + width, pos.y + h), track, S(5), 0);
    ImDrawList_AddRect(dl, pos, v2(pos.x + width, pos.y + h), t->input_border, S(5), 1.0f, 0);
    float seg = width / (float)n;
    float sel = ui_anim(igGetID_Str("sel"), (float)*value, 20.0f);
    ImVec2 sa = v2(pos.x + seg * sel + S(2), pos.y + S(2));
    ImU32 knob = t->dark ? COL32(0x3A, 0x3C, 0x3E, 255) : COL32(0xFF, 0xFF, 0xFF, 255);
    ImDrawList_AddRectFilled(dl, sa, v2(sa.x + seg - S(4), pos.y + h - S(2)), knob, S(4), 0);
    if (!t->dark)
        ImDrawList_AddRect(dl, sa, v2(sa.x + seg - S(4), pos.y + h - S(2)), COL32(0, 0, 0, 28), S(4), 1.0f, 0);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        igSetCursorScreenPos(v2(pos.x + seg * i, pos.y));
        igPushID_Int(i);
        if (igInvisibleButton("seg", v2(seg, h), 0) && *value != i) {
            *value = i;
            changed = true;
        }
        bool hov = igIsItemHovered(0);
        igPopID();
        ImVec2 ts = ui_text_size(g_app->font_ui, g_app->font_size * g_app->ui_scale, labels[i], NULL);
        ImU32 col = i == *value ? t->fg : (hov ? t->fg : t->fg_dim);
        ui_text(dl, g_app->font_ui, g_app->font_size * g_app->ui_scale,
                v2(pos.x + seg * i + (seg - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col, labels[i], NULL);
    }
    igPopID();
    igSetCursorScreenPos(pos);
    igDummy(v2(width, h));
    return changed;
}

bool ui_section_header(const char *label, bool *open)
{
    const Theme *t = &g_app->theme;
    ImVec2 pos = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x;
    float h = S(26);
    if (igInvisibleButton(label, v2(w, h), 0))
        *open = !*open;
    bool hov = igIsItemHovered(0);
    ImDrawList *dl = igGetWindowDrawList();
    float rot = ui_anim(igGetItemID(), *open ? 1.0f : 0.0f, 20.0f);
    ui_draw_icon_rotated(dl, v2(pos.x + S(8), pos.y + h * 0.5f), ICON_CHEVRON_RIGHT, hov ? t->fg : t->icon, S(14),
                         rot * 1.5707963f);
    float fs = S(11);
    ImVec2 ts = ui_text_size(g_app->font_ui_bold, fs, label, NULL);
    ui_text(dl, g_app->font_ui_bold, fs, v2(pos.x + S(20), pos.y + (h - ts.y) * 0.5f), hov ? t->fg : t->fg_dim,
            label, NULL);
    return *open;
}

bool ui_button(const char *label, const char *icon, ImVec2 size, bool primary)
{
    const Theme *t = &g_app->theme;
    ImVec2 pos = igGetCursorScreenPos();
    bool pressed = igInvisibleButton(label, size, 0);
    bool hov = igIsItemHovered(0), held = igIsItemActive();
    ImDrawList *dl = igGetWindowDrawList();
    float k = ui_anim(igGetItemID(), hov ? 1.0f : 0.0f, 18.0f);
    ImU32 bg, fg;
    if (primary) {
        bg = col_lerp(t->accent, t->accent_hover, k);
        if (hov)
            bg = col_lerp(bg, 0xFFFFFFFFu, 0.06f);
        if (held)
            bg = col_lerp(bg, 0xFF000000u, 0.10f);
        fg = t->accent_fg;
    } else {
        bg = col_lerp(t->secondary_bg, t->secondary_hover, k);
        fg = t->secondary_fg;
    }
    ImDrawList_AddRectFilled(dl, pos, v2(pos.x + size.x, pos.y + size.y), bg, S(5), 0);
    if (!primary)
        ImDrawList_AddRect(dl, pos, v2(pos.x + size.x, pos.y + size.y), t->input_border, S(5), 1.0f, 0);
    float fs = g_app->font_size * g_app->ui_scale;
    /* Label text after "##" is the id only. */
    const char *end = strstr(label, "##");
    ImVec2 ts = ui_text_size(g_app->font_ui_bold, fs, label, end);
    float iw = icon ? S(16) + S(6) : 0;
    float x = pos.x + (size.x - ts.x - iw) * 0.5f;
    if (icon)
        ui_draw_icon(dl, v2(x + S(8), pos.y + size.y * 0.5f), icon, fg, S(16));
    ui_text(dl, g_app->font_ui_bold, fs, v2(x + iw, pos.y + (size.y - ts.y) * 0.5f), fg, label, end);
    return pressed;
}

bool ui_splitter_v(const char *id, float x, float y0, float y1, float w, float *value, float vmin, float vmax,
                   float def)
{
    const Theme *t = &g_app->theme;
    igSetCursorScreenPos(v2(x - S(2), y0));
    igInvisibleButton(id, v2(w + S(4), y1 - y0), 0);
    bool hov = igIsItemHovered(0), active = igIsItemActive();
    bool changed = false;
    if (hov || active)
        igSetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active) {
        float dx = igGetIO_Nil()->MouseDelta.x;
        if (dx != 0) {
            *value = clampf(*value + dx, vmin, vmax);
            changed = true;
        }
    }
    if (hov && igIsMouseDoubleClicked_Nil(0)) {
        *value = def;
        changed = true;
    }
    float k = ui_anim(igGetItemID(), active ? 1.0f : hov ? 0.7f : 0.0f, 14.0f);
    ImDrawList *dl = igGetWindowDrawList();
    float cx = x + w * 0.5f, cy = (y0 + y1) * 0.5f;
    if (k > 0.01f) {
        /* A soft rail that fades in to show the drag target. */
        ImDrawList_AddRectFilled(dl, v2(cx - S(1), y0 + S(8)), v2(cx + S(1), y1 - S(8)), col_alpha(t->tab_top, k * 0.55f),
                                 S(1), 0);
    }
    ImU32 dot = col_lerp(col_alpha(t->fg_dim, 0.55f), t->tab_top, k);
    float r = S(1.6f), gap = S(5.5f);
    for (int i = -1; i <= 1; i++) {
        ImVec2 c = v2(cx, cy + gap * (float)i);
        if (k > 0.01f)
            ImDrawList_AddCircleFilled(dl, c, r + S(2.2f), col_alpha(t->bg, k), 12);
        ImDrawList_AddCircleFilled(dl, c, r, dot, 12);
    }
    return changed;
}

void ui_form_label(const char *label, float label_w)
{
    ImVec2 pos = igGetCursorScreenPos();
    float h = igGetFrameHeight();
    float fs = g_app->font_size * g_app->ui_scale;
    ImVec2 ts = ui_text_size(g_app->font_ui, fs, label, NULL);
    ui_text(igGetWindowDrawList(), g_app->font_ui, fs, v2(pos.x, pos.y + (h - ts.y) * 0.5f), g_app->theme.fg_dim,
            label, NULL);
    igSetCursorScreenPos(v2(pos.x + label_w, pos.y));
}

void ui_titlebar_hole(ImVec2 a, ImVec2 b)
{
    App *app = g_app;
    if (app->nholes < (int)(sizeof app->hit_holes / sizeof app->hit_holes[0])) {
        UiRect *r = &app->hit_holes[app->nholes++];
        r->x = a.x;
        r->y = a.y;
        r->w = b.x - a.x;
        r->h = b.y - a.y;
    }
}

void ui_format_bytes(uint64_t n, char *out, size_t outlen)
{
    if (n < 1024)
        snprintf(out, outlen, "%llu B", (unsigned long long)n);
    else if (n < 1024 * 1024)
        snprintf(out, outlen, "%.1f KB", (double)n / 1024.0);
    else if (n < 1024ull * 1024 * 1024)
        snprintf(out, outlen, "%.1f MB", (double)n / (1024.0 * 1024.0));
    else
        snprintf(out, outlen, "%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
}
