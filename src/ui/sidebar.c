/* Sidebar card: port list, connection settings and terminal options for the
 * active tab, with the connect button pinned to the bottom. */
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k_bauds[] = {300,    1200,   2400,   4800,   9600,    19200,   38400,   57600,  74880,
                                   115200, 230400, 250000, 460800, 500000, 921600, 1000000, 1500000, 2000000,
                                   3000000, 4000000};

static void section_title(const char *label)
{
    ImVec2 p = igGetCursorScreenPos();
    float fs = S(11);
    ui_text(igGetWindowDrawList(), g_app->font_ui_bold, fs, v2(p.x + S(8), p.y + S(4)), g_app->theme.fg_dim, label, NULL);
    igDummy(v2(1, S(22)));
}

/* Is any tab using this port? Returns that tab's link state. */
static bool port_in_use(App *a, const SerialPortInfo *p, LinkState *st)
{
    for (int i = 0; i < a->ntabs; i++) {
        Tab *t = a->tabs[i];
        if (strcmp(t->target.id, p->id) == 0) {
            LinkState s = session_state(t->session);
            if (s != LINK_IDLE) {
                *st = s;
                return true;
            }
        }
    }
    return false;
}

static void port_row(App *a, Tab *t, const SerialPortInfo *p, bool present)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    ImVec2 pos = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x, h = S(42);
    igPushID_Str(p->id);
    bool clicked = igInvisibleButton("##port", v2(w, h), 0);
    bool hov = igIsItemHovered(0);
    bool selected = t && strcmp(t->target.id, p->id) == 0;
    LinkState tab_state = t ? session_state(t->session) : LINK_IDLE;
    bool live = tab_state != LINK_IDLE;

    if (clicked && t && !live)
        app_tab_set_port(a, t, p);
    if (hov && igIsMouseDoubleClicked_Nil(0) && present) {
        if (t && (!live || selected)) {
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
    }
    if (igBeginPopupContextItem("##portctx", ImGuiPopupFlags_MouseButtonRight)) {
        ui_menu_push();
        if (igMenuItem_Bool("Connect in This Tab", NULL, false, present && t)) {
            app_tab_set_port(a, t, p);
            app_tab_connect(a, t);
        }
        if (igMenuItem_Bool("Open in New Tab", NULL, false, present)) {
            PortTarget pt = {0};
            snprintf(pt.id, sizeof pt.id, "%s", p->id);
            snprintf(pt.path, sizeof pt.path, "%s", p->path);
            snprintf(pt.name, sizeof pt.name, "%s", p->name);
            Tab *nt = app_new_tab(a, &pt);
            if (nt)
                app_tab_connect(a, nt);
        }
        igSeparator();
        if (igMenuItem_Bool("Copy Device Path", NULL, false, true))
            igSetClipboardText(p->path);
        ui_menu_pop();
        igEndPopup();
    }

    float k = ui_anim(igGetItemID(), hov ? 1.0f : 0.0f, 20.0f);
    if (selected)
        ImDrawList_AddRectFilled(dl, pos, v2(pos.x + w, pos.y + h), th->list_active, S(5), 0);
    else if (k > 0)
        ImDrawList_AddRectFilled(dl, pos, v2(pos.x + w, pos.y + h), col_alpha(th->list_hover, k), S(5), 0);

    LinkState st = LINK_IDLE;
    ImVec2 ic = v2(pos.x + S(16), pos.y + h * 0.5f);
    if (port_in_use(a, p, &st))
        ui_link_icon(dl, ic, st, S(16));
    else
        ui_draw_icon(dl, ic, p->usb ? ICON_PLUG : ICON_SERVER, present ? th->icon : th->fg_disabled, S(16));

    float fs = a->font_size * a->ui_scale;
    float tx = pos.x + S(32), maxw = w - S(38);
    ImU32 name_col = present ? (selected ? th->list_active_fg : th->fg) : th->fg_disabled;
    ui_text_ellipsis(dl, selected ? a->font_ui_bold : a->font_ui, fs, v2(tx, pos.y + S(5)), name_col, p->name, maxw);
    char detail[160];
    if (present)
        snprintf(detail, sizeof detail, "%s", p->detail);
    else
        snprintf(detail, sizeof detail, "%s \xC2\xB7 not present", p->path);
    ui_text_ellipsis(dl, a->font_ui, S(11.5f), v2(tx, pos.y + S(23)), present ? th->fg_dim : th->fg_disabled, detail,
                     maxw);
    if (hov && !igIsPopupOpen_Str("##portctx", 0)) {
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(8), S(5)));
        if (igBeginItemTooltip()) {
            igTextUnformatted(p->name, NULL);
            igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
            igText("%s", p->path);
            if (p->manufacturer[0])
                igText("%s", p->manufacturer);
            if (p->serial[0])
                igText("Serial: %s", p->serial);
            if (live && !selected)
                igTextUnformatted("Double-click to open in a new tab", NULL);
            else
                igTextUnformatted("Double-click to connect", NULL);
            igPopStyleColor(1);
            igEndTooltip();
        }
        igPopStyleVar(1);
    }
    igPopID();
}

