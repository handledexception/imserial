/* serial.h implemented on top of sigrok's libserialport, which handles the
 * per-platform details (termios/termios2 on Linux, overlapped I/O + SetupAPI
 * on Windows, IOKit on macOS). This file adds what libserialport lacks for a
 * terminal: a wakeable wait, hangup detection and stable port identities. */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "serial/serial.h"

#include <libserialport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#endif

struct SerialPort {
    struct sp_port *port;
    struct sp_event_set *events;
    char name[128];
    int idle_wakeups; /* consecutive "readable" wakeups that produced no data */
#ifdef _WIN32
    HANDLE wake_event;
#else
    int wake_rd, wake_wr;
#endif
};

SerialConfig serial_config_default(void)
{
    SerialConfig c = {0};
    c.baud = 115200;
    c.data_bits = 8;
    c.stop_bits = 1;
    c.parity = SERIAL_PARITY_NONE;
    c.flow = SERIAL_FLOW_NONE;
    c.dtr = true;
    c.rts = true;
    return c;
}

void serial_config_describe(const SerialConfig *cfg, char *out, size_t outlen)
{
    static const char par[] = {'N', 'O', 'E', 'M', 'S'};
    snprintf(out, outlen, "%u %u%c%u", (unsigned)cfg->baud, (unsigned)cfg->data_bits,
             par[cfg->parity % 5], (unsigned)cfg->stop_bits);
}

static uint64_t mono_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* Turns libserialport's last error into a short, actionable message. */
static void last_error(char *err, size_t errlen, const char *what)
{
    if (!err || !errlen)
        return;
    int code = sp_last_error_code();
#ifdef _WIN32
    if (code == ERROR_ACCESS_DENIED) {
        snprintf(err, errlen, "Port is busy (open in another program)");
        return;
    }
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_DEV_NOT_EXIST) {
        snprintf(err, errlen, "Device not present");
        return;
    }
#else
    if (code == EACCES) {
        snprintf(err, errlen, "Permission denied (is your user in the 'dialout' group?)");
        return;
    }
    if (code == EBUSY || code == EWOULDBLOCK) {
        snprintf(err, errlen, "Port is busy (open in another program)");
        return;
    }
    if (code == ENOENT || code == ENODEV || code == ENXIO) {
        snprintf(err, errlen, "Device not present");
        return;
    }
#endif
    char *msg = sp_last_error_message();
    snprintf(err, errlen, "%s: %s", what, msg ? msg : "unknown error");
    if (msg)
        sp_free_error_message(msg);
}

bool serial_apply_config(SerialPort *p, const SerialConfig *cfg, char *err, size_t errlen)
{
    static const enum sp_parity parity[] = {SP_PARITY_NONE, SP_PARITY_ODD, SP_PARITY_EVEN,
                                            SP_PARITY_MARK, SP_PARITY_SPACE};
    static const enum sp_flowcontrol flow[] = {SP_FLOWCONTROL_NONE, SP_FLOWCONTROL_RTSCTS,
                                               SP_FLOWCONTROL_XONXOFF};
    if (sp_set_baudrate(p->port, (int)cfg->baud) != SP_OK) {
        last_error(err, errlen, "Unsupported baud rate");
        return false;
    }
    if (sp_set_bits(p->port, cfg->data_bits) != SP_OK || sp_set_parity(p->port, parity[cfg->parity % 5]) != SP_OK ||
        sp_set_stopbits(p->port, cfg->stop_bits) != SP_OK ||
        sp_set_flowcontrol(p->port, flow[cfg->flow % 3]) != SP_OK) {
        last_error(err, errlen, "Unable to configure port");
        return false;
    }
    /* Modem lines are best effort: some adapters don't implement them. */
    serial_set_dtr(p, cfg->dtr);
    if (cfg->flow != SERIAL_FLOW_RTSCTS)
        serial_set_rts(p, cfg->rts);
    return true;
}

/* Adds our wake handle to libserialport's event set so sp_wait() returns
 * when another thread calls serial_wake(). The struct is public API. */
static bool add_wake_handle(SerialPort *p)
{
    struct sp_event_set *es = p->events;
#ifdef _WIN32
    p->wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!p->wake_event)
        return false;
    HANDLE *h = realloc(es->handles, sizeof(HANDLE) * (es->count + 1));
    if (!h)
        return false;
    es->handles = h;
    h[es->count] = p->wake_event;
#else
    int fds[2];
    if (pipe(fds) != 0)
        return false;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFL, O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    p->wake_rd = fds[0];
    p->wake_wr = fds[1];
    int *h = realloc(es->handles, sizeof(int) * (es->count + 1));
    if (!h)
        return false;
    es->handles = h;
    h[es->count] = p->wake_rd;
