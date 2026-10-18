/* Scrollback + screen model and VT parser. See termbuf.h for the layout. */
#include "term/termbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAB_WIDTH 8
#define MAX_ROWS 500
#define MAX_COLS 1000
#define ALT_CAP (MAX_ROWS + 1)
#define TO_EOL 0x7FFFFFFF

enum { P_NORMAL, P_ESC, P_ESC_INTER, P_CSI, P_OSC, P_OSC_ESC, P_STR, P_STR_ESC };

static char s_spaces[MAX_COLS];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- Styles ---------------------------------------------------------------- */

static uint16_t intern_style(TermBuf *tb, TermStyle st)
{
    for (uint32_t i = 0; i < tb->nstyles; i++) {
        const TermStyle *s = &tb->styles[i];
        if (s->fg == st.fg && s->bg == st.bg && s->attr == st.attr)
            return (uint16_t)i;
    }
    if (tb->nstyles >= 65535)
        return 0;
    if (tb->nstyles == tb->styles_cap) {
        tb->styles_cap = tb->styles_cap ? tb->styles_cap * 2 : 32;
        tb->styles = realloc(tb->styles, tb->styles_cap * sizeof *tb->styles);
    }
    tb->styles[tb->nstyles] = st;
    return (uint16_t)tb->nstyles++;
}

const TermStyle *termbuf_style(const TermBuf *tb, uint16_t id)
{
    return &tb->styles[id < tb->nstyles ? id : 0];
}

/* Erased cells keep the current background color (xterm's BCE). */
static uint16_t erase_style(TermBuf *tb)
{
    if (tb->cur.bg == TC_DEFAULT)
        return 0;
    TermStyle s = {0};
    s.bg = tb->cur.bg;
    return intern_style(tb, s);
}

/* ---- Lines ----------------------------------------------------------------- */

static void free_line(TermLine *l)
{
    free(l->text);
    free(l->sty);
    memset(l, 0, sizeof *l);
}

/* Empties a line but keeps its text buffer for reuse. */
static void reset_line(TermLine *l, TermLineKind kind, double time)
{
    l->len = 0;
    l->cells = 0;
    free(l->sty);
    l->sty = NULL;
    l->style0 = 0;
    l->kind = (uint8_t)kind;
    l->flags = 0;
    l->time = time;
}

static void line_reserve(TermLine *l, uint32_t need)
{
    if (need <= l->cap)
        return;
    uint32_t cap = l->cap ? l->cap : 64;
    while (cap < need)
        cap *= 2;
    l->text = realloc(l->text, cap);
    if (l->sty)
        l->sty = realloc(l->sty, cap * sizeof *l->sty);
    l->cap = cap;
}

static uint32_t count_cells(const char *s, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        c += ((uint8_t)s[i] & 0xC0) != 0x80;
    return c;
}

/* Byte offset of cell `col`, or l->len when the line has no such cell. */
static uint32_t cell_off(const TermLine *l, int col)
{
    if (col <= 0)
        return 0;
    if ((uint32_t)col >= l->cells)
        return l->len;
    if (l->cells == l->len)
        return (uint32_t)col; /* pure ASCII */
    int c = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        if (((uint8_t)l->text[i] & 0xC0) != 0x80) {
            if (c == col)
                return i;
            c++;
        }
    }
    return l->len;
}

/* Replaces bytes [a, b) of l with n bytes of src, all in `style`. */
static void line_splice(TermBuf *tb, TermLine *l, uint32_t a, uint32_t b, const char *src, uint32_t n, uint16_t style)
{
    uint32_t removed = b - a;
    uint32_t newlen = l->len - removed + n;
    uint32_t old_cells = removed ? count_cells(l->text + a, removed) : 0;
    bool whole = a == 0 && b >= l->len;
    line_reserve(l, newlen ? newlen : 1);
    if (whole && n) {
        free(l->sty);
        l->sty = NULL;
        l->style0 = style;
    } else if (!l->sty && n && style != l->style0) {
        l->sty = malloc(l->cap * sizeof *l->sty);
        for (uint32_t i = 0; i < l->cap; i++)
            l->sty[i] = l->style0;
    }
    if (b < l->len) {
        memmove(l->text + a + n, l->text + b, l->len - b);
        if (l->sty)
            memmove(l->sty + a + n, l->sty + b, (l->len - b) * sizeof *l->sty);
    }
    if (n) {
        memcpy(l->text + a, src, n);
        if (l->sty)
            for (uint32_t i = a; i < a + n; i++)
                l->sty[i] = style;
    }
    /* A line's timestamp is when its current content started: an erased-empty
     * line has none, and gets a new one when written again. */
    if (newlen == 0)
        l->time = 0;
    else if (l->len == 0)
        l->time = tb->now;
    l->len = newlen;
    l->cells = l->cells - old_cells + count_cells(src ? src : "", n);
    if (l->cells > tb->max_cells)
        tb->max_cells = l->cells;
    tb->version++;
}

/* Extends a line with default-styled spaces so it has at least `col` cells. */
static void line_pad(TermBuf *tb, TermLine *l, int col)
{
    if ((int)l->cells >= col)
        return;
    line_splice(tb, l, l->len, l->len, s_spaces, (uint32_t)col - l->cells, 0);
}

/* ---- Screens (rings of lines) ---------------------------------------------- */

