/* A Session owns one serial connection and its I/O thread. The thread runs a
 * small state machine that keeps the link up: when the device disappears it
 * waits for a device with the same stable identity to come back and reopens
 * it. Everything that happens (data, state changes) is appended to an event
 * stream that the UI thread drains once per frame. */
#pragma once
#include "serial/serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LINK_IDLE,         /* not connected, not trying */
    LINK_CONNECTING,   /* first attempt in progress */
    LINK_CONNECTED,
    LINK_RECONNECTING, /* lost (or not yet present); retrying until it's back */
} LinkState;

typedef enum {
    SEV_INFO,
    SEV_OK,
    SEV_WARN,
    SEV_ERROR,
} Severity;

typedef struct {
    char id[192];   /* stable identity from serial_enumerate(); may be empty */
    char path[128]; /* last known device path */
    char name[96];  /* display name */
} PortTarget;

typedef enum {
    SREC_RX,    /* payload: received bytes */
    SREC_TX,    /* payload: bytes written to the port */
    SREC_STATE, /* payload: status message; state/severity in header */
} SessionRecordType;

typedef struct {
    uint8_t type;
    uint8_t state;
    uint8_t severity;
    uint8_t pad;
    uint32_t len; /* payload bytes following the header */
    double time;  /* wall clock, seconds since the Unix epoch */
} SessionRecord;

/* A batch of records: [SessionRecord][payload][SessionRecord][payload]... */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} SessionBuf;

typedef struct Session Session;
typedef void (*SessionWakeFn)(void *userdata);

Session *session_create(SessionWakeFn wake, void *userdata);
void session_destroy(Session *s);

void session_open(Session *s, const PortTarget *target, const SerialConfig *cfg, bool auto_reconnect);
void session_close(Session *s);
void session_set_config(Session *s, const SerialConfig *cfg);
void session_set_auto_reconnect(Session *s, bool on);
void session_write(Session *s, const void *data, size_t len);
void session_send_break(Session *s);

LinkState session_state(Session *s);
/* Seconds since the current state was entered. */
double session_state_age(Session *s);
void session_status(Session *s, char *msg, size_t msglen, Severity *sev);
/* The device path currently open (or last opened). */
void session_current_path(Session *s, char *out, size_t outlen);
uint64_t session_rx_bytes(Session *s);
uint64_t session_tx_bytes(Session *s);

/* Swaps out pending records into *out (which is reset first). Returns true
 * if anything was pending. Call from the UI thread. */
bool session_drain(Session *s, SessionBuf *out);
void session_buf_free(SessionBuf *b);

/* Iteration helper: returns the next record or NULL; *payload points into buf. */
const SessionRecord *session_buf_next(const SessionBuf *b, size_t *offset, const uint8_t **payload);

double wall_time_now(void);