static bool baud_input(Tab *t)
{
    /* Editable while focused; otherwise mirrors the tab's current baud rate. */
    static char buf[32];
    bool changed = false;
    float avail = igGetContentRegionAvail().x;
    float bw = igGetFrameHeight();
    igSetNextItemWidth(avail);
    if (igGetActiveID() != igGetID_Str("##baud"))
        snprintf(buf, sizeof buf, "%u", (unsigned)t->cfg.baud);
    igSetNextItemAllowOverlap();
    igInputText("##baud", buf, sizeof buf, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll, NULL,
                NULL);
    if (igIsItemDeactivatedAfterEdit()) {
        unsigned long v = strtoul(buf, NULL, 10);
        if (v >= 50 && v <= 20000000 && v != t->cfg.baud) {
            t->cfg.baud = (uint32_t)v;
            changed = true;
        }
    }
    ImVec2 imax = igGetItemRectMax(), imin = igGetItemRectMin();
    ImVec2 bp = v2(imax.x - bw, imin.y);
    igSetCursorScreenPos(bp);
    if (igInvisibleButton("##baudmenu", v2(bw, bw), 0))
        igOpenPopup_Str("##baudlist", 0);
    bool hov = igIsItemHovered(0);
    const Theme *th = &g_app->theme;
    ImDrawList *dl = igGetWindowDrawList();
    if (hov)
        ImDrawList_AddRectFilled(dl, v2(bp.x + S(3), bp.y + S(3)), v2(bp.x + bw - S(3), bp.y + bw - S(3)), th->toolbar_hover,
                                 S(4), 0);
    ui_draw_icon(dl, v2(bp.x + bw * 0.5f - S(1), bp.y + bw * 0.5f), ICON_CHEVRON_DOWN, hov ? th->fg : th->fg_dim, S(14));
    ui_tooltip("Common baud rates");
    igSetNextWindowPos(v2(bp.x + bw, bp.y + bw + S(2)), ImGuiCond_Always, v2(1, 0));
    igSetNextWindowSizeConstraints(v2(avail, 0), v2(avail, S(320)), NULL, NULL);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(4), S(4)));
    if (igBeginPopup("##baudlist", 0)) {
        igPushStyleColor_U32(ImGuiCol_HeaderHovered, th->menu_sel);
        for (size_t i = 0; i < sizeof k_bauds / sizeof k_bauds[0]; i++) {
            char label[32];
            snprintf(label, sizeof label, "%u", (unsigned)k_bauds[i]);
            if (igSelectable_Bool(label, t->cfg.baud == k_bauds[i], 0, v2(0, 0)) && t->cfg.baud != k_bauds[i]) {
                t->cfg.baud = k_bauds[i];
                snprintf(buf, sizeof buf, "%s", label);
                changed = true;
            }
        }
        igPopStyleColor(1);
        igEndPopup();
    }
    igPopStyleVar(1);
    return changed;
}

static void switch_row(const char *label, const char *tooltip, bool *v, bool *changed)
{
    float w = igGetContentRegionAvail().x;
    ImVec2 p = igGetCursorScreenPos();
    float fs = g_app->font_size * g_app->ui_scale;
    float h = S(26);
    igBeginGroup();
    ImVec2 ts = ui_text_size(g_app->font_ui, fs, label, NULL);
    ui_text(igGetWindowDrawList(), g_app->font_ui, fs, v2(p.x, p.y + (h - ts.y) * 0.5f), g_app->theme.fg, label, NULL);
    igSetCursorScreenPos(v2(p.x + w - S(32), p.y + (h - S(18)) * 0.5f));
    if (ui_toggle(label, v))
        *changed = true;
    igSetCursorScreenPos(v2(p.x, p.y + h));
    igDummy(v2(1, 0));
    igEndGroup();
    ui_tooltip(tooltip);
}