static void screen_alloc(TermScreen *s, uint32_t cap)
{
    memset(s, 0, sizeof *s);
    s->cap = cap;
    s->lines = calloc(cap, sizeof *s->lines);
    s->saved.autowrap = true;
}

static void screen_free(TermScreen *s)
{
    for (uint32_t i = 0; i < s->cap; i++)
        free_line(&s->lines[i]);
    free(s->lines);
    memset(s, 0, sizeof *s);
}

static TermLine *screen_line(const TermScreen *s, uint64_t abs)
{
    if (abs < s->first_abs || abs >= s->first_abs + s->count)
        return NULL;
    return &s->lines[(s->head + (uint32_t)(abs - s->first_abs)) % s->cap];
}

TermLine *termbuf_line(TermBuf *tb, uint64_t abs) { return screen_line(tb->scr, abs); }

static TermLine *screen_append(TermBuf *tb, TermScreen *s, TermLineKind kind)
{
    uint32_t idx;
    if (s->count < s->cap) {
        idx = (s->head + s->count) % s->cap;
        s->count++;
    } else {
        idx = s->head;
        s->head = (s->head + 1) % s->cap;
        s->first_abs++;
    }
    TermLine *l = &s->lines[idx];
    reset_line(l, kind, tb->now);
    if (s == &tb->main)
        tb->total_added++;
    tb->version++;
    return l;
}

/* Drops lines before absolute index `upto`. */
static void screen_drop_front(TermScreen *s, uint64_t upto)
{
    while (s->count && s->first_abs < upto) {
        reset_line(&s->lines[s->head], TL_DATA, 0);
        s->head = (s->head + 1) % s->cap;
        s->first_abs++;
        s->count--;
    }
    if (!s->count && s->first_abs < upto)
        s->first_abs = upto;
}

/* Drops lines at absolute index `from` and later. */
static void screen_drop_back(TermScreen *s, uint64_t from)
{
    while (s->count && s->first_abs + s->count > from) {
        s->count--;
        reset_line(&s->lines[(s->head + s->count) % s->cap], TL_DATA, 0);
    }
}

/* Screen row -> line, creating blank lines up to it as needed. */
static TermLine *row_line(TermBuf *tb, int row)
{
    TermScreen *s = tb->scr;
    if (s->top < s->first_abs)
        s->top = s->first_abs;
    while (s->first_abs + s->count <= s->top + (uint64_t)row)
        screen_append(tb, s, TL_DATA);
    return screen_line(s, s->top + (uint64_t)row);
}

/* Screen row -> line, or NULL if nothing was written there yet. */
static TermLine *row_line_if(TermBuf *tb, int row) { return screen_line(tb->scr, tb->scr->top + (uint64_t)row); }

static uint32_t row_idx(const TermScreen *s, int row)
{
    return (s->head + (uint32_t)(s->top + (uint64_t)row - s->first_abs)) % s->cap;
}

static void blank_row(TermBuf *tb, TermLine *l)
{
    reset_line(l, TL_DATA, tb->now);
    uint16_t es = erase_style(tb);
    if (es)
        line_splice(tb, l, 0, 0, s_spaces, (uint32_t)tb->cols, es);
}

/* Rows t..b move up by n; blank rows appear at the bottom. Nothing goes to scrollback. */
static void rotate_up(TermBuf *tb, int t, int b, int n)
{
    TermScreen *s = tb->scr;
    if (n > b - t + 1)
        n = b - t + 1;
    if (n <= 0)
        return;
    row_line(tb, b);
    for (int k = 0; k < n; k++) {
        TermLine tmp = s->lines[row_idx(s, t)];
        for (int r = t; r < b; r++)
            s->lines[row_idx(s, r)] = s->lines[row_idx(s, r + 1)];
        s->lines[row_idx(s, b)] = tmp;
        blank_row(tb, &s->lines[row_idx(s, b)]);
    }
    tb->version++;
}

/* Rows t..b move down by n; blank rows appear at the top. */
static void rotate_down(TermBuf *tb, int t, int b, int n)
{
    TermScreen *s = tb->scr;
    if (n > b - t + 1)
        n = b - t + 1;
    if (n <= 0)
        return;
    row_line(tb, b);
    for (int k = 0; k < n; k++) {
        TermLine tmp = s->lines[row_idx(s, b)];
        for (int r = b; r > t; r--)
            s->lines[row_idx(s, r)] = s->lines[row_idx(s, r - 1)];
        s->lines[row_idx(s, t)] = tmp;
        blank_row(tb, &s->lines[row_idx(s, t)]);
    }
    tb->version++;
}

/* Scrolls the scroll region up. With the region covering the whole screen,
 * the main screen's top lines become scrollback (the alternate screen has
 * none, so they are dropped). */
static void scroll_up(TermBuf *tb, int n)
{
    TermScreen *s = tb->scr;
    if (n > tb->rows)
        n = tb->rows;
    if (tb->scroll_top == 0 && tb->scroll_bot == tb->rows - 1) {
        s->top += (uint64_t)n;
        if (s == &tb->alt)
            screen_drop_front(s, s->top);
        tb->version++;
        return;
    }
    rotate_up(tb, tb->scroll_top, tb->scroll_bot, n);
}

static void linefeed(TermBuf *tb)
{
    TermScreen *s = tb->scr;
    s->wrap_pending = false;
    if (s->row == tb->scroll_bot)
        scroll_up(tb, 1);
    else if (s->row < tb->rows - 1)
        s->row++;
}

