/* Status bar: link state, traffic counters and quick toggles. */
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

static float g_x;
static float g_y0, g_y1;

/* Draws a clickable status item at g_x (left-aligned) or ending at g_x (right). */
static bool item(const char *id, const char *icon, ImU32 icon_col, const char *text, const char *tip, bool right,
                 LinkState *spin)
{
    App *a = g_app;
    const Theme *th = &a->theme;
    float fs = S(12);
    ImVec2 ts = ui_text_size(a->font_ui, fs, text, NULL);
    float iw = (icon || spin) ? S(18) : 0;
    float w = ts.x + iw + S(16);
    float x = right ? g_x - w : g_x;
    ImVec2 p0 = v2(x, g_y0 + S(2)), p1 = v2(x + w, g_y1 - S(2));
    igSetCursorScreenPos(p0);
    bool clicked = igInvisibleButton(id, v2(w, p1.y - p0.y), 0);
    bool hov = igIsItemHovered(0);
    ImDrawList *dl = igGetWindowDrawList();
    if (hov)
        ImDrawList_AddRectFilled(dl, p0, p1, th->status_hover, S(4), 0);
    float cy = (g_y0 + g_y1) * 0.5f;
    if (spin)
        ui_link_icon(dl, v2(x + S(8) + S(7), cy), *spin, S(14));
    else if (icon)
        ui_draw_icon(dl, v2(x + S(8) + S(7), cy), icon, icon_col, S(14));
    ui_text(dl, a->font_ui, fs, v2(x + S(8) + iw, cy - ts.y * 0.5f), hov ? th->fg : th->fg_dim, text, NULL);
    ui_tooltip(tip);
    g_x = right ? x - S(2) : x + w + S(2);
    return clicked;
}

void ui_statusbar(App *a, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, p0, p1, th->bg, 0, 0);
    Tab *t = app_active_tab(a);
    if (!t)
        return;
    g_y0 = p0.y;
    g_y1 = p1.y;
    g_x = p0.x + S(8);

    LinkState st = session_state(t->session);
    char text[256], status[256];
    Severity sev;
    session_status(t->session, status, sizeof status, &sev);
    char path[128];
    session_current_path(t->session, path, sizeof path);
    switch (st) {
    case LINK_CONNECTED:
        snprintf(text, sizeof text, "Connected \xC2\xB7 %s", path);
        break;
    case LINK_CONNECTING:
        snprintf(text, sizeof text, "Connecting\xE2\x80\xA6");
        break;
    case LINK_RECONNECTING: {
        double age = session_state_age(t->session);
        snprintf(text, sizeof text, "Reconnecting \xC2\xB7 %.0fs", age);
        break;
    }
    default:
        snprintf(text, sizeof text, t->target.path[0] ? "Disconnected" : "No port");
        break;
    }
    if (item("##sb_state", NULL, 0, text, status[0] ? status : NULL, false, &st)) {
        if (st == LINK_IDLE)
            app_tab_connect(a, t);
        else
            app_tab_disconnect(a, t);
    }

    char rx[32], tx[32], traffic[80];
    ui_format_bytes(session_rx_bytes(t->session), rx, sizeof rx);
    ui_format_bytes(session_tx_bytes(t->session), tx, sizeof tx);
    snprintf(traffic, sizeof traffic, "\xE2\x86\x93 %s   \xE2\x86\x91 %s", rx, tx);
    item("##sb_traffic", NULL, 0, traffic, "Received / sent bytes this session", false, NULL);

    /* right side */
    g_x = p1.x - S(8);
    static const char *eols[] = {"None", "LF", "CR", "CRLF"};
    if (item("##sb_eol", NULL, 0, eols[t->tx_eol & 3], "Line ending for sent lines (click to cycle)", true, NULL)) {
        t->tx_eol = (TxEol)((t->tx_eol + 1) & 3);
        app_mark_dirty(a);
    }
    if (log_is_open(&t->slog)) {
        char tip[1500];
        snprintf(tip, sizeof tip, "%s %s\nClick to open the log folder.",
                 t->slog.pending ? "Waiting to log to" : "Logging to", t->slog.path);
        if (item("##sb_log", ICON_RECORD, t->slog.pending ? th->warn : th->error, "Log", tip, true, NULL))
            app_open_log_folder(a);
    }
    char size[32];
    snprintf(size, sizeof size, "%d\xC3\x97%d", t->tb.cols, t->tb.rows);
    if (item("##sb_size", NULL, 0, size,
             st == LINK_CONNECTED ? "Terminal size (columns \xC3\x97 rows). Click to send it to the device with stty."
                                  : "Terminal size (columns \xC3\x97 rows)",
             true, NULL) &&
        st == LINK_CONNECTED)
        app_tab_send_size(a, t);
    char cfg[48];
    serial_config_describe(&t->cfg, cfg, sizeof cfg);
    if (item("##sb_cfg", NULL, 0, cfg, "Connection settings", true, NULL)) {
        a->sidebar_visible = true;
        a->sect_conn_open = true;
    }
    if (item("##sb_auto", t->auto_reconnect ? ICON_SYNC : ICON_SYNC_IGNORED, t->auto_reconnect ? th->ok : th->fg_disabled,
             t->auto_reconnect ? "Auto-reconnect" : "No auto-reconnect", "Click to toggle auto-reconnect", true, NULL)) {
        t->auto_reconnect = !t->auto_reconnect;
        app_tab_apply_config(a, t);
    }
    if (st == LINK_RECONNECTING)
        a->animating = true; /* keep the counter ticking */
}
