/* VS Code-style quick pick: filterable list of ports plus a few commands,
 * opened from the command center or Ctrl+P. */
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

typedef enum { QP_PORT, QP_NEW_TAB, QP_DISCONNECT, QP_THEME, QP_SIDEBAR, QP_CLEAR } QpKind;

typedef struct {
    QpKind kind;
    int port;
    const char *icon;
    char label[160];
    char detail[160];
} QpItem;

void ui_open_quickpick(App *a)
{
    a->quickpick_open = true;
    a->quickpick_focus = true;
    a->quickpick_text[0] = 0;
    a->quickpick_sel = 0;
}

static bool matches(const char *hay, const char *needle)
{
    if (!*needle)
        return true;
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < n && h[k]) {
            char x = h[k], y = needle[k];
            if (x >= 'A' && x <= 'Z')
                x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z')
                y = (char)(y + 32);
            if (x != y)
                break;
            k++;
        }
        if (k == n)
            return true;
    }
    return false;
}

static void activate(App *a, const QpItem *it)
{
    Tab *t = app_active_tab(a);
    switch (it->kind) {
    case QP_PORT: {
        const SerialPortInfo *p = &a->ports[it->port];
        LinkState st = t ? session_state(t->session) : LINK_IDLE;
        if (t && (st == LINK_IDLE || strcmp(t->target.id, p->id) == 0)) {
            app_tab_set_port(a, t, p);
            app_tab_connect(a, t);
        } else {
            PortTarget pt = {0};
            snprintf(pt.id, sizeof pt.id, "%s", p->id);
            snprintf(pt.path, sizeof pt.path, "%s", p->path);
            snprintf(pt.name, sizeof pt.name, "%s", p->name);
            Tab *nt = app_new_tab(a, &pt);
            if (nt)
                app_tab_connect(a, nt);
        }
        break;
    }
    case QP_NEW_TAB:
        app_new_tab(a, NULL);
        break;
    case QP_DISCONNECT:
        if (t)
            app_tab_disconnect(a, t);
        break;
    case QP_THEME:
        app_set_theme(a, !a->dark);
        break;
    case QP_SIDEBAR:
        a->sidebar_visible = !a->sidebar_visible;
        app_mark_dirty(a);
        break;
    case QP_CLEAR:
        if (t)
            termbuf_clear(&t->tb);
        break;
    }
}