static void reverse_linefeed(TermBuf *tb)
{
    TermScreen *s = tb->scr;
    s->wrap_pending = false;
    if (s->row == tb->scroll_top)
        rotate_down(tb, tb->scroll_top, tb->scroll_bot, 1);
    else if (s->row > 0)
        s->row--;
}

/* ---- Editing ---------------------------------------------------------------- */

/* Blanks cells [c0, c1) of a row; c1 == TO_EOL erases to the end of the line. */
static void erase_cells(TermBuf *tb, int row, int c0, int c1)
{
    uint16_t es = erase_style(tb);
    TermLine *l = es ? row_line(tb, row) : row_line_if(tb, row);
    if (!l)
        return;
    if (c1 >= tb->cols) {
        uint32_t a = cell_off(l, c0);
        if (a < l->len)
            line_splice(tb, l, a, l->len, NULL, 0, 0);
        l->flags &= (uint8_t)~TLF_WRAPPED;
        if (es && c0 < tb->cols) {
            line_pad(tb, l, c0);
            line_splice(tb, l, l->len, l->len, s_spaces, (uint32_t)(tb->cols - c0), es);
        }
        tb->version++;
        return;
    }
    int end = c1;
    if (!es && end > (int)l->cells)
        end = (int)l->cells;
    if (end <= c0)
        return;
    line_pad(tb, l, c0);
    uint32_t a = cell_off(l, c0), b = cell_off(l, end);
    line_splice(tb, l, a, b, s_spaces, (uint32_t)(end - c0), es);
}

static void erase_display(TermBuf *tb, int mode)
{
    TermScreen *s = tb->scr;
    /* Erasing to the end from the top-left corner is a whole-screen clear
     * (that's how BusyBox `clear` and TERM=vt100 do it), like PuTTY. */
    if (mode == 0 && s->row == 0 && s->col == 0)
        mode = 2;
    if (mode == 0) {
        erase_cells(tb, s->row, s->col, TO_EOL);
        for (int r = s->row + 1; r < tb->rows; r++)
            erase_cells(tb, r, 0, TO_EOL);
    } else if (mode == 1) {
        for (int r = 0; r < s->row; r++)
            erase_cells(tb, r, 0, TO_EOL);
        erase_cells(tb, s->row, 0, s->col + 1);
    } else if (mode == 2) {
        if (s == &tb->main) {
            /* Keep what was on screen in the scrollback rather than wiping it. */
            uint64_t end = s->first_abs + s->count;
            if (end > s->top)
                s->top = end;
            if (erase_style(tb))
                for (int r = 0; r < tb->rows; r++)
                    erase_cells(tb, r, 0, TO_EOL);
            tb->version++;
        } else {
            for (int r = 0; r < tb->rows; r++)
                erase_cells(tb, r, 0, TO_EOL);
        }
    }
    /* mode 3 (erase scrollback) is ignored: the serial log is worth keeping. */
}

static void insert_cells(TermBuf *tb, int n)
{
    TermScreen *s = tb->scr;
    TermLine *l = row_line_if(tb, s->row);
    if (!l || (int)l->cells <= s->col)
        return;
    if (n > tb->cols - s->col)
        n = tb->cols - s->col;
    uint32_t a = cell_off(l, s->col);
    line_splice(tb, l, a, a, s_spaces, (uint32_t)n, erase_style(tb));
    if ((int)l->cells > tb->cols) {
        uint32_t cut = cell_off(l, tb->cols);
        line_splice(tb, l, cut, l->len, NULL, 0, 0);
    }
}

static void delete_cells(TermBuf *tb, int n)
{
    TermScreen *s = tb->scr;
    TermLine *l = row_line_if(tb, s->row);
    if (!l || (int)l->cells <= s->col)
        return;
    uint32_t a = cell_off(l, s->col), b = cell_off(l, s->col + n);
    line_splice(tb, l, a, b, NULL, 0, 0);
}

/* Writes ncells cells (nbytes of UTF-8) at the cursor. The caller makes sure
 * they fit before the right margin. */
static void put_cells(TermBuf *tb, const char *b, uint32_t nbytes, uint32_t ncells, uint16_t style)
{
    TermScreen *s = tb->scr;
    if (s->wrap_pending) {
        s->wrap_pending = false;
        if (tb->autowrap) {
            row_line(tb, s->row)->flags |= TLF_WRAPPED;
            linefeed(tb);
            s->col = 0;
        }
    }
    if (tb->insert)
        insert_cells(tb, (int)ncells);
    TermLine *l = row_line(tb, s->row);
    line_pad(tb, l, s->col);
    uint32_t a = cell_off(l, s->col), e = cell_off(l, s->col + (int)ncells);
    line_splice(tb, l, a, e, b, nbytes, style);
    s->col += (int)ncells;
    if (s->col >= tb->cols) {
        s->col = tb->cols - 1;
        s->wrap_pending = true;
    }
    /* Remember the last character for REP (CSI b). */
    uint32_t st = nbytes;
    do
        st--;
    while (st > 0 && ((uint8_t)b[st] & 0xC0) == 0x80);
    if (nbytes - st <= sizeof tb->last_ch) {
        memmove(tb->last_ch, b + st, nbytes - st); /* REP passes last_ch itself */
        tb->last_ch_len = (int)(nbytes - st);
    }
}

