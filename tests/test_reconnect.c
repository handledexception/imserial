/* Exercises the session's reconnect state machine against the fake backend:
 * unplug/replug, re-enumeration under a new path, impostor devices on the old
 * path, user disconnect and auto-reconnect off. */
#include "fake_serial.h"
#include "serial/session.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static int failures;
static SessionBuf g_buf;
static char g_log[8192];

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

/* Drains records into g_log (state messages and RX text). */
static void drain(Session *s)
{
    if (!session_drain(s, &g_buf))
        return;
    size_t off = 0;
    const uint8_t *p;
    const SessionRecord *r;
    while ((r = session_buf_next(&g_buf, &off, &p))) {
        if (r->type == SREC_RX || (r->type == SREC_STATE && r->len)) {
            size_t n = strlen(g_log);
            if (n + r->len + 2 < sizeof g_log) {
                memcpy(g_log + n, p, r->len);
                g_log[n + r->len] = '\n';
                g_log[n + r->len + 1] = 0;
            }
        }
    }
}

static bool wait_state(Session *s, LinkState want, int ms)
{
    uint64_t end = SDL_GetTicks() + (uint64_t)ms;
    while (SDL_GetTicks() < end) {
        drain(s);
        if (session_state(s) == want)
            return true;
        SDL_Delay(10);
    }
    drain(s);
    return session_state(s) == want;
}

static bool log_has(const char *needle) { return strstr(g_log, needle) != NULL; }

int main(void)
{
    SDL_Init(0);
    const char *id = "usb:0403:6010:ABC:1";
    fake_plug(0, id, "/dev/ttyFAKE1");

    PortTarget t = {0};
    snprintf(t.id, sizeof t.id, "%s", id);
    snprintf(t.path, sizeof t.path, "/dev/ttyFAKE1");
    snprintf(t.name, sizeof t.name, "Fake board");
    SerialConfig cfg = serial_config_default();

    Session *s = session_create(NULL, NULL);
    session_open(s, &t, &cfg, true);
    CHECK(wait_state(s, LINK_CONNECTED, 2000));
    CHECK(log_has("Connected to /dev/ttyFAKE1"));

    fake_inject("hello\r\n");
    SDL_Delay(300);
    drain(s);
    CHECK(log_has("hello"));

    session_write(s, "ls\n", 3);
    SDL_Delay(200);
    CHECK(fake_tx_total() == 3);

    /* Unplug: the link must drop to RECONNECTING and say so. */
    fake_unplug(0);
    CHECK(wait_state(s, LINK_RECONNECTING, 2000));
    CHECK(log_has("lost"));

    /* A different device shows up on the old path: must not be opened. */
    fake_plug(1, "usb:1a86:7523::0", "/dev/ttyFAKE1");
    SDL_Delay(1200);
    CHECK(session_state(s) == LINK_RECONNECTING);
    fake_unplug(1);

    /* Our device comes back under a new path: reconnect to it. */
    fake_plug(0, id, "/dev/ttyFAKE7");
    CHECK(wait_state(s, LINK_CONNECTED, 2000));
    CHECK(log_has("Reconnected to /dev/ttyFAKE7"));
    char path[128];
    session_current_path(s, path, sizeof path);
    CHECK(strcmp(path, "/dev/ttyFAKE7") == 0);

    /* Quick replug (same path, new generation) is detected via the stale handle. */
    fake_unplug(0);
    fake_plug(0, id, "/dev/ttyFAKE7");
    SDL_Delay(1500);
    drain(s);
    CHECK(session_state(s) == LINK_CONNECTED);

    /* User disconnect goes idle and stays idle. */
    session_close(s);
    CHECK(wait_state(s, LINK_IDLE, 2000));
    SDL_Delay(600);
    CHECK(session_state(s) == LINK_IDLE);

    /* Connecting while the device is absent waits for it (auto-reconnect on). */
    fake_unplug(0);
    session_open(s, &t, &cfg, true);
    CHECK(wait_state(s, LINK_RECONNECTING, 2000));
    fake_plug(0, id, "/dev/ttyFAKE2");
    CHECK(wait_state(s, LINK_CONNECTED, 2000));

    /* With auto-reconnect off, a lost device ends the session. */
    session_set_auto_reconnect(s, false);
    fake_unplug(0);
    CHECK(wait_state(s, LINK_IDLE, 2000));
    fake_plug(0, id, "/dev/ttyFAKE2");
    SDL_Delay(800);
    CHECK(session_state(s) == LINK_IDLE);

    session_destroy(s);
    session_buf_free(&g_buf);
    SDL_Quit();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n--- log ---\n%s", failures, g_log);
        return 1;
    }
    printf("reconnect: all tests passed\n");
    return 0;
}
