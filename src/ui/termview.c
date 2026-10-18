/* Terminal view: virtualized rendering of the scrollback (only visible lines
 * are drawn), ANSI styles, selection/copy, raw keyboard input to the port,
 * find, and friendly empty states. */
#include "ui/ui.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float fs, line_h, char_w, gutter, x0, text_dy;
    ImVec2 origin; /* screen position of line 0 */
} Metrics;

static int utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}

/* Byte offset -> display cell index (one cell per code point). */
static int col_to_cells(const TermLine *l, uint32_t col)
{
    int cells = 0;
    uint32_t i = 0;
    uint32_t end = col < l->len ? col : l->len;
    while (i < end) {
        i += (uint32_t)utf8_len((unsigned char)l->text[i]);
        cells++;
    }
    if (col > l->len)
        cells += (int)(col - l->len);
    return cells;
}

static uint32_t cells_to_col(const TermLine *l, int cells)
{
    uint32_t i = 0;
    while (cells > 0 && i < l->len) {
        i += (uint32_t)utf8_len((unsigned char)l->text[i]);
        cells--;
    }
    return i > l->len ? l->len : i;
}

static int pos_cmp(TermPos a, TermPos b)
{
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    return a.col < b.col ? -1 : a.col > b.col ? 1 : 0;
}

static void sel_range(const Tab *t, TermPos *s, TermPos *e)
{
    if (pos_cmp(t->sel_anchor, t->sel_head) <= 0) {
        *s = t->sel_anchor;
        *e = t->sel_head;
    } else {
        *s = t->sel_head;
        *e = t->sel_anchor;
    }
}

void termview_select_all(Tab *t)
{
    if (!termbuf_count(&t->tb))
        return;
    t->sel_anchor.line = termbuf_first_abs(&t->tb);
    t->sel_anchor.col = 0;
    t->sel_head.line = termbuf_end_abs(&t->tb) - 1;
    t->sel_head.col = termbuf_line(&t->tb, t->sel_head.line)->len;
    t->has_sel = true;
}

void termview_copy_selection(Tab *t)
{
    if (!t->has_sel)
        return;
    TermPos s, e;
    sel_range(t, &s, &e);
    if (s.line < termbuf_first_abs(&t->tb)) {
        s.line = termbuf_first_abs(&t->tb);
        s.col = 0;
    }
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    for (uint64_t ln = s.line; ln <= e.line; ln++) {
        TermLine *l = termbuf_line(&t->tb, ln);
        if (!l)
            break;
        uint32_t from = ln == s.line ? (s.col < l->len ? s.col : l->len) : 0;
        uint32_t to = ln == e.line ? (e.col < l->len ? e.col : l->len) : l->len;
        size_t need = len + (to > from ? to - from : 0) + 2;
        if (need > cap) {
            while (cap < need)
                cap *= 2;
            out = realloc(out, cap);
        }
        if (to > from) {
            memcpy(out + len, l->text + from, to - from);
            len += to - from;
        }
        if (ln != e.line && !(l->flags & TLF_WRAPPED))
            out[len++] = '\n';
    }
    out[len] = 0;
    igSetClipboardText(out);
    free(out);
}

void termview_paste(App *a, Tab *t)
{
    char *clip = SDL_GetClipboardText();
    if (!clip)
        return;
    static const char *eols[] = {"\r", "\n", "\r", "\r\n"};
    const char *eol = eols[t->tx_eol & 3];
    size_t n = strlen(clip), el = strlen(eol);
    char *buf = malloc(n * 2 + 13);
    size_t len = 0;
    bool bracketed = t->tb.bracketed_paste;
    if (bracketed) {
        memcpy(buf, "\x1b[200~", 6);
        len = 6;
    }
    for (size_t i = 0; i < n; i++) {
        if (clip[i] == '\r' && clip[i + 1] == '\n')
            continue;
        if (clip[i] == '\n' || clip[i] == '\r') {
            memcpy(buf + len, eol, el);
            len += el;
        } else {
            buf[len++] = clip[i];
        }
    }
    if (bracketed) {
        memcpy(buf + len, "\x1b[201~", 6);
        len += 6;
    }
    app_tab_send(a, t, buf, len, false);
    free(buf);
    SDL_free(clip);
}

/* ---- Find ------------------------------------------------------------- */

static int ci_eq(char a, char b)
{
    if (a >= 'A' && a <= 'Z')
        a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z')
        b = (char)(b + 32);
    return a == b;
}

/* Returns the byte offset of the next match at or after `from`, or -1. */
static int find_in_line(const TermLine *l, const char *q, size_t qn, bool cs, uint32_t from)
{
    if (!qn || l->len < qn)
        return -1;
    for (uint32_t i = from; i + qn <= l->len; i++) {
        size_t k = 0;
        if (cs) {
            if (memcmp(l->text + i, q, qn) == 0)
                return (int)i;
            continue;
        }
        while (k < qn && ci_eq(l->text[i + k], q[k]))
            k++;
        if (k == qn)
            return (int)i;
    }
    return -1;
}

static int find_last_before(const TermLine *l, const char *q, size_t qn, bool cs, uint32_t before)
{
    int last = -1, at = 0;
    while ((at = find_in_line(l, q, qn, cs, (uint32_t)at)) >= 0 && (uint32_t)at < before) {
        last = at;
        at++;
    }
    return last;
}

/* Moves to the next (dir>0) or previous match, wrapping around. With
 * inclusive set, a match at the current position counts (used while typing
 * the query so the highlight doesn't run ahead). */
