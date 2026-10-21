#include "app/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_SCAN_MS 1500

Session *app_create_session(App *a);

Tab *app_new_tab(App *a, const PortTarget *target)
{
    if (a->ntabs >= MAX_TABS)
        return NULL;
    Tab *t = calloc(1, sizeof *t);
    t->uid = ++a->next_uid;
    t->cfg = serial_config_default();
    t->auto_reconnect = true;
    t->tx_eol = EOL_LF;
    t->nl_mode = TB_NL_TERMINAL;
    t->follow = true;
    t->history_pos = -1;
    t->scroll_to_line = -1;
    termbuf_init(&t->tb, (uint32_t)(a->scrollback_lines ? a->scrollback_lines : SCROLLBACK_DEFAULT));
    if (target)
        t->target = *target;
    t->batch_redraws = true;
    /* New tabs inherit the active tab's settings so "+" feels like "duplicate". */
    Tab *cur = app_active_tab(a);
    if (cur) {
        t->cfg = cur->cfg;
        t->auto_reconnect = cur->auto_reconnect;
        t->timestamps = cur->timestamps;
        t->local_echo = cur->local_echo;
        t->batch_redraws = cur->batch_redraws;
        t->nl_mode = cur->nl_mode;
        t->tx_eol = cur->tx_eol;
    }
    t->tb.nl_mode = t->nl_mode;
    if (a->window)
        t->session = app_create_session(a);
    a->tabs[a->ntabs++] = t;
    a->active = a->ntabs - 1;
    t->focus_terminal = true;
    app_mark_dirty(a);
    return t;
}

void app_close_tab(App *a, int index)
{
    if (index < 0 || index >= a->ntabs)
        return;
    Tab *t = a->tabs[index];
    log_close(&t->slog);
    session_destroy(t->session);
    termbuf_free(&t->tb);
    session_buf_free(&t->drain);
    for (int i = 0; i < t->history_count; i++)
        free(t->history[i]);
    free(t);
    memmove(&a->tabs[index], &a->tabs[index + 1], sizeof(Tab *) * (size_t)(a->ntabs - index - 1));
    a->ntabs--;
    if (a->active >= a->ntabs)
        a->active = a->ntabs - 1;
    else if (a->active > index)
        a->active--;
    if (a->ntabs == 0)
        app_new_tab(a, NULL);
    app_mark_dirty(a);
}

void app_tab_connect(App *a, Tab *t)
{
    if (!t->target.path[0])
        return;
    t->want_connected = true;
    session_open(t->session, &t->target, &t->cfg, t->auto_reconnect);
    t->focus_terminal = true;
    app_mark_dirty(a);
}

void app_tab_disconnect(App *a, Tab *t)
{
    t->want_connected = false;
    session_close(t->session);
    app_mark_dirty(a);
}

void app_tab_set_port(App *a, Tab *t, const SerialPortInfo *p)
{
    bool same = strcmp(t->target.id, p->id) == 0;
    snprintf(t->target.id, sizeof t->target.id, "%s", p->id);
    snprintf(t->target.path, sizeof t->target.path, "%s", p->path);
    snprintf(t->target.name, sizeof t->target.name, "%s", p->name);
    if (!same && session_state(t->session) != LINK_IDLE)
        app_tab_connect(a, t); /* switch the live connection to the new port */
    app_mark_dirty(a);
}

void app_tab_apply_config(App *a, Tab *t)
{
    session_set_config(t->session, &t->cfg);
    session_set_auto_reconnect(t->session, t->auto_reconnect);
    t->tb.nl_mode = t->nl_mode;
    app_mark_dirty(a);
}

static void history_push(Tab *t, const char *line)
{
    if (!line[0])
        return;
    if (t->history_count && strcmp(t->history[t->history_count - 1], line) == 0)
        return;
    if (t->history_count == HISTORY_MAX) {
        free(t->history[0]);
        memmove(t->history, t->history + 1, sizeof(char *) * (HISTORY_MAX - 1));
        t->history_count--;
    }
    size_t n = strlen(line) + 1;
    t->history[t->history_count] = malloc(n);
    memcpy(t->history[t->history_count], line, n);
    t->history_count++;
}

void app_set_console_size(App *a, bool fixed, int cols, int rows)
{
    a->console_fixed = fixed;
    a->console_cols = cols < CONSOLE_COLS_MIN ? CONSOLE_COLS_MIN : cols > CONSOLE_COLS_MAX ? CONSOLE_COLS_MAX : cols;
    a->console_rows = rows < CONSOLE_ROWS_MIN ? CONSOLE_ROWS_MIN : rows > CONSOLE_ROWS_MAX ? CONSOLE_ROWS_MAX : rows;
    app_mark_dirty(a);
}

