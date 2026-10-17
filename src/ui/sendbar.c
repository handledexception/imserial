/* Line-mode input below the terminal: history (Up/Down) and send button.
 * Typing directly in the terminal sends raw keystrokes instead. */
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static int history_cb(ImGuiInputTextCallbackData *d)
{
    Tab *t = d->UserData;
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || !t->history_count)
        return 0;
    int prev = t->history_pos;
    if (d->EventKey == ImGuiKey_UpArrow) {
        if (t->history_pos == -1)
            t->history_pos = t->history_count - 1;
        else if (t->history_pos > 0)
            t->history_pos--;
    } else if (d->EventKey == ImGuiKey_DownArrow) {
        if (t->history_pos != -1 && ++t->history_pos >= t->history_count)
            t->history_pos = -1;
    }
    if (prev != t->history_pos) {
        const char *s = t->history_pos >= 0 ? t->history[t->history_pos] : "";
        ImGuiInputTextCallbackData_DeleteChars(d, 0, d->BufTextLen);
        ImGuiInputTextCallbackData_InsertChars(d, 0, s, NULL);
    }
    return 0;
}

void ui_sendbar(App *a, Tab *t, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    LinkState st = session_state(t->session);
    bool connected = st == LINK_CONNECTED;
    ImGuiID input_id;

    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "##send%d", t->uid);
    input_id = igGetID_Str(idbuf);
    bool active = igGetActiveID() == input_id;
    float k = ui_anim(input_id + 1, active ? 1.0f : 0.0f, 16.0f);
    ui_card(dl, p0, p1, th->editor_bg, col_lerp(th->border, th->focus | 0xFF000000u, k), S(8), 0);

    float h = p1.y - p0.y;
    ui_draw_icon(dl, v2(p0.x + S(18), p0.y + h * 0.5f), ICON_CHEVRON_RIGHT, active ? th->tab_top : th->fg_dim, S(16));

    float right_w = S(34);
    float fh = igGetFrameHeight();
    igSetCursorScreenPos(v2(p0.x + S(30), p0.y + (h - fh) * 0.5f));
    igSetNextItemWidth(p1.x - p0.x - S(30) - right_w - S(8));
    igPushStyleColor_U32(ImGuiCol_FrameBg, 0);
    igPushStyleColor_U32(ImGuiCol_FrameBgHovered, 0);
    igPushStyleColor_U32(ImGuiCol_FrameBgActive, 0);
    igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, 0);
    igPushFont(a->font_mono, a->mono_size);
    char hint[192];
    char title[64];
    app_tab_title(t, title, sizeof title);
    if (connected)
        snprintf(hint, sizeof hint, "Send to %s  (Enter to send, \xE2\x86\x91\xE2\x86\x93 history)", title);
    else if (st == LINK_IDLE)
        snprintf(hint, sizeof hint, "Not connected");
    else
        snprintf(hint, sizeof hint, "Waiting for %s\xE2\x80\xA6", title);
    if (t->focus_input) {
        igSetKeyboardFocusHere(0);
        t->focus_input = false;
    }
    bool submit = igInputTextWithHint(idbuf, hint, t->input, sizeof t->input,
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                      history_cb, t);
    igPopFont();
    igPopStyleVar(1);
    igPopStyleColor(3);
    if (igIsItemActive() && igIsKeyPressed_Bool(ImGuiKey_Escape, false))
        t->focus_terminal = true;

    /* Send button */
    float bs = S(28);
    ImVec2 b0 = v2(p1.x - bs - S(9), p0.y + (h - bs) * 0.5f);
    igSetCursorScreenPos(b0);
    bool can_send = connected;
    bool clicked = igInvisibleButton("##sendbtn", v2(bs, bs), 0);
    bool bh = igIsItemHovered(0);
    ui_tooltip("Send (Enter)");
    ImU32 bbg = can_send && t->input[0] ? (bh ? th->accent_hover : th->accent) : (bh ? th->list_hover : 0);
    ImDrawList_AddRectFilled(dl, b0, v2(b0.x + bs, b0.y + bs), bbg, S(6), 0);
    ui_draw_icon(dl, v2(b0.x + bs * 0.5f, b0.y + bs * 0.5f), ICON_SEND,
                 can_send && t->input[0] ? th->accent_fg : th->fg_dim, S(16));

    if ((submit || clicked) && can_send) {
        app_tab_send(a, t, t->input, strlen(t->input), true);
        t->input[0] = 0;
        t->history_pos = -1;
        t->focus_input = true;
    } else if (submit) {
        t->focus_input = true;
    }
}
