/* Editor-style tab strip: one tab per session with a live link indicator,
 * close buttons, drag-to-reorder and terminal actions on the right. */
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static void more_menu(App *a, Tab *t)
{
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(6), S(6)));
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(S(8), S(2)));
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, v2(S(8), S(5)));
    if (igBeginPopup("##termmore", 0)) {
        ui_menu_push();
        if (igMenuItem_Bool("Show Timestamps", NULL, t->timestamps, true)) {
            t->timestamps = !t->timestamps;
            app_mark_dirty(a);
        }
        if (igMenuItem_Bool("Local Echo", NULL, t->local_echo, true)) {
            t->local_echo = !t->local_echo;
            app_mark_dirty(a);
        }
        if (igMenuItem_Bool("Treat CR as New Line", NULL, t->nl_mode == TB_NL_ANY, true)) {
            t->nl_mode = t->nl_mode == TB_NL_ANY ? TB_NL_TERMINAL : TB_NL_ANY;
            app_tab_apply_config(a, t);
        }
        if (igMenuItem_Bool("Batch Screen Redraws", NULL, t->batch_redraws, true)) {
            t->batch_redraws = !t->batch_redraws;
            app_mark_dirty(a);
        }
        if (igMenuItem_Bool("Auto-Reconnect", NULL, t->auto_reconnect, true)) {
            t->auto_reconnect = !t->auto_reconnect;
            app_tab_apply_config(a, t);
        }
        igSeparator();
        if (igMenuItem_Bool("Copy", "Ctrl+Shift+C", false, t->has_sel))
            termview_copy_selection(t);
        if (igMenuItem_Bool("Select All", NULL, false, true))
            termview_select_all(t);
        if (igMenuItem_Bool("Clear Terminal", "Ctrl+Shift+K", false, true))
            termbuf_clear(&t->tb);
        igSeparator();
        if (igMenuItem_Bool("Send Break", NULL, false, session_state(t->session) == LINK_CONNECTED))
            session_send_break(t->session);
        ui_menu_pop();
        igEndPopup();
    }
    igPopStyleVar(3);
}

