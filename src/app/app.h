#pragma once
#include "app/log.h"
#include "serial/session.h"
#include "term/termbuf.h"
#include "ui/imgui_c.h"
#include "ui/theme.h"

#define MAX_TABS 32
#define MAX_PORTS 64
#define HISTORY_MAX 100
#define INPUT_MAX 2048

/* Preferences: defaults and allowed ranges. */
#define UI_FONT_DEFAULT 13.0f
#define UI_FONT_MIN 10.0f
#define UI_FONT_MAX 20.0f
#define MONO_FONT_DEFAULT 14.0f
#define MONO_FONT_MIN 8.0f
#define MONO_FONT_MAX 32.0f
#define ZOOM_MIN 0.5f
#define ZOOM_MAX 2.0f
#define CONSOLE_COLS_DEFAULT 80
#define CONSOLE_COLS_MIN 20
#define CONSOLE_COLS_MAX 1000
#define CONSOLE_ROWS_DEFAULT 24
#define CONSOLE_ROWS_MIN 5
#define CONSOLE_ROWS_MAX 500
#define SCROLLBACK_DEFAULT 200000
#define SCROLLBACK_MIN 1000
#define SCROLLBACK_MAX 1000000

typedef enum { EOL_NONE, EOL_LF, EOL_CR, EOL_CRLF } TxEol;

typedef struct {
    uint64_t line;
    uint32_t col; /* byte offset within the line */
} TermPos;

typedef struct Tab {
    int uid;
    PortTarget target;   /* chosen port; target.path empty = none chosen */
    SerialConfig cfg;
    bool auto_reconnect;
    bool timestamps;
    bool local_echo;
    TermNewlineMode nl_mode;
    TxEol tx_eol;
    bool want_connected; /* user intent, persisted across restarts */

    Session *session;
    TermBuf tb;
    SessionBuf drain;
    LinkState link;      /* last state seen by the UI */

    /* terminal view */
    bool follow;           /* stick to bottom */
    uint64_t seen_total;   /* tb.total_added when the user last was at the bottom */
    bool focus_terminal;   /* request keyboard focus */
    bool has_sel, selecting;
    TermPos sel_anchor, sel_head;
    float scroll_to_line;  /* -1 none */
    uint64_t view_first_abs; /* first line index at the last frame, to compensate eviction */
    bool view_alt;           /* alternate screen was showing at the last frame */

    /* find */
    bool find_open, find_focus;
    char find_text[256];
    bool find_case;
    uint64_t find_line;
    uint32_t find_col;
    int find_count;
    uint64_t find_version;

    /* send bar */
    char input[INPUT_MAX];
    char *history[HISTORY_MAX];
    int history_count;
    int history_pos; /* -1 = editing new line */
    bool focus_input;

    SessionLog slog; /* session log file (Preferences > Logging) */
} Tab;

typedef struct {
    float x, y, w, h;
} UiRect;

typedef struct App {
    SDL_Window *window;
    SDL_GLContext gl;
    Uint32 wake_event;
    bool running;
    bool window_focused;

    Theme theme;
    bool dark;
    bool theme_dirty;
    float ui_scale;      /* display_scale * zoom; what S() multiplies by */
    float display_scale; /* from SDL (monitor DPI) */
    float zoom;          /* user zoom factor (Preferences) */

    ImFont *font_ui;      /* Inter + codicons */
    ImFont *font_ui_bold; /* Inter SemiBold + codicons */
    ImFont *font_mono;    /* terminal font (JetBrains Mono or the user's) + codicons */
    ImFont *font_mono_bold;
    float font_size;       /* UI text px (unscaled) */
    float mono_size;       /* terminal text px (unscaled) */
    char mono_font_path[1024]; /* user's terminal font file; empty = built-in */
    bool fonts_dirty;          /* reload the terminal font before the next frame */
    char font_error[256];      /* why the user's font couldn't be used */
    bool prefs_open;           /* request to open Preferences on the next frame */

    /* Console (Preferences) */
    bool console_fixed;            /* fixed columns x rows instead of fitting the window */
    int console_cols, console_rows;
    int scrollback_lines;

    LogSettings logcfg;

    Tab *tabs[MAX_TABS];
    int ntabs;
    int active;
    int next_uid;

    SerialPortInfo ports[MAX_PORTS];
    int nports;
    uint64_t ports_scanned_at;

    bool sidebar_visible;
    float sidebar_w;
    bool sect_conn_open, sect_term_open;

    bool quickpick_open; /* request to open the quick pick on the next frame */
    bool quickpick_focus;
    char quickpick_text[128];
    int quickpick_sel;

    bool about_open;

    /* Title bar interactive regions (no window drag there), in window points. */
    UiRect hit_holes[32];
    int nholes;
    float titlebar_h;
    SDL_HitTestResult last_hit;
    uint64_t last_hit_event_ns;   /* dedupes the per-device duplicate hit-test events */
    uint64_t last_title_click_ns; /* for title bar double-click to maximize */
    float last_title_click_x, last_title_click_y;

    /* settings persistence */
    bool settings_dirty;
    uint64_t settings_dirty_at;
    int win_x, win_y, win_w, win_h;
    bool win_maximized;

    int term_focus_uid; /* tab whose terminal had keyboard focus last frame, 0 = none */

    /* frame pacing */
    int frames_wanted;
    bool animating;
} App;

extern App *g_app;

static inline Tab *app_active_tab(App *a) { return a->ntabs ? a->tabs[a->active] : NULL; }

Tab *app_new_tab(App *a, const PortTarget *target);
void app_close_tab(App *a, int index);
void app_tab_connect(App *a, Tab *t);
void app_tab_disconnect(App *a, Tab *t);
void app_tab_set_port(App *a, Tab *t, const SerialPortInfo *port);
void app_tab_apply_config(App *a, Tab *t);
void app_tab_send(App *a, Tab *t, const char *data, size_t len, bool add_eol);
void app_tab_send_size(App *a, Tab *t);
void app_tab_title(const Tab *t, char *out, size_t outlen);
void app_scan_ports(App *a, bool force);
bool app_pump_sessions(App *a);
void app_mark_dirty(App *a);
void app_request_frames(App *a, int n);
void app_set_theme(App *a, bool dark);
void app_set_ui_font_size(App *a, float px);
void app_set_mono_size(App *a, float px);
void app_set_zoom(App *a, float zoom);
void app_set_mono_font(App *a, const char *path);
void app_set_console_size(App *a, bool fixed, int cols, int rows);
void app_set_scrollback(App *a, int lines);
/* Call after changing a->logcfg: open logs are closed and reopen with the new settings. */
void app_logging_changed(App *a);
/* Answers "the log file exists" for a tab: 0 overwrite, 1 append, 2 don't log. */
void app_tab_log_resolve(App *a, Tab *t, int choice);
/* Opens the folder the active tab logs to (or would log to) in the file manager. */
void app_open_log_folder(App *a);
const SerialPortInfo *app_find_port(App *a, const PortTarget *t);

void app_frame(App *a);
Session *app_create_session(App *a);
bool app_can_maximize(App *a);
void app_toggle_maximize(App *a);

/* settings.c */
void settings_load(App *a);
void settings_save(App *a);
