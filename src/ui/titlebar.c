/* Custom client-side title bar in the VS Code style: menu bar on the left, a
 * "command center" in the middle and window controls on the right. Empty
 * areas drag the window (see hit_test() in main.c). */
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static bool chrome_button(const char *id, const char *icon, ImVec2 pos, ImVec2 size, bool danger)
{
    const Theme *th = &g_app->theme;
    igSetCursorScreenPos(pos);
    bool pressed = igInvisibleButton(id, size, 0);
    bool hov = igIsItemHovered(0), held = igIsItemActive();
    ImDrawList *dl = igGetWindowDrawList();
    float k = ui_anim(igGetItemID(), hov ? 1.0f : 0.0f, 22.0f);
    ImU32 bg = danger ? th->close_hover : th->toolbar_hover;
    if (held && danger)
        bg = col_lerp(bg, 0xFF000000u, 0.15f);
    if (k > 0)
        ImDrawList_AddRectFilled(dl, pos, v2(pos.x + size.x, pos.y + size.y), col_alpha(bg, k), 0, 0);
    ImU32 fg = danger && hov ? COL32(255, 255, 255, 255) : (g_app->window_focused ? th->fg : th->fg_dim);
    ui_draw_icon(dl, v2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), icon, fg, S(16));
    ui_titlebar_hole(pos, v2(pos.x + size.x, pos.y + size.y));
    return pressed;
}

/* ---- Menu bar --------------------------------------------------------------- */

enum { MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_CONNECTION, MENU_HELP, MENU_COUNT };
static const char *const k_menus[MENU_COUNT] = {"File", "Edit", "View", "Connection", "Help"};

static void menu_contents(App *a, int m)
{
    Tab *t = app_active_tab(a);
    LinkState st = t ? session_state(t->session) : LINK_IDLE;
    switch (m) {
    case MENU_FILE:
        if (igMenuItem_Bool("New Tab", "Ctrl+Shift+T", false, true)) {
            app_new_tab(a, NULL);
            ui_open_quickpick(a);
        }
        if (igMenuItem_Bool("Close Tab", "Ctrl+Shift+W", false, t != NULL))
            app_close_tab(a, a->active);
        igSeparator();
        if (igMenuItem_Bool("Preferences\xE2\x80\xA6", "Ctrl+,", false, true))
            a->prefs_open = true;
        if (igMenuItem_Bool("Open Log Folder", NULL, false, true))
            app_open_log_folder(a);
        igSeparator();
        if (igMenuItem_Bool("Quit", "Ctrl+Shift+Q", false, true))
            a->running = false;
        break;
    case MENU_EDIT:
        if (igMenuItem_Bool("Copy", "Ctrl+Shift+C", false, t && t->has_sel))
            termview_copy_selection(t);
        if (igMenuItem_Bool("Paste", "Ctrl+Shift+V", false, st == LINK_CONNECTED))
            termview_paste(a, t);
        if (igMenuItem_Bool("Select All", "Ctrl+Shift+A", false, t != NULL))
            termview_select_all(t);
        igSeparator();
        if (igMenuItem_Bool("Find\xE2\x80\xA6", "Ctrl+Shift+F", false, t != NULL)) {
            t->find_open = true;
            t->find_focus = true;
        }
        igSeparator();
        if (igMenuItem_Bool("Clear Terminal", "Ctrl+Shift+K", false, t != NULL))
            termbuf_clear(&t->tb);
        break;
    case MENU_VIEW:
        if (igMenuItem_Bool("Sidebar", "Ctrl+Shift+B", a->sidebar_visible, true)) {
            a->sidebar_visible = !a->sidebar_visible;
            app_mark_dirty(a);
        }
        if (igMenuItem_Bool("Timestamps", NULL, t && t->timestamps, t != NULL)) {
            t->timestamps = !t->timestamps;
            app_mark_dirty(a);
        }
        igSeparator();
        if (igMenuItem_Bool("Dark Theme", NULL, a->dark, true) && !a->dark)
            app_set_theme(a, true);
        if (igMenuItem_Bool("Light Theme", NULL, !a->dark, true) && a->dark)
            app_set_theme(a, false);
        igSeparator();
        if (igMenuItem_Bool("Increase Font Size", "Ctrl+=", false, a->mono_size < MONO_FONT_MAX))
            app_set_mono_size(a, a->mono_size + 1);
        if (igMenuItem_Bool("Decrease Font Size", "Ctrl+-", false, a->mono_size > MONO_FONT_MIN))
            app_set_mono_size(a, a->mono_size - 1);
        if (igMenuItem_Bool("Reset Font Size", "Ctrl+0", false, a->mono_size != MONO_FONT_DEFAULT))
            app_set_mono_size(a, MONO_FONT_DEFAULT);
        break;
    case MENU_CONNECTION:
        if (igMenuItem_Bool("Select Port\xE2\x80\xA6", "Ctrl+Shift+P", false, true))
            ui_open_quickpick(a);
        igSeparator();
        if (igMenuItem_Bool("Connect", "F5", false, t && st == LINK_IDLE && t->target.path[0]))
            app_tab_connect(a, t);
        if (igMenuItem_Bool("Disconnect", "Shift+F5", false, st != LINK_IDLE))
            app_tab_disconnect(a, t);
        if (igMenuItem_Bool("Send Break", NULL, false, st == LINK_CONNECTED))
            session_send_break(t->session);
        if (igMenuItem_Bool("Send Terminal Size", NULL, false, st == LINK_CONNECTED))
            app_tab_send_size(a, t);
        igSeparator();
        if (igMenuItem_Bool("Auto-Reconnect", NULL, t && t->auto_reconnect, t != NULL)) {
            t->auto_reconnect = !t->auto_reconnect;
            app_tab_apply_config(a, t);
        }
        if (igMenuItem_Bool("Local Echo", NULL, t && t->local_echo, t != NULL)) {
            t->local_echo = !t->local_echo;
            app_mark_dirty(a);
        }
        if (igMenuItem_Bool("Treat CR as New Line", NULL, t && t->nl_mode == TB_NL_ANY, t != NULL)) {
            t->nl_mode = t->nl_mode == TB_NL_ANY ? TB_NL_TERMINAL : TB_NL_ANY;
            app_tab_apply_config(a, t);
        }
        break;
    case MENU_HELP:
        if (igMenuItem_Bool("About imserial", NULL, false, true))
            a->about_open = true;
        break;
    }
}

