#include "serial/session.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRY_INTERVAL_MS 400
#define READ_TIMEOUT_MS 250
#define PRESENCE_CHECK_MS 1000
#define WRITE_TIMEOUT_MS 2000
#define INBOX_LIMIT (64u << 20) /* drop data rather than eat all memory if the UI stalls */

struct Session {
    SDL_Thread *thread;
    SDL_Mutex *mtx;
    SDL_Semaphore *sem;
    SessionWakeFn wake;
    void *wake_ud;
    SDL_AtomicInt wake_pending;

    /* Requests from the UI (guarded by mtx). */
    bool quit;
    bool want_open;
    bool auto_reconnect;
    bool break_req;
    uint32_t open_gen; /* bumps on every session_open() */
    uint32_t cfg_gen;  /* bumps on every config change */
    PortTarget target;
    SerialConfig cfg;
    SessionBuf tx;

    /* State published by the I/O thread (guarded by mtx). */
    SerialPort *port;
    LinkState state;
    uint64_t state_since;
    char status[256];
    Severity status_sev;
    char cur_path[128];
    uint64_t rx_bytes, tx_bytes;
    SessionBuf inbox;
    size_t last_rx_rec; /* offset of a trailing RX record we may extend, or SIZE_MAX */
};

double wall_time_now(void)
{
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    return (double)t / 1e9;
}

/* ---- Record buffers ------------------------------------------------------ */

static void buf_reserve(SessionBuf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra)
        cap *= 2;
    b->data = realloc(b->data, cap);
    b->cap = cap;
}

void session_buf_free(SessionBuf *b)
{
    free(b->data);
    memset(b, 0, sizeof *b);
}

const SessionRecord *session_buf_next(const SessionBuf *b, size_t *offset, const uint8_t **payload)
{
    if (*offset + sizeof(SessionRecord) > b->len)
        return NULL;
    const SessionRecord *r = (const SessionRecord *)(b->data + *offset);
    *payload = b->data + *offset + sizeof(SessionRecord);
    *offset += sizeof(SessionRecord) + ((r->len + 7u) & ~7u);
    return r;
}

/* Caller holds mtx. */
static void push_record_locked(Session *s, uint8_t type, const void *data, size_t len)
{
    if (s->inbox.len + len > INBOX_LIMIT)
        return;
    if (type == SREC_RX && s->last_rx_rec != SIZE_MAX) {
        /* Extend the trailing RX record instead of adding a header per read. */
        SessionRecord *r = (SessionRecord *)(s->inbox.data + s->last_rx_rec);
        size_t old_padded = (r->len + 7u) & ~7u;
        size_t new_padded = (r->len + len + 7u) & ~7u;
        buf_reserve(&s->inbox, new_padded - old_padded);
        r = (SessionRecord *)(s->inbox.data + s->last_rx_rec);
        memcpy((uint8_t *)(r + 1) + r->len, data, len);
        r->len += (uint32_t)len;
        s->inbox.len = s->last_rx_rec + sizeof(SessionRecord) + new_padded;
        return;
    }
    size_t padded = (len + 7u) & ~7u;
    buf_reserve(&s->inbox, sizeof(SessionRecord) + padded);
    SessionRecord *r = (SessionRecord *)(s->inbox.data + s->inbox.len);
    memset(r, 0, sizeof *r);
    r->type = type;
    r->state = (uint8_t)s->state;
    r->severity = (uint8_t)s->status_sev;
    r->len = (uint32_t)len;
    r->time = wall_time_now();
    if (len)
        memcpy(r + 1, data, len);
    s->last_rx_rec = type == SREC_RX ? s->inbox.len : SIZE_MAX;
    s->inbox.len += sizeof(SessionRecord) + padded;
}

static void notify_ui(Session *s)
{
    if (SDL_CompareAndSwapAtomicInt(&s->wake_pending, 0, 1) && s->wake)
        s->wake(s->wake_ud);
}

static void push_data(Session *s, uint8_t type, const void *data, size_t len)
{
    SDL_LockMutex(s->mtx);
    push_record_locked(s, type, data, len);
    if (type == SREC_RX)
        s->rx_bytes += len;
    else if (type == SREC_TX)
        s->tx_bytes += len;
    SDL_UnlockMutex(s->mtx);
    notify_ui(s);
}

/* Updates the published state. If log is set the message also goes into the
 * terminal stream as a marker line. */