static void put_ascii_run(TermBuf *tb, const uint8_t *p, size_t n)
{
    while (n) {
        TermScreen *s = tb->scr;
        int room = s->wrap_pending ? (tb->autowrap ? tb->cols : 1) : tb->cols - s->col;
        size_t k = n < (size_t)room ? n : (size_t)room;
        put_cells(tb, (const char *)p, (uint32_t)k, (uint32_t)k, tb->cur_id);
        p += k;
        n -= k;
    }
}

static void put_codepoint(TermBuf *tb, uint32_t cp, uint16_t style)
{
    char u[4];
    uint32_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    put_cells(tb, u, n, 1, style);
}

/* Visible glyph for a stray control byte (Unicode "control pictures"). */
static void put_control(TermBuf *tb, uint8_t c)
{
    TermStyle st = tb->cur;
    st.attr |= TA_CONTROL;
    put_codepoint(tb, c == 0x7F ? 0x2421 : 0x2400u + c, intern_style(tb, st));
}

/* DEC Special Graphics (ESC ( 0): line drawing for 0x5F..0x7E. */
static uint32_t dec_graphics(uint8_t c)
{
    static const uint16_t map[32] = {
        0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
        0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
        0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
    };
    return c >= 0x5F && c <= 0x7E ? map[c - 0x5F] : c;
}

/* ---- Cursor and modes ------------------------------------------------------- */

static void set_cursor(TermBuf *tb, int row, int col)
{
    TermScreen *s = tb->scr;
    s->row = clampi(row, 0, tb->rows - 1);
    s->col = clampi(col, 0, tb->cols - 1);
    s->wrap_pending = false;
}

/* CUP-style absolute positioning, honoring origin mode. */
static void goto_rc(TermBuf *tb, int row, int col)
{
    if (tb->origin)
        row = clampi(row + tb->scroll_top, tb->scroll_top, tb->scroll_bot);
    set_cursor(tb, row, col);
}

static void save_cursor(TermBuf *tb)
{
    TermScreen *s = tb->scr;
    TermSaved *v = &s->saved;
    v->row = s->row;
    v->col = s->col;
    v->style = tb->cur;
    v->origin = tb->origin;
    v->autowrap = tb->autowrap;
    memcpy(v->g, tb->g, sizeof v->g);
    v->gl = tb->gl;
}

static void restore_cursor(TermBuf *tb)
{
    TermSaved *v = &tb->scr->saved;
    tb->cur = v->style;
    tb->cur_id = intern_style(tb, tb->cur);
    tb->origin = v->origin;
    tb->autowrap = v->autowrap;
    if (v->g[0])
        memcpy(tb->g, v->g, sizeof tb->g);
    tb->gl = v->gl;
    set_cursor(tb, v->row, v->col);
}

static void reset_modes(TermBuf *tb)
{
    tb->autowrap = true;
    tb->origin = false;
    tb->insert = false;
    tb->cursor_visible = true;
    tb->app_cursor = false;
    tb->bracketed_paste = false;
    tb->g[0] = tb->g[1] = 'B';
    tb->gl = 0;
    tb->scroll_top = 0;
    tb->scroll_bot = tb->rows - 1;
    memset(&tb->cur, 0, sizeof tb->cur);
    tb->cur_id = 0;
}

static void enter_alt(TermBuf *tb, bool save)
{
    if (tb->scr == &tb->alt)
        return;
    if (save)
        save_cursor(tb);
    TermScreen *a = &tb->alt;
    screen_drop_back(a, 0);
    a->head = 0;
    a->first_abs = a->top = 0;
    a->row = tb->main.row;
    a->col = tb->main.col;
    a->wrap_pending = false;
    tb->scr = a;
    tb->version++;
}

static void leave_alt(TermBuf *tb, bool restore)
{
    if (tb->scr != &tb->alt)
        return;
    tb->scr = &tb->main;
    if (restore)
        restore_cursor(tb);
    tb->version++;
}

static void set_dec_mode(TermBuf *tb, int mode, bool on)
{
    switch (mode) {
    case 1: tb->app_cursor = on; break;
    case 6:
        tb->origin = on;
        goto_rc(tb, 0, 0);
        break;
    case 7: tb->autowrap = on; break;
    case 25: tb->cursor_visible = on; break;
    case 47:
    case 1047:
        if (on)
            enter_alt(tb, false);
        else
            leave_alt(tb, false);
        break;
    case 1048:
        if (on)
            save_cursor(tb);
        else
            restore_cursor(tb);
        break;
    case 1049:
        if (on)
            enter_alt(tb, true);
        else
            leave_alt(tb, true);
        break;
    case 2004: tb->bracketed_paste = on; break;
    default: break;
    }
}

static void full_reset(TermBuf *tb)
{
    leave_alt(tb, false);
    reset_modes(tb);
    erase_display(tb, 2);
    set_cursor(tb, 0, 0);
    tb->main.saved = (TermSaved){0};
    tb->main.saved.autowrap = true;
}

static void reply(TermBuf *tb, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && tb->reply_len + n <= (int)sizeof tb->reply) {
        memcpy(tb->reply + tb->reply_len, buf, (size_t)n);
        tb->reply_len += n;
    }
}

/* ---- Parser ---------------------------------------------------------------- */

