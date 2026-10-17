/* Scrollback and screen for one session, with a VT100/xterm-style parser.
 *
 * Storage is a ring of variable-length styled UTF-8 lines. The terminal
 * "screen" is a rows x cols window onto the bottom of that ring: the cursor
 * is addressed by (row, col) cells relative to screen.top, and lines that
 * scroll off the top of the screen simply remain in the ring as scrollback.
 * Full-screen programs get a separate alternate screen (no scrollback).
 * Every code point is one cell wide. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Colors: 0 = theme default, palette index, or 24-bit RGB. */
typedef uint32_t TermColor;
#define TC_DEFAULT 0u
#define TC_PAL(i) (0x01000000u | (uint32_t)(i))
#define TC_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define TC_KIND(c) ((c) >> 24)

enum {
    TA_BOLD = 1 << 0,
    TA_DIM = 1 << 1,
    TA_ITALIC = 1 << 2,
    TA_UNDERLINE = 1 << 3,
    TA_INVERSE = 1 << 4,
    TA_CONTROL = 1 << 5, /* control-character placeholder glyph */
    TA_TX = 1 << 6,      /* local echo of sent data */
};

typedef struct {
    TermColor fg, bg;
    uint8_t attr;
} TermStyle;

typedef enum {
    TL_DATA,  /* received text */
    TL_TX,    /* (unused; local echo is inline text with TA_TX) */
    TL_INFO,  /* connection markers */
    TL_OK,
    TL_WARN,
    TL_ERROR,
} TermLineKind;

enum {
    TLF_WRAPPED = 1 << 0, /* soft-wrapped: the text continues on the next line */
};

typedef struct {
    char *text;
    uint16_t *sty; /* per-byte style ids, or NULL when every byte uses style0 */
    uint32_t len, cap;
    uint32_t cells; /* code points in text; == len when the line is pure ASCII */
    uint16_t style0;
    uint8_t kind;
    uint8_t flags; /* TLF_* */
    double time;
} TermLine;

typedef enum {
    TB_NL_TERMINAL, /* LF = new line, CR = back to column 0 (progress bars work) */
    TB_NL_ANY,      /* CR, LF and CRLF all end a line */
} TermNewlineMode;

typedef struct {
    int row, col;
    TermStyle style;
    bool origin, autowrap;
    uint8_t g[2], gl;
} TermSaved;

/* A ring of lines plus the screen window onto it. */
typedef struct {
    TermLine *lines;
    uint32_t cap, head, count;
    uint64_t first_abs; /* absolute index of the oldest retained line */
    uint64_t top;       /* absolute index of screen row 0 */
    int row, col;       /* cursor */
    bool wrap_pending;  /* cursor sits past the last column; next char wraps */
    TermSaved saved;    /* DECSC */
} TermScreen;

typedef struct {
    TermScreen main, alt;
    TermScreen *scr; /* active: &main or &alt */

    TermStyle *styles;
    uint32_t nstyles, styles_cap;

    int rows, cols;
    int scroll_top, scroll_bot; /* scroll region rows, inclusive */

    /* modes */
    bool autowrap;        /* DECAWM */
    bool origin;          /* DECOM */
    bool insert;          /* IRM */
    bool cursor_visible;  /* DECTCEM */
    bool app_cursor;      /* DECCKM: arrows send ESC O x */
    bool bracketed_paste; /* ?2004 */
    uint8_t g[2], gl;     /* G0/G1 charsets ('B' ASCII, '0' line drawing), active set */
    TermNewlineMode nl_mode;

    /* parser */
    int pstate;
    char seq[64];
    int seq_len;
    char esc_inter; /* ESC intermediate byte for charset designation */
    uint8_t u8[4];
    int u8_len, u8_need;
    TermStyle cur;
    uint16_t cur_id;
    bool last_cr;       /* previous byte was CR (for TB_NL_ANY) */
    char last_ch[4];    /* last printed character, for REP */
    int last_ch_len;
    double now;         /* timestamp for lines created by the current call */

    /* Replies to the device (cursor position reports, device attributes);
     * the owner sends and clears them after each feed. */
    char reply[256];
    int reply_len;

    uint32_t max_len;     /* longest line in bytes, for horizontal extent */
    uint64_t version;     /* bumps on every change */
    uint64_t total_added; /* lines ever created; drives "N new lines" */
} TermBuf;

void termbuf_init(TermBuf *tb, uint32_t max_lines);
void termbuf_free(TermBuf *tb);
void termbuf_clear(TermBuf *tb);
void termbuf_set_capacity(TermBuf *tb, uint32_t max_lines);
/* Sets the screen size in cells (the visible view). No-op if unchanged. */
void termbuf_resize(TermBuf *tb, int rows, int cols);

void termbuf_feed(TermBuf *tb, const uint8_t *data, size_t len, double time);
/* Adds a full-width marker line (connection events). Leaves the alternate screen. */
void termbuf_add_marker(TermBuf *tb, TermLineKind kind, const char *text, double time);
/* Adds local echo of transmitted data inline, styled with TA_TX. */
void termbuf_add_tx(TermBuf *tb, const uint8_t *data, size_t len, double time);

/* Line access on the active screen (main scrollback, or the alternate screen). */
static inline uint64_t termbuf_first_abs(const TermBuf *tb) { return tb->scr->first_abs; }
static inline uint32_t termbuf_count(const TermBuf *tb) { return tb->scr->count; }
static inline uint64_t termbuf_end_abs(const TermBuf *tb) { return tb->scr->first_abs + tb->scr->count; }
static inline bool termbuf_alt_active(const TermBuf *tb) { return tb->scr == &tb->alt; }
static inline uint64_t termbuf_cursor_line(const TermBuf *tb) { return tb->scr->top + (uint64_t)tb->scr->row; }
static inline int termbuf_cursor_col(const TermBuf *tb) { return tb->scr->col; }
TermLine *termbuf_line(TermBuf *tb, uint64_t abs);
const TermStyle *termbuf_style(const TermBuf *tb, uint16_t id);
static inline uint16_t termline_style_at(const TermLine *l, uint32_t i) { return l->sty ? l->sty[i] : l->style0; }