static void find_step(Tab *t, int dir, bool inclusive)
{
    size_t qn = strlen(t->find_text);
    TermBuf *tb = &t->tb;
    if (!qn || !termbuf_count(tb))
        return;
    uint64_t first = termbuf_first_abs(tb), end = termbuf_end_abs(tb);
    uint64_t ln = t->find_line;
    uint32_t col = t->find_col;
    if (ln < first || ln >= end) {
        ln = dir > 0 ? first : end - 1;
        col = dir > 0 ? 0 : UINT32_MAX;
        inclusive = true;
    }
    for (uint64_t n = 0; n <= termbuf_count(tb); n++) {
        TermLine *l = termbuf_line(tb, ln);
        int at;
        if (dir > 0) {
            uint32_t from = n ? 0 : (inclusive ? col : col + 1);
            at = find_in_line(l, t->find_text, qn, t->find_case, from);
        } else {
            uint32_t before = n ? UINT32_MAX : (inclusive && col != UINT32_MAX ? col + 1 : col);
            at = find_last_before(l, t->find_text, qn, t->find_case, before);
        }
        if (at >= 0) {
            t->find_line = ln;
            t->find_col = (uint32_t)at;
            t->scroll_to_line = (float)(ln - first);
            t->follow = false;
            return;
        }
        if (dir > 0)
            ln = ln + 1 >= end ? first : ln + 1;
        else
            ln = ln <= first ? end - 1 : ln - 1;
    }
}

static void find_count(Tab *t)
{
    size_t qn = strlen(t->find_text);
    int count = 0;
    if (qn) {
        for (uint64_t ln = termbuf_first_abs(&t->tb); ln < termbuf_end_abs(&t->tb) && count < 9999; ln++) {
            TermLine *l = termbuf_line(&t->tb, ln);
            int at = 0;
            while ((at = find_in_line(l, t->find_text, qn, t->find_case, (uint32_t)at)) >= 0 && count < 9999) {
                count++;
                at += (int)qn;
            }
        }
    }
    t->find_count = count;
    t->find_version = t->tb.version;
}

static void find_bar(App *a, Tab *t, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    float w = minf(S(380), p1.x - p0.x - S(24)), h = S(36);
    ImVec2 b0 = v2(p1.x - w - S(16), p0.y + S(6));
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, v2(b0.x + S(1), b0.y + S(3)), v2(b0.x + w + S(1), b0.y + h + S(3)), th->shadow, S(6), 0);
    ui_card(dl, b0, v2(b0.x + w, b0.y + h), th->widget_bg, th->widget_border, S(6), 0);

    float ib = S(24);
    float input_w = w - S(12) - ib * 4 - S(64);
    igSetCursorScreenPos(v2(b0.x + S(6), b0.y + (h - igGetFrameHeight()) * 0.5f));
    igSetNextItemWidth(input_w);
    if (t->find_focus) {
        igSetKeyboardFocusHere(0);
        t->find_focus = false;
    }
    igPushStyleColor_U32(ImGuiCol_FrameBg, th->input_bg);
    bool edited = igInputTextWithHint("##findinput", "Find", t->find_text, sizeof t->find_text, 0, NULL, NULL);
    igPopStyleColor(1);
    bool input_focused = igIsItemFocused();
    if (edited) {
        find_count(t);
        find_step(t, +1, true);
    }
    if (input_focused && igIsKeyPressed_Bool(ImGuiKey_Enter, true))
        find_step(t, igGetIO_Nil()->KeyShift ? -1 : +1, false);
    /* Recount as new data arrives, at most a few times per second. */
    static uint64_t last_count;
    if (t->find_version != t->tb.version && SDL_GetTicks() - last_count > 250) {
        last_count = SDL_GetTicks();
        find_count(t);
    }

    char cnt[32];
    if (!t->find_text[0])
        cnt[0] = 0;
    else if (t->find_count == 0)
        snprintf(cnt, sizeof cnt, "No results");
    else
        snprintf(cnt, sizeof cnt, "%s%d results", t->find_count >= 9999 ? ">" : "", t->find_count);
    float fs = S(11.5f);
    ImVec2 cs = ui_text_size(a->font_ui, fs, cnt, NULL);
    float cx = b0.x + S(6) + input_w + S(6);
    ui_text(dl, a->font_ui, fs, v2(cx, b0.y + (h - cs.y) * 0.5f), t->find_count ? th->fg_dim : th->error, cnt, NULL);

    float bx = b0.x + w - S(6) - ib * 4;
    float by = b0.y + (h - ib) * 0.5f;
    igSetCursorScreenPos(v2(bx, by));
    if (ui_icon_button("##case", ICON_CASE_SENSITIVE, "Match Case", t->find_case, ib)) {
        t->find_case = !t->find_case;
        find_count(t);
    }
    igSetCursorScreenPos(v2(bx + ib, by));
    if (ui_icon_button("##prev", ICON_ARROW_UP, "Previous Match (Shift+Enter)", false, ib))
        find_step(t, -1, false);
    igSetCursorScreenPos(v2(bx + ib * 2, by));
    if (ui_icon_button("##next", ICON_ARROW_DOWN, "Next Match (Enter)", false, ib))
        find_step(t, +1, false);
    igSetCursorScreenPos(v2(bx + ib * 3, by));
    if (ui_icon_button("##closefind", ICON_CLOSE, "Close (Escape)", false, ib) ||
        (input_focused && igIsKeyPressed_Bool(ImGuiKey_Escape, false))) {
        t->find_open = false;
        t->focus_terminal = true;
    }
}

/* ---- Drawing ------------------------------------------------------------ */