static int parse_params(const char *s, int len, int *out, int max)
{
    int n = 0, v = 0;
    bool have = false;
    for (int i = 0; i <= len; i++) {
        char c = i < len ? s[i] : ';';
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            if (v > 65535)
                v = 65535;
            have = true;
        } else if (c == ';' || c == ':') {
            if (n < max)
                out[n++] = have ? v : 0;
            v = 0;
            have = false;
        }
    }
    return n;
}

static void apply_sgr(TermBuf *tb, const int *p, int n)
{
    TermStyle *s = &tb->cur;
    if (n == 0) {
        memset(s, 0, sizeof *s);
        return;
    }
    for (int i = 0; i < n; i++) {
        int c = p[i];
        if (c == 0)
            memset(s, 0, sizeof *s);
        else if (c == 1)
            s->attr |= TA_BOLD;
        else if (c == 2)
            s->attr |= TA_DIM;
        else if (c == 3)
            s->attr |= TA_ITALIC;
        else if (c == 4 || c == 21)
            s->attr |= TA_UNDERLINE;
        else if (c == 7)
            s->attr |= TA_INVERSE;
        else if (c == 22)
            s->attr &= (uint8_t)~(TA_BOLD | TA_DIM);
        else if (c == 23)
            s->attr &= (uint8_t)~TA_ITALIC;
        else if (c == 24)
            s->attr &= (uint8_t)~TA_UNDERLINE;
        else if (c == 27)
            s->attr &= (uint8_t)~TA_INVERSE;
        else if (c >= 30 && c <= 37)
            s->fg = TC_PAL(c - 30);
        else if (c == 39)
            s->fg = TC_DEFAULT;
        else if (c >= 40 && c <= 47)
            s->bg = TC_PAL(c - 40);
        else if (c == 49)
            s->bg = TC_DEFAULT;
        else if (c >= 90 && c <= 97)
            s->fg = TC_PAL(c - 90 + 8);
        else if (c >= 100 && c <= 107)
            s->bg = TC_PAL(c - 100 + 8);
        else if (c == 38 || c == 48) {
            TermColor col = TC_DEFAULT;
            if (i + 2 < n && p[i + 1] == 5) {
                col = TC_PAL(p[i + 2] & 0xFF);
                i += 2;
            } else if (i + 4 < n && p[i + 1] == 2) {
                col = TC_RGB(p[i + 2] & 0xFF, p[i + 3] & 0xFF, p[i + 4] & 0xFF);
                i += 4;
            } else {
                break;
            }
            if (c == 38)
                s->fg = col;
            else
                s->bg = col;
        }
    }
}

