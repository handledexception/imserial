/* Thin cross-platform serial port API. One implementation per platform:
 * serial_posix.c (Linux/macOS) and serial_win32.c (Windows). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SERIAL_PARITY_NONE,
    SERIAL_PARITY_ODD,
    SERIAL_PARITY_EVEN,
    SERIAL_PARITY_MARK,
    SERIAL_PARITY_SPACE,
} SerialParity;

typedef enum {
    SERIAL_FLOW_NONE,
    SERIAL_FLOW_RTSCTS,
    SERIAL_FLOW_XONXOFF,
} SerialFlow;

typedef struct {
    uint32_t baud;
    uint8_t data_bits; /* 5..8 */
    uint8_t stop_bits; /* 1 or 2 */
    SerialParity parity;
    SerialFlow flow;
    bool dtr;
    bool rts;
} SerialConfig;

SerialConfig serial_config_default(void);
/* "115200 8N1" style summary. */
void serial_config_describe(const SerialConfig *cfg, char *out, size_t outlen);

typedef struct SerialPort SerialPort;

/* Result codes for serial_read/serial_write. */
enum {
    SERIAL_ERR_GONE = -1, /* device disappeared (unplugged, hung up) */
    SERIAL_ERR_IO = -2,   /* other I/O error; treat as connection lost */
};

bool serial_open(const char *path, const SerialConfig *cfg, SerialPort **out, char *err, size_t errlen);
void serial_close(SerialPort *port);

/* Waits up to timeout_ms for incoming data. Returns the number of bytes read
 * (>0), 0 on timeout or serial_wake(), or a negative SERIAL_ERR_* value. */
int serial_read(SerialPort *port, void *buf, size_t len, int timeout_ms);

/* Writes all bytes (waiting up to timeout_ms for the driver to accept them).
 * Returns bytes written or a negative SERIAL_ERR_* value. */
int serial_write(SerialPort *port, const void *buf, size_t len, int timeout_ms);

/* Thread-safe: makes a blocked serial_read() return 0 promptly. */
void serial_wake(SerialPort *port);

bool serial_apply_config(SerialPort *port, const SerialConfig *cfg, char *err, size_t errlen);
bool serial_set_dtr(SerialPort *port, bool on);
bool serial_set_rts(SerialPort *port, bool on);
bool serial_send_break(SerialPort *port, int duration_ms);

/* False once the device node behind an open port has gone away. Cheap. */
bool serial_still_present(SerialPort *port);

/* ---- Enumeration --------------------------------------------------------- */

typedef struct {
    char path[128];         /* /dev/ttyUSB1, COM3 */
    char id[192];           /* stable identity that survives re-enumeration */
    char name[96];          /* human friendly: product string or driver name */
    char detail[128];       /* secondary line: "ttyUSB1 · 0403:6010 · if 1" */
    char manufacturer[64];
    char serial[64];
    uint16_t vid, pid;
    int iface;              /* USB interface number or -1 */
    bool usb;
} SerialPortInfo;

/* Fills up to max entries, sorted by path. Returns the number found. */
int serial_enumerate(SerialPortInfo *out, int max);