static void draw_marker(App *a, ImDrawList *dl, const TermLine *l, float x, float y, const Metrics *m, float right)
{
    const Theme *th = &a->theme;
    ImU32 col;
    const char *icon;
    switch (l->kind) {
    case TL_OK: col = th->ok; icon = ICON_PASS; break;
    case TL_WARN: col = th->warn; icon = ICON_WARNING; break;
    case TL_ERROR: col = th->error; icon = ICON_ERROR; break;
    default: col = th->info; icon = ICON_INFO; break;
    }
    float fs = m->fs * 0.92f;
    char text[512];
    uint32_t n = l->len < sizeof text - 1 ? l->len : (uint32_t)sizeof text - 1;
    memcpy(text, l->text, n);
    text[n] = 0;
    ImVec2 ts = ui_text_size(a->font_ui, fs, text, NULL);
    float cy = y + m->line_h * 0.5f;
    float pw = ts.x + S(30);
    ImDrawList_AddRectFilled(dl, v2(x - S(4), cy - m->line_h * 0.5f + S(1)), v2(x - S(4) + pw, cy + m->line_h * 0.5f - S(1)),
                             col_alpha(col, 0.13f), S(4), 0);
    ui_draw_icon(dl, v2(x + S(7), cy), icon, col, S(14));
    ui_text(dl, a->font_ui, fs, v2(x + S(20), cy - ts.y * 0.5f), col, text, NULL);
    float lx = x - S(4) + pw + S(8);
    if (right > lx)
        ImDrawList_AddLine(dl, v2(lx, cy), v2(right, cy), col_alpha(col, 0.18f), 1.0f);
}

static void draw_text_line(App *a, Tab *t, ImDrawList *dl, const TermLine *l, float x, float y, const Metrics *m)
{
    const Theme *th = &a->theme;
    uint32_t i = 0;
    int cell = 0;
    while (i < l->len) {
        uint16_t sid = termline_style_at(l, i);
        uint32_t j = i;
        int cells = 0;
        while (j < l->len && termline_style_at(l, j) == sid) {
            j += (uint32_t)utf8_len((unsigned char)l->text[j]);
            cells++;
        }
        if (j > l->len)
            j = l->len;
        const TermStyle *st = termbuf_style(&t->tb, sid);
        ImU32 fg = (st->attr & TA_TX) ? th->info : theme_term_color(th, st->fg, true);
        ImU32 bg = 0;
        if (st->bg != TC_DEFAULT)
            bg = theme_term_color(th, st->bg, false);
        if (st->attr & TA_INVERSE) {
            ImU32 tmp = fg;
            fg = st->bg != TC_DEFAULT ? bg : th->editor_bg;
            bg = tmp;
        }
        if (st->attr & TA_DIM)
            fg = col_alpha(fg, 0.6f);
        if (st->attr & TA_CONTROL)
            fg = col_alpha(th->fg_dim, 0.8f);
        float rx = x + (float)cell * m->char_w;
        if (bg)
            ImDrawList_AddRectFilled(dl, v2(rx, y), v2(rx + (float)cells * m->char_w, y + m->line_h), bg, 0, 0);
        ImFont *font = (st->attr & TA_BOLD) ? a->font_mono_bold : a->font_mono;
        ImDrawList_AddText_FontPtr(dl, font, m->fs, v2(rx, y + m->text_dy), fg, l->text + i, l->text + j, 0, NULL);
        if (st->attr & TA_UNDERLINE)
            ImDrawList_AddLine(dl, v2(rx, y + m->line_h - S(2)), v2(rx + (float)cells * m->char_w, y + m->line_h - S(2)),
                               fg, 1.0f);
        cell += cells;
        i = j;
    }
}

static void format_time(double t, char *out, size_t outlen)
{
    SDL_DateTime dt;
    SDL_Time ns = (SDL_Time)(t * 1e9);
    if (!SDL_TimeToDateTime(ns, &dt, true)) {
        out[0] = 0;
        return;
    }
    snprintf(out, outlen, "%02d:%02d:%02d.%03d", dt.hour, dt.minute, dt.second, dt.nanosecond / 1000000);
}

/* ---- Keyboard ---------------------------------------------------------- */

/* Keyboard handling inside the frame for the focused terminal: copy/paste
 * shortcuts, plus any keystrokes that reached ImGui instead of
 * termview_sdl_event() (e.g. typed in the same batch as the click that focused
 * the terminal). Events consumed by termview_sdl_event() never reach ImGui, so
 * nothing is sent twice. */
