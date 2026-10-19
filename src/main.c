/* imserial — a fast, minimal serial terminal.
 *
 * Event-driven main loop: the app sleeps in SDL_WaitEventTimeout() and only
 * renders when something changed (input, serial data, animations), so an idle
 * window costs ~0% CPU. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* sigaction */
#endif
#include "app/app.h"
#include "ui/ui.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cimgui_impl.h"
#include <SDL3/SDL_main.h> /* WinMain shim for the Windows GUI subsystem */
#include <SDL3/SDL_opengl.h>

#include "app/fontlist.h"

#define ASSET(name)                                                                                                  \
    extern const unsigned char asset_##name[];                                                                       \
    extern const unsigned int asset_##name##_size;
ASSET(Inter_Regular_ttf)
ASSET(Inter_SemiBold_ttf)
ASSET(IBMPlexMono_Regular_ttf)
ASSET(IBMPlexMono_Bold_ttf)
ASSET(JetBrainsMono_Regular_ttf)
ASSET(JetBrainsMono_Bold_ttf)
ASSET(codicon_ttf)

static App s_app;
App *g_app = &s_app;

/* SIGTERM/SIGHUP request a clean shutdown (settings saved, ports closed). */
static volatile sig_atomic_t g_quit_signal;
#ifndef _WIN32
static void on_quit_signal(int sig)
{
    (void)sig;
    g_quit_signal = 1;
}
#endif