static void handle_csi(TermBuf *tb, char final)
{
    TermScreen *s = tb->scr;
    char priv = 0, inter = 0;
    const char *q = tb->seq;
    int qlen = tb->seq_len;
    if (qlen && strchr("<=>?", q[0])) {
        priv = q[0];
        q++;
        qlen--;
    }
    for (int i = 0; i < qlen; i++)
        if (q[i] >= 0x20 && q[i] <= 0x2F)
            inter = q[i];
    int p[32];
    int n = parse_params(q, qlen, p, 32);
#define ARG(i, d) (n > (i) && p[i] ? p[i] : (d))

    if (inter) {
        if (inter == '!' && final == 'p') { /* DECSTR soft reset */
            reset_modes(tb);
            s->saved = (TermSaved){0};
            s->saved.autowrap = true;
        }
        return; /* DECSCUSR, DECRQM, ...: nothing to do */
    }
    if (priv == '?') {
        if (final == 'h' || final == 'l')
            for (int i = 0; i < n; i++)
                set_dec_mode(tb, p[i], final == 'h');
        return;
    }
    if (priv == '>') {
        if (final == 'c')
            reply(tb, "\x1b[>0;10;1c"); /* DA2: VT100, firmware 10 */
        return;
    }
    if (priv)
        return;

    int lo, hi;
    switch (final) {
    case '@': insert_cells(tb, ARG(0, 1)); break;
    case 'A':
    case 'F':
        lo = s->row >= tb->scroll_top ? tb->scroll_top : 0;
        set_cursor(tb, s->row - ARG(0, 1) < lo ? lo : s->row - ARG(0, 1), final == 'F' ? 0 : s->col);
        break;
    case 'B':
    case 'e':
    case 'E':
        hi = s->row <= tb->scroll_bot ? tb->scroll_bot : tb->rows - 1;
        set_cursor(tb, s->row + ARG(0, 1) > hi ? hi : s->row + ARG(0, 1), final == 'E' ? 0 : s->col);
        break;
    case 'C':
    case 'a': set_cursor(tb, s->row, s->col + ARG(0, 1)); break;
    case 'D': set_cursor(tb, s->row, s->col - ARG(0, 1)); break;
    case 'G':
    case '`': set_cursor(tb, s->row, ARG(0, 1) - 1); break;
    case 'H':
    case 'f': goto_rc(tb, ARG(0, 1) - 1, ARG(1, 1) - 1); break;
    case 'd': goto_rc(tb, ARG(0, 1) - 1, s->col); break;
    case 'I':
        for (int k = ARG(0, 1); k > 0; k--)
            set_cursor(tb, s->row, (s->col / TAB_WIDTH + 1) * TAB_WIDTH);
        break;
    case 'Z':
        for (int k = ARG(0, 1); k > 0; k--)
            set_cursor(tb, s->row, s->col > 0 ? ((s->col - 1) / TAB_WIDTH) * TAB_WIDTH : 0);
        break;
    case 'J': erase_display(tb, ARG(0, 0)); break;
    case 'K': {
        int m = ARG(0, 0);
        if (m == 0)
            erase_cells(tb, s->row, s->col, TO_EOL);
        else if (m == 1)
            erase_cells(tb, s->row, 0, s->col + 1);
        else if (m == 2)
            erase_cells(tb, s->row, 0, TO_EOL);
        break;
    }
    case 'L':
        if (s->row >= tb->scroll_top && s->row <= tb->scroll_bot) {
            rotate_down(tb, s->row, tb->scroll_bot, ARG(0, 1));
            set_cursor(tb, s->row, 0);
        }
        break;
    case 'M':
        if (s->row >= tb->scroll_top && s->row <= tb->scroll_bot) {
            rotate_up(tb, s->row, tb->scroll_bot, ARG(0, 1));
            set_cursor(tb, s->row, 0);
        }
        break;
    case 'P': delete_cells(tb, ARG(0, 1)); break;
    case 'X': erase_cells(tb, s->row, s->col, s->col + ARG(0, 1)); break;
    case 'S': scroll_up(tb, ARG(0, 1)); break;
    case 'T':
        if (n <= 1)
            rotate_down(tb, tb->scroll_top, tb->scroll_bot, ARG(0, 1));
        break;
    case 'b':
        if (tb->last_ch_len)
            for (int k = clampi(ARG(0, 1), 1, MAX_COLS); k > 0; k--)
                put_cells(tb, tb->last_ch, (uint32_t)tb->last_ch_len, 1, tb->cur_id);
        break;
    case 'm':
        apply_sgr(tb, p, n);
        tb->cur_id = intern_style(tb, tb->cur);
        break;
    case 'n':
        if (ARG(0, 0) == 5)
            reply(tb, "\x1b[0n");
        else if (ARG(0, 0) == 6)
            reply(tb, "\x1b[%d;%dR", s->row - (tb->origin ? tb->scroll_top : 0) + 1, s->col + 1);
        break;
    case 'c':
        if (ARG(0, 0) == 0)
            reply(tb, "\x1b[?62;22c"); /* DA1: VT220 with ANSI color */
        break;
    case 'r': {
        int t = ARG(0, 1) - 1, b = ARG(1, tb->rows) - 1;
        if (b > tb->rows - 1)
            b = tb->rows - 1;
        if (t < b) {
            tb->scroll_top = t;
            tb->scroll_bot = b;
            goto_rc(tb, 0, 0);
        }
        break;
    }
    case 's': save_cursor(tb); break;
    case 'u': restore_cursor(tb); break;
    case 't':
        if (ARG(0, 0) == 18)
            reply(tb, "\x1b[8;%d;%dt", tb->rows, tb->cols);
        break;
    case 'h':
    case 'l':
        for (int i = 0; i < n; i++)
            if (p[i] == 4)
                tb->insert = final == 'h';
        break;
    default: break;
    }
#undef ARG
}

static void handle_esc(TermBuf *tb, uint8_t c)
{
    tb->pstate = P_NORMAL;
    switch (c) {
    case '[':
        tb->pstate = P_CSI;
        tb->seq_len = 0;
        break;
    case ']': tb->pstate = P_OSC; break;
    case 'P': /* DCS */
    case 'X': /* SOS */
    case '^': /* PM */
    case '_': /* APC */ tb->pstate = P_STR; break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
    case '#':
    case '%':
    case ' ':
        tb->esc_inter = (char)c;
        tb->pstate = P_ESC_INTER;
        break;
    case '7': save_cursor(tb); break;
    case '8': restore_cursor(tb); break;
    case 'D': linefeed(tb); break;
    case 'E':
        linefeed(tb);
        tb->scr->col = 0;
        break;
    case 'M': reverse_linefeed(tb); break;
    case 'c': full_reset(tb); break;
    default: break; /* ESC = / ESC > (keypad modes) and the rest */
    }
}

static void handle_control(TermBuf *tb, uint8_t c, bool was_cr)
{
    TermScreen *s = tb->scr;
    bool on_main = s == &tb->main;
    switch (c) {
    case 0x1B: tb->pstate = P_ESC; break;
    case '\n':
    case '\v':
    case '\f':
        if (on_main) {
            /* On the main screen LF also returns to column 0: embedded devices
             * commonly send bare LF line endings. */
            if (tb->nl_mode == TB_NL_ANY && was_cr)
                break;
            linefeed(tb);
            s->col = 0;
        } else {
            linefeed(tb);
        }
        break;
    case '\r':
        s->col = 0;
        s->wrap_pending = false;
        if (on_main && tb->nl_mode == TB_NL_ANY)
            linefeed(tb);
        break;
    case '\b':
        if (s->col > 0)
            s->col--;
        s->wrap_pending = false;
        break;
    case '\t':
        set_cursor(tb, s->row, (s->col / TAB_WIDTH + 1) * TAB_WIDTH);
        break;
    case 0x07:
    case 0x18:
    case 0x1A: break;
    case 0x0E: tb->gl = 1; break;
    case 0x0F: tb->gl = 0; break;
    default:
        if (on_main)
            put_control(tb, c);
        break;
    }
}