#endif
    enum sp_event *m = realloc(es->masks, sizeof(enum sp_event) * (es->count + 1));
    if (!m)
        return false;
    es->masks = m;
    m[es->count] = SP_EVENT_RX_READY;
    es->count++;
    return true;
}

bool serial_open(const char *path, const SerialConfig *cfg, SerialPort **out, char *err, size_t errlen)
{
    *out = NULL;
    SerialPort *p = calloc(1, sizeof *p);
#ifndef _WIN32
    p->wake_rd = p->wake_wr = -1;
#endif
    snprintf(p->name, sizeof p->name, "%s", path);
    if (sp_get_port_by_name(path, &p->port) != SP_OK) {
        snprintf(err, errlen, "Device not present");
        free(p);
        return false;
    }
    if (sp_open(p->port, SP_MODE_READ_WRITE) != SP_OK) {
        last_error(err, errlen, "Unable to open port");
        sp_free_port(p->port);
        free(p);
        return false;
    }
    if (!serial_apply_config(p, cfg, err, errlen)) {
        serial_close(p);
        return false;
    }
    if (sp_new_event_set(&p->events) != SP_OK ||
        sp_add_port_events(p->events, p->port, SP_EVENT_RX_READY | SP_EVENT_ERROR) != SP_OK ||
        !add_wake_handle(p)) {
        snprintf(err, errlen, "Unable to set up port events");
        serial_close(p);
        return false;
    }
    sp_flush(p->port, SP_BUF_BOTH);
    *out = p;
    return true;
}

void serial_close(SerialPort *p)
{
    if (!p)
        return;
    if (p->events)
        sp_free_event_set(p->events);
    sp_close(p->port);
    sp_free_port(p->port);
#ifdef _WIN32
    if (p->wake_event)
        CloseHandle(p->wake_event);
#else
    if (p->wake_rd >= 0)
        close(p->wake_rd);
    if (p->wake_wr >= 0)
        close(p->wake_wr);
#endif
    free(p);
}

static int classify_error(void)
{
#ifdef _WIN32
    return SERIAL_ERR_GONE; /* Windows reports unplug as assorted errors */
#else
    int e = sp_last_error_code();
    return (e == EIO || e == ENXIO || e == ENODEV || e == EBADF || e == ENOENT) ? SERIAL_ERR_GONE
                                                                                : SERIAL_ERR_IO;
#endif
}

static bool consume_wake(SerialPort *p)
{
#ifdef _WIN32
    return WaitForSingleObject(p->wake_event, 0) == WAIT_OBJECT_0;
#else
    char tmp[64];
    bool woken = false;
    while (read(p->wake_rd, tmp, sizeof tmp) > 0)
        woken = true;
    return woken;
#endif
}

int serial_read(SerialPort *p, void *buf, size_t len, int timeout_ms)
{
    if (timeout_ms < 1)
        timeout_ms = 1; /* 0 means "forever" to sp_wait */
    uint64_t t0 = mono_ms();
    if (sp_wait(p->events, (unsigned)timeout_ms) != SP_OK)
        return classify_error();
    bool woken = consume_wake(p);
    int n = sp_nonblocking_read(p->port, buf, len);
    if (n < 0)
        return classify_error();
    if (n > 0) {
        p->idle_wakeups = 0;
        return n;
    }
#ifndef _WIN32
    /* A hung-up tty (USB adapter unplugged or reset) polls as readable but
     * reads zero bytes forever. A healthy port never does that repeatedly. */
    if (!woken && mono_ms() - t0 + 2 < (uint64_t)timeout_ms) {
        if (++p->idle_wakeups >= 3)
            return SERIAL_ERR_GONE;
    } else {
        p->idle_wakeups = 0; /* timeout or deliberate wake: not a hangup symptom */
    }
#else
    (void)woken;
    (void)t0;
#endif
    return 0;
}

int serial_write(SerialPort *p, const void *buf, size_t len, int timeout_ms)
{
    int n = sp_blocking_write(p->port, buf, len, timeout_ms < 1 ? 1 : (unsigned)timeout_ms);
    return n < 0 ? classify_error() : n;
}

void serial_wake(SerialPort *p)
{
    if (!p)
        return;
#ifdef _WIN32
    SetEvent(p->wake_event);
#else
    char c = 1;
    ssize_t r = write(p->wake_wr, &c, 1);
    (void)r;
#endif
}

