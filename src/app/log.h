/* Session logging (PuTTY-style): each tab can write what it receives to a
 * file, either as printable text or as the raw byte stream. */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    LOG_NONE,
    LOG_PRINTABLE, /* text only: escape sequences and control codes stripped */
    LOG_ALL,       /* every received byte, unmodified */
} LogMode;

typedef enum {
    LOG_EXISTING_ASK, /* ask each time: overwrite, append or don't log */
    LOG_EXISTING_OVERWRITE,
    LOG_EXISTING_APPEND,
} LogExisting;

typedef struct {
    LogMode mode;
    char path[1024];      /* file path pattern (&Y &M &D date, &T time, &P port); empty = default */
    LogExisting existing; /* what to do if the file already exists */
    bool flush;           /* flush to disk after every write */
    bool header;          /* write a header line when the log is opened */
    bool timestamps;      /* printable logs: prefix each line with the time */
} LogSettings;

typedef struct {
    FILE *f; /* opened shared, so other programs can read (tail) it while we write */
    char path[1400];
    LogMode mode;
    bool timestamps, flush;
    bool failed;  /* opening failed; don't retry until the settings change */
    bool pending; /* waiting for the user to say what to do with an existing file */
    bool dropped; /* pending output overflowed the buffer */
    /* printable filter */
    int pstate;
    bool line_start, last_cr;
    char *buf;
    size_t len;
} SessionLog;

/* The default path pattern: <Documents>/imserial logs/imserial-&P-&Y&M&D-&T.log */
void log_default_path(char *out, size_t outlen);

/* Starts logging a session on `port`; `info` goes in the header line. If the
 * file exists and the setting is "ask", the log becomes pending: output is
 * buffered until log_resolve(). On failure fills err and returns false. */
bool log_open(SessionLog *lg, const LogSettings *cfg, const char *port, const char *info, char *err, size_t errlen);
/* Answers the "file exists" question: overwrite, append, or (neither) don't log. */
bool log_resolve(SessionLog *lg, bool overwrite, bool append, char *err, size_t errlen);
void log_close(SessionLog *lg);
static inline bool log_is_open(const SessionLog *lg) { return lg->f != NULL || lg->pending; }

void log_rx(SessionLog *lg, const uint8_t *data, size_t len, double time);
/* A connection event, as its own line (printable logs only). */
void log_event(SessionLog *lg, const char *text, double time);
