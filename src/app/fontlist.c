/* Installed-font scan for the terminal font picker (see fontlist.h). */
#include "app/fontlist.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FACES 4096
#define MAX_DEPTH 6

/* What the scan thread builds; handed to the main thread when done. */
typedef struct Scan {
    FT_Library ft;
    FontFace *faces;
    int count, cap;
    int depth;
} Scan;

enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE };

static SDL_Thread *s_thread;
static SDL_AtomicInt s_state;
static SDL_AtomicInt s_abort;
static Scan s_scan; /* owned by the thread while SCAN_RUNNING */
static Uint32 s_wake_event;

static FontFace *s_faces; /* the published list */
static const char **s_labels;
static int s_count;
static bool s_ready;

/* ---- Scan thread --------------------------------------------------------- */

static bool is_regular_style(const char *s)
{
    return !s || !*s || !strcmp(s, "Regular") || !strcmp(s, "Normal") || !strcmp(s, "Book");
}

static bool has_font_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return !SDL_strcasecmp(dot, ".ttf") || !SDL_strcasecmp(dot, ".otf") || !SDL_strcasecmp(dot, ".ttc");
}

/* Terminal fonts: fixed pitch per the face flags, or named like one (some
 * patched/programming fonts forget to set the flag). */
static bool looks_monospace(FT_Face face)
{
    if (FT_IS_FIXED_WIDTH(face))
        return true;
    const char *fam = face->family_name;
    if (!fam)
        return false;
    return SDL_strcasestr(fam, "mono") || SDL_strcasestr(fam, "code") || SDL_strcasestr(fam, "console") ||
           SDL_strcasestr(fam, "terminal") || SDL_strcasestr(fam, "courier");
}

static void consider_file(Scan *s, const char *path, const char *fname)
{
    if (!has_font_ext(fname) || s->count >= MAX_FACES)
        return;
    FT_Face face = NULL;
    if (FT_New_Face(s->ft, path, 0, &face) != 0 || !face)
        return;
    if (FT_IS_SCALABLE(face) && looks_monospace(face)) {
        if (s->count == s->cap) {
            int cap = s->cap ? s->cap * 2 : 64;
            FontFace *grown = realloc(s->faces, (size_t)cap * sizeof *grown);
            if (grown) {
                s->faces = grown;
                s->cap = cap;
            }
        }
        if (s->count < s->cap) {
            FontFace *f = &s->faces[s->count++];
            char family[128];
            if (face->family_name && *face->family_name) {
                snprintf(family, sizeof family, "%s", face->family_name);
            } else {
                snprintf(family, sizeof family, "%s", fname);
                char *dot = strrchr(family, '.');
                if (dot)
                    *dot = 0;
            }
            snprintf(f->path, sizeof f->path, "%s", path);
            if (is_regular_style(face->style_name))
                snprintf(f->label, sizeof f->label, "%s", family);
            else
                snprintf(f->label, sizeof f->label, "%s %s", family, face->style_name);
        }
    }
    FT_Done_Face(face);
}

static SDL_EnumerationResult SDLCALL on_entry(void *ud, const char *dirname, const char *fname)
{
    Scan *s = ud;
    if (SDL_GetAtomicInt(&s_abort))
        return SDL_ENUM_FAILURE;
    size_t dl = strlen(dirname);
    bool sep = dl && (dirname[dl - 1] == '/' || dirname[dl - 1] == '\\');
    char path[1024];
    if ((size_t)snprintf(path, sizeof path, "%s%s%s", dirname, sep ? "" : "/", fname) >= sizeof path)
        return SDL_ENUM_CONTINUE;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info))
        return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        /* Linux nests families in subfolders; keep the walk bounded. */
        if (s->depth < MAX_DEPTH) {
            s->depth++;
            SDL_EnumerateDirectory(path, on_entry, s);
            s->depth--;
        }
    } else if (info.type == SDL_PATHTYPE_FILE) {
        consider_file(s, path, fname);
    }
    return SDL_ENUM_CONTINUE;
}

static void scan_dir(Scan *s, const char *dir)
{
    if (!dir || !*dir)
        return;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY)
        return;
    s->depth = 0;
    SDL_EnumerateDirectory(dir, on_entry, s);
}

/* Family, then the regular face before its siblings, then style, then path:
 * the same order every run. Labels encode family + style, so comparing them
 * with a regular-first tweak is enough. */
static int cmp_face(const void *pa, const void *pb)
{
    const FontFace *a = pa, *b = pb;
    int c = SDL_strcasecmp(a->label, b->label);
    if (c == 0)
        return strcmp(a->path, b->path);
    /* "Family" (regular) sorts before "Family Bold". */
    size_t la = strlen(a->label), lb = strlen(b->label);
    if (la < lb && !SDL_strncasecmp(a->label, b->label, la) && b->label[la] == ' ')
        return -1;
    if (lb < la && !SDL_strncasecmp(a->label, b->label, lb) && a->label[lb] == ' ')
        return 1;
    return c;
}