bool serial_set_dtr(SerialPort *p, bool on) { return sp_set_dtr(p->port, on ? SP_DTR_ON : SP_DTR_OFF) == SP_OK; }
bool serial_set_rts(SerialPort *p, bool on) { return sp_set_rts(p->port, on ? SP_RTS_ON : SP_RTS_OFF) == SP_OK; }

bool serial_send_break(SerialPort *p, int duration_ms)
{
    if (sp_start_break(p->port) != SP_OK)
        return false;
#ifdef _WIN32
    Sleep((DWORD)duration_ms);
#else
    struct timespec ts = {duration_ms / 1000, (long)(duration_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
    return sp_end_break(p->port) == SP_OK;
}

bool serial_still_present(SerialPort *p)
{
    struct sp_port *tmp;
    if (sp_get_port_by_name(p->name, &tmp) != SP_OK)
        return false;
    sp_free_port(tmp);
    return true;
}

/* ---- Enumeration --------------------------------------------------------- */

/* libserialport doesn't expose the USB interface number, which is what tells
 * the ports of a multi-port adapter (FTDI 2232, Digilent boards) apart. */
static int usb_interface_number(const char *path)
{
#ifdef __linux__
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char link[PATH_MAX], dir[PATH_MAX];
    snprintf(link, sizeof link, "/sys/class/tty/%s/device", base);
    if (!realpath(link, dir))
        return -1;
    for (int depth = 0; depth < 3; depth++) {
        char attr[PATH_MAX + 32];
        snprintf(attr, sizeof attr, "%s/bInterfaceNumber", dir);
        FILE *f = fopen(attr, "r");
        if (f) {
            unsigned v = 0;
            int ok = fscanf(f, "%x", &v);
            fclose(f);
            return ok == 1 ? (int)v : -1;
        }
        char *s = strrchr(dir, '/');
        if (!s || s == dir)
            break;
        *s = 0;
    }
#else
    (void)path;
#endif
    return -1;
}

static int cmp_natural(const void *a, const void *b)
{
    const char *x = ((const SerialPortInfo *)a)->path, *y = ((const SerialPortInfo *)b)->path;
    while (*x && *y) {
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            char *ex, *ey;
            unsigned long nx = strtoul(x, &ex, 10), ny = strtoul(y, &ey, 10);
            if (nx != ny)
                return nx < ny ? -1 : 1;
            x = ex, y = ey;
            continue;
        }
        if (*x != *y)
            return (unsigned char)*x - (unsigned char)*y;
        x++, y++;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

int serial_enumerate(SerialPortInfo *out, int max)
{
    struct sp_port **list = NULL;
    if (sp_list_ports(&list) != SP_OK)
        return 0;
    int count = 0;
    for (int i = 0; list[i] && count < max; i++) {
        struct sp_port *sp = list[i];
        SerialPortInfo *info = &out[count];
        memset(info, 0, sizeof *info);
        info->iface = -1;
        const char *name = sp_get_port_name(sp);
        snprintf(info->path, sizeof info->path, "%s", name);
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        const char *desc = sp_get_port_description(sp);

        if (sp_get_port_transport(sp) == SP_TRANSPORT_USB) {
            int vid = 0, pid = 0;
            sp_get_port_usb_vid_pid(sp, &vid, &pid);
            info->usb = true;
            info->vid = (uint16_t)vid;
            info->pid = (uint16_t)pid;
            info->iface = usb_interface_number(name);
            const char *s;
            if ((s = sp_get_port_usb_manufacturer(sp)))
                snprintf(info->manufacturer, sizeof info->manufacturer, "%s", s);
            if ((s = sp_get_port_usb_serial(sp)))
                snprintf(info->serial, sizeof info->serial, "%s", s);
            const char *product = sp_get_port_usb_product(sp);
            snprintf(info->name, sizeof info->name, "%s", product && *product ? product : (desc ? desc : base));
            snprintf(info->id, sizeof info->id, "usb:%04x:%04x:%s:%d", info->vid, info->pid, info->serial,
                     info->iface);
            if (info->iface >= 0)
                snprintf(info->detail, sizeof info->detail, "%s \xC2\xB7 %04x:%04x \xC2\xB7 if %d", base,
                         info->vid, info->pid, info->iface);
            else
                snprintf(info->detail, sizeof info->detail, "%s \xC2\xB7 %04x:%04x", base, info->vid, info->pid);
        } else {
            snprintf(info->name, sizeof info->name, "%s", desc && *desc ? desc : base);
            snprintf(info->id, sizeof info->id, "path:%s", name);
            snprintf(info->detail, sizeof info->detail, "%s", base);
        }
        count++;
    }
    sp_free_port_list(list);
    qsort(out, (size_t)count, sizeof *out, cmp_natural);
    return count;
}