void termbuf_feed(TermBuf *tb, const uint8_t *data, size_t len, double time)
{
    tb->now = time;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (tb->pstate) {
        case P_ESC: handle_esc(tb, c); continue;
        case P_ESC_INTER:
            if (tb->esc_inter == '(')
                tb->g[0] = c;
            else if (tb->esc_inter == ')')
                tb->g[1] = c;
            tb->pstate = P_NORMAL;
            continue;
        case P_CSI:
            if (c >= 0x40 && c <= 0x7E) {
                tb->pstate = P_NORMAL;
                handle_csi(tb, (char)c);
            } else if (c == 0x18 || c == 0x1A) {
                tb->pstate = P_NORMAL;
            } else if (c == 0x1B) {
                tb->pstate = P_ESC;
            } else if (tb->seq_len < (int)sizeof tb->seq - 1) {
                tb->seq[tb->seq_len++] = (char)c;
            }
            continue;
        case P_OSC:
            if (c == 0x07)
                tb->pstate = P_NORMAL;
            else if (c == 0x1B)
                tb->pstate = P_OSC_ESC;
            continue;
        case P_OSC_ESC: tb->pstate = P_NORMAL; continue;
        case P_STR:
            if (c == 0x1B)
                tb->pstate = P_STR_ESC;
            else if (c == 0x07 || c == 0x18 || c == 0x1A)
                tb->pstate = P_NORMAL;
            continue;
        case P_STR_ESC: tb->pstate = c == '\\' ? P_NORMAL : P_STR; continue;
        default: break;
        }

        if (tb->u8_need) {
            if ((c & 0xC0) == 0x80) {
                tb->u8[tb->u8_len++] = c;
                if (tb->u8_len == tb->u8_need) {
                    tb->u8_need = 0;
                    put_cells(tb, (const char *)tb->u8, (uint32_t)tb->u8_len, 1, tb->cur_id);
                }
                continue;
            }
            tb->u8_need = 0; /* truncated sequence; c is handled below */
            put_codepoint(tb, 0xFFFD, tb->cur_id);
        }

        bool was_cr = tb->last_cr;
        tb->last_cr = c == '\r';
        if (c >= 0x20 && c < 0x7F) {
            if (tb->g[tb->gl] == '0') {
                put_codepoint(tb, dec_graphics(c), tb->cur_id);
                continue;
            }
            /* Fast path: a run of printable ASCII in one go. */
            size_t j = i + 1;
            while (j < len && data[j] >= 0x20 && data[j] < 0x7F)
                j++;
            put_ascii_run(tb, data + i, j - i);
            i = j - 1;
            continue;
        }
        if (c >= 0x80) {
            if (c >= 0xC2 && c <= 0xF4) {
                tb->u8[0] = c;
                tb->u8_len = 1;
                tb->u8_need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            } else {
                put_codepoint(tb, 0xFFFD, tb->cur_id);
            }
            continue;
        }
        handle_control(tb, c, was_cr);
    }
}

/* ---- Public API ---------------------------------------------------------- */

void termbuf_init(TermBuf *tb, uint32_t max_lines)
{
    memset(tb, 0, sizeof *tb);
    memset(s_spaces, ' ', sizeof s_spaces);
    screen_alloc(&tb->main, max_lines < 16 ? 16 : max_lines);
    screen_alloc(&tb->alt, ALT_CAP);
    tb->scr = &tb->main;
    TermStyle def = {0};
    intern_style(tb, def);
    tb->cols = 80;
    tb->rows = clampi(24, 2, (int)tb->main.cap - 1);
    reset_modes(tb);
}

void termbuf_free(TermBuf *tb)
{
    screen_free(&tb->main);
    screen_free(&tb->alt);
    free(tb->styles);
    memset(tb, 0, sizeof *tb);
}

void termbuf_clear(TermBuf *tb)
{
    TermScreen *screens[2] = {&tb->main, &tb->alt};
    for (int i = 0; i < 2; i++) {
        TermScreen *s = screens[i];
        screen_drop_front(s, s->first_abs + s->count);
        s->top = s->first_abs;
        s->row = s->col = 0;
        s->wrap_pending = false;
    }
    tb->max_cells = 0;
    tb->version++;
}

void termbuf_set_capacity(TermBuf *tb, uint32_t max_lines)
{
    TermScreen *s = &tb->main;
    if (max_lines < 16)
        max_lines = 16;
    if (max_lines == s->cap)
        return;
    TermLine *nl = calloc(max_lines, sizeof *nl);
    uint32_t keep = s->count < max_lines ? s->count : max_lines;
    uint32_t drop = s->count - keep;
    for (uint32_t i = 0; i < s->count; i++) {
        TermLine *l = &s->lines[(s->head + i) % s->cap];
        if (i < drop)
            free_line(l);
        else
            nl[i - drop] = *l;
    }
    for (uint32_t i = s->count; i < s->cap; i++)
        free_line(&s->lines[(s->head + i) % s->cap]);
    free(s->lines);
    s->lines = nl;
    s->cap = max_lines;
    s->head = 0;
    s->count = keep;
    s->first_abs += drop;
    if (s->top < s->first_abs) {
        uint64_t d = s->first_abs - s->top;
        s->top = s->first_abs;
        s->row = (uint64_t)s->row > d ? s->row - (int)d : 0;
    }
    if (tb->rows > (int)max_lines - 1)
        termbuf_resize(tb, (int)max_lines - 1, tb->cols);
    tb->version++;
}

