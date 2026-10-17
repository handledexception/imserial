/* In-memory serial backend for testing the session/reconnect state machine.
 * Devices can be plugged, unplugged and re-enumerated under new paths. */
#include "fake_serial.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool present;
    char id[192];
    char path[128];
    int generation; /* bumps on every plug: open ports from older generations are dead */
} FakeDevice;

static SDL_Mutex *g_mtx;
static FakeDevice g_dev[4];
static uint8_t g_rx[4096];
static size_t g_rx_len;
static size_t g_tx_total;

struct SerialPort {
    int dev;
    int generation;
    SDL_Semaphore *wake;
};

static void lock(void)
{
    if (!g_mtx)
        g_mtx = SDL_CreateMutex();
    SDL_LockMutex(g_mtx);
}
static void unlock(void) { SDL_UnlockMutex(g_mtx); }

void fake_plug(int dev, const char *id, const char *path)
{
    lock();
    g_dev[dev].present = true;
    snprintf(g_dev[dev].id, sizeof g_dev[dev].id, "%s", id);
    snprintf(g_dev[dev].path, sizeof g_dev[dev].path, "%s", path);
    g_dev[dev].generation++;
    unlock();
}

void fake_unplug(int dev)
{
    lock();
    g_dev[dev].present = false;
    unlock();
}

void fake_inject(const char *data)
{
    lock();
    size_t n = strlen(data);
    memcpy(g_rx + g_rx_len, data, n);
    g_rx_len += n;
    unlock();
}

size_t fake_tx_total(void)
{
    lock();
    size_t n = g_tx_total;
    unlock();
    return n;
}

SerialConfig serial_config_default(void)
{
    SerialConfig c = {115200, 8, 1, SERIAL_PARITY_NONE, SERIAL_FLOW_NONE, true, true};
    return c;
}

void serial_config_describe(const SerialConfig *cfg, char *out, size_t outlen)
{
    snprintf(out, outlen, "%u 8N1", (unsigned)cfg->baud);
}

bool serial_open(const char *path, const SerialConfig *cfg, SerialPort **out, char *err, size_t errlen)
{
    (void)cfg;
    lock();
    for (int i = 0; i < 4; i++) {
        if (g_dev[i].present && strcmp(g_dev[i].path, path) == 0) {
            SerialPort *p = calloc(1, sizeof *p);
            p->dev = i;
            p->generation = g_dev[i].generation;
            p->wake = SDL_CreateSemaphore(0);
            *out = p;
            unlock();
            return true;
        }
    }
    unlock();
    snprintf(err, errlen, "Device not present");
    return false;
}

void serial_close(SerialPort *p)
{
    SDL_DestroySemaphore(p->wake);
    free(p);
}

static bool alive(SerialPort *p)
{
    return g_dev[p->dev].present && g_dev[p->dev].generation == p->generation;
}

int serial_read(SerialPort *p, void *buf, size_t len, int timeout_ms)
{
    uint64_t end = SDL_GetTicks() + (uint64_t)timeout_ms;
    for (;;) {
        lock();
        if (!alive(p)) {
            unlock();
            return SERIAL_ERR_GONE;
        }
        if (g_rx_len) {
            size_t n = g_rx_len < len ? g_rx_len : len;
            memcpy(buf, g_rx, n);
            memmove(g_rx, g_rx + n, g_rx_len - n);
            g_rx_len -= n;
            unlock();
            return (int)n;
        }
        unlock();
        if (SDL_GetTicks() >= end)
            return 0;
        if (SDL_WaitSemaphoreTimeout(p->wake, 10))
            return 0;
    }
}

int serial_write(SerialPort *p, const void *buf, size_t len, int timeout_ms)
{
    (void)buf;
    (void)timeout_ms;
    lock();
    bool ok = alive(p);
    if (ok)
        g_tx_total += len;
    unlock();
    return ok ? (int)len : SERIAL_ERR_GONE;
}

void serial_wake(SerialPort *p) { SDL_SignalSemaphore(p->wake); }
bool serial_apply_config(SerialPort *p, const SerialConfig *cfg, char *err, size_t errlen)
{
    (void)p, (void)cfg, (void)err, (void)errlen;
    return true;
}
bool serial_set_dtr(SerialPort *p, bool on) { return (void)p, (void)on, true; }
bool serial_set_rts(SerialPort *p, bool on) { return (void)p, (void)on, true; }
bool serial_send_break(SerialPort *p, int ms) { return (void)p, (void)ms, true; }

bool serial_still_present(SerialPort *p)
{
    lock();
    bool ok = alive(p);
    unlock();
    return ok;
}

int serial_enumerate(SerialPortInfo *out, int max)
{
    int n = 0;
    lock();
    for (int i = 0; i < 4 && n < max; i++) {
        if (!g_dev[i].present)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].id, sizeof out[n].id, "%s", g_dev[i].id);
        snprintf(out[n].path, sizeof out[n].path, "%s", g_dev[i].path);
        snprintf(out[n].name, sizeof out[n].name, "Fake %d", i);
        n++;
    }
    unlock();
    return n;
}
