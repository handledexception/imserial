/* Top-level frame: lays out title bar, sidebar, editor card and status bar,
 * and handles global keyboard shortcuts. */
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>

ImU32 ui_link_color(const Theme *th, LinkState s)
{
    switch (s) {
    case LINK_CONNECTED: return th->ok;
    case LINK_RECONNECTING: return th->warn;
    case LINK_CONNECTING: return th->info;
    default: return th->fg_disabled;
    }
}

/* Status glyph: filled dot when up, spinning sync while (re)connecting, ring when idle. */
void ui_link_icon(ImDrawList *dl, ImVec2 c, LinkState s, float size)
{
    const Theme *th = &g_app->theme;
    ImU32 col = ui_link_color(th, s);
    if (s == LINK_RECONNECTING || s == LINK_CONNECTING) {
        float ang = (float)fmod(igGetTime() * 4.0, 6.2831853);
        ui_draw_icon_rotated(dl, c, ICON_SYNC, col, size, ang);
        g_app->animating = true;
    } else if (s == LINK_CONNECTED) {
        ImDrawList_AddCircleFilled(dl, c, size * 0.28f, col, 16);
        ImDrawList_AddCircleFilled(dl, c, size * 0.46f, col_alpha(col, 0.18f), 20);
    } else {
        ImDrawList_AddCircle(dl, c, size * 0.26f, col, 16, S(1.3f));
    }
}

/* Ctrl+key or Ctrl+Shift+key. While the terminal has keyboard focus and is
 * connected, plain Ctrl+letter goes to the device instead (termview.c), so
 * the Ctrl+Shift form is the one that always works. */
static bool chord(ImGuiKey key)
{
    return igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | key) || igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | key);
}

static void shortcuts(App *a)
{
    Tab *t = app_active_tab(a);
    ImGuiIO *io = igGetIO_Nil();
    if (chord(ImGuiKey_T)) {
        app_new_tab(a, NULL);
        ui_open_quickpick(a);
    }
    if (chord(ImGuiKey_Q))
        a->running = false;
    if (chord(ImGuiKey_W)) {
        app_close_tab(a, a->active);
        t = app_active_tab(a);
    }
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Tab) ||
        igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_PageDown)) {
        a->active = (a->active + 1) % a->ntabs;
        a->tabs[a->active]->focus_terminal = true;
    }
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab) ||
        igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_PageUp)) {
        a->active = (a->active + a->ntabs - 1) % a->ntabs;
        a->tabs[a->active]->focus_terminal = true;
    }
    for (int i = 0; i < 9 && i < a->ntabs; i++)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Alt | (ImGuiKey)(ImGuiKey_1 + i)))
            a->active = i;
    if (chord(ImGuiKey_B)) {
        a->sidebar_visible = !a->sidebar_visible;
        app_mark_dirty(a);
    }
    if (chord(ImGuiKey_P))
        ui_open_quickpick(a);
    if (chord(ImGuiKey_F) && t) {
        t->find_open = true;
        t->find_focus = true;
    }
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K) && t)
        termbuf_clear(&t->tb);
    if (igIsKeyPressed_Bool(ImGuiKey_F5, false) && t && !io->KeyShift && t->target.path[0])
        app_tab_connect(a, t);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Shift | ImGuiKey_F5) && t)
        app_tab_disconnect(a, t);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Equal) ||
        igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd))
        app_set_mono_size(a, a->mono_size + 1);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Minus) ||
        igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract))
        app_set_mono_size(a, a->mono_size - 1);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_0))
        app_set_mono_size(a, MONO_FONT_DEFAULT);
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Comma))
        a->prefs_open = true;
}

void app_frame(App *a)
{
    ImGuiIO *io = igGetIO_Nil();
    const Theme *th = &a->theme;
    float W = io->DisplaySize.x, H = io->DisplaySize.y;

    igSetNextWindowPos(v2(0, 0), 0, v2(0, 0));
    igSetNextWindowSize(v2(W, H), 0);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(0, 0));
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0);
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0);
    igBegin("##root", NULL,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoNav);
    igPopStyleVar(3);

    shortcuts(a);

    float title_h = S(38), status_h = S(26), m = S(6), gap = S(6);
    a->titlebar_h = title_h;
    ui_titlebar(a, v2(0, 0), v2(W, title_h));

    float top = title_h, bottom = H - status_h;
    float x = m;
    if (a->sidebar_visible) {
        float maxw = fmaxf(S(200), W * 0.5f);
        float sw = clampf(a->sidebar_w * a->ui_scale, S(200), maxw);
        ui_sidebar(a, v2(x, top), v2(x + sw, bottom));
        float val = sw;
        if (ui_splitter_v("##split_side", x + sw, top + S(4), bottom - S(4), gap, &val, S(200), maxw, S(280))) {
            a->sidebar_w = val / a->ui_scale;
            app_mark_dirty(a);
        }
        x += sw + gap;
    }

    Tab *t = app_active_tab(a);
    float send_h = S(46);
    ImVec2 e0 = v2(x, top), e1 = v2(W - m, bottom - send_h - gap);
    ImDrawList *dl = igGetWindowDrawList();
    ui_card(dl, e0, e1, th->editor_bg, th->border, S(8), 0);
    float tabs_h = ui_tabs(a, e0, e1.x - e0.x);
    t = app_active_tab(a); /* ui_tabs may have closed the active tab */
    if (t)
        ui_termview(a, t, v2(e0.x + 1, e0.y + tabs_h), v2(e1.x - 1, e1.y - 1));
    if (t)
        ui_sendbar(a, t, v2(x, bottom - send_h), v2(W - m, bottom));

    ui_statusbar(a, v2(0, H - status_h), v2(W, H));

    /* Thin window outline (borderless windows otherwise have no edge). */
    if (!(SDL_GetWindowFlags(a->window) & SDL_WINDOW_MAXIMIZED))
        ImDrawList_AddRect(igGetForegroundDrawList_Nil(), v2(0, 0), v2(W, H), th->dark ? COL32(0x30, 0x31, 0x33, 255) : COL32(0xD0, 0xD0, 0xD4, 255), 0, 1.0f, 0);

    ui_quickpick(a);
    ui_preferences(a);
    ui_log_prompt(a);
    igEnd();
}