static void set_state(Session *s, LinkState st, Severity sev, bool log, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SDL_LockMutex(s->mtx);
    if (s->state != st)
        s->state_since = SDL_GetTicks();
    s->state = st;
    s->status_sev = sev;
    snprintf(s->status, sizeof s->status, "%s", msg);
    if (log)
        push_record_locked(s, SREC_STATE, msg, strlen(msg));
    else
        push_record_locked(s, SREC_STATE, NULL, 0);
    SDL_UnlockMutex(s->mtx);
    notify_ui(s);
}

/* ---- Port resolution ----------------------------------------------------- */

/* Finds the device for a target. A USB identity must match exactly (so a
 * different adapter that lands on the same /dev path is never opened);
 * among identical identities (adapters without serial numbers) the last
 * known path wins. Targets without an identity match by path. */
static bool resolve_target(const PortTarget *t, char *path, size_t pathlen)
{
    SerialPortInfo ports[64];
    int n = serial_enumerate(ports, 64);
    int found = -1;
    bool by_id = t->id[0] && strncmp(t->id, "path:", 5) != 0;
    for (int i = 0; i < n; i++) {
        bool match = by_id ? strcmp(ports[i].id, t->id) == 0 : strcmp(ports[i].path, t->path) == 0;
        if (!match)
            continue;
        if (found < 0 || strcmp(ports[i].path, t->path) == 0)
            found = i;
    }
    if (found < 0)
        return false;
    snprintf(path, pathlen, "%s", ports[found].path);
    return true;
}

/* ---- I/O thread ---------------------------------------------------------- */

static void close_port(Session *s, SerialPort **port)
{
    SDL_LockMutex(s->mtx);
    s->port = NULL;
    SDL_UnlockMutex(s->mtx);
    serial_close(*port);
    *port = NULL;
}