/* VS Code-style menu bar: click a title to open its menu; while one is open,
 * hovering another title switches to it. Returns the x just past the last title. */
static float menu_bar(App *a, float x, float y0, float h)
{
    static int s_open = -1;     /* menu whose popup was open at the end of the last frame */
    static bool s_drag = false; /* a menu was opened by a press that's still held */
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    float fs = a->font_size * a->ui_scale;
    float bh = S(26), by = y0 + (h - bh) * 0.5f;
    ImVec2 r0[MENU_COUNT], r1[MENU_COUNT];
    bool hov[MENU_COUNT];
    int want = -2; /* -2: no change */
    for (int i = 0; i < MENU_COUNT; i++) {
        float bw = ui_text_size(a->font_ui, fs, k_menus[i], NULL).x + S(16);
        r0[i] = v2(x, by);
        r1[i] = v2(x + bw, by + bh);
        x += bw;
        igSetCursorScreenPos(r0[i]);
        igPushID_Int(i);
        /* A click on the open menu's title has already closed its popup (click
         * outside), so only reopen it if it wasn't the one open. The title
         * lets go of the mouse once pressed, so a press-drag-release onto a
         * menu item chooses it (menu items act on release), as on Windows. */
        bool pressed = igInvisibleButton("##menu", v2(bw, bh),
                                         ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoHoldingActiveId |
                                             ImGuiButtonFlags_NoSetKeyOwner);
        igPopID();
        hov[i] = igIsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        ui_titlebar_hole(r0[i], r1[i]);
        if (pressed) {
            want = s_open == i ? -1 : i;
            s_drag = want == i;
        } else if (hov[i] && s_open >= 0 && s_open != i)
            want = i;
    }
    char id[16];
    if (want >= 0) {
        snprintf(id, sizeof id, "##menu%d", want);
        igOpenPopup_Str(id, 0);
    }

    int open_now = -1;
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(6), S(6)));
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(S(8), S(2)));
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, v2(S(8), S(5)));
    for (int i = 0; i < MENU_COUNT; i++) {
        snprintf(id, sizeof id, "##menu%d", i);
        igSetNextWindowPos(v2(r0[i].x, r1[i].y + S(2)), ImGuiCond_Always, v2(0, 0));
        if (igBeginPopup(id, 0)) {
            open_now = i;
            ui_menu_push();
            menu_contents(a, i);
            ui_menu_pop();
            /* Pressed on a title, dragged off and released outside both the
             * menu and the titles: close it, as Windows does. (Released on a
             * title, it stays open: that was a click.) */
            if (s_drag && igIsMouseReleased_Nil(ImGuiMouseButton_Left) && !igIsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                bool on_title = false;
                for (int k = 0; k < MENU_COUNT; k++)
                    on_title |= hov[k];
                if (!on_title)
                    igCloseCurrentPopup();
            }
            igEndPopup();
        }
    }
    igPopStyleVar(3);
    s_open = open_now;
    if (!igIsMouseDown_Nil(ImGuiMouseButton_Left))
        s_drag = false;

    for (int i = 0; i < MENU_COUNT; i++) {
        bool open = open_now == i;
        if (open || hov[i])
            ImDrawList_AddRectFilled(dl, r0[i], r1[i], th->toolbar_hover, S(5), 0);
        ImVec2 ts = ui_text_size(a->font_ui, fs, k_menus[i], NULL);
        ImU32 col = a->window_focused || open ? th->fg : th->fg_dim;
        ui_text(dl, a->font_ui, fs, v2(r0[i].x + S(8), by + (bh - ts.y) * 0.5f), col, k_menus[i], NULL);
    }
    return x;
}