void app_set_scrollback(App *a, int lines)
{
    a->scrollback_lines = lines < SCROLLBACK_MIN ? SCROLLBACK_MIN : lines > SCROLLBACK_MAX ? SCROLLBACK_MAX : lines;
    for (int i = 0; i < a->ntabs; i++)
        termbuf_set_capacity(&a->tabs[i]->tb, (uint32_t)a->scrollback_lines);
    app_mark_dirty(a);
}

/* Serial has no window-size signal, so tell the device's tty explicitly. */
void app_tab_send_size(App *a, Tab *t)
{
    static const char *eols[] = {"\r", "\n", "\r", "\r\n"};
    char cmd[64];
    int n = snprintf(cmd, sizeof cmd, "stty rows %d cols %d%s", t->tb.rows, t->tb.cols, eols[t->tx_eol & 3]);
    app_tab_send(a, t, cmd, (size_t)n, false);
}

void app_tab_send(App *a, Tab *t, const char *data, size_t len, bool add_eol)
{
    static const char *eols[] = {"", "\n", "\r", "\r\n"};
    if (!add_eol) {
        /* Raw keystrokes and pastes go out as-is, whatever their size. */
        session_write(t->session, data, len);
        t->follow = true;
        return;
    }
    char buf[INPUT_MAX + 4];
    if (len > INPUT_MAX)
        len = INPUT_MAX;
    memcpy(buf, data, len);
    if (add_eol) {
        char tmp[INPUT_MAX + 1];
        memcpy(tmp, data, len);
        tmp[len] = 0;
        history_push(t, tmp);
        const char *e = eols[t->tx_eol & 3];
        size_t el = strlen(e);
        memcpy(buf + len, e, el);
        len += el;
    }
    session_write(t->session, buf, len);
    t->follow = true;
    app_mark_dirty(a);
}

void app_tab_title(const Tab *t, char *out, size_t outlen)
{
    if (!t->target.path[0]) {
        snprintf(out, outlen, "New Session");
        return;
    }
    const char *base = strrchr(t->target.path, '/');
    base = base ? base + 1 : t->target.path;
    snprintf(out, outlen, "%s", base);
}

const SerialPortInfo *app_find_port(App *a, const PortTarget *t)
{
    for (int i = 0; i < a->nports; i++)
        if (t->id[0] && strcmp(a->ports[i].id, t->id) == 0)
            return &a->ports[i];
    for (int i = 0; i < a->nports; i++)
        if (strcmp(a->ports[i].path, t->path) == 0)
            return &a->ports[i];
    return NULL;
}

void app_scan_ports(App *a, bool force)
{
    uint64_t now = SDL_GetTicks();
    if (!force && now - a->ports_scanned_at < PORT_SCAN_MS)
        return;
    a->ports_scanned_at = now;
    SerialPortInfo ports[MAX_PORTS];
    int n = serial_enumerate(ports, MAX_PORTS);
    if (n != a->nports || memcmp(ports, a->ports, sizeof(SerialPortInfo) * (size_t)n) != 0) {
        memcpy(a->ports, ports, sizeof(SerialPortInfo) * (size_t)n);
        a->nports = n;
        app_request_frames(a, 2);
    }
}

/* ---- Session logs ----------------------------------------------------------- */

/* Opens the tab's log on first use (a connection starting, or data). */
static void tab_log_ensure(App *a, Tab *t, double time)
{
    if (a->logcfg.mode == LOG_NONE || log_is_open(&t->slog) || t->slog.failed)
        return;
    char path[128], cfg[48], info[300], err[1600] = "";
    session_current_path(t->session, path, sizeof path);
    if (!path[0])
        snprintf(path, sizeof path, "%s", t->target.path);
    serial_config_describe(&t->cfg, cfg, sizeof cfg);
    snprintf(info, sizeof info, "%s  %s", path, cfg);
    if (!log_open(&t->slog, &a->logcfg, path, info, err, sizeof err) && err[0])
        termbuf_add_marker(&t->tb, TL_ERROR, err, time);
}

void app_logging_changed(App *a)
{
    for (int i = 0; i < a->ntabs; i++) {
        log_close(&a->tabs[i]->slog);
        a->tabs[i]->slog.failed = false;
    }
    app_mark_dirty(a);
}