/* ---- Title bar hit testing ------------------------------------------------ */

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *win, const SDL_Point *pt, void *data)
{
    App *a = data;
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    SDL_WindowFlags fl = SDL_GetWindowFlags(win);
    SDL_HitTestResult r = SDL_HITTEST_NORMAL;
    if (!(fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int b = (int)(5 * a->ui_scale), c = (int)(12 * a->ui_scale);
        bool l = pt->x < b, rt = pt->x >= w - b, t = pt->y < b, bt = pt->y >= h - b;
        bool lc = pt->x < c, rc = pt->x >= w - c, tc = pt->y < c, bc = pt->y >= h - c;
        if ((t && lc) || (l && tc))
            r = SDL_HITTEST_RESIZE_TOPLEFT;
        else if ((t && rc) || (rt && tc))
            r = SDL_HITTEST_RESIZE_TOPRIGHT;
        else if ((bt && lc) || (l && bc))
            r = SDL_HITTEST_RESIZE_BOTTOMLEFT;
        else if ((bt && rc) || (rt && bc))
            r = SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        else if (t)
            r = SDL_HITTEST_RESIZE_TOP;
        else if (bt)
            r = SDL_HITTEST_RESIZE_BOTTOM;
        else if (l)
            r = SDL_HITTEST_RESIZE_LEFT;
        else if (rt)
            r = SDL_HITTEST_RESIZE_RIGHT;
    }
    if (r == SDL_HITTEST_NORMAL && pt->y < a->titlebar_h) {
        r = SDL_HITTEST_DRAGGABLE;
        for (int i = 0; i < a->nholes; i++) {
            const UiRect *q = &a->hit_holes[i];
            if (pt->x >= q->x && pt->x < q->x + q->w && pt->y >= q->y && pt->y < q->y + q->h) {
                r = SDL_HITTEST_NORMAL;
                break;
            }
        }
    }
    a->last_hit = r;
    return r;
}

/* ---- Window icon (ui/appicon.c draws it) ---------------------------------- */

static SDL_Surface *icon_surface(int n)
{
    SDL_Surface *s = SDL_CreateSurface(n, n, SDL_PIXELFORMAT_RGBA32);
    if (s)
        app_icon_render(s->pixels, s->pitch, n);
    return s;
}

/* One image per common size, so the OS never has to scale the small ones
 * (they're drawn on the pixel grid). */
static void set_window_icon(SDL_Window *win)
{
    SDL_Surface *s = icon_surface(64);
    if (!s)
        return;
    static const int alt[] = {16, 20, 24, 32, 48, 128, 256};
    for (size_t i = 0; i < sizeof alt / sizeof alt[0]; i++) {
        SDL_Surface *a = icon_surface(alt[i]);
        if (a) {
            SDL_AddSurfaceAlternateImage(s, a);
            SDL_DestroySurface(a);
        }
    }
    SDL_SetWindowIcon(win, s);
    SDL_DestroySurface(s);
}

/* ---- Fonts & style -------------------------------------------------------- */

/* Adds an embedded font, or the font file at path when it is non-NULL.
 * Returns NULL (without asserting) if the file is missing or not a font. */
static ImFont *add_font(ImGuiIO *io, const unsigned char *data, unsigned size, const char *path, float px,
                        unsigned flags, bool merge_icons)
{
    ImFontConfig *cfg = ImFontConfig_ImFontConfig();
    cfg->FontLoaderFlags = flags;
    ImFont *f;
    if (path) {
        cfg->Flags |= ImFontFlags_NoLoadError;
        f = ImFontAtlas_AddFontFromFileTTF(io->Fonts, path, px, cfg, NULL);
    } else {
        cfg->FontDataOwnedByAtlas = false;
        f = ImFontAtlas_AddFontFromMemoryTTF(io->Fonts, (void *)data, (int)size, px, cfg, NULL);
    }
    if (f && merge_icons) {
        static const ImWchar icon_ranges[] = {ICON_MIN, ICON_MAX, 0};
        cfg->MergeMode = true;
        cfg->FontLoaderFlags = 0;
        cfg->FontDataOwnedByAtlas = false;
        ImFontAtlas_AddFontFromMemoryTTF(io->Fonts, (void *)asset_codicon_ttf, (int)asset_codicon_ttf_size, px, cfg,
                                         icon_ranges);
    }
    ImFontConfig_destroy(cfg);
    return f;
}

/* Terminal fonts: the user's font file when set (FreeType synthesizes the
 * bold face), falling back to the chosen built-in font (IBM Plex Mono by
 * default, or JetBrains Mono). */
static void load_mono_fonts(App *a)
{
    ImGuiIO *io = igGetIO_Nil();
    unsigned fl = ImGuiFreeTypeLoaderFlags_LightHinting;
    a->font_error[0] = 0;
    if (a->mono_font_path[0]) {
        ImFont *r = add_font(io, NULL, 0, a->mono_font_path, a->mono_size, fl, true);
        ImFont *b = r ? add_font(io, NULL, 0, a->mono_font_path, a->mono_size, fl | ImGuiFreeTypeLoaderFlags_Bold, false)
                      : NULL;
        if (r && b) {
            a->font_mono = r;
            a->font_mono_bold = b;
            return;
        }
        if (r)
            ImFontAtlas_RemoveFont(io->Fonts, r);
        snprintf(a->font_error, sizeof a->font_error, "Couldn't load this font file. Using the built-in font.");
    }
    const unsigned char *reg = asset_IBMPlexMono_Regular_ttf, *bold = asset_IBMPlexMono_Bold_ttf;
    unsigned reg_size = asset_IBMPlexMono_Regular_ttf_size, bold_size = asset_IBMPlexMono_Bold_ttf_size;
    if (a->mono_builtin == MONO_BUILTIN_JETBRAINS) {
        reg = asset_JetBrainsMono_Regular_ttf;
        reg_size = asset_JetBrainsMono_Regular_ttf_size;
        bold = asset_JetBrainsMono_Bold_ttf;
        bold_size = asset_JetBrainsMono_Bold_ttf_size;
    }
    a->font_mono = add_font(io, reg, reg_size, NULL, a->mono_size, fl, true);
    a->font_mono_bold = add_font(io, bold, bold_size, NULL, a->mono_size, fl, false);
}

static void load_fonts(App *a)
{
    ImGuiIO *io = igGetIO_Nil();
    unsigned ui_flags = ImGuiFreeTypeLoaderFlags_LightHinting;
    a->font_ui = add_font(io, asset_Inter_Regular_ttf, asset_Inter_Regular_ttf_size, NULL, a->font_size, ui_flags, true);
    a->font_ui_bold =
        add_font(io, asset_Inter_SemiBold_ttf, asset_Inter_SemiBold_ttf_size, NULL, a->font_size, ui_flags, true);
    load_mono_fonts(a);
}

/* Swaps the terminal font between frames (the atlas is dynamic in 1.92). */
static void reload_mono_fonts(App *a)
{
    ImGuiIO *io = igGetIO_Nil();
    ImFontAtlas_RemoveFont(io->Fonts, a->font_mono);
    ImFontAtlas_RemoveFont(io->Fonts, a->font_mono_bold);
    load_mono_fonts(a);
    a->fonts_dirty = false;
}

static void rebuild_style(App *a)
{
    ImGuiStyle *style = igGetStyle();
    igStyleColorsDark(style);
    theme_apply_style(&a->theme, style);
    float scale = a->ui_scale;
    /* ScaleAllSizes is cumulative, so start from unscaled values each time. */
    ImGuiStyle_ScaleAllSizes(style, scale);
    style->FontSizeBase = a->font_size;
    style->FontScaleDpi = scale;
    a->theme_dirty = false;
}

void app_set_ui_font_size(App *a, float px)
{
    a->font_size = clampf(px, UI_FONT_MIN, UI_FONT_MAX);
    a->theme_dirty = true; /* style->FontSizeBase */
    app_mark_dirty(a);
}

void app_set_mono_size(App *a, float px)
{
    a->mono_size = clampf(px, MONO_FONT_MIN, MONO_FONT_MAX);
    app_mark_dirty(a);
}

void app_set_zoom(App *a, float zoom)
{
    a->zoom = clampf(zoom, ZOOM_MIN, ZOOM_MAX);
    a->ui_scale = a->display_scale * a->zoom;
    a->theme_dirty = true;
    app_mark_dirty(a);
}

void app_set_mono_font(App *a, const char *path)
{
    snprintf(a->mono_font_path, sizeof a->mono_font_path, "%s", path ? path : "");
    a->fonts_dirty = true;
    app_mark_dirty(a);
}

/* Picks a built-in terminal font; also drops any user font file. */
void app_set_mono_builtin(App *a, int which)
{
    if (which < 0 || which >= MONO_BUILTIN_COUNT)
        which = MONO_BUILTIN_DEFAULT;
    a->mono_builtin = which;
    app_set_mono_font(a, NULL);
}

void app_set_theme(App *a, bool dark)
{
    a->dark = dark;
    theme_build(&a->theme, dark);
    a->theme_dirty = true;
    app_mark_dirty(a);
}

/* ---- Main loop ------------------------------------------------------------ */

static uint64_t frame_period_ns(App *a)
{
    /* Frames only happen while something changes, so matching even a
     * 480 Hz display costs nothing at idle; the cap only guards against a
     * bogus refresh rate. */
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(a->window));
    float hz = mode && mode->refresh_rate > 1 ? mode->refresh_rate : 60.0f;
    if (hz > 1000)
        hz = 1000;
    return (uint64_t)(1e9 / hz);
}

