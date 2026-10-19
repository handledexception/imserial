/* Tests for the installed-font scan behind the terminal font picker: the
 * scan finishes and signals its event, the list is sorted and labelled, and
 * path lookup ignores separators (and case on Windows). The number of fonts
 * found depends on the machine, so only the shape of the result is checked. */
#include "app/fontlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

/* Pumps events until the scan has been published or the deadline passes. */
static bool wait_ready(Uint32 wake, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    bool woke = false;
    while (!fontlist_ready() && SDL_GetTicks() < deadline) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50) && e.type == wake)
            woke = true;
    }
    CHECK(woke || fontlist_ready());
    return fontlist_ready();
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 wake = SDL_RegisterEvents(1);

    CHECK(!fontlist_ready());
    CHECK(fontlist_count() == 0);
    CHECK(fontlist_find("nothing") == -1);

    fontlist_start(wake);
    fontlist_start(wake); /* a second call while scanning is a no-op */
    CHECK(wait_ready(wake, 60000));
    CHECK(!fontlist_scanning());
    int n = fontlist_count();
    printf("found %d monospace font file(s)\n", n);
    const char *const *labels = fontlist_labels();
    CHECK(n == 0 || labels != NULL);
    for (int i = 0; i < n; i++) {
        const FontFace *f = fontlist_face(i);
        CHECK(f != NULL);
        CHECK(f->path[0] != 0);
        CHECK(f->label[0] != 0);
        CHECK(labels[i] == f->label);
        if (i > 0)
            CHECK(SDL_strcasecmp(labels[i - 1], labels[i]) != 0); /* no duplicates */
        if (i < 5)
            printf("  %s  (%s)\n", f->label, f->path);
    }
    CHECK(fontlist_face(n) == NULL);
    CHECK(fontlist_face(-1) == NULL);

    if (n > 0) {
        /* Lookup by path tolerates the other separator (and case on Windows). */
        char alt[1024];
        snprintf(alt, sizeof alt, "%s", fontlist_face(n - 1)->path);
        for (char *p = alt; *p; p++) {
            if (*p == '\\')
                *p = '/';
            else if (*p == '/')
                *p = '\\';
#ifdef _WIN32
            else if (*p >= 'a' && *p <= 'z')
                *p -= 'a' - 'A';
#endif
        }
        CHECK(fontlist_find(fontlist_face(n - 1)->path) == n - 1);
        CHECK(fontlist_find(alt) == n - 1);
        CHECK(fontlist_find("C:/definitely/not/a/font.ttf") == -1);
    }

    /* A rescan starts over and lands on the same list. */
    fontlist_rescan(wake);
    CHECK(wait_ready(wake, 60000));
    CHECK(fontlist_count() == n);

    fontlist_shutdown();
    CHECK(!fontlist_ready());
    CHECK(fontlist_count() == 0);
    SDL_Quit();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("all fontlist tests passed\n");
    return failures ? 1 : 0;
}
