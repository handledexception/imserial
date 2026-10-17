/* Unit tests for the scrollback/VT parser. */
#include "term/termbuf.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static void feed(TermBuf *tb, const char *s) { termbuf_feed(tb, (const uint8_t *)s, strlen(s), 0); }

static int text_is(const TermLine *l, const char *expect)
{
    size_t n = strlen(expect);
    return l ? l->len == n && memcmp(l->text, expect, n) == 0 : n == 0;
}

/* Line i counted from the oldest retained line. */
static int line_is(TermBuf *tb, uint64_t i, const char *expect)
{
    TermLine *l = termbuf_line(tb, termbuf_first_abs(tb) + i);
    return l && text_is(l, expect);
}

/* Screen row r (a row nothing was written to counts as empty). */
static int row_is(TermBuf *tb, int r, const char *expect)
{
    return text_is(termbuf_line(tb, tb->scr->top + (uint64_t)r), expect);
}

static int cursor_at(TermBuf *tb, int row, int col) { return tb->scr->row == row && tb->scr->col == col; }

static void screen_tests(void)
{
    TermBuf tb;

    /* cursor addressing and erase-display on a small screen */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 5, 10);
    feed(&tb, "\x1b[2J\x1b[H");
    feed(&tb, "\x1b[3;4Hx\x1b[1;1Hab\x1b[5;10Hz");
    CHECK(row_is(&tb, 0, "ab"));
    CHECK(row_is(&tb, 1, ""));
    CHECK(row_is(&tb, 2, "   x"));
    CHECK(row_is(&tb, 4, "         z"));
    CHECK(tb.scr->wrap_pending);
    feed(&tb, "\x1b[3;1H\x1b[J");
    CHECK(row_is(&tb, 0, "ab") && row_is(&tb, 2, "") && row_is(&tb, 4, ""));
    /* ED 2 keeps the old screen in the scrollback */
    feed(&tb, "\x1b[H\x1b[2Jnew");
    CHECK(row_is(&tb, 0, "new") && line_is(&tb, 0, "ab"));
    termbuf_free(&tb);

    /* autowrap at the right margin marks the line as soft-wrapped */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 5, 4);
    feed(&tb, "abcdef");
    CHECK(line_is(&tb, 0, "abcd") && line_is(&tb, 1, "ef"));
    CHECK(termbuf_line(&tb, termbuf_first_abs(&tb))->flags & TLF_WRAPPED);
    CHECK(!(termbuf_line(&tb, termbuf_first_abs(&tb) + 1)->flags & TLF_WRAPPED));
    /* the pending wrap is cancelled by CR */
    feed(&tb, "\r\nwxyz\rq");
    CHECK(line_is(&tb, 2, "qxyz") && !(termbuf_line(&tb, termbuf_first_abs(&tb) + 2)->flags & TLF_WRAPPED));
    termbuf_free(&tb);

    /* insert/delete characters and lines inside a scroll region */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 5, 10);
    feed(&tb, "\x1b[H1\r\n2\r\n3\r\n4\r\n5");
    feed(&tb, "\x1b[2;4r"); /* rows 2..4 */
    feed(&tb, "\x1b[2;1H\x1b[M");
    CHECK(row_is(&tb, 0, "1") && row_is(&tb, 1, "3") && row_is(&tb, 2, "4") && row_is(&tb, 3, "") && row_is(&tb, 4, "5"));
    feed(&tb, "\x1b[2;1H\x1b[L");
    CHECK(row_is(&tb, 1, "") && row_is(&tb, 2, "3") && row_is(&tb, 3, "4") && row_is(&tb, 4, "5"));
    feed(&tb, "\x1b[r\x1b[1;1Habcdef\x1b[1;3H\x1b[2P");
    CHECK(row_is(&tb, 0, "abef"));
    feed(&tb, "\x1b[1;2H\x1b[3@");
    CHECK(row_is(&tb, 0, "a   bef"));
    feed(&tb, "\x1b[1;1H\x1b[2X");
    CHECK(row_is(&tb, 0, "    bef"));
    termbuf_free(&tb);

    /* scrolling inside a region doesn't touch the scrollback */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 10);
    feed(&tb, "\x1b[Ha\r\nb\r\nc\r\nstatus\x1b[1;3r\x1b[3;1H\n\n");
    CHECK(row_is(&tb, 0, "c") && row_is(&tb, 1, "") && row_is(&tb, 3, "status"));
    CHECK(termbuf_count(&tb) == 4);
    /* reverse index at the top of the region scrolls it down */
    feed(&tb, "\x1b[1;1H\x1bM");
    CHECK(row_is(&tb, 0, "") && row_is(&tb, 1, "c") && row_is(&tb, 3, "status"));
    termbuf_free(&tb);

    /* alternate screen: separate content, main screen restored on exit */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 20);
    feed(&tb, "$ vim\r\n");
    feed(&tb, "\x1b[?1049h\x1b[H\x1b[2J~\r\n~\r\n\x1b[4;1H-- INSERT --");
    CHECK(termbuf_alt_active(&tb));
    CHECK(row_is(&tb, 0, "~") && row_is(&tb, 3, "-- INSERT --"));
    feed(&tb, "\x1b[?1049l");
    CHECK(!termbuf_alt_active(&tb));
    CHECK(line_is(&tb, 0, "$ vim") && cursor_at(&tb, 1, 0));
    termbuf_free(&tb);

    /* on the alt screen LF keeps the column; on the main screen it returns to 0 */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 20);
    feed(&tb, "ab\n");
    CHECK(cursor_at(&tb, 1, 0));
    feed(&tb, "\x1b[?1049hab\n");
    CHECK(cursor_at(&tb, 2, 2));
    termbuf_free(&tb);

    /* replies: cursor position, device attributes, window size */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 24, 80);
    feed(&tb, "\x1b[5;7H\x1b[6n\x1b[c\x1b[18t");
    CHECK(tb.reply_len > 0 && memcmp(tb.reply, "\x1b[5;7R\x1b[?62;22c\x1b[8;24;80t", (size_t)tb.reply_len) == 0);
    termbuf_free(&tb);

    /* DEC line drawing and UTF-8 split across feeds */
    termbuf_init(&tb, 100);
    feed(&tb, "\x1b(0lqk\x1b(B|");
    CHECK(line_is(&tb, 0, "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90|"));
    feed(&tb, "\r\n\xC3");
    feed(&tb, "\xA9t\xE2\x82");
    feed(&tb, "\xAC");
    CHECK(line_is(&tb, 1, "\xC3\xA9t\xE2\x82\xAC") && termbuf_line(&tb, termbuf_first_abs(&tb) + 1)->cells == 3);
    /* overwriting a multi-byte character with ASCII */
    feed(&tb, "\rX");
    CHECK(line_is(&tb, 1, "Xt\xE2\x82\xAC"));
    termbuf_free(&tb);

    /* erase with a background color fills to the right margin */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 3, 6);
    feed(&tb, "\x1b[44m\x1b[2K");
    TermLine *l = termbuf_line(&tb, tb.scr->top);
    CHECK(l && l->cells == 6 && termbuf_style(&tb, termline_style_at(l, 0))->bg == TC_PAL(4));
    termbuf_free(&tb);

    /* resizing while vi is up: leaving the alt screen restores the cursor to
     * the same line of text, not the same screen row */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 20);
    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "line %d\r\n", i);
        feed(&tb, buf);
    }
    feed(&tb, "$ ");
    uint64_t prompt_line = termbuf_cursor_line(&tb);
    feed(&tb, "\x1b[?1049h\x1b[H\x1b[2Jvi");
    termbuf_resize(&tb, 8, 20); /* taller: scrollback is pulled down onto the screen */
    feed(&tb, "\x1b[?1049l");
    CHECK(termbuf_cursor_line(&tb) == prompt_line && termbuf_cursor_col(&tb) == 2);
    termbuf_resize(&tb, 3, 20);
    CHECK(termbuf_cursor_line(&tb) == prompt_line);
    termbuf_free(&tb);

    /* narrowing past the cursor: the next character wraps instead of
     * overwriting the text under the new margin */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 20);
    feed(&tb, "hello world");
    termbuf_resize(&tb, 4, 5);
    feed(&tb, "X");
    CHECK(line_is(&tb, 0, "hello world") && line_is(&tb, 1, "X"));
    termbuf_free(&tb);

    /* `clear` as ESC[H ESC[J (BusyBox, TERM=vt100) behaves like ESC[2J: the old
     * screen goes to the scrollback */
    termbuf_init(&tb, 100);
    termbuf_resize(&tb, 4, 20);
    termbuf_feed(&tb, (const uint8_t *)"one\r\ntwo\r\n", 10, 5.0);
    feed(&tb, "\x1b[H\x1b[J$ ");
    CHECK(row_is(&tb, 0, "$ ") && row_is(&tb, 1, "") && line_is(&tb, 0, "one") && line_is(&tb, 1, "two"));
    /* a line erased to empty loses its timestamp; writing to it sets a new one */
    termbuf_feed(&tb, (const uint8_t *)"\x1b[2K", 4, 7.0);
    CHECK(termbuf_line(&tb, tb.scr->top)->time == 0);
    termbuf_feed(&tb, (const uint8_t *)"x", 1, 9.0);
    CHECK(termbuf_line(&tb, tb.scr->top)->time == 9.0);
    termbuf_free(&tb);

    /* REP repeats the last character */
    termbuf_init(&tb, 100);
    feed(&tb, "ab\x1b[3b");
    CHECK(line_is(&tb, 0, "abbbb"));
    termbuf_free(&tb);
}

