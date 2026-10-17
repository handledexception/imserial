/* Session logging. Output goes through a memory buffer: written out when it
 * fills, after every call with "flush frequently", and on close. While the
 * user is asked whether to overwrite an existing file, the buffer just holds
 * what arrives. */
#include "app/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <share.h>
#endif

#define LOG_BUF (256 * 1024)

#ifdef _WIN32
#define NL "\r\n"
#define SEP '\\'
#else
#define NL "\n"
#define SEP '/'
#endif

enum { LP_NORMAL, LP_ESC, LP_ESC_INTER, LP_CSI, LP_OSC, LP_OSC_ESC, LP_STR, LP_STR_ESC };

void log_default_path(char *out, size_t outlen)
{
    /* Both SDL folders end with a path separator. */
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    char *pref = docs ? NULL : SDL_GetPrefPath(NULL, "imserial");
    snprintf(out, outlen, "%simserial logs%cimserial-&P-&Y&M&D-&T.log", docs ? docs : pref ? pref : "", SEP);
    SDL_free(pref);
}

/* Port name usable in a file name: the last path component, with characters
 * Windows doesn't allow in names replaced. */
static void file_safe_port(const char *port, char *out, size_t outlen)
{
    const char *b = port;
    for (const char *p = port; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    size_t i = 0;
    for (; *b && i + 1 < outlen; b++)
        out[i++] = strchr("<>:\"|?*", *b) || (unsigned char)*b < 0x20 ? '_' : *b;
    out[i] = 0;
    if (!i)
        snprintf(out, outlen, "serial");
}

static void expand_path(const char *pattern, const char *port, const SDL_DateTime *dt, char *out, size_t outlen)
{
    char safe[96];
    file_safe_port(port, safe, sizeof safe);
    size_t o = 0;
    for (const char *p = pattern; *p && o + 1 < outlen; p++) {
        char tmp[96];
        const char *ins = NULL;
        if (*p == '&' && p[1]) {
            switch (p[1]) {
            case 'Y': snprintf(tmp, sizeof tmp, "%04d", dt->year), ins = tmp; break;
            case 'M': snprintf(tmp, sizeof tmp, "%02d", dt->month), ins = tmp; break;
            case 'D': snprintf(tmp, sizeof tmp, "%02d", dt->day), ins = tmp; break;
            case 'T': snprintf(tmp, sizeof tmp, "%02d%02d%02d", dt->hour, dt->minute, dt->second), ins = tmp; break;
            case 'P':
            case 'H': ins = safe; break; /* &H: PuTTY's "host" */
            case '&': ins = "&"; break;
            default: break;
            }
        }
        if (ins) {
            size_t n = strlen(ins);
            if (o + n >= outlen)
                break;
            memcpy(out + o, ins, n);
            o += n;
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void write_buf(SessionLog *lg)
{
    if (lg->f && lg->len) {
        fwrite(lg->buf, 1, lg->len, lg->f);
        lg->len = 0;
    }
}

static void out(SessionLog *lg, const char *s, size_t n)
{
    if (lg->len + n > LOG_BUF) {
        if (!lg->f) { /* pending: keep what fits */
            lg->dropped = true;
            n = LOG_BUF - lg->len;
        } else {
            write_buf(lg);
            if (n > LOG_BUF) {
                fwrite(s, 1, n, lg->f);
                return;
            }
        }
    }
    memcpy(lg->buf + lg->len, s, n);
    lg->len += n;
}

static void outs(SessionLog *lg, const char *s) { out(lg, s, strlen(s)); }

static void done(SessionLog *lg)
{
    if (lg->f && lg->flush) {
        write_buf(lg);
        fflush(lg->f);
    }
}

static void stamp(SessionLog *lg, double time)
{
    SDL_DateTime dt;
    char s[48];
    if (SDL_TimeToDateTime((SDL_Time)(time * 1e9), &dt, true)) {
        snprintf(s, sizeof s, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                 dt.second, dt.nanosecond / 1000000);
        outs(lg, s);
    }
}

static void newline(SessionLog *lg)
{
    outs(lg, NL);
    lg->line_start = true;
}

static bool open_file(SessionLog *lg, bool overwrite, char *err, size_t errlen)
{
    /* Not SDL_IOFromFile: on Windows it opens files for writing without
     * sharing, so a text editor couldn't open (or tail) the log meanwhile. */
#ifdef _WIN32
    wchar_t *wpath = (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", lg->path, strlen(lg->path) + 1);
    lg->f = wpath ? _wfsopen(wpath, overwrite ? L"wb" : L"ab", _SH_DENYNO) : NULL;
    SDL_free(wpath);
#else
    lg->f = fopen(lg->path, overwrite ? "wb" : "ab");
#endif
    if (!lg->f) {
        char why[128];
#ifdef _MSC_VER
        strerror_s(why, sizeof why, errno);
#else
        snprintf(why, sizeof why, "%s", strerror(errno));
#endif
        snprintf(err, errlen, "Couldn't open log file %s: %s", lg->path, why);
        return false;
    }
    write_buf(lg);
    if (lg->flush)
        fflush(lg->f);
    return true;
}

bool log_open(SessionLog *lg, const LogSettings *cfg, const char *port, const char *info, char *err, size_t errlen)
{
    log_close(lg);
    if (cfg->mode == LOG_NONE)
        return false;

    SDL_Time now;
    SDL_DateTime dt;
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &dt, true);
    char pattern[1024];
    if (cfg->path[0])
        snprintf(pattern, sizeof pattern, "%s", cfg->path);
    else
        log_default_path(pattern, sizeof pattern);
    expand_path(pattern, port, &dt, lg->path, sizeof lg->path);

    /* Create the folder (and its parents) if needed. */
    char dir[sizeof lg->path];
    snprintf(dir, sizeof dir, "%s", lg->path);
    char *slash = NULL;
    for (char *p = dir; *p; p++)
        if (*p == '/' || *p == '\\')
            slash = p;
    if (slash && slash != dir) {
        *slash = 0;
        SDL_CreateDirectory(dir);
    }

    lg->buf = SDL_malloc(LOG_BUF);
    lg->mode = cfg->mode;
    lg->timestamps = cfg->timestamps && cfg->mode == LOG_PRINTABLE;
    lg->flush = cfg->flush;
    lg->line_start = true;
    if (cfg->header) {
        char h[512];
        snprintf(h, sizeof h, "=~=~=~=~=~=~=~=~=~=~=~= imserial log %04d.%02d.%02d %02d:%02d:%02d  %s =~=~=~=~=~=~=~=~=~=~=~=",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, info ? info : port);
        outs(lg, h);
        newline(lg);
    }

    bool exists = SDL_GetPathInfo(lg->path, NULL);
    if (exists && cfg->existing == LOG_EXISTING_ASK) {
        lg->pending = true;
        return true;
    }
    if (!open_file(lg, cfg->existing == LOG_EXISTING_OVERWRITE, err, errlen)) {
        log_close(lg);
        lg->failed = true;
        return false;
    }
    return true;
}

bool log_resolve(SessionLog *lg, bool overwrite, bool append, char *err, size_t errlen)
{
    if (!lg->pending)
        return false;
    lg->pending = false;
    if (!overwrite && !append) {
        log_close(lg);
        lg->failed = true; /* "don't log": stay off for this session */
        return false;
    }
    if (!open_file(lg, overwrite, err, errlen)) {
        log_close(lg);
        lg->failed = true;
        return false;
    }
    return true;
}

void log_close(SessionLog *lg)
{
    if (lg->f) {
        write_buf(lg);
        fclose(lg->f);
    }
    SDL_free(lg->buf);
    bool failed = lg->failed;
    memset(lg, 0, sizeof *lg);
    lg->failed = failed;
}

void log_rx(SessionLog *lg, const uint8_t *data, size_t len, double time)
{
    if (!lg->buf)
        return;
    if (lg->mode == LOG_ALL) {
        out(lg, (const char *)data, len);
        done(lg);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (lg->pstate) {
        case LP_ESC:
            if (c == '[')
                lg->pstate = LP_CSI;
            else if (c == ']')
                lg->pstate = LP_OSC;
            else if (c == 'P' || c == 'X' || c == '^' || c == '_')
                lg->pstate = LP_STR;
            else if (c >= 0x20 && c <= 0x2F)
                lg->pstate = LP_ESC_INTER;
            else
                lg->pstate = LP_NORMAL;
            continue;
        case LP_ESC_INTER: lg->pstate = LP_NORMAL; continue;
        case LP_CSI:
            if ((c >= 0x40 && c <= 0x7E) || c == 0x18 || c == 0x1A)
                lg->pstate = LP_NORMAL;
            else if (c == 0x1B)
                lg->pstate = LP_ESC;
            continue;
        case LP_OSC:
            if (c == 0x07)
                lg->pstate = LP_NORMAL;
            else if (c == 0x1B)
                lg->pstate = LP_OSC_ESC;
            continue;
        case LP_OSC_ESC: lg->pstate = LP_NORMAL; continue;
        case LP_STR:
            if (c == 0x1B)
                lg->pstate = LP_STR_ESC;
            else if (c == 0x07 || c == 0x18 || c == 0x1A)
                lg->pstate = LP_NORMAL;
            continue;
        case LP_STR_ESC: lg->pstate = c == '\\' ? LP_NORMAL : LP_STR; continue;
        default: break;
        }
        if (c == 0x1B) {
            lg->pstate = LP_ESC;
            continue;
        }
        /* CR, LF and CRLF each end one line. */
        if (c == '\r' || c == '\n') {
            if (!(c == '\n' && lg->last_cr))
                newline(lg);
            lg->last_cr = c == '\r';
            continue;
        }
        lg->last_cr = false;
        if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            if (lg->line_start && lg->timestamps)
                stamp(lg, time);
            lg->line_start = false;
            out(lg, (const char *)&c, 1);
        }
    }
    done(lg);
}

void log_event(SessionLog *lg, const char *text, double time)
{
    if (!lg->buf || lg->mode != LOG_PRINTABLE)
        return;
    if (!lg->line_start)
        newline(lg);
    if (lg->timestamps)
        stamp(lg, time);
    outs(lg, "[imserial] ");
    outs(lg, text);
    newline(lg);
    lg->last_cr = false;
    done(lg);
}