void termbuf_resize(TermBuf *tb, int rows, int cols)
{
    rows = clampi(rows, 2, MAX_ROWS);
    cols = clampi(cols, 2, MAX_COLS);
    if (rows > (int)tb->main.cap - 1)
        rows = (int)tb->main.cap - 1;
    if (rows == tb->rows && cols == tb->cols)
        return;

    /* Main screen: keep the bottom anchored, pulling scrollback down when it
     * grows. The cursor, and the saved cursor that ?1049l / ESC 8 restore,
     * stay on the same lines of text; only their screen rows change. */
    TermScreen *m = &tb->main;
    uint64_t cabs = m->top + (uint64_t)m->row, end = m->first_abs + m->count;
    uint64_t saved_abs = m->top + (uint64_t)m->saved.row;
    uint64_t bottom = end > cabs + 1 ? end : cabs + 1;
    uint64_t top = bottom > (uint64_t)rows ? bottom - (uint64_t)rows : 0;
    if (top < m->first_abs)
        top = m->first_abs;
    if (top > cabs)
        top = cabs;
    m->top = top;
    m->row = clampi((int)(cabs - top), 0, rows - 1);
    m->saved.row = saved_abs < top ? 0 : clampi((int)(saved_abs - top), 0, rows - 1);

    /* Alternate screen: keep the top; rows that no longer fit are dropped. */
    TermScreen *a = &tb->alt;
    screen_drop_back(a, a->top + (uint64_t)rows);
    a->row = clampi(a->row, 0, rows - 1);
    a->saved.row = clampi(a->saved.row, 0, rows - 1);

    TermScreen *screens[2] = {m, a};
    for (int i = 0; i < 2; i++) {
        TermScreen *s = screens[i];
        /* A cursor past the new right margin waits to wrap, so the next
         * character starts a new line instead of overwriting the text under
         * the margin. A pending wrap at the old margin moves on one column
         * when there is room now. */
        if (s->col >= cols) {
            s->col = cols - 1;
            s->wrap_pending = true;
        } else if (s->wrap_pending && s->col == tb->cols - 1 && s->col + 1 < cols) {
            s->col++;
            s->wrap_pending = false;
        }
        s->saved.col = clampi(s->saved.col, 0, cols - 1);
    }
    tb->rows = rows;
    tb->cols = cols;
    tb->scroll_top = 0;
    tb->scroll_bot = rows - 1;
    tb->version++;
}

void termbuf_add_marker(TermBuf *tb, TermLineKind kind, const char *text, double time)
{
    tb->now = time;
    /* Whatever ran full-screen is gone or out of sync after a connection event.
     * The main screen's cursor was left untouched while the alt screen was up. */
    leave_alt(tb, false);
    TermScreen *s = &tb->main;
    uint64_t cabs = s->top + (uint64_t)s->row, end = s->first_abs + s->count;
    TermLine *l = end > s->first_abs ? screen_line(s, end - 1) : NULL;
    if (l && end - 1 >= cabs && l->len == 0 && l->kind == TL_DATA) {
        /* reuse an empty line at the cursor */
        reset_line(l, kind, time);
    } else {
        l = screen_append(tb, s, kind);
        end++;
    }
    uint32_t n = (uint32_t)strlen(text);
    line_reserve(l, n ? n : 1);
    memcpy(l->text, text, n);
    l->len = n;
    l->cells = count_cells(text, n);
    /* (markers don't count toward max_cells: they're drawn to fit the view) */

    /* The cursor continues on a fresh line below the marker. */
    cabs = s->first_abs + s->count;
    if (cabs - s->top >= (uint64_t)tb->rows)
        s->top = cabs - (uint64_t)tb->rows + 1;
    s->row = (int)(cabs - s->top);
    s->col = 0;
    s->wrap_pending = false;
    tb->version++;
}

void termbuf_add_tx(TermBuf *tb, const uint8_t *data, size_t len, double time)
{
    /* Echo goes into the same stream as received text (so it lands after a
     * prompt) but in its own style. Line endings are normalized so a CR-only
     * EOL doesn't rewind the cursor over the echoed text. */
    tb->now = time;
    TermStyle st = {0};
    st.attr = TA_TX;
    uint16_t id = intern_style(tb, st);
    TermScreen *s = tb->scr;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r') {
            if (i + 1 < len && data[i + 1] == '\n')
                continue;
            linefeed(tb);
            s->col = 0;
        } else if (c == '\n') {
            linefeed(tb);
            s->col = 0;
        } else if (c == 0x7F || c == '\b') {
            if (s->col > 0)
                s->col--;
            s->wrap_pending = false;
        } else if (c == 0x1B) {
            /* Escape sequences (arrow keys etc.) aren't echoed: skip ESC x or
             * a whole CSI sequence up to its final byte. */
            if (i + 1 < len && data[i + 1] == '[') {
                i += 2;
                while (i < len && !(data[i] >= 0x40 && data[i] <= 0x7E))
                    i++;
            } else if (i + 1 < len) {
                i++;
            }
        } else if (c < 0x20) {
            /* other control codes (Ctrl+C etc.) aren't echoed */
        } else {
            size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (i + n > len)
                n = len - i;
            put_cells(tb, (const char *)data + i, (uint32_t)n, 1, id);
            i += n - 1;
        }
    }
    tb->last_cr = false;
}
