/* Hardware smoke test for the session/reconnect machinery.
 *   test_session            list ports
 *   test_session PATH [baud] [seconds]
 * Opens PATH with auto-reconnect and prints every state change and the
 * received byte count. Unplug and replug the device while it runs to watch
 * the reconnect logic. */
#include "serial/session.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    SerialPortInfo ports[64];
    int n = serial_enumerate(ports, 64);
    if (argc < 2) {
        for (int i = 0; i < n; i++)
            printf("%-16s %-40s %s  [%s]\n", ports[i].path, ports[i].name, ports[i].detail, ports[i].id);
        return 0;
    }
    SDL_Init(0);
    PortTarget t = {0};
    snprintf(t.path, sizeof t.path, "%s", argv[1]);
    snprintf(t.name, sizeof t.name, "%s", argv[1]);
    for (int i = 0; i < n; i++)
        if (strcmp(ports[i].path, argv[1]) == 0) {
            snprintf(t.id, sizeof t.id, "%s", ports[i].id);
            snprintf(t.name, sizeof t.name, "%s", ports[i].name);
        }
    SerialConfig cfg = serial_config_default();
    if (argc > 2)
        cfg.baud = (uint32_t)atoi(argv[2]);
    int seconds = argc > 3 ? atoi(argv[3]) : 10;
    printf("target id=%s path=%s\n", t.id, t.path);

    Session *s = session_create(NULL, NULL);
    session_open(s, &t, &cfg, true);
    SessionBuf buf = {0};
    uint64_t rx = 0, end = SDL_GetTicks() + (uint64_t)seconds * 1000;
    while (SDL_GetTicks() < end) {
        SDL_Delay(50);
        if (!session_drain(s, &buf))
            continue;
        size_t off = 0;
        const uint8_t *p;
        const SessionRecord *r;
        while ((r = session_buf_next(&buf, &off, &p))) {
            if (r->type == SREC_RX) {
                rx += r->len;
                fwrite(p, 1, r->len, stdout);
            } else if (r->type == SREC_STATE && r->len) {
                printf("\n[state %d sev %d] %.*s\n", r->state, r->severity, (int)r->len, (const char *)p);
            }
        }
        fflush(stdout);
    }
    printf("\nreceived %llu bytes\n", (unsigned long long)rx);
    session_destroy(s);
    session_buf_free(&buf);
    SDL_Quit();
    return 0;
}