void ui_quickpick(App *a)
{
    const Theme *th = &a->theme;
    /* One-shot: after this, ImGui owns the popup's lifetime, so clicking
     * outside or pressing Escape closes it for good. */
    if (a->quickpick_open) {
        igOpenPopup_Str("##quickpick", 0);
        a->quickpick_open = false;
    }

    ImGuiViewport *vp = igGetMainViewport();
    float w = minf(S(600), vp->Size.x - S(40));
    igSetNextWindowPos(v2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + a->titlebar_h - S(4)), ImGuiCond_Always,
                       v2(0, 0));
    igSetNextWindowSize(v2(w, 0), ImGuiCond_Always);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(6), S(6)));
    igPushStyleVar_Float(ImGuiStyleVar_PopupRounding, S(8));
    igPushStyleColor_U32(ImGuiCol_PopupBg, th->dark ? COL32(0x20, 0x21, 0x22, 255) : th->menu_bg);
    if (!igBeginPopup("##quickpick", ImGuiWindowFlags_NoMove)) {
        igPopStyleColor(1);
        igPopStyleVar(2);
        return;
    }

    QpItem items[MAX_PORTS + 8];
    int n = 0;
    Tab *t = app_active_tab(a);
    for (int i = 0; i < a->nports; i++) {
        const SerialPortInfo *p = &a->ports[i];
        char hay[300];
        snprintf(hay, sizeof hay, "%s %s %s", p->name, p->path, p->detail);
        if (!matches(hay, a->quickpick_text))
            continue;
        QpItem *it = &items[n++];
        it->kind = QP_PORT;
        it->port = i;
        it->icon = ICON_PLUG;
        snprintf(it->label, sizeof it->label, "%s", p->name);
        snprintf(it->detail, sizeof it->detail, "%s", p->detail);
    }
    struct {
        QpKind k;
        const char *icon, *label, *detail;
    } cmds[] = {
        {QP_NEW_TAB, ICON_ADD, "New Tab", "Ctrl+Shift+T"},
        {QP_DISCONNECT, ICON_DEBUG_DISCONNECT, "Disconnect", "Shift+F5"},
        {QP_CLEAR, ICON_CLEAR_ALL, "Clear Terminal", "Ctrl+Shift+K"},
        {QP_SIDEBAR, ICON_LAYOUT_SIDEBAR_LEFT, "Toggle Sidebar", "Ctrl+Shift+B"},
        {QP_THEME, ICON_COLOR_MODE, a->dark ? "Switch to Light Theme" : "Switch to Dark Theme", ""},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (cmds[i].k == QP_DISCONNECT && (!t || session_state(t->session) == LINK_IDLE))
            continue;
        if (!matches(cmds[i].label, a->quickpick_text))
            continue;
        QpItem *it = &items[n++];
        it->kind = cmds[i].k;
        it->icon = cmds[i].icon;
        snprintf(it->label, sizeof it->label, "%s", cmds[i].label);
        snprintf(it->detail, sizeof it->detail, "%s", cmds[i].detail);
    }
    if (a->quickpick_sel >= n)
        a->quickpick_sel = n ? n - 1 : 0;

    if (a->quickpick_focus) {
        igSetKeyboardFocusHere(0);
        a->quickpick_focus = false;
    }
    igSetNextItemWidth(-1);
    igPushStyleColor_U32(ImGuiCol_FrameBg, th->input_bg);
    igPushStyleColor_U32(ImGuiCol_Border, th->focus);
    if (igInputTextWithHint("##qp", "Select a serial port, or type to filter commands", a->quickpick_text,
                            sizeof a->quickpick_text, 0, NULL, NULL))
        a->quickpick_sel = 0;
    igPopStyleColor(2);
    if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true))
        a->quickpick_sel = n ? (a->quickpick_sel + 1) % n : 0;
    if (igIsKeyPressed_Bool(ImGuiKey_UpArrow, true))
        a->quickpick_sel = n ? (a->quickpick_sel + n - 1) % n : 0;
    bool enter = igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false);
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        igCloseCurrentPopup();
    }
    igDummy(v2(1, S(2)));

    ImDrawList *dl = igGetWindowDrawList();
    float fs = a->font_size * a->ui_scale;
    int clicked = -1;
    bool shown_cmd_sep = false;
    for (int i = 0; i < n; i++) {
        const QpItem *it = &items[i];
        if (it->kind != QP_PORT && !shown_cmd_sep) {
            shown_cmd_sep = true;
            if (i > 0) {
                ImVec2 sp = igGetCursorScreenPos();
                ImDrawList_AddLine(dl, v2(sp.x, sp.y + S(3)), v2(sp.x + igGetContentRegionAvail().x, sp.y + S(3)),
                                   th->border, 1.0f);
                igDummy(v2(1, S(6)));
            }
        }
        ImVec2 p = igGetCursorScreenPos();
        float rw = igGetContentRegionAvail().x, rh = S(28);
        igPushID_Int(i);
        if (igInvisibleButton("##qpi", v2(rw, rh), 0))
            clicked = i;
        if (igIsItemHovered(0) && igGetIO_Nil()->MouseDelta.x + igGetIO_Nil()->MouseDelta.y != 0)
            a->quickpick_sel = i;
        igPopID();
        bool sel = i == a->quickpick_sel;
        if (sel)
            ImDrawList_AddRectFilled(dl, p, v2(p.x + rw, p.y + rh), th->menu_sel, S(5), 0);
        bool in_use = false;
        if (it->kind == QP_PORT && t && strcmp(t->target.id, a->ports[it->port].id) == 0)
            in_use = true;
        ui_draw_icon(dl, v2(p.x + S(14), p.y + rh * 0.5f), it->icon, in_use ? th->ok : th->icon, S(16));
        ImVec2 ls = ui_text_size(a->font_ui, fs, it->label, NULL);
        ui_text(dl, a->font_ui, fs, v2(p.x + S(30), p.y + (rh - ls.y) * 0.5f), th->fg, it->label, NULL);
        if (it->detail[0]) {
            float ds = S(12);
            ImVec2 dsz = ui_text_size(a->font_ui, ds, it->detail, NULL);
            float dx = it->kind == QP_PORT ? p.x + S(30) + ls.x + S(10) : p.x + rw - dsz.x - S(10);
            ui_text(dl, a->font_ui, ds, v2(dx, p.y + (rh - dsz.y) * 0.5f), th->fg_dim, it->detail, NULL);
        }
    }
    if (n == 0) {
        igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
        igSetCursorPosX(igGetCursorPosX() + S(8));
        igTextUnformatted("No matching ports or commands", NULL);
        igPopStyleColor(1);
    }
    if (enter && n)
        clicked = a->quickpick_sel;
    if (clicked >= 0) {
        QpItem it = items[clicked];
        igCloseCurrentPopup();
        activate(a, &it);
    }
    igEndPopup();
    igPopStyleColor(1);
    igPopStyleVar(2);
}