float ui_tabs(App *a, ImVec2 p0, float width)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    float h = S(36), r = S(8);
    ImVec2 p1 = v2(p0.x + width, p0.y + h);
    ImDrawList_AddRectFilled(dl, v2(p0.x + 1, p0.y + 1), v2(p1.x - 1, p1.y), th->tabs_bg, r - 1,
                             ImDrawFlags_RoundCornersTop);
    ImDrawList_AddLine(dl, v2(p0.x + 1, p1.y - 0.5f), v2(p1.x - 1, p1.y - 0.5f), th->border, 1.0f);

    Tab *t = app_active_tab(a);
    float act_w = S(28) * 4 + S(8);
    float plus_w = S(32);
    float avail = width - act_w - plus_w - S(4);
    float fs = a->font_size * a->ui_scale;

    /* Natural widths, then shrink evenly if they don't fit. */
    float widths[MAX_TABS], total = 0;
    char titles[MAX_TABS][64];
    for (int i = 0; i < a->ntabs; i++) {
        app_tab_title(a->tabs[i], titles[i], sizeof titles[i]);
        float tw = ui_text_size(a->font_ui, fs, titles[i], NULL).x;
        widths[i] = clampf(tw + S(16 + 22 + 30), S(110), S(220));
        total += widths[i];
    }
    if (total > avail && a->ntabs > 0) {
        float each = maxf(avail / (float)a->ntabs, S(72));
        for (int i = 0; i < a->ntabs; i++)
            widths[i] = minf(widths[i], each);
    }

    float x = p0.x + 1;
    int close_idx = -1;
    for (int i = 0; i < a->ntabs; i++) {
        Tab *tab = a->tabs[i];
        bool active = i == a->active;
        float tw = widths[i];
        ImVec2 t0 = v2(x, p0.y + 1), t1 = v2(x + tw, p1.y);
        igPushID_Int(tab->uid);
        igSetCursorScreenPos(t0);
        igSetNextItemAllowOverlap();
        igInvisibleButton("##tab", v2(tw, t1.y - t0.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        bool hov = igIsItemHovered(0);
        if (igIsItemActivated() && igIsMouseClicked_Bool(0, false)) {
            a->active = i;
            tab->focus_terminal = true;
            app_mark_dirty(a);
        }
        if (hov && igIsMouseClicked_Bool(2, false))
            close_idx = i;
        /* Drag to reorder */
        if (igIsItemActive() && igIsMouseDragging(0, S(4))) {
            float mx = igGetIO_Nil()->MousePos.x;
            if (i > 0 && mx < x - widths[i - 1] * 0.5f + S(10)) {
                Tab *tmp = a->tabs[i - 1];
                a->tabs[i - 1] = tab;
                a->tabs[i] = tmp;
                a->active = i - 1;
                app_mark_dirty(a);
            } else if (i + 1 < a->ntabs && mx > x + tw + widths[i + 1] * 0.5f - S(10)) {
                Tab *tmp = a->tabs[i + 1];
                a->tabs[i + 1] = tab;
                a->tabs[i] = tmp;
                a->active = i + 1;
                app_mark_dirty(a);
            }
        }

        float hk = ui_anim(igGetID_Str("hov"), hov ? 1.0f : 0.0f, 20.0f);
        if (active) {
            ImDrawFlags fl = i == 0 ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersNone;
            ImDrawList_AddRectFilled(dl, t0, v2(t1.x, t1.y + 1), th->tab_active_bg, r - 1, fl);
            ImDrawList_AddRectFilled(dl, v2(t0.x + (i == 0 ? S(5) : 0), t0.y), v2(t1.x, t0.y + S(2)), th->tab_top, 0, 0);
        } else if (hk > 0) {
            ImDrawFlags fl = i == 0 ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersNone;
            ImDrawList_AddRectFilled(dl, t0, t1, col_alpha(th->tab_hover_bg, hk), r - 1, fl);
        }
        /* separators */
        ImDrawList_AddLine(dl, v2(t1.x - 0.5f, t0.y + (active ? 0 : S(9))), v2(t1.x - 0.5f, t1.y - (active ? 0 : S(9))),
                           th->border, 1.0f);

        LinkState ls = session_state(tab->session);
        ui_link_icon(dl, v2(t0.x + S(16), (t0.y + t1.y) * 0.5f), ls, S(14));
        ImVec2 ts = ui_text_size(a->font_ui, fs, titles[i], NULL);
        ImU32 tc = active ? th->tab_active_fg : th->tab_inactive_fg;
        ui_text_ellipsis(dl, a->font_ui, fs, v2(t0.x + S(30), (t0.y + t1.y - ts.y) * 0.5f), tc, titles[i],
                         tw - S(30) - S(28));

        /* close button */
        {
            float cs = S(20);
            ImVec2 c0 = v2(t1.x - cs - S(6), (t0.y + t1.y - cs) * 0.5f);
            igSetCursorScreenPos(c0);
            if (igInvisibleButton("##close", v2(cs, cs), 0))
                close_idx = i;
            bool ch = igIsItemHovered(0);
            if (ch)
                ImDrawList_AddRectFilled(dl, c0, v2(c0.x + cs, c0.y + cs), th->toolbar_hover, S(4), 0);
            if (active || hov || ch)
                ui_draw_icon(dl, v2(c0.x + cs * 0.5f, c0.y + cs * 0.5f), ICON_CLOSE, ch ? th->fg : th->fg_dim, S(14));
            if (ch) {
                igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(8), S(5)));
                igSetTooltip("Close (Ctrl+Shift+W)");
                igPopStyleVar(1);
            }
        }
        if (hov && !igIsAnyItemActive() && tab->target.path[0]) {
            char status[256];
            session_status(tab->session, status, sizeof status, NULL);
            igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(8), S(5)));
            if (igBeginItemTooltip()) {
                igTextUnformatted(tab->target.name, NULL);
                igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
                igTextUnformatted(tab->target.path, NULL);
                if (status[0])
                    igTextUnformatted(status, NULL);
                igPopStyleColor(1);
                igEndTooltip();
            }
            igPopStyleVar(1);
        }
        igPopID();
        x += tw;
    }

    /* new tab */
    igSetCursorScreenPos(v2(x + S(4), p0.y + (h - S(26)) * 0.5f));
    if (ui_icon_button("##newtab", ICON_ADD, "New Tab (Ctrl+Shift+T)", false, S(26))) {
        app_new_tab(a, NULL);
        ui_open_quickpick(a);
    }

    /* actions */
    if (t) {
        float ax = p1.x - act_w + S(4);
        float ay = p0.y + (h - S(26)) * 0.5f;
        igSetCursorScreenPos(v2(ax, ay));
        if (ui_icon_button("##ts", ICON_WATCH, "Toggle Timestamps", t->timestamps, S(26))) {
            t->timestamps = !t->timestamps;
            app_mark_dirty(a);
        }
        igSetCursorScreenPos(v2(ax + S(28), ay));
        if (ui_icon_button("##find", ICON_SEARCH, "Find (Ctrl+Shift+F)", t->find_open, S(26))) {
            t->find_open = !t->find_open;
            t->find_focus = t->find_open;
        }
        igSetCursorScreenPos(v2(ax + S(56), ay));
        if (ui_icon_button("##clear", ICON_CLEAR_ALL, "Clear Terminal (Ctrl+Shift+K)", false, S(26)))
            termbuf_clear(&t->tb);
        igSetCursorScreenPos(v2(ax + S(84), ay));
        if (ui_icon_button("##more", ICON_ELLIPSIS, "More Actions", igIsPopupOpen_Str("##termmore", 0), S(26)))
            igOpenPopup_Str("##termmore", 0);
        igSetNextWindowPos(v2(p1.x - S(4), p1.y), ImGuiCond_Always, v2(1, 0));
        more_menu(a, t);
    }

    if (close_idx >= 0)
        app_close_tab(a, close_idx);
    return h;
}