static void SDLCALL wake_ui(void *ud)
{
    App *a = ud;
    SDL_Event e;
    SDL_zero(e);
    e.type = a->wake_event;
    SDL_PushEvent(&e);
}

/* Maximizing needs a monitor to maximize onto. With every output
 * disconnected (e.g. a DisplayPort monitor asleep), GNOME/Mutter 46 aborts
 * on a maximize request (meta_window_get_work_area_for_logical_monitor
 * assertion), taking the whole shell down. Don't ask in that state. */
bool app_can_maximize(App *a)
{
    SDL_DisplayID d = SDL_GetDisplayForWindow(a->window);
    SDL_Rect r;
    return d && SDL_GetDisplayUsableBounds(d, &r) && r.w > 0 && r.h > 0;
}

void app_toggle_maximize(App *a)
{
    if (SDL_GetWindowFlags(a->window) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(a->window);
    else if (app_can_maximize(a))
        SDL_MaximizeWindow(a->window);
}

static void handle_event(App *a, const SDL_Event *e)
{
    if (e->type == a->wake_event)
        return;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        a->term_focus_uid = 0; /* focus may move; the frame re-establishes it */
    if (termview_sdl_event(a, e))
        return;
    ImGui_ImplSDL3_ProcessEvent(e);
    switch (e->type) {
    case SDL_EVENT_QUIT:
        a->running = false;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID == SDL_GetWindowID(a->window))
            a->running = false;
        break;
#ifndef _WIN32
    case SDL_EVENT_WINDOW_HIT_TEST:
        /* X11/Wayland start a WM move on press in the drag area, so ImGui never
         * sees the click; detect title bar double-clicks here instead. Not on
         * Windows: there SDL posts this for every WM_NCHITTEST (i.e. on mouse
         * motion, not just presses), and HTCAPTION already gets the native
         * double-click-to-maximize. */
        if (a->last_hit == SDL_HITTEST_DRAGGABLE) {
            /* With XInput2, SDL handles one physical click twice (slave and
             * master pointer device) and reports a hit test for each, a few
             * microseconds apart. Collapse those into one press. */
            uint64_t now = e->window.timestamp;
            if (now - a->last_hit_event_ns < 40 * SDL_NS_PER_MS)
                break;
            a->last_hit_event_ns = now;
            float mx, my;
            SDL_GetGlobalMouseState(&mx, &my);
            bool same_spot = fabsf(mx - a->last_title_click_x) < 6 && fabsf(my - a->last_title_click_y) < 6;
            if (a->last_title_click_ns && now - a->last_title_click_ns < 400 * SDL_NS_PER_MS && same_spot) {
                app_toggle_maximize(a);
                a->last_title_click_ns = 0;
            } else {
                a->last_title_click_ns = now;
                a->last_title_click_x = mx;
                a->last_title_click_y = my;
            }
        }
        break;
#endif
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        a->window_focused = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        a->window_focused = false;
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        float s = SDL_GetWindowDisplayScale(a->window);
        if (s > 0 && fabsf(s - a->display_scale) > 0.01f) {
            a->display_scale = s;
            a->ui_scale = s * a->zoom;
            a->theme_dirty = true;
        }
        break;
    }
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        app_mark_dirty(a);
        break;
    default:
        break;
    }
}