static int SDLCALL scan_main(void *ud)
{
    Scan *s = ud;
    if (FT_Init_FreeType(&s->ft) == 0) {
#if defined(_WIN32)
        char dir[1024];
        const char *local = SDL_getenv("LOCALAPPDATA");
        if (local && *local) {
            snprintf(dir, sizeof dir, "%s\\Microsoft\\Windows\\Fonts", local); /* per-user installs */
            scan_dir(s, dir);
        }
        const char *windir = SDL_getenv("WINDIR");
        snprintf(dir, sizeof dir, "%s\\Fonts", windir && *windir ? windir : "C:\\Windows");
        scan_dir(s, dir);
#else
        const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME); /* ends with a separator */
        char dir[1024];
#if defined(__APPLE__)
        if (home) {
            snprintf(dir, sizeof dir, "%sLibrary/Fonts", home);
            scan_dir(s, dir);
        }
        scan_dir(s, "/Library/Fonts");
        scan_dir(s, "/System/Library/Fonts");
#else
        if (home) {
            snprintf(dir, sizeof dir, "%s.local/share/fonts", home);
            scan_dir(s, dir);
            snprintf(dir, sizeof dir, "%s.fonts", home);
            scan_dir(s, dir);
        }
        scan_dir(s, "/usr/local/share/fonts");
        scan_dir(s, "/usr/share/fonts");
#endif
#endif
        FT_Done_FreeType(s->ft);
        s->ft = NULL;
    }
    if (s->count > 1)
        qsort(s->faces, (size_t)s->count, sizeof *s->faces, cmp_face);
    /* The same face installed twice (or a .ttf beside its .ttc): keep the first. */
    int n = 0;
    for (int i = 0; i < s->count; i++)
        if (n == 0 || strcmp(s->faces[n - 1].label, s->faces[i].label) != 0)
            s->faces[n++] = s->faces[i];
    s->count = n;

    SDL_SetAtomicInt(&s_state, SCAN_DONE);
    if (s_wake_event) {
        SDL_Event e;
        SDL_zero(e);
        e.type = s_wake_event;
        SDL_PushEvent(&e);
    }
    return 0;
}

/* ---- Main thread ---------------------------------------------------------- */

static void publish(void)
{
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    free(s_faces);
    free(s_labels);
    s_faces = s_scan.faces;
    s_count = s_scan.count;
    s_labels = s_count ? malloc((size_t)s_count * sizeof *s_labels) : NULL;
    if (s_labels)
        for (int i = 0; i < s_count; i++)
            s_labels[i] = s_faces[i].label;
    else
        s_count = 0;
    memset(&s_scan, 0, sizeof s_scan);
    s_ready = true;
    SDL_SetAtomicInt(&s_state, SCAN_IDLE);
}

bool fontlist_ready(void)
{
    if (SDL_GetAtomicInt(&s_state) == SCAN_DONE)
        publish();
    return s_ready;
}

bool fontlist_scanning(void)
{
    return SDL_GetAtomicInt(&s_state) == SCAN_RUNNING;
}

void fontlist_start(Uint32 wake_event)
{
    if (s_ready || SDL_GetAtomicInt(&s_state) != SCAN_IDLE)
        return;
    s_wake_event = wake_event;
    memset(&s_scan, 0, sizeof s_scan);
    SDL_SetAtomicInt(&s_abort, 0);
    SDL_SetAtomicInt(&s_state, SCAN_RUNNING);
    s_thread = SDL_CreateThread(scan_main, "fontscan", &s_scan);
    if (!s_thread) {
        /* No thread: publish the empty list so the picker still works. */
        SDL_SetAtomicInt(&s_state, SCAN_DONE);
        fontlist_ready();
    }
}

void fontlist_rescan(Uint32 wake_event)
{
    if (SDL_GetAtomicInt(&s_state) != SCAN_IDLE)
        return;
    s_ready = false;
    fontlist_start(wake_event);
}

int fontlist_count(void)
{
    return fontlist_ready() ? s_count : 0;
}

const FontFace *fontlist_face(int i)
{
    return (fontlist_ready() && i >= 0 && i < s_count) ? &s_faces[i] : NULL;
}

const char *const *fontlist_labels(void)
{
    return fontlist_ready() ? s_labels : NULL;
}

static bool same_path(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca == '\\')
            ca = '/';
        if (cb == '\\')
            cb = '/';
#if defined(_WIN32)
        ca = (unsigned char)tolower(ca);
        cb = (unsigned char)tolower(cb);
#endif
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

int fontlist_find(const char *path)
{
    if (!path || !*path || !fontlist_ready())
        return -1;
    for (int i = 0; i < s_count; i++)
        if (same_path(s_faces[i].path, path))
            return i;
    return -1;
}

void fontlist_shutdown(void)
{
    SDL_SetAtomicInt(&s_abort, 1);
    if (s_thread) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
        free(s_scan.faces);
        memset(&s_scan, 0, sizeof s_scan);
    }
    free(s_faces);
    free(s_labels);
    s_faces = NULL;
    s_labels = NULL;
    s_count = 0;
    s_ready = false;
    SDL_SetAtomicInt(&s_state, SCAN_IDLE);
}