void app_tab_log_resolve(App *a, Tab *t, int choice)
{
    char err[1600] = "";
    if (!log_resolve(&t->slog, choice == 0, choice == 1, err, sizeof err) && err[0])
        termbuf_add_marker(&t->tb, TL_ERROR, err, (double)SDL_GetTicks() / 1000.0);
    (void)a;
}

void app_open_log_folder(App *a)
{
    Tab *t = app_active_tab(a);
    char dir[1400];
    if (t && t->slog.path[0])
        snprintf(dir, sizeof dir, "%s", t->slog.path);
    else if (a->logcfg.path[0])
        snprintf(dir, sizeof dir, "%s", a->logcfg.path);
    else
        log_default_path(dir, sizeof dir);
    char *slash = NULL;
    for (char *p = dir; *p; p++)
        if (*p == '/' || *p == '\\')
            slash = p;
    if (!slash)
        return;
    *slash = 0;
    SDL_CreateDirectory(dir);
#ifdef _WIN32
    SDL_OpenURL(dir);
#else
    char url[1500];
    snprintf(url, sizeof url, "file://%s", dir);
    SDL_OpenURL(url);
#endif
}

/* Batched redraws: a full-screen program's page arrives over several reads
 * (a USB serial adapter hands over a chunk every 16 ms or so, and a page is
 * tens of milliseconds of line time). Showing each chunk as it lands wipes
 * the screen top to bottom on every refresh. Once a clear screen or cursor
 * home shows up in the pending bytes, the tab leaves them in the session
 * until the stream has been quiet for a moment, so the whole page lands in
 * one frame. The cap keeps a continuous stream from stalling the view. */
#define REDRAW_QUIET_NS (60ull * 1000000)
#define REDRAW_HOLD_MAX_NS (250ull * 1000000)

/* Moves everything the I/O threads produced into the tabs' scrollback. */
bool app_pump_sessions(App *a)
{
    bool any = false;
    uint64_t now = SDL_GetTicksNS();
    a->rx_holding = false;
    for (int i = 0; i < a->ntabs; i++) {
        Tab *t = a->tabs[i];
        if (t->batch_redraws) {
            int sync = session_peek_sync(t->session);
            if ((sync & SESSION_SYNC_BEGIN) && !t->hold_since_ns)
                t->hold_since_ns = now;
            if (t->hold_since_ns) {
                bool quiet = now - session_last_rx_ns(t->session) >= REDRAW_QUIET_NS;
                bool expired = now - t->hold_since_ns >= REDRAW_HOLD_MAX_NS;
                if (!quiet && !expired && !(sync & SESSION_SYNC_END)) {
                    a->rx_holding = true;
                    continue;
                }
            }
        }
        t->hold_since_ns = 0;
        if (!session_drain(t->session, &t->drain))
            continue;
        any = true;
        size_t off = 0;
        const uint8_t *payload;
        const SessionRecord *r;
        while ((r = session_buf_next(&t->drain, &off, &payload))) {
            switch (r->type) {
            case SREC_RX:
                termbuf_feed(&t->tb, payload, r->len, r->time);
                if (t->tb.reply_len) {
                    /* cursor position reports etc. answer the device directly */
                    session_write(t->session, t->tb.reply, (size_t)t->tb.reply_len);
                    t->tb.reply_len = 0;
                }
                if (a->logcfg.mode != LOG_NONE) {
                    tab_log_ensure(a, t, r->time);
                    log_rx(&t->slog, payload, r->len, r->time);
                }
                break;
            case SREC_TX:
                if (t->local_echo)
                    termbuf_add_tx(&t->tb, payload, r->len, r->time);
                break;
            case SREC_STATE: {
                t->link = (LinkState)r->state;
                if (r->len) {
                    char msg[300];
                    size_t n = r->len < sizeof msg - 1 ? r->len : sizeof msg - 1;
                    memcpy(msg, payload, n);
                    msg[n] = 0;
                    static const TermLineKind kinds[] = {TL_INFO, TL_OK, TL_WARN, TL_ERROR};
                    termbuf_add_marker(&t->tb, kinds[r->severity & 3], msg, r->time);
                    if (a->logcfg.mode != LOG_NONE) {
                        if (r->state != LINK_IDLE)
                            tab_log_ensure(a, t, r->time);
                        log_event(&t->slog, msg, r->time);
                    }
                }
                /* The log covers one connection, reconnects included. */
                if (r->state == LINK_IDLE) {
                    log_close(&t->slog);
                    t->slog.failed = false;
                }
                break;
            }
            }
        }
    }
    return any;
}

void app_mark_dirty(App *a)
{
    if (!a->settings_dirty)
        a->settings_dirty_at = SDL_GetTicks();
    a->settings_dirty = true;
}