static void render(App *a)
{
    if (a->fonts_dirty)
        reload_mono_fonts(a);
    if (a->theme_dirty)
        rebuild_style(a);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    igNewFrame();
    a->animating = false;
    a->nholes = 0;
    a->term_focus_uid = 0;
    app_frame(a);
    igRender();
    ImGuiIO *io = igGetIO_Nil();
    glViewport(0, 0, (int)(io->DisplaySize.x * io->DisplayFramebufferScale.x),
               (int)(io->DisplaySize.y * io->DisplayFramebufferScale.y));
    float c[4] = {(float)(a->theme.bg & 0xFF) / 255.0f, (float)((a->theme.bg >> 8) & 0xFF) / 255.0f,
                  (float)((a->theme.bg >> 16) & 0xFF) / 255.0f, 1.0f};
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
    SDL_GL_SwapWindow(a->window);
}

/* While a window edge is dragged, Windows runs its own message loop and ours
 * stalls until the drag ends, so nothing would be drawn (stale or missing
 * pixels). SDL sends live-resize expose events (data1 = 1) from inside that
 * loop, to event watchers; draw from those so the window keeps up. */
static bool SDLCALL live_resize_watch(void *userdata, SDL_Event *e)
{
    App *a = userdata;
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1 && a->running &&
        e->window.windowID == SDL_GetWindowID(a->window)) {
        app_pump_sessions(a); /* keep incoming data flowing too */
        render(a);
    }
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    App *a = g_app;
#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "imserial");
    SDL_SetHint(SDL_HINT_APP_ID, "imserial");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    a->wake_event = SDL_RegisterEvents(1);
#ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#endif
    a->font_size = UI_FONT_DEFAULT;
    a->mono_size = MONO_FONT_DEFAULT;
    a->mono_builtin = MONO_BUILTIN_DEFAULT;
    a->zoom = 1.0f;
    a->console_cols = CONSOLE_COLS_DEFAULT;
    a->console_rows = CONSOLE_ROWS_DEFAULT;
    a->scrollback_lines = SCROLLBACK_DEFAULT;
    a->logcfg.existing = LOG_EXISTING_ASK;
    a->logcfg.flush = true;
    a->logcfg.header = true;
    a->sidebar_visible = true;
    a->sidebar_w = 280;
    a->sect_conn_open = true;
    a->sect_term_open = true;
    a->dark = true;
    a->active = 0;
    a->win_w = 1180;
    a->win_h = 740;
    a->win_x = a->win_y = (int)SDL_WINDOWPOS_CENTERED;
    settings_load(a);
    theme_build(&a->theme, a->dark);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "imserial");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, a->win_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, a->win_y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, a->win_w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, a->win_h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                              SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN);
    a->window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!a->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(a->window, 640, 400);
    if (!SDL_SetWindowHitTest(a->window, hit_test, a))
        SDL_Log("Custom title bar hit testing unavailable: %s", SDL_GetError());
    set_window_icon(a->window);
    a->gl = SDL_GL_CreateContext(a->window);
    if (!a->gl) {
        fprintf(stderr, "OpenGL context failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(a->window, a->gl);
    SDL_GL_SetSwapInterval(0);
    a->display_scale = SDL_GetWindowDisplayScale(a->window);
    if (a->display_scale <= 0)
        a->display_scale = 1;
    a->ui_scale = a->display_scale * a->zoom;

    igCreateContext(NULL);
    ImGuiIO *io = igGetIO_Nil();
    io->IniFilename = NULL;
    io->LogFilename = NULL;
    io->ConfigInputTextCursorBlink = false;
    io->ConfigWindowsMoveFromTitleBarOnly = true;
    /* Ctrl+Tab switches session tabs, not ImGui windows. */
    igGetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    igGetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    load_fonts(a);
    a->theme_dirty = true;
    ImGui_ImplSDL3_InitForOpenGL(a->window, a->gl);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    /* Restore tabs (settings_load stashed their configs); start one tab otherwise. */
    for (int i = 0; i < a->ntabs; i++) {
        Tab *t = a->tabs[i];
        t->session = session_create(wake_ui, a);
        if (t->want_connected && t->target.path[0])
            app_tab_connect(a, t);
    }
    if (a->ntabs == 0)
        app_new_tab(a, NULL);
    for (int i = 0; i < a->ntabs; i++)
        if (!a->tabs[i]->session)
            a->tabs[i]->session = session_create(wake_ui, a);

    app_scan_ports(a, true);
    if (a->win_maximized && app_can_maximize(a))
        SDL_MaximizeWindow(a->window);
    render(a); /* draw once before showing to avoid a blank flash */
    SDL_ShowWindow(a->window);
    a->window_focused = true;
    a->running = true;
    SDL_AddEventWatch(live_resize_watch, a);
    a->frames_wanted = 3;

    /* Frames are paced by us, not by vsync: with some drivers (NVIDIA on a
     * headless/remote X display) a vsync'd swap blocks for a full second. */
    uint64_t last_frame_ns = 0;
    bool tooltip_tick = false;
    while (a->running) {
        SDL_Event e;
        /* Re-read each time: the window may have moved to another display. */
        uint64_t period_ns = frame_period_ns(a);
        bool busy = a->frames_wanted > 0 || a->animating;
        int timeout;
        if (busy) {
            /* Round the wait down: rounding a 2.08 ms (480 Hz) gap up to 3 ms
             * would cap the frame rate well below the display's. The last
             * sub-millisecond is polled. */
            uint64_t now = SDL_GetTicksNS(), next = last_frame_ns + period_ns;
            timeout = now >= next ? 0 : (int)((next - now) / 1000000);
        } else {
            /* Hovering may show a tooltip after a delay; otherwise sleep until
             * the next periodic chore (port rescan, settings save). */
            tooltip_tick = igIsAnyItemHovered() || igIsAnyItemActive();
            timeout = tooltip_tick ? 100 : 1000;
        }
        bool got = SDL_WaitEventTimeout(&e, timeout);
        if (got) {
            do
                handle_event(a, &e);
            while (SDL_PollEvent(&e));
            a->frames_wanted = a->frames_wanted < 3 ? 3 : a->frames_wanted;
        }
        if (app_pump_sessions(a))
            a->frames_wanted = a->frames_wanted < 2 ? 2 : a->frames_wanted;
        if (g_quit_signal)
            a->running = false;
        app_scan_ports(a, false);
        if (a->settings_dirty && SDL_GetTicks() - a->settings_dirty_at > 1500)
            settings_save(a);

        if (SDL_GetWindowFlags(a->window) & SDL_WINDOW_MINIMIZED) {
            a->frames_wanted = 0;
            a->animating = false;
            continue;
        }
        bool want = a->frames_wanted > 0 || a->animating || (!got && tooltip_tick);
        uint64_t now = SDL_GetTicksNS();
        if (want && now - last_frame_ns >= period_ns - period_ns / 8) {
            last_frame_ns = now;
            render(a);
            if (a->frames_wanted > 0)
                a->frames_wanted--;
            /* ImGui trickles queued input (one key/button change per frame);
             * keep rendering until it has consumed everything. */
            if (igGetCurrentContext()->InputEventsQueue.Size > 0 && a->frames_wanted < 1)
                a->frames_wanted = 1;
        } else if (want && a->frames_wanted == 0) {
            a->frames_wanted = 1; /* render at the next frame slot */
        }
    }

    SDL_RemoveEventWatch(live_resize_watch, a);
    settings_save(a);
    for (int i = 0; i < a->ntabs; i++) {
        session_destroy(a->tabs[i]->session);
        a->tabs[i]->session = NULL;
        log_close(&a->tabs[i]->slog);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    igDestroyContext(NULL);
    SDL_GL_DestroyContext(a->gl);
    SDL_DestroyWindow(a->window);
    fontlist_shutdown();
    SDL_Quit();
    return 0;
}

void app_request_frames(App *a, int n)
{
    if (a->frames_wanted < n)
        a->frames_wanted = n;
}

/* Exposed for app.c so new tabs get a session wired to the UI wakeup. */
Session *app_create_session(App *a)
{
    return session_create(wake_ui, a);
}