int main(void)
{
    TermBuf tb;

    termbuf_init(&tb, 100);
    feed(&tb, "hello\r\nworld\n");
    CHECK(termbuf_count(&tb) == 2);
    CHECK(line_is(&tb, 0, "hello"));
    CHECK(line_is(&tb, 1, "world"));
    CHECK(termbuf_cursor_line(&tb) == termbuf_end_abs(&tb) && termbuf_cursor_col(&tb) == 0);
    termbuf_free(&tb);

    /* split CRLF across feeds, partial line stays open */
    termbuf_init(&tb, 100);
    feed(&tb, "abc\r");
    feed(&tb, "\ndef");
    CHECK(termbuf_count(&tb) == 2 && line_is(&tb, 1, "def"));
    CHECK(termbuf_cursor_line(&tb) == termbuf_end_abs(&tb) - 1 && termbuf_cursor_col(&tb) == 3);
    termbuf_free(&tb);

    /* CR overwrite (progress bar) and erase-in-line */
    termbuf_init(&tb, 100);
    feed(&tb, "10%\r20%\r100%\n");
    CHECK(line_is(&tb, 0, "100%"));
    feed(&tb, "long text\rab\x1b[K\n");
    CHECK(line_is(&tb, 1, "ab"));
    termbuf_free(&tb);

    /* backspace editing */
    termbuf_init(&tb, 100);
    feed(&tb, "helo\b\blo\n");
    CHECK(line_is(&tb, 0, "hello") == 0); /* "he" + "lo" overwrites -> "helo" */
    CHECK(line_is(&tb, 0, "helo"));
    termbuf_free(&tb);

    /* SGR colors produce per-byte styles; text excludes escapes */
    termbuf_init(&tb, 100);
    feed(&tb, "ok \x1b[1;32mPASS\x1b[0m done\n");
    CHECK(line_is(&tb, 0, "ok PASS done"));
    TermLine *l = termbuf_line(&tb, termbuf_first_abs(&tb));
    CHECK(l->sty != NULL);
    const TermStyle *st = termbuf_style(&tb, termline_style_at(l, 3));
    CHECK(st->fg == TC_PAL(2) && (st->attr & TA_BOLD));
    CHECK(termbuf_style(&tb, termline_style_at(l, 0))->fg == TC_DEFAULT);
    CHECK(termbuf_style(&tb, termline_style_at(l, 8))->fg == TC_DEFAULT);
    feed(&tb, "\x1b[38;2;10;20;30mrgb\x1b[38;5;196m!\n");
    l = termbuf_line(&tb, termbuf_first_abs(&tb) + 1);
    CHECK(termbuf_style(&tb, termline_style_at(l, 0))->fg == TC_RGB(10, 20, 30));
    CHECK(termbuf_style(&tb, termline_style_at(l, 3))->fg == TC_PAL(196));
    termbuf_free(&tb);

    /* CR-as-newline mode */
    termbuf_init(&tb, 100);
    tb.nl_mode = TB_NL_ANY;
    feed(&tb, "a\rb\r\nc\n");
    CHECK(termbuf_count(&tb) == 3 && line_is(&tb, 0, "a") && line_is(&tb, 1, "b") && line_is(&tb, 2, "c"));
    termbuf_free(&tb);

    /* ring buffer eviction keeps absolute indices monotonic */
    termbuf_init(&tb, 16);
    for (int i = 0; i < 40; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "line %d\n", i);
        feed(&tb, buf);
    }
    CHECK(termbuf_count(&tb) == 16 && termbuf_first_abs(&tb) == 24);
    CHECK(line_is(&tb, 0, "line 24"));
    CHECK(line_is(&tb, 15, "line 39"));
    termbuf_free(&tb);

    /* control characters become visible glyphs; OSC is swallowed */
    termbuf_init(&tb, 100);
    feed(&tb, "\x01x\x1b]0;title\x07y\n");
    CHECK(line_is(&tb, 0, "\xE2\x90\x81xy"));
    termbuf_free(&tb);

    /* markers close the open line */
    termbuf_init(&tb, 100);
    feed(&tb, "partial");
    termbuf_add_marker(&tb, TL_WARN, "lost", 0);
    feed(&tb, "next\n");
    CHECK(termbuf_count(&tb) == 3 && line_is(&tb, 1, "lost") && termbuf_line(&tb, termbuf_first_abs(&tb) + 1)->kind == TL_WARN);
    CHECK(line_is(&tb, 2, "next"));
    termbuf_free(&tb);

    /* local echo is inline after a prompt, styled TA_TX, one line per Enter */
    termbuf_init(&tb, 100);
    feed(&tb, "$ ");
    termbuf_add_tx(&tb, (const uint8_t *)"h", 1, 0);
    termbuf_add_tx(&tb, (const uint8_t *)"i", 1, 0);
    termbuf_add_tx(&tb, (const uint8_t *)"\x1b[A\r", 5, 0);
    feed(&tb, "out\n");
    CHECK(termbuf_count(&tb) == 2 && line_is(&tb, 0, "$ hi") && line_is(&tb, 1, "out"));
    l = termbuf_line(&tb, termbuf_first_abs(&tb));
    CHECK(termbuf_style(&tb, termline_style_at(l, 2))->attr & TA_TX);
    CHECK(!(termbuf_style(&tb, termline_style_at(l, 0))->attr & TA_TX));
    termbuf_free(&tb);

    /* absurd cursor positions are clamped */
    termbuf_init(&tb, 100);
    feed(&tb, "x\x1b[60000Gy\n");
    CHECK(termbuf_line(&tb, termbuf_first_abs(&tb))->len <= 80);
    termbuf_free(&tb);

    screen_tests();

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("termbuf: all tests passed\n");
    return failures != 0;
}
