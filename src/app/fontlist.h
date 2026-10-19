/* Installed monospace fonts, for the terminal font picker in Preferences.
 *
 * The platform font directories are walked on a background thread and every
 * TTF/OTF/TTC is opened with FreeType to read its family and style names (a
 * millisecond or so per file, so a few hundred files take a noticeable
 * moment). Only fixed-pitch faces are kept, since the list is for a terminal.
 * The result is kept for the rest of the run; a rescan starts over. */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct FontFace {
    char path[1024]; /* absolute, as the file system reports it */
    char label[192]; /* "Family" for the regular face, else "Family Style" */
} FontFace;

/* Starts the first scan; later calls are no-ops. Safe to call every frame.
 * When the scan finishes an event of type wake_event is pushed (0 = none). */
void fontlist_start(Uint32 wake_event);
/* Drops the list and scans again. */
void fontlist_rescan(Uint32 wake_event);
/* Picks up a finished scan. True once a list (possibly empty) is available. */
bool fontlist_ready(void);
bool fontlist_scanning(void);
int fontlist_count(void);
const FontFace *fontlist_face(int i);
/* fontlist_count() labels, for a combo. */
const char *const *fontlist_labels(void);
/* Index of the face at path (separators and, on Windows, case ignored), or -1. */
int fontlist_find(const char *path);
/* Waits for a running scan; call before SDL_Quit. */
void fontlist_shutdown(void);