static int SDLCALL io_thread(void *arg)
{
    Session *s = arg;
    uint8_t *rxbuf = malloc(65536);
    SessionBuf txbuf = {0};
    SerialPort *port = NULL;
    uint32_t open_gen = 0, applied_cfg_gen = 0;
    uint64_t lost_at = 0, last_presence = 0;
    char last_err[256] = "";
    char cfgdesc[64];

    for (;;) {
        /* Drop stale kicks (writes while connected never consume them) before
         * reading the request state; any kick after this point stays counted,
         * so waits below can't miss it but also can't spin on a backlog. */
        while (SDL_TryWaitSemaphore(s->sem)) {
        }
        SDL_LockMutex(s->mtx);
        bool quit = s->quit, want = s->want_open, autorc = s->auto_reconnect, brk = s->break_req;
        uint32_t og = s->open_gen, cg = s->cfg_gen;
        PortTarget target = s->target;
        SerialConfig cfg = s->cfg;
        s->break_req = false;
        SDL_UnlockMutex(s->mtx);
        if (quit)
            break;

        if (port && (!want || og != open_gen))
            close_port(s, &port);
        if (!want) {
            if (session_state(s) != LINK_IDLE)
                set_state(s, LINK_IDLE, SEV_INFO, true, "Disconnected");
            SDL_WaitSemaphore(s->sem);
            continue;
        }
        if (og != open_gen) {
            open_gen = og;
            lost_at = 0;
            last_err[0] = 0;
            set_state(s, LINK_CONNECTING, SEV_INFO, false, "Connecting to %s\xE2\x80\xA6", target.name);
        }

        if (!port) {
            char path[128], err[200] = "";
            if (!resolve_target(&target, path, sizeof path))
                snprintf(err, sizeof err, "Device not present");
            else if (serial_open(path, &cfg, &port, err, sizeof err)) {
                SDL_LockMutex(s->mtx);
                s->port = port;
                snprintf(s->cur_path, sizeof s->cur_path, "%s", path);
                SDL_UnlockMutex(s->mtx);
                applied_cfg_gen = cg;
                last_presence = SDL_GetTicks();
                last_err[0] = 0;
                serial_config_describe(&cfg, cfgdesc, sizeof cfgdesc);
                if (lost_at)
                    set_state(s, LINK_CONNECTED, SEV_OK, true, "Reconnected to %s after %.1fs", path,
                              (double)(SDL_GetTicks() - lost_at) / 1000.0);
                else
                    set_state(s, LINK_CONNECTED, SEV_OK, true, "Connected to %s \xC2\xB7 %s", path, cfgdesc);
                lost_at = 0;
                continue;
            }
            if (!autorc) {
                SDL_LockMutex(s->mtx);
                if (s->open_gen == og)
                    s->want_open = false;
                SDL_UnlockMutex(s->mtx);
                set_state(s, LINK_IDLE, SEV_ERROR, true, "Could not open %s: %s", target.name, err);
                continue;
            }
            bool changed = strcmp(err, last_err) != 0;
            snprintf(last_err, sizeof last_err, "%s", err);
            if (changed || session_state(s) != LINK_RECONNECTING)
                set_state(s, LINK_RECONNECTING, SEV_WARN, changed, "Waiting for %s: %s", target.name, err);
            SDL_WaitSemaphoreTimeout(s->sem, RETRY_INTERVAL_MS);
            continue;
        }

        /* Connected. */
        const char *lost_reason = NULL;
        if (cg != applied_cfg_gen) {
            char err[200];
            applied_cfg_gen = cg;
            serial_config_describe(&cfg, cfgdesc, sizeof cfgdesc);
            if (serial_apply_config(port, &cfg, err, sizeof err))
                set_state(s, LINK_CONNECTED, SEV_OK, true, "Settings changed \xC2\xB7 %s", cfgdesc);
            else
                set_state(s, LINK_CONNECTED, SEV_ERROR, true, "%s", err);
        }
        if (brk)
            serial_send_break(port, 250);

        SDL_LockMutex(s->mtx);
        SessionBuf t = s->tx;
        s->tx = txbuf;
        s->tx.len = 0;
        SDL_UnlockMutex(s->mtx);
        txbuf = t;
        if (txbuf.len) {
            int w = serial_write(port, txbuf.data, txbuf.len, WRITE_TIMEOUT_MS);
            if (w < 0) {
                lost_reason = "write failed";
            } else {
                if (w > 0)
                    push_data(s, SREC_TX, txbuf.data, (size_t)w);
                if ((size_t)w < txbuf.len) {
                    /* Flow control held us off: put the rest back in front of
                     * anything queued meanwhile and retry next iteration. */
                    size_t rest = txbuf.len - (size_t)w;
                    SDL_LockMutex(s->mtx);
                    buf_reserve(&s->tx, rest);
                    memmove(s->tx.data + rest, s->tx.data, s->tx.len);
                    memcpy(s->tx.data, txbuf.data + w, rest);
                    s->tx.len += rest;
                    SDL_UnlockMutex(s->mtx);
                }
            }
            txbuf.len = 0;
        }

        if (!lost_reason) {
            int n = serial_read(port, rxbuf, 65536, READ_TIMEOUT_MS);
            if (n > 0)
                push_data(s, SREC_RX, rxbuf, (size_t)n);
            else if (n < 0)
                lost_reason = n == SERIAL_ERR_GONE ? "device removed" : "I/O error";
            else if (SDL_GetTicks() - last_presence >= PRESENCE_CHECK_MS) {
                last_presence = SDL_GetTicks();
                if (!serial_still_present(port))
                    lost_reason = "device removed";
            }
        }

        if (lost_reason) {
            char path[128];
            SDL_LockMutex(s->mtx);
            snprintf(path, sizeof path, "%s", s->cur_path);
            SDL_UnlockMutex(s->mtx);
            close_port(s, &port);
            lost_at = SDL_GetTicks();
            snprintf(last_err, sizeof last_err, "Device not present");
            if (autorc) {
                set_state(s, LINK_RECONNECTING, SEV_WARN, true, "Connection to %s lost (%s) \xE2\x80\x94 reconnecting\xE2\x80\xA6",
                          path, lost_reason);
            } else {
                SDL_LockMutex(s->mtx);
                if (s->open_gen == og) /* don't cancel a newer connect request */
                    s->want_open = false;
                SDL_UnlockMutex(s->mtx);
                set_state(s, LINK_IDLE, SEV_ERROR, true, "Connection to %s lost (%s)", path, lost_reason);
            }
        }
    }
    if (port)
        close_port(s, &port);
    free(rxbuf);
    session_buf_free(&txbuf);
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

Session *session_create(SessionWakeFn wake, void *userdata)
{
    Session *s = calloc(1, sizeof *s);
    s->mtx = SDL_CreateMutex();
    s->sem = SDL_CreateSemaphore(0);
    s->wake = wake;
    s->wake_ud = userdata;
    s->cfg = serial_config_default();
    s->auto_reconnect = true;
    s->last_rx_rec = SIZE_MAX;
    s->state_since = SDL_GetTicks();
    s->thread = SDL_CreateThread(io_thread, "serial-io", s);
    return s;
}

static void kick(Session *s)
{
    /* Caller holds mtx so s->port can't be closed underneath us. */
    if (s->port)
        serial_wake(s->port);
    SDL_SignalSemaphore(s->sem);
}

void session_destroy(Session *s)
{
    if (!s)
        return;
    SDL_LockMutex(s->mtx);
    s->quit = true;
    kick(s);
    SDL_UnlockMutex(s->mtx);
    SDL_WaitThread(s->thread, NULL);
    SDL_DestroySemaphore(s->sem);
    SDL_DestroyMutex(s->mtx);
    session_buf_free(&s->tx);
    session_buf_free(&s->inbox);
    free(s);
}

void session_open(Session *s, const PortTarget *target, const SerialConfig *cfg, bool auto_reconnect)
{
    SDL_LockMutex(s->mtx);
    s->target = *target;
    s->cfg = *cfg;
    s->auto_reconnect = auto_reconnect;
    s->want_open = true;
    s->open_gen++;
    s->cfg_gen++;
    s->tx.len = 0;
    kick(s);
    SDL_UnlockMutex(s->mtx);
}

void session_close(Session *s)
{
    SDL_LockMutex(s->mtx);
    s->want_open = false;
    kick(s);
    SDL_UnlockMutex(s->mtx);
}

void session_set_config(Session *s, const SerialConfig *cfg)
{
    SDL_LockMutex(s->mtx);
    if (memcmp(&s->cfg, cfg, sizeof *cfg) != 0) {
        s->cfg = *cfg;
        s->cfg_gen++;
        kick(s);
    }
    SDL_UnlockMutex(s->mtx);
}

void session_set_auto_reconnect(Session *s, bool on)
{
    SDL_LockMutex(s->mtx);
    s->auto_reconnect = on;
    kick(s);
    SDL_UnlockMutex(s->mtx);
}

void session_write(Session *s, const void *data, size_t len)
{
    if (!len)
        return;
    SDL_LockMutex(s->mtx);
    if (s->port) {
        buf_reserve(&s->tx, len);
        memcpy(s->tx.data + s->tx.len, data, len);
        s->tx.len += len;
        kick(s);
    }
    SDL_UnlockMutex(s->mtx);
}

void session_send_break(Session *s)
{
    SDL_LockMutex(s->mtx);
    s->break_req = true;
    kick(s);
    SDL_UnlockMutex(s->mtx);
}

LinkState session_state(Session *s)
{
    SDL_LockMutex(s->mtx);
    LinkState st = s->state;
    SDL_UnlockMutex(s->mtx);
    return st;
}

double session_state_age(Session *s)
{
    SDL_LockMutex(s->mtx);
    uint64_t since = s->state_since;
    SDL_UnlockMutex(s->mtx);
    return (double)(SDL_GetTicks() - since) / 1000.0;
}

void session_status(Session *s, char *msg, size_t msglen, Severity *sev)
{
    SDL_LockMutex(s->mtx);
    if (msg)
        snprintf(msg, msglen, "%s", s->status);
    if (sev)
        *sev = s->status_sev;
    SDL_UnlockMutex(s->mtx);
}

void session_current_path(Session *s, char *out, size_t outlen)
{
    SDL_LockMutex(s->mtx);
    snprintf(out, outlen, "%s", s->cur_path);
    SDL_UnlockMutex(s->mtx);
}

uint64_t session_rx_bytes(Session *s)
{
    SDL_LockMutex(s->mtx);
    uint64_t v = s->rx_bytes;
    SDL_UnlockMutex(s->mtx);
    return v;
}

uint64_t session_tx_bytes(Session *s)
{
    SDL_LockMutex(s->mtx);
    uint64_t v = s->tx_bytes;
    SDL_UnlockMutex(s->mtx);
    return v;
}

bool session_drain(Session *s, SessionBuf *out)
{
    out->len = 0;
    SDL_SetAtomicInt(&s->wake_pending, 0);
    SDL_LockMutex(s->mtx);
    bool any = s->inbox.len > 0;
    if (any) {
        SessionBuf t = s->inbox;
        s->inbox = *out;
        *out = t;
        s->last_rx_rec = SIZE_MAX;
    }
    SDL_UnlockMutex(s->mtx);
    return any;
}