static void about_popup(App *a)
{
    if (a->about_open) {
        igOpenPopup_Str("About imserial", 0);
        a->about_open = false;
    }
    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(v2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.4f), ImGuiCond_Appearing,
                       v2(0.5f, 0.5f));
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(20), S(16)));
    if (igBeginPopupModal("About imserial", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        igPushFont(a->font_ui_bold, 16);
        igTextUnformatted("imserial " IMSERIAL_VERSION, NULL);
        igPopFont();
        igPushStyleColor_U32(ImGuiCol_Text, a->theme.fg_dim);
        igTextUnformatted("A fast, minimal serial terminal.", NULL);
        igSpacing();
        igTextUnformatted("Built with Dear ImGui (cimgui), SDL3, libserialport,", NULL);
        igTextUnformatted("FreeType, cJSON, Inter, IBM Plex Mono, JetBrains Mono and Codicons.", NULL);
        igPopStyleColor(1);
        igSpacing();
        if (ui_button("Close", NULL, v2(S(90), S(28)), true) || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            igCloseCurrentPopup();
        igEndPopup();
    }
    igPopStyleVar(1);
}

void ui_titlebar(App *a, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    float h = p1.y - p0.y, w = p1.x - p0.x;
    ImDrawList_AddRectFilled(dl, p0, p1, th->bg, 0, 0);

    /* Logo + menu bar */
    float x = p0.x + S(10);
    ImVec2 lc = v2(x + S(11), p0.y + h * 0.5f);
    ui_draw_app_logo(dl, lc, S(20));
    x = menu_bar(a, x + S(28), p0.y, h);
    about_popup(a);

    /* Window controls */
    float bw = S(46);
    float rx = p1.x - bw * 3;
    bool maxed = (SDL_GetWindowFlags(a->window) & SDL_WINDOW_MAXIMIZED) != 0;
    if (chrome_button("##min", ICON_CHROME_MINIMIZE, v2(rx, p0.y), v2(bw, h), false))
        SDL_MinimizeWindow(a->window);
    if (chrome_button("##max", maxed ? ICON_CHROME_RESTORE : ICON_CHROME_MAXIMIZE, v2(rx + bw, p0.y), v2(bw, h), false))
        app_toggle_maximize(a);
    if (chrome_button("##close", ICON_CHROME_CLOSE, v2(rx + bw * 2, p0.y), v2(bw, h), true))
        a->running = false;

    /* Layout + theme toggles */
    float ib = S(26);
    float ax = rx - S(8) - ib * 2 - S(4);
    igSetCursorScreenPos(v2(ax, p0.y + (h - ib) * 0.5f));
    if (ui_icon_button("##tb_sidebar", a->sidebar_visible ? ICON_LAYOUT_SIDEBAR_LEFT : ICON_LAYOUT_SIDEBAR_LEFT_OFF,
                       "Toggle Sidebar (Ctrl+Shift+B)", false, ib)) {
        a->sidebar_visible = !a->sidebar_visible;
        app_mark_dirty(a);
    }
    ui_titlebar_hole(igGetItemRectMin(), igGetItemRectMax());
    igSetCursorScreenPos(v2(ax + ib + S(4), p0.y + (h - ib) * 0.5f));
    if (ui_icon_button("##tb_theme", ICON_COLOR_MODE, a->dark ? "Switch to Light Theme" : "Switch to Dark Theme", false, ib))
        app_set_theme(a, !a->dark);
    ui_titlebar_hole(igGetItemRectMin(), igGetItemRectMax());

    /* Command center */
    Tab *t = app_active_tab(a);
    float cw = clampf(w * 0.42f, S(240), S(560));
    float ch = S(26);
    float cx = p0.x + (w - cw) * 0.5f;
    float left_limit = x + S(12), right_limit = ax - S(12);
    if (cx < left_limit)
        cx = left_limit;
    if (cx + cw > right_limit)
        cw = right_limit - cx;
    if (cw > S(120)) {
        ImVec2 c0 = v2(cx, p0.y + (h - ch) * 0.5f), c1 = v2(cx + cw, c0.y + ch);
        igSetCursorScreenPos(c0);
        if (igInvisibleButton("##cmdcenter", v2(cw, ch), 0))
            ui_open_quickpick(a);
        bool hov = igIsItemHovered(0);
        ui_titlebar_hole(c0, c1);
        float k = ui_anim(igGetItemID(), hov ? 1.0f : 0.0f, 18.0f);
        ImDrawList_AddRectFilled(dl, c0, c1, col_lerp(th->cmd_bg, th->cmd_hover, k), S(6), 0);
        ImDrawList_AddRect(dl, c0, c1, th->cmd_border, S(6), 1.0f, 0);

        char label[256];
        LinkState ls = t ? session_state(t->session) : LINK_IDLE;
        if (t && t->target.path[0]) {
            char cfg[48], title[64];
            serial_config_describe(&t->cfg, cfg, sizeof cfg);
            app_tab_title(t, title, sizeof title);
            snprintf(label, sizeof label, "%s  \xC2\xB7  %s  \xC2\xB7  %s", t->target.name[0] ? t->target.name : title,
                     title, cfg);
        } else {
            snprintf(label, sizeof label, "Select a serial port\xE2\x80\xA6");
        }
        float fs = a->font_size * a->ui_scale;
        ImVec2 ts = ui_text_size(a->font_ui, fs, label, NULL);
        float max_text = cw - S(70);
        float tw = ts.x < max_text ? ts.x : max_text;
        float tx = cx + (cw - tw - S(22)) * 0.5f;
        if (t && t->target.path[0])
            ui_link_icon(dl, v2(tx + S(7), c0.y + ch * 0.5f), ls, S(14));
        else
            ui_draw_icon(dl, v2(tx + S(7), c0.y + ch * 0.5f), ICON_PLUG, th->fg_dim, S(14));
        ImU32 tc = a->window_focused ? (hov ? th->fg : th->fg_dim) : th->fg_disabled;
        ui_text_ellipsis(dl, a->font_ui, fs, v2(tx + S(22), c0.y + (ch - ts.y) * 0.5f), tc, label, max_text);
        if (cw > S(380)) {
            const char *hint = "Ctrl+Shift+P";
            ImVec2 hs = ui_text_size(a->font_ui, S(11), hint, NULL);
            ui_text(dl, a->font_ui, S(11), v2(c1.x - hs.x - S(10), c0.y + (ch - hs.y) * 0.5f),
                    col_alpha(th->fg_dim, 0.7f), hint, NULL);
        }
    }
    ImDrawList_AddLine(dl, v2(p0.x, p1.y - 0.5f), v2(p1.x, p1.y - 0.5f), col_alpha(th->border, 0.0f), 1.0f);
}