void ui_sidebar(App *a, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    ui_card(dl, p0, p1, th->dark ? COL32(0x16, 0x17, 0x18, 255) : th->editor_bg, th->border, S(8), 0);
    Tab *t = app_active_tab(a);

    float btn_h = S(32), pad = S(10);
    ImVec2 c0 = v2(p0.x + 1, p0.y + 1), c1 = v2(p1.x - 1, p1.y - btn_h - pad * 2);
    igSetCursorScreenPos(c0);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(8), S(8)));
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(S(6), S(6)));
    igPushStyleVar_Float(ImGuiStyleVar_ScrollbarSize, S(8));
    igBeginChild_Str("##sidebar", v2(c1.x - c0.x, c1.y - c0.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                     ImGuiWindowFlags_NoBackground);

    /* PORTS header with refresh */
    ImVec2 hp = igGetCursorScreenPos();
    float hw = igGetContentRegionAvail().x;
    section_title("PORTS");
    igSetCursorScreenPos(v2(hp.x + hw - S(22), hp.y));
    if (ui_icon_button("##refresh", ICON_REFRESH, "Rescan Ports", false, S(22)))
        app_scan_ports(a, true);
    igSetCursorScreenPos(v2(hp.x, hp.y + S(26)));

    /* The active tab's port, if it's currently unplugged */
    if (t && t->target.path[0] && !app_find_port(a, &t->target)) {
        SerialPortInfo ghost = {0};
        snprintf(ghost.id, sizeof ghost.id, "%s", t->target.id);
        snprintf(ghost.path, sizeof ghost.path, "%s", t->target.path);
        snprintf(ghost.name, sizeof ghost.name, "%.95s", t->target.name[0] ? t->target.name : t->target.path);
        ghost.usb = strncmp(t->target.id, "usb:", 4) == 0;
        port_row(a, t, &ghost, false);
    }
    for (int i = 0; i < a->nports; i++)
        port_row(a, t, &a->ports[i], true);
    if (a->nports == 0) {
        igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
        igSetCursorPosX(igGetCursorPosX() + S(8));
        igTextUnformatted("No serial ports found.", NULL);
        igPopStyleColor(1);
    }
    igDummy(v2(1, S(6)));

    if (t) {
        bool changed = false;
        float lw = S(92);
        if (ui_section_header("CONNECTION", &a->sect_conn_open)) {
            igIndent(S(8));
            /* Each row is a group so the tooltip shows over the label as
             * well as the control. */
            igBeginGroup();
            ui_form_label("Baud rate", lw);
            changed |= baud_input(t);
            igEndGroup();
            ui_tooltip("Bits per second on the wire. Must match the device.\n"
                       "115200 is usual for modern boards, 9600 for older ones.");

            static const char *bits[] = {"5", "6", "7", "8"};
            int b = t->cfg.data_bits - 5;
            igBeginGroup();
            ui_form_label("Data bits", lw);
            if (ui_segmented("##bits", bits, 4, &b, igGetContentRegionAvail().x)) {
                t->cfg.data_bits = (uint8_t)(b + 5);
                changed = true;
            }
            igEndGroup();
            ui_tooltip("Data bits per character. Almost everything uses 8.\n"
                       "7 is seen on older equipment that also uses parity.");

            static const char *par[] = {"None", "Odd", "Even", "Mark", "Space"};
            int pv = (int)t->cfg.parity;
            igBeginGroup();
            ui_form_label("Parity", lw);
            if (ui_combo("##parity", &pv, par, 5)) {
                t->cfg.parity = (SerialParity)pv;
                changed = true;
            }
            igEndGroup();
            ui_tooltip("Error-check bit added to each character.\n"
                       "None for nearly all devices (the N in 8N1).\n"
                       "Mark and Space always send a 1 or a 0 instead of a check.");

            static const char *stops[] = {"1", "2"};
            int sv = t->cfg.stop_bits == 2 ? 1 : 0;
            igBeginGroup();
            ui_form_label("Stop bits", lw);
            if (ui_segmented("##stop", stops, 2, &sv, igGetContentRegionAvail().x)) {
                t->cfg.stop_bits = (uint8_t)(sv + 1);
                changed = true;
            }
            igEndGroup();
            ui_tooltip("Idle time after each character. 1 is standard.\n"
                       "2 gives slow or old receivers extra time.");

            static const char *flow[] = {"None", "RTS/CTS", "XON/XOFF"};
            int fv = (int)t->cfg.flow;
            igBeginGroup();
            ui_form_label("Flow control", lw);
            if (ui_combo("##flow", &fv, flow, 3)) {
                t->cfg.flow = (SerialFlow)fv;
                changed = true;
            }
            igEndGroup();
            ui_tooltip("How each side asks the other to pause.\n"
                       "None: no pausing. Right for most boards.\n"
                       "RTS/CTS: hardware lines. The device must drive CTS or sending stalls.\n"
                       "XON/XOFF: control bytes in the data stream. Not safe for binary data.");

            igDummy(v2(1, S(2)));
            switch_row("DTR",
                       "Data Terminal Ready output line.\n"
                       "Many boards wire it to reset: an Arduino restarts when it toggles,\n"
                       "and ESP32 flashers pulse it together with RTS.\n"
                       "Turn it off to attach without resetting the board.",
                       &t->cfg.dtr, &changed);
            switch_row("RTS",
                       "Request To Send output line, set by hand when flow control is not RTS/CTS.\n"
                       "ESP32 and ESP8266 boards wire it to EN or GPIO0;\n"
                       "the wrong state can hold the chip in reset or boot mode.",
                       &t->cfg.rts, &changed);
            switch_row("Auto-reconnect",
                       "Follow this device by its USB identity and reopen it when it\n"
                       "comes back, even if it appears under a different port name.",
                       &t->auto_reconnect, &changed);
            igUnindent(S(8));
            igDummy(v2(1, S(4)));
        }
        if (ui_section_header("TERMINAL", &a->sect_term_open)) {
            igIndent(S(8));
            static const char *eol[] = {"None", "LF", "CR", "CRLF"};
            int ev = (int)t->tx_eol;
            igBeginGroup();
            ui_form_label("Send ending", lw);
            if (ui_segmented("##eol", eol, 4, &ev, igGetContentRegionAvail().x)) {
                t->tx_eol = (TxEol)ev;
                changed = true;
            }
            igEndGroup();
            ui_tooltip("Line ending added when you press Enter in the send bar.\n"
                       "CR is what most shells and bootloaders expect. LF for Unix-style devices.\n"
                       "None sends the text as is. Keys typed directly into the terminal are not affected.");
            igDummy(v2(1, S(2)));
            bool cr_nl = t->nl_mode == TB_NL_ANY;
            bool nl_changed = false;
            switch_row("Timestamps",
                       "Show the time each line arrived, in a gutter on the left,\n"
                       "with millisecond precision. Hidden on the alternate screen.", &t->timestamps, &changed);
            switch_row("Local echo",
                       "Show what you send in the terminal as well. Turn it on when\n"
                       "the device does not echo your typing back and you cannot see it.", &t->local_echo, &changed);
            switch_row("Treat CR as new line",
                       "Off: LF starts a new line and a bare CR returns to the start of the\n"
                       "line so progress bars redraw in place, like a real terminal.\n"
                       "On: CR, LF and CRLF each end a line, for devices that only send CR.", &cr_nl, &nl_changed);
            if (nl_changed) {
                t->nl_mode = cr_nl ? TB_NL_ANY : TB_NL_TERMINAL;
                changed = true;
            }
            switch_row("Batch screen redraws",
                       "When a program clears the screen and redraws it, wait for the whole\n"
                       "page to arrive and show it in one frame instead of line by line.",
                       &t->batch_redraws, &changed);
            igUnindent(S(8));
        }
        if (changed)
            app_tab_apply_config(a, t);
    }
    igEndChild();
    igPopStyleVar(3);

    /* Connect / disconnect */
    if (t) {
        LinkState st = session_state(t->session);
        ImVec2 bpos = v2(p0.x + pad, p1.y - pad - btn_h);
        ImDrawList_AddLine(dl, v2(p0.x + 1, bpos.y - pad), v2(p1.x - 1, bpos.y - pad), th->border, 1.0f);
        igSetCursorScreenPos(bpos);
        ImVec2 bsize = v2(p1.x - p0.x - pad * 2, btn_h);
        if (st == LINK_IDLE) {
            igBeginDisabled(!t->target.path[0]);
            if (ui_button("Connect", ICON_PLUG, bsize, true))
                app_tab_connect(a, t);
            igEndDisabled();
        } else if (st == LINK_RECONNECTING) {
            if (ui_button("Stop Reconnecting", ICON_DEBUG_STOP, bsize, false))
                app_tab_disconnect(a, t);
        } else {
            if (ui_button("Disconnect", ICON_DEBUG_DISCONNECT, bsize, false))
                app_tab_disconnect(a, t);
        }
    }
}
