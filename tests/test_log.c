/* Unit tests for session logging: the printable filter, raw mode, headers,
 * timestamps and the "file exists" handling. Writes into the temp folder. */
#include "app/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

#ifdef _WIN32
#define NL "\r\n"
#else
#define NL "\n"
#endif

static char g_path[1024];

static char *slurp(const char *path)
{
    size_t n = 0;
    char *s = SDL_LoadFile(path, &n);
    return s ? s : SDL_strdup("");
}

static void feed(SessionLog *lg, const char *s) { log_rx(lg, (const uint8_t *)s, strlen(s), 0); }

static LogSettings settings(LogMode mode, LogExisting existing, bool header)
{
    LogSettings c = {0};
    c.mode = mode;
    snprintf(c.path, sizeof c.path, "%s", g_path);
    c.existing = existing;
    c.header = header;
    c.flush = true;
    return c;
}

int main(void)
{
    const char *tmp = SDL_getenv("TEMP");
    if (!tmp)
        tmp = SDL_getenv("TMPDIR");
    snprintf(g_path, sizeof g_path, "%s/imserial-test-&P.log", tmp ? tmp : ".");
    char real[1024];
    snprintf(real, sizeof real, "%s/imserial-test-COM9.log", tmp ? tmp : ".");
    SDL_RemovePath(real);
    char err[1600];
    SessionLog lg = {0};

    /* printable: escapes and controls stripped, CR / LF / CRLF are one line end */
    LogSettings c = settings(LOG_PRINTABLE, LOG_EXISTING_OVERWRITE, false);
    CHECK(log_open(&lg, &c, "\\\\.\\COM9", NULL, err, sizeof err));
    CHECK(strcmp(lg.path, real) == 0);
    feed(&lg, "\x1b[1;32mok\x1b[0m\r\nline\x07 two\rthree\n\x1b]0;title\x07" "end\x1b[2J\x1b(0q\x1b(B\n");
    log_event(&lg, "Disconnected", 0);
    log_close(&lg);
    char *s = slurp(real);
    CHECK(strcmp(s, "ok" NL "line two" NL "three" NL "endq" NL "[imserial] Disconnected" NL) == 0);
    SDL_free(s);

    /* raw: bytes unmodified, events not written */
    c = settings(LOG_ALL, LOG_EXISTING_OVERWRITE, false);
    CHECK(log_open(&lg, &c, "COM9", NULL, err, sizeof err));
    feed(&lg, "\x1b[Ha\r\n");
    log_event(&lg, "Connected", 0);
    log_close(&lg);
    s = slurp(real);
    CHECK(strcmp(s, "\x1b[Ha\r\n") == 0);
    SDL_free(s);

    /* append with a header */
    c = settings(LOG_PRINTABLE, LOG_EXISTING_APPEND, true);
    CHECK(log_open(&lg, &c, "COM9", "COM9  115200 8N1", err, sizeof err));
    feed(&lg, "more\n");
    log_close(&lg);
    s = slurp(real);
    CHECK(strncmp(s, "\x1b[Ha\r\n=~=~=", 9) == 0 && strstr(s, "COM9  115200 8N1") && strstr(s, NL "more" NL));
    SDL_free(s);

    /* ask: pending until resolved; output is kept meanwhile */
    c = settings(LOG_PRINTABLE, LOG_EXISTING_ASK, false);
    CHECK(log_open(&lg, &c, "COM9", NULL, err, sizeof err));
    CHECK(lg.pending && log_is_open(&lg));
    feed(&lg, "early\n");
    CHECK(log_resolve(&lg, true, false, err, sizeof err));
    feed(&lg, "late\n");
    log_close(&lg);
    s = slurp(real);
    CHECK(strcmp(s, "early" NL "late" NL) == 0);
    SDL_free(s);

    /* ask, then "don't log": nothing written, and no retry for this session */
    CHECK(log_open(&lg, &c, "COM9", NULL, err, sizeof err) && lg.pending);
    CHECK(!log_resolve(&lg, false, false, err, sizeof err));
    CHECK(!log_is_open(&lg) && lg.failed);
    s = slurp(real);
    CHECK(strcmp(s, "early" NL "late" NL) == 0);
    SDL_free(s);
    lg.failed = false;

    /* another program can read the log while it is being written (tail -f, editors) */
    c = settings(LOG_PRINTABLE, LOG_EXISTING_OVERWRITE, false);
    CHECK(log_open(&lg, &c, "COM9", NULL, err, sizeof err));
    feed(&lg, "live\n");
    FILE *reader = fopen(real, "rb");
    CHECK(reader != NULL);
    if (reader) {
        char got[32] = "";
        size_t n = fread(got, 1, sizeof got - 1, reader);
        got[n] = 0;
        CHECK(strcmp(got, "live" NL) == 0);
        fclose(reader);
    }
    log_close(&lg);

    /* timestamps prefix each printable line */
    c = settings(LOG_PRINTABLE, LOG_EXISTING_OVERWRITE, false);
    c.timestamps = true;
    CHECK(log_open(&lg, &c, "COM9", NULL, err, sizeof err));
    log_rx(&lg, (const uint8_t *)"a\nb\n", 4, 1700000000.5);
    log_close(&lg);
    s = slurp(real);
    CHECK(s[0] == '[' && strstr(s, ".500] a" NL "[") && strstr(s, ".500] b" NL));
    SDL_free(s);

    SDL_RemovePath(real);
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("log: all tests passed\n");
    return failures != 0;
}