static void handle_keys(App *a, Tab *t)
{
    ImGuiIO *io = igGetIO_Nil();
    bool ctrl = io->KeyCtrl, shift = io->KeyShift;
    a->term_focus_uid = t->uid;
    if ((ctrl && shift && igIsKeyPressed_Bool(ImGuiKey_C, false)) ||
        (ctrl && !shift && igIsKeyPressed_Bool(ImGuiKey_C, false) && t->has_sel)) {
        termview_copy_selection(t);
        if (!shift)
            t->has_sel = false;
        return;
    }
    bool connected = session_state(t->session) == LINK_CONNECTED;
    if ((ctrl && shift && igIsKeyPressed_Bool(ImGuiKey_V, false)) || (shift && igIsKeyPressed_Bool(ImGuiKey_Insert, false))) {
        if (connected)
            termview_paste(a, t);
        return;
    }
    if (ctrl && shift && igIsKeyPressed_Bool(ImGuiKey_A, false)) {
        termview_select_all(t);
        return;
    }
    if (!connected)
        return;

    for (int i = 0; i < io->InputQueueCharacters.Size; i++) {
        unsigned c = io->InputQueueCharacters.Data[i];
        char u[5] = {0};
        int n = 0;
        if (c < 0x20)
            continue;
        if (c < 0x80)
            u[n++] = (char)c;
        else if (c < 0x800) {
            u[n++] = (char)(0xC0 | (c >> 6));
            u[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            u[n++] = (char)(0xE0 | (c >> 12));
            u[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            u[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            u[n++] = (char)(0xF0 | (c >> 18));
            u[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            u[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            u[n++] = (char)(0x80 | (c & 0x3F));
        }
        app_tab_send(a, t, u, (size_t)n, false);
        t->has_sel = false;
    }
    static const struct {
        ImGuiKey key;
        const char *seq, *app_seq; /* app_seq: with application cursor keys (DECCKM) */
    } keys[] = {
        {ImGuiKey_Backspace, "\x7f", NULL},        {ImGuiKey_Tab, "\t", NULL},
        {ImGuiKey_Escape, "\x1b", NULL},           {ImGuiKey_UpArrow, "\x1b[A", "\x1bOA"},
        {ImGuiKey_DownArrow, "\x1b[B", "\x1bOB"},  {ImGuiKey_RightArrow, "\x1b[C", "\x1bOC"},
        {ImGuiKey_LeftArrow, "\x1b[D", "\x1bOD"},  {ImGuiKey_Home, "\x1b[H", "\x1bOH"},
        {ImGuiKey_End, "\x1b[F", "\x1bOF"},        {ImGuiKey_Delete, "\x1b[3~", NULL},
    };
    if (!ctrl) {
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (igIsKeyPressed_Bool(keys[i].key, true)) {
                const char *seq = t->tb.app_cursor && keys[i].app_seq ? keys[i].app_seq : keys[i].seq;
                app_tab_send(a, t, seq, strlen(seq), false);
            }
        if (igIsKeyPressed_Bool(ImGuiKey_Enter, true) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, true)) {
            static const char *eols[] = {"\r", "\n", "\r", "\r\n"};
            app_tab_send(a, t, eols[t->tx_eol & 3], strlen(eols[t->tx_eol & 3]), false);
            t->follow = true;
        }
    }
}

/* Raw keystrokes go straight from SDL events to the port (bypassing ImGui's
 * one-event-per-frame input queue) so fast typing and key repeat never lag.
 * Returns true if the event was consumed. */
bool termview_sdl_event(App *a, const SDL_Event *e)
{
    if (!a->term_focus_uid)
        return false;
    Tab *t = NULL;
    for (int i = 0; i < a->ntabs; i++)
        if (a->tabs[i]->uid == a->term_focus_uid)
            t = a->tabs[i];
    if (!t || session_state(t->session) != LINK_CONNECTED)
        return false;

    /* Alt+letter is sent as ESC + letter on key down; some platforms also
     * deliver the letter as text input, which must not be sent again. */
    static char s_alt_char;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        bool dup = s_alt_char && e->text.text[0] == s_alt_char && !e->text.text[1];
        s_alt_char = 0;
        if (dup)
            return true;
        app_tab_send(a, t, e->text.text, strlen(e->text.text), false);
        t->has_sel = false;
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN)
        return false;
    s_alt_char = 0;
    SDL_Keymod mod = e->key.mod;
    bool ctrl = (mod & SDL_KMOD_CTRL) != 0, shift = (mod & SDL_KMOD_SHIFT) != 0, alt = (mod & SDL_KMOD_ALT) != 0;
    SDL_Keycode k = e->key.key;
    bool appc = t->tb.app_cursor;
    const char *seq = NULL;
    static const char *eols[] = {"\r", "\n", "\r", "\r\n"};
    if (!ctrl && !alt) {
        switch (k) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: seq = eols[t->tx_eol & 3]; break;
        case SDLK_BACKSPACE: seq = "\x7f"; break;
        case SDLK_TAB: seq = shift ? "\x1b[Z" : "\t"; break;
        case SDLK_ESCAPE: seq = "\x1b"; break;
        case SDLK_UP: seq = appc ? "\x1bOA" : "\x1b[A"; break;
        case SDLK_DOWN: seq = appc ? "\x1bOB" : "\x1b[B"; break;
        case SDLK_RIGHT: seq = appc ? "\x1bOC" : "\x1b[C"; break;
        case SDLK_LEFT: seq = appc ? "\x1bOD" : "\x1b[D"; break;
        case SDLK_HOME: seq = appc ? "\x1bOH" : "\x1b[H"; break;
        case SDLK_END: seq = appc ? "\x1bOF" : "\x1b[F"; break;
        case SDLK_DELETE: seq = "\x1b[3~"; break;
        case SDLK_PAGEUP: seq = shift ? NULL : "\x1b[5~"; break;
        case SDLK_PAGEDOWN: seq = shift ? NULL : "\x1b[6~"; break;
        case SDLK_INSERT: seq = shift ? NULL : "\x1b[2~"; break;
        case SDLK_F1: seq = "\x1bOP"; break;
        case SDLK_F2: seq = "\x1bOQ"; break;
        case SDLK_F3: seq = "\x1bOR"; break;
        case SDLK_F4: seq = "\x1bOS"; break;
        case SDLK_F5: seq = shift ? NULL : "\x1b[15~"; break; /* Shift+F5 disconnects */
        case SDLK_F6: seq = "\x1b[17~"; break;
        case SDLK_F7: seq = "\x1b[18~"; break;
        case SDLK_F8: seq = "\x1b[19~"; break;
        case SDLK_F9: seq = "\x1b[20~"; break;
        case SDLK_F10: seq = "\x1b[21~"; break;
        case SDLK_F11: seq = "\x1b[23~"; break;
        case SDLK_F12: seq = "\x1b[24~"; break;
        default: break;
        }
        if (seq) {
            app_tab_send(a, t, seq, strlen(seq), false);
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                t->follow = true;
            return true;
        }
        return false;
    }
    if (alt && !ctrl && k >= SDLK_A && k <= SDLK_Z) {
        char s[2] = {0x1b, (char)(shift ? k - SDLK_A + 'A' : k - SDLK_A + 'a')};
        s_alt_char = s[1];
        app_tab_send(a, t, s, 2, false);
        return true;
    }
    if (ctrl && !shift && !alt) {
        /* Every Ctrl+letter goes to the device (Vim and shells use them);
         * the app's own shortcuts are on Ctrl+Shift+letter. */
        char c = 0;
        if (k >= SDLK_A && k <= SDLK_Z) {
            if (k == SDLK_C && t->has_sel)
                return false; /* copy (handled in the frame) */
            c = (char)(k - SDLK_A + 1);
        } else if (k == SDLK_SPACE || k == SDLK_2) {
            c = 0; /* NUL */
            app_tab_send(a, t, &c, 1, false);
            return true;
        } else if (k == SDLK_LEFTBRACKET) {
            c = 0x1b;
        } else if (k == SDLK_BACKSLASH) {
            c = 0x1c;
        } else if (k == SDLK_RIGHTBRACKET) {
            c = 0x1d;
        } else if (k == SDLK_6) {
            c = 0x1e;
        } else if (k == SDLK_SLASH) {
            c = 0x1f;
        }
        if (c) {
            app_tab_send(a, t, &c, 1, false);
            return true;
        }
    }
    return false;
}

/* ---- Empty states ------------------------------------------------------ */

static void empty_state(App *a, Tab *t, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImDrawList *dl = igGetWindowDrawList();
    LinkState st = session_state(t->session);
    float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.45f;
    const char *title, *button = NULL;
    char sub[256], cfg[48];
    serial_config_describe(&t->cfg, cfg, sizeof cfg);
    if (!t->target.path[0]) {
        title = "No port selected";
        snprintf(sub, sizeof sub, "Pick a port in the sidebar, or press Ctrl+P");
        button = "Select Port\xE2\x80\xA6";
        ui_draw_icon(dl, v2(cx, cy - S(52)), ICON_PLUG, th->fg_disabled, S(44));
    } else if (st == LINK_IDLE) {
        title = t->target.name[0] ? t->target.name : t->target.path;
        snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", t->target.path, cfg);
        button = "Connect";
        ui_draw_icon(dl, v2(cx, cy - S(52)), ICON_PLUG, th->fg_disabled, S(44));
    } else {
        title = st == LINK_CONNECTED ? "Connected" : "Waiting for device\xE2\x80\xA6";
        session_status(t->session, sub, sizeof sub, NULL);
        ui_link_icon(dl, v2(cx, cy - S(52)), st, S(40));
    }
    float fs = S(15);
    ImVec2 ts = ui_text_size(a->font_ui_bold, fs, title, NULL);
    ui_text(dl, a->font_ui_bold, fs, v2(cx - ts.x * 0.5f, cy - S(18)), th->fg, title, NULL);
    float ss = a->font_size * a->ui_scale;
    ImVec2 ss2 = ui_text_size(a->font_ui, ss, sub, NULL);
    ui_text(dl, a->font_ui, ss, v2(cx - ss2.x * 0.5f, cy + S(6)), th->fg_dim, sub, NULL);
    if (button) {
        float bw = S(150), bh = S(30);
        igSetCursorScreenPos(v2(cx - bw * 0.5f, cy + S(36)));
        if (ui_button(button, NULL, v2(bw, bh), true)) {
            if (!t->target.path[0])
                ui_open_quickpick(a);
            else
                app_tab_connect(a, t);
        }
    }
}

/* ---- Main view ----------------------------------------------------------- */

static TermPos mouse_to_pos(Tab *t, const Metrics *m, ImVec2 mp)
{
    TermPos p = {0, 0};
    TermBuf *tb = &t->tb;
    if (!termbuf_count(tb))
        return p;
    double row = floor((mp.y - m->origin.y) / m->line_h);
    if (row < 0)
        row = 0;
    uint64_t ln = termbuf_first_abs(tb) + (uint64_t)row;
    uint64_t end = termbuf_end_abs(tb);
    if (ln >= end) {
        p.line = end - 1;
        p.col = termbuf_line(tb, p.line)->len;
        return p;
    }
    p.line = ln;
    TermLine *l = termbuf_line(tb, ln);
    int cells = (int)floorf((mp.x - m->x0) / m->char_w + 0.5f);
    p.col = cells <= 0 ? 0 : cells_to_col(l, cells);
    return p;
}

static bool is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || c == '/' || c == ':' || (unsigned char)c >= 0x80;
}

void ui_termview(App *a, Tab *t, ImVec2 p0, ImVec2 p1)
{
    const Theme *th = &a->theme;
    ImGuiIO *io = igGetIO_Nil();
    TermBuf *tb = &t->tb;

    Metrics m;
    m.fs = floorf(a->mono_size * a->ui_scale + 0.5f);
    m.line_h = floorf(m.fs * 1.4f + 0.5f);
    m.char_w = ui_text_size(a->font_mono, m.fs, "M", NULL).x;
    m.text_dy = floorf((m.line_h - m.fs) * 0.5f);
    bool alt = termbuf_alt_active(tb);
    m.gutter = t->timestamps && !alt ? m.char_w * 13.0f : 0;
    float pad_x = S(14), pad_y = S(8);

    /* The terminal's size in cells: fixed (Preferences > Console), or
     * whatever fits in the view. */
    ImVec2 avail1 = p1;
    if (a->console_fixed) {
        termbuf_resize(tb, a->console_rows, a->console_cols);
        /* The view is exactly the console (like PuTTY's window): text is
         * clipped to it and scrollback is only seen by scrolling. A window
         * smaller than the console scrolls instead. */
        float want_w = pad_x * 2 + m.gutter + (float)tb->cols * m.char_w + S(12);
        bool hscroll = want_w > p1.x - p0.x; /* leave room for the horizontal scrollbar */
        p1.x = minf(p1.x, p0.x + want_w);
        p1.y = minf(p1.y, p0.y + pad_y * 2 + (float)tb->rows * m.line_h + (hscroll ? S(12) : 0));
    } else {
        float cells_w = (p1.x - p0.x) - pad_x * 2 - m.gutter - S(12);
        float cells_h = (p1.y - p0.y) - pad_y * 2;
        /* Lines wider than the view (written before the window was narrowed)
         * bring up a horizontal scrollbar; the rows must fit above it, or the
         * top row ends up scrolled out of sight. */
        if ((float)tb->max_cells * m.char_w > cells_w)
            cells_h -= S(12);
        termbuf_resize(tb, (int)floorf(cells_h / m.line_h), (int)floorf(cells_w / m.char_w));
    }

    /* Switching screens invalidates selection and find positions. */
    if (alt != t->view_alt) {
        t->view_alt = alt;
        t->has_sel = t->selecting = false;
        t->find_line = UINT64_MAX;
        t->follow = true;
    }

    char id[32];
    snprintf(id, sizeof id, "##term%d", t->uid);
    if (termbuf_count(tb) == 0 && !alt) {
        /* Same child id as the terminal so keyboard focus carries over when data arrives. */
        igSetCursorScreenPos(p0);
        igBeginChild_Str(id, v2(p1.x - p0.x, p1.y - p0.y), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
        empty_state(a, t, p0, p1);
        if (t->focus_terminal) {
            igSetWindowFocus_Nil();
            t->focus_terminal = false;
        }
        if (igIsWindowFocused(0))
            handle_keys(a, t);
        igEndChild();
        if (t->find_open)
            find_bar(a, t, p0, p1);
        return;
    }

    /* Rows: every line, plus the cursor's row (it may not exist yet), plus the
     * rest of the screen. Including the whole screen keeps it pinned to the
     * view like a real terminal: after a clear (ESC[2J) the new content starts
     * at the top of the view instead of piling up under the old screen, so
     * "watch"-style tools redraw in place. */
    uint64_t first = termbuf_first_abs(tb), cur_line = termbuf_cursor_line(tb);
    uint64_t nlines = termbuf_count(tb);
    if (cur_line >= first && cur_line - first + 1 > nlines)
        nlines = cur_line - first + 1;
    uint64_t screen_end = tb->scr->top + (uint64_t)tb->rows;
    if (screen_end > first && screen_end - first > nlines)
        nlines = screen_end - first;
    if (alt)
        t->follow = true;
    /* Content width: the widest line in cells (never bytes: box-drawing
     * characters are 3 bytes each), or exactly the console when its size is
     * fixed (older, wider lines are clipped rather than widening the view).
     * No slack beyond that: a full-width line must not bring up a scrollbar. */
    float content_w = pad_x * 2 + m.gutter +
                      (float)(a->console_fixed ? (uint32_t)tb->cols : tb->max_cells) * m.char_w;
    igSetCursorScreenPos(p0);
    igSetNextWindowContentSize(v2(content_w, 0));
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(0, pad_y));
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(0, 0));
    igPushStyleVar_Float(ImGuiStyleVar_ScrollbarSize, S(12));
    igBeginChild_Str(id, v2(p1.x - p0.x, p1.y - p0.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                     ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    if (t->focus_terminal) {
        igSetWindowFocus_Nil();
        t->focus_terminal = false;
    }
    bool focused = igIsWindowFocused(0);
    bool hovered = igIsWindowHovered(0);
    ImDrawList *dl = igGetWindowDrawList();
    ImVec2 wpos = igGetWindowPos(), wsize = igGetWindowSize();
    /* Visible text height: without the horizontal scrollbar when it shows. */
    float view_h = wsize.y - pad_y * 2 - (igGetCurrentWindow()->ScrollbarX ? igGetStyle()->ScrollbarSize : 0);

    /* Lines are positioned relative to the oldest retained line, so when the
     * ring evicts lines while the user is scrolled up, shift the scroll to
     * keep the same text on screen. */
    if (!t->follow && termbuf_first_abs(tb) > t->view_first_abs)
        igSetScrollY_Float(maxf(0, igGetScrollY() - (float)(termbuf_first_abs(tb) - t->view_first_abs) * m.line_h));
    t->view_first_abs = termbuf_first_abs(tb);

    /* Follow mode: scrolling up detaches, reaching the bottom re-attaches. */
    float sy = igGetScrollY(), smax = igGetScrollMaxY();
    if (hovered && io->MouseWheel > 0)
        t->follow = false;
    else if (!t->follow && smax > 0 && sy >= smax - 1 && io->MouseWheel <= 0 && !t->selecting)
        t->follow = true;
    if (smax <= 0)
        t->follow = true;
    if (t->follow)
        t->seen_total = tb->total_added;

    m.origin = igGetCursorScreenPos();
    m.origin.x += pad_x;
    m.x0 = m.origin.x + m.gutter;

    /* Selection with the mouse */
    ImVec2 mp = io->MousePos;
    bool in_view = mp.x >= wpos.x && mp.x < wpos.x + wsize.x - S(12) && mp.y >= wpos.y && mp.y < wpos.y + wsize.y - S(4);
    if (hovered && in_view && igIsMouseClicked_Bool(0, false)) {
        TermPos p = mouse_to_pos(t, &m, mp);
        int clicks = igGetMouseClickedCount(0);
        if (clicks == 2) {
            TermLine *l = termbuf_line(tb, p.line);
            uint32_t s = p.col, e = p.col;
            while (s > 0 && is_word(l->text[s - 1]))
                s--;
            while (e < l->len && is_word(l->text[e]))
                e++;
            t->sel_anchor = (TermPos){p.line, s};
            t->sel_head = (TermPos){p.line, e};
            t->has_sel = e > s;
            t->selecting = false;
        } else if (clicks >= 3) {
            t->sel_anchor = (TermPos){p.line, 0};
            t->sel_head = (TermPos){p.line, termbuf_line(tb, p.line)->len};
            t->has_sel = true;
            t->selecting = false;
        } else {
            if (io->KeyShift && t->has_sel) {
                t->sel_head = p;
            } else {
                t->sel_anchor = t->sel_head = p;
                t->has_sel = false;
            }
            t->selecting = true;
        }
    }
    if (t->selecting) {
        if (igIsMouseDown_Nil(0)) {
            t->sel_head = mouse_to_pos(t, &m, mp);
            t->has_sel = pos_cmp(t->sel_anchor, t->sel_head) != 0;
            /* auto-scroll while dragging past the edges */
            if (mp.y < wpos.y + pad_y)
                igSetScrollY_Float(igGetScrollY() - m.line_h), t->follow = false;
            else if (mp.y > wpos.y + wsize.y - pad_y)
                igSetScrollY_Float(igGetScrollY() + m.line_h);
            g_app->animating = true;
        } else {
            t->selecting = false;
        }
    }
    if (hovered && in_view && !t->selecting)
        igSetMouseCursor(ImGuiMouseCursor_TextInput);

    TermPos ss = {0, 0}, se = {0, 0};
    if (t->has_sel)
        sel_range(t, &ss, &se);
    size_t fq = t->find_open ? strlen(t->find_text) : 0;

    /* Visible lines. Drawing is clipped to the area inside the vertical
     * padding, and rows entirely above it aren't drawn at all: otherwise the
     * row above the first visible one shows in the top padding, and its
     * descenders can hang into the first row. */
    float inner_top = wpos.y + pad_y, inner_bot = inner_top + view_h;
    ImDrawList_PushClipRect(dl, v2(wpos.x, inner_top), v2(wpos.x + wsize.x, inner_bot), true);
    ImGuiListClipper *clip = ImGuiListClipper_ImGuiListClipper();
    ImGuiListClipper_Begin(clip, (int)nlines, m.line_h);
    float right = wpos.x + wsize.x - S(16);
    while (ImGuiListClipper_Step(clip)) {
        for (int i = clip->DisplayStart; i < clip->DisplayEnd; i++) {
            float y = m.origin.y + (float)i * m.line_h;
            if (y + m.line_h <= inner_top + 0.5f || y >= inner_bot - 0.5f) {
                igDummy(v2(content_w, m.line_h));
                continue;
            }
            uint64_t abs = termbuf_first_abs(tb) + (uint64_t)i;
            TermLine *l = termbuf_line(tb, abs);
            if (l) {
                if (m.gutter > 0 && l->time > 0) { /* erased lines have no time */
                    char ts[32];
                    format_time(l->time, ts, sizeof ts);
                    ImDrawList_AddText_FontPtr(dl, a->font_mono, m.fs, v2(m.origin.x, y + m.text_dy),
                                               col_alpha(th->fg_dim, 0.75f), ts, NULL, 0, NULL);
                }
                /* selection */
                if (t->has_sel && abs >= ss.line && abs <= se.line) {
                    int c0 = abs == ss.line ? col_to_cells(l, ss.col) : 0;
                    int c1 = abs == se.line ? col_to_cells(l, se.col) : col_to_cells(l, l->len) + 1;
                    if (c1 > c0)
                        ImDrawList_AddRectFilled(dl, v2(m.x0 + (float)c0 * m.char_w, y),
                                                 v2(m.x0 + (float)c1 * m.char_w, y + m.line_h), th->selection, 0, 0);
                }
                /* find highlights */
                if (fq && l->kind < TL_INFO) {
                    int at = 0;
                    while ((at = find_in_line(l, t->find_text, fq, t->find_case, (uint32_t)at)) >= 0) {
                        int c0 = col_to_cells(l, (uint32_t)at), c1 = col_to_cells(l, (uint32_t)at + (uint32_t)fq);
                        bool cur = abs == t->find_line && (uint32_t)at == t->find_col;
                        ImDrawList_AddRectFilled(dl, v2(m.x0 + (float)c0 * m.char_w, y + S(1)),
                                                 v2(m.x0 + (float)c1 * m.char_w, y + m.line_h - S(1)),
                                                 cur ? th->search_match_current : th->search_match, S(2), 0);
                        at += (int)fq;
                    }
                }
                if (l->kind >= TL_INFO)
                    draw_marker(a, dl, l, m.x0, y, &m, right);
                else
                    draw_text_line(a, t, dl, l, m.x0, y, &m);
            }
            /* cursor */
            bool cursor_here = abs == cur_line && tb->cursor_visible;
            if (cursor_here && session_state(t->session) == LINK_CONNECTED) {
                int cc = termbuf_cursor_col(tb);
                float cx = m.x0 + (float)cc * m.char_w;
                if (focused)
                    ImDrawList_AddRectFilled(dl, v2(cx, y + S(1)), v2(cx + m.char_w, y + m.line_h - S(1)),
                                             col_alpha(th->term_cursor, 0.75f), S(1), 0);
                else
                    ImDrawList_AddRect(dl, v2(cx, y + S(1)), v2(cx + m.char_w, y + m.line_h - S(1)),
                                       col_alpha(th->term_cursor, 0.5f), S(1), 1.0f, 0);
                if (focused) {
                    ImGuiContext *ctx = igGetCurrentContext();
                    ctx->PlatformImeData.WantVisible = true;
                    ctx->PlatformImeData.WantTextInput = true;
                    ctx->PlatformImeData.InputPos = v2(cx, y);
                    ctx->PlatformImeData.InputLineHeight = m.line_h;
                    ctx->PlatformImeData.ViewportId = igGetMainViewport()->ID;
                }
            }
            igDummy(v2(content_w, m.line_h));
        }
    }
    ImGuiListClipper_End(clip);
    ImGuiListClipper_destroy(clip);
    ImDrawList_PopClipRect(dl);
    /* The view is rarely a whole number of rows tall. Put the leftover below
     * the last row (as a terminal does), so that scrolled to the bottom the
     * screen's first row sits at the top of the view, not a sliver of the
     * scrollback line above it. */
    float rem = view_h - (float)tb->rows * m.line_h;
    if (rem > 0)
        igDummy(v2(1, rem));


    if (t->scroll_to_line >= 0) {
        float target = t->scroll_to_line * m.line_h - view_h * 0.4f;
        igSetScrollY_Float(maxf(target, 0));
        t->scroll_to_line = -1;
    } else if (t->follow) {
        igSetScrollY_Float((float)nlines * m.line_h);
    }

    /* Context menu */
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(6), S(6)));
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, v2(S(8), S(2)));
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, v2(S(8), S(5)));
    if (igBeginPopupContextWindow("##termctx", ImGuiPopupFlags_MouseButtonRight)) {
        ui_menu_push();
        bool connected = session_state(t->session) == LINK_CONNECTED;
        if (igMenuItem_Bool("Copy", "Ctrl+Shift+C", false, t->has_sel))
            termview_copy_selection(t);
        if (igMenuItem_Bool("Paste", "Ctrl+Shift+V", false, connected))
            termview_paste(a, t);
        if (igMenuItem_Bool("Select All", "Ctrl+Shift+A", false, true))
            termview_select_all(t);
        igSeparator();
        if (igMenuItem_Bool("Find\xE2\x80\xA6", "Ctrl+Shift+F", false, true)) {
            t->find_open = true;
            t->find_focus = true;
        }
        if (igMenuItem_Bool("Clear", "Ctrl+Shift+K", false, true))
            termbuf_clear(tb);
        ui_menu_pop();
        igEndPopup();
    }
    igPopStyleVar(3);

    if (focused)
        handle_keys(a, t);
    igEndChild();
    igPopStyleVar(3);

    /* A fixed console smaller than the window: mark its right and bottom edges. */
    if (a->console_fixed) {
        ImDrawList *pdl = igGetWindowDrawList();
        ImU32 edge = col_alpha(th->border, 0.9f);
        if (p1.x < avail1.x - 1)
            ImDrawList_AddLine(pdl, v2(p1.x, p0.y), v2(p1.x, p1.y), edge, 1.0f);
        if (p1.y < avail1.y - 1)
            ImDrawList_AddLine(pdl, v2(p0.x, p1.y), v2(p1.x, p1.y), edge, 1.0f);
    }

    /* Focus ring */
    if (focused)
        ImDrawList_AddRect(igGetWindowDrawList(), v2(p0.x - 1, p0.y), v2(p1.x + 1, p1.y + 1), col_alpha(th->focus, 0.0f),
                           S(8), 1.0f, ImDrawFlags_RoundCornersBottom);

    /* "Jump to bottom" pill */
    uint64_t unseen = tb->total_added - t->seen_total;
    if (!t->follow) {
        char label[64];
        if (unseen)
            snprintf(label, sizeof label, "%llu new line%s", (unsigned long long)unseen, unseen == 1 ? "" : "s");
        else
            snprintf(label, sizeof label, "Jump to bottom");
        float fs = S(12);
        ImVec2 ts = ui_text_size(a->font_ui_bold, fs, label, NULL);
        float pw = ts.x + S(40), ph = S(26);
        ImVec2 q0 = v2((p0.x + p1.x - pw) * 0.5f, p1.y - ph - S(18));
        ImDrawList *fdl = igGetWindowDrawList();
        igSetCursorScreenPos(q0);
        if (igInvisibleButton("##jump", v2(pw, ph), 0)) {
            t->follow = true;
            t->focus_terminal = true;
        }
        bool hov = igIsItemHovered(0);
        ImDrawList_AddRectFilled(fdl, v2(q0.x, q0.y + S(2)), v2(q0.x + pw, q0.y + ph + S(2)), th->shadow, ph * 0.5f, 0);
        ImU32 bg = unseen ? th->badge_bg : th->widget_bg;
        if (hov)
            bg = col_lerp(bg, 0xFFFFFFFFu, 0.08f);
        ImDrawList_AddRectFilled(fdl, q0, v2(q0.x + pw, q0.y + ph), bg, ph * 0.5f, 0);
        if (!unseen)
            ImDrawList_AddRect(fdl, q0, v2(q0.x + pw, q0.y + ph), th->widget_border, ph * 0.5f, 1.0f, 0);
        ImU32 fg = unseen ? th->badge_fg : th->fg;
        ui_draw_icon(fdl, v2(q0.x + S(16), q0.y + ph * 0.5f), ICON_ARROW_DOWN, fg, S(14));
        ui_text(fdl, a->font_ui_bold, fs, v2(q0.x + S(28), q0.y + (ph - ts.y) * 0.5f), fg, label, NULL);
    }

    if (t->find_open)
        find_bar(a, t, p0, p1);
}
