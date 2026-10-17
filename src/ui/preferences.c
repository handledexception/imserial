/* Preferences dialog (File > Preferences, Ctrl+,): appearance, terminal font,
 * console size and session logging. Changes apply immediately and are saved
 * with the rest of the settings. Also the "log file exists" prompt. */
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float k_zooms[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
static const char *const k_zoom_labels[] = {"80%", "90%", "100%", "110%", "125%", "150%", "175%", "200%"};
#define NZOOMS ((int)(sizeof k_zooms / sizeof k_zooms[0]))

/* File dialogs may call back on another thread; hand the path over. The
 * ready flag says what was picked. */
enum { PICK_NONE, PICK_FONT, PICK_LOG };
static char s_picked[1024];
static SDL_AtomicInt s_picked_ready;

static void picked(App *a, const char *const *files, int kind)
{
    if (!files || !files[0])
        return; /* cancelled or failed */
    snprintf(s_picked, sizeof s_picked, "%s", files[0]);
    SDL_SetAtomicInt(&s_picked_ready, kind);
    SDL_Event e;
    SDL_zero(e);
    e.type = a->wake_event;
    SDL_PushEvent(&e);
}

static void SDLCALL on_font_picked(void *ud, const char *const *files, int filter)
{
    (void)filter;
    picked(ud, files, PICK_FONT);
}

static void SDLCALL on_log_picked(void *ud, const char *const *files, int filter)
{
    (void)filter;
    picked(ud, files, PICK_LOG);
}

static void browse_font(App *a)
{
    static const SDL_DialogFileFilter filters[] = {{"Fonts", "ttf;otf;ttc"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(on_font_picked, a, a->window, filters, 2, a->mono_font_path[0] ? a->mono_font_path : NULL,
                           false);
}

static void browse_log(App *a)
{
    static const SDL_DialogFileFilter filters[] = {{"Log files", "log;txt"}, {"All files", "*"}};
    SDL_ShowSaveFileDialog(on_log_picked, a, a->window, filters, 2, a->logcfg.path[0] ? a->logcfg.path : NULL);
}

static void section(const char *label)
{
    igDummy(v2(1, S(6)));
    ImVec2 p = igGetCursorScreenPos();
    /* Brighter than the dim setting labels, so sections stand out. */
    ui_text(igGetWindowDrawList(), g_app->font_ui_bold, S(11), p, g_app->theme.fg, label, NULL);
    igDummy(v2(1, S(18)));
}

/* "[-] [value] [+] unit" stepper. The value is editable; typed values apply
 * on Enter or when focus leaves. buf must persist across frames (it holds the
 * text while editing). */
static bool stepper(const char *id, float *v, float step, float lo, float hi, const char *unit, char *buf,
                    size_t buflen)
{
    float h = igGetFrameHeight();
    bool changed = false;
    igPushID_Str(id);
    if (ui_icon_button("##dec", ICON_REMOVE, NULL, false, h) && *v > lo) {
        *v = clampf(*v - step, lo, hi);
        changed = true;
    }
    igSameLine(0, S(4));
    if (igGetActiveID() != igGetID_Str("##value"))
        snprintf(buf, buflen, "%g", *v);
    igSetNextItemWidth(S(56));
    igInputText("##value", buf, buflen, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll, NULL,
                NULL);
    if (igIsItemDeactivatedAfterEdit()) {
        char *end;
        float f = strtof(buf, &end);
        if (end != buf && f > 0) {
            *v = clampf(f, lo, hi);
            changed = true;
        }
    }
    igSameLine(0, S(4));
    if (ui_icon_button("##inc", ICON_ADD, NULL, false, h) && *v < hi) {
        *v = clampf(*v + step, lo, hi);
        changed = true;
    }
    if (unit) {
        igSameLine(0, S(8));
        igAlignTextToFramePadding();
        igPushStyleColor_U32(ImGuiCol_Text, g_app->theme.fg_dim);
        igTextUnformatted(unit, NULL);
        igPopStyleColor(1);
    }
    igPopID();
    return changed;
}

void ui_preferences(App *a)
{
    int pick = SDL_GetAtomicInt(&s_picked_ready);
    if (pick) {
        SDL_SetAtomicInt(&s_picked_ready, PICK_NONE);
        if (pick == PICK_FONT) {
            app_set_mono_font(a, s_picked);
        } else if (pick == PICK_LOG) {
            snprintf(a->logcfg.path, sizeof a->logcfg.path, "%s", s_picked);
            app_logging_changed(a);
        }
    }
    if (a->prefs_open) {
        igOpenPopup_Str("Preferences", 0);
        a->prefs_open = false;
    }
    const Theme *th = &a->theme;
    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(v2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.4f), ImGuiCond_Appearing,
                       v2(0.5f, 0.5f));
    igSetNextWindowSize(v2(S(520), 0), ImGuiCond_Always);
    /* Taller than the window: scroll inside the dialog. */
    igSetNextWindowSizeConstraints(v2(S(520), 0), v2(S(520), vp->Size.y - S(40)), NULL, NULL);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(20), S(16)));
    if (!igBeginPopupModal("Preferences", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        igPopStyleVar(1);
        return;
    }
    float lw = S(140);

    igPushFont(a->font_ui_bold, 16);
    igTextUnformatted("Preferences", NULL);
    igPopFont();

    section("APPEARANCE");
    static const char *const themes[] = {"Dark", "Light"};
    int tv = a->dark ? 0 : 1;
    ui_form_label("Theme", lw);
    if (ui_segmented("##theme", themes, 2, &tv, igGetContentRegionAvail().x))
        app_set_theme(a, tv == 0);

    int zi = 0;
    for (int i = 1; i < NZOOMS; i++)
        if (fabsf(k_zooms[i] - a->zoom) < fabsf(k_zooms[zi] - a->zoom))
            zi = i;
    ui_form_label("Zoom", lw);
    if (ui_combo("##zoom", &zi, k_zoom_labels, NZOOMS))
        app_set_zoom(a, k_zooms[zi]);

    static char ui_fs_buf[16], mono_fs_buf[16];
    float ui_fs = a->font_size;
    ui_form_label("UI font size", lw);
    if (stepper("##uifs", &ui_fs, 1, UI_FONT_MIN, UI_FONT_MAX, "px", ui_fs_buf, sizeof ui_fs_buf))
        app_set_ui_font_size(a, ui_fs);

    section("TERMINAL");
    ui_form_label("Font", lw);
    {
        static char buf[1024];
        ImGuiID fid = igGetID_Str("##fontpath");
        if (igGetActiveID() != fid)
            snprintf(buf, sizeof buf, "%s", a->mono_font_path);
        float bw = S(80), gap = S(6);
        igSetNextItemWidth(igGetContentRegionAvail().x - bw - gap);
        igInputTextWithHint("##fontpath", "Built-in (JetBrains Mono)", buf, sizeof buf, 0, NULL, NULL);
        if (igIsItemDeactivatedAfterEdit())
            app_set_mono_font(a, buf);
        igSameLine(0, gap);
        if (ui_button("Browse\xE2\x80\xA6", NULL, v2(bw, igGetFrameHeight()), false))
            browse_font(a);
    }
    if (a->mono_font_path[0]) {
        igSetCursorPosX(igGetCursorPosX() + lw);
        if (igTextLink("Use built-in font"))
            app_set_mono_font(a, NULL);
    }
    if (a->font_error[0]) {
        igSetCursorPosX(igGetCursorPosX() + lw);
        igPushStyleColor_U32(ImGuiCol_Text, th->error);
        igPushTextWrapPos(0);
        igTextUnformatted(a->font_error, NULL);
        igPopTextWrapPos();
        igPopStyleColor(1);
    }

    float mono_fs = a->mono_size;
    ui_form_label("Font size", lw);
    if (stepper("##monofs", &mono_fs, 1, MONO_FONT_MIN, MONO_FONT_MAX, "px", mono_fs_buf, sizeof mono_fs_buf))
        app_set_mono_size(a, mono_fs);

    section("CONSOLE");
    static const char *const size_modes[] = {"Fit to window", "Fixed"};
    int sm = a->console_fixed ? 1 : 0;
    ui_form_label("Size", lw);
    /* The fixed size is kept while fitting to the window, so switching back
     * restores it (80 x 24 until changed, like PuTTY). */
    if (ui_segmented("##consolesize", size_modes, 2, &sm, igGetContentRegionAvail().x))
        app_set_console_size(a, sm == 1, a->console_cols, a->console_rows);
    if (a->console_fixed) {
        static char cols_buf[16], rows_buf[16];
        float cols = (float)a->console_cols, rows = (float)a->console_rows;
        ui_form_label("Columns", lw);
        if (stepper("##cols", &cols, 1, CONSOLE_COLS_MIN, CONSOLE_COLS_MAX, NULL, cols_buf, sizeof cols_buf))
            app_set_console_size(a, true, (int)cols, a->console_rows);
        ui_form_label("Rows", lw);
        if (stepper("##rows", &rows, 1, CONSOLE_ROWS_MIN, CONSOLE_ROWS_MAX, NULL, rows_buf, sizeof rows_buf))
            app_set_console_size(a, true, a->console_cols, (int)rows);
    }

    static const int k_scrollback[] = {1000, 10000, 50000, 100000, 200000, 500000, 1000000};
    static const char *const k_scrollback_labels[] = {"1,000 lines",   "10,000 lines",  "50,000 lines",
                                                      "100,000 lines", "200,000 lines", "500,000 lines",
                                                      "1,000,000 lines"};
    int si = 0;
    for (int i = 1; i < (int)(sizeof k_scrollback / sizeof k_scrollback[0]); i++)
        if (abs(k_scrollback[i] - a->scrollback_lines) < abs(k_scrollback[si] - a->scrollback_lines))
            si = i;
    ui_form_label("Scrollback", lw);
    if (ui_combo("##scrollback", &si, k_scrollback_labels, (int)(sizeof k_scrollback / sizeof k_scrollback[0])))
        app_set_scrollback(a, k_scrollback[si]);

    section("LOGGING");
    LogSettings *lc = &a->logcfg;
    static const char *const log_modes[] = {"None", "Printable output", "All output"};
    int lm = (int)lc->mode;
    ui_form_label("Session logging", lw);
    if (ui_segmented("##logmode", log_modes, 3, &lm, igGetContentRegionAvail().x)) {
        lc->mode = (LogMode)lm;
        app_logging_changed(a);
    }
    if (lc->mode != LOG_NONE) {
        ui_form_label("Log file name", lw);
        static char path_buf[1024];
        char def[1024];
        log_default_path(def, sizeof def);
        if (igGetActiveID() != igGetID_Str("##logpath"))
            snprintf(path_buf, sizeof path_buf, "%s", lc->path[0] ? lc->path : def);
        float bw = S(80), gap = S(6);
        igSetNextItemWidth(igGetContentRegionAvail().x - bw - gap);
        igInputText("##logpath", path_buf, sizeof path_buf, 0, NULL, NULL);
        if (igIsItemDeactivatedAfterEdit()) {
            /* the default stays "default" (empty) so it follows Documents */
            snprintf(lc->path, sizeof lc->path, "%s", strcmp(path_buf, def) ? path_buf : "");
            app_logging_changed(a);
        }
        igSameLine(0, gap);
        if (ui_button("Browse\xE2\x80\xA6##log", NULL, v2(bw, igGetFrameHeight()), false))
            browse_log(a);
        igSetCursorPosX(igGetCursorPosX() + lw);
        igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
        igTextUnformatted("&Y &M &D date, &T time, &P port", NULL);
        igPopStyleColor(1);
        igSetCursorPosX(igGetCursorPosX() + lw);
        if (igTextLink("Open log folder"))
            app_open_log_folder(a);

        igDummy(v2(1, S(4)));
        igPushStyleColor_U32(ImGuiCol_Text, th->fg_dim);
        igTextUnformatted("What to do if the log file already exists:", NULL);
        igPopStyleColor(1);
        static const char *const existing[] = {"Always overwrite it", "Always append to the end",
                                               "Ask the user every time"};
        static const LogExisting existing_val[] = {LOG_EXISTING_OVERWRITE, LOG_EXISTING_APPEND, LOG_EXISTING_ASK};
        for (int i = 0; i < 3; i++) {
            igSetCursorPosX(igGetCursorPosX() + S(8));
            if (igRadioButton_Bool(existing[i], lc->existing == existing_val[i]) && lc->existing != existing_val[i]) {
                lc->existing = existing_val[i];
                app_logging_changed(a);
            }
        }
        igDummy(v2(1, S(4)));
        bool changed = igCheckbox("Flush log file frequently", &lc->flush);
        changed |= igCheckbox("Include header", &lc->header);
        igBeginDisabled(lc->mode != LOG_PRINTABLE);
        changed |= igCheckbox("Timestamp each line", &lc->timestamps);
        igEndDisabled();
        if (changed)
            app_logging_changed(a);
    }

    igDummy(v2(1, S(10)));
    float cw = S(90);
    igSetCursorPosX(igGetWindowWidth() - igGetStyle()->WindowPadding.x - cw);
    if (ui_button("Close", NULL, v2(cw, S(28)), true) || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
        igCloseCurrentPopup();
    igEndPopup();
    igPopStyleVar(1);
}

/* "The log file already exists" (Preferences > Logging set to ask). Output
 * is buffered meanwhile, so nothing is lost while the question is up. */
void ui_log_prompt(App *a)
{
    static int s_uid; /* tab the open prompt is about */
    Tab *t = NULL;
    for (int i = 0; i < a->ntabs; i++)
        if (a->tabs[i]->slog.pending && (!s_uid || a->tabs[i]->uid == s_uid))
            t = a->tabs[i];
    if (!t) {
        s_uid = 0;
        return;
    }
    const char *id = "Log file exists";
    if (!igIsPopupOpen_Str(id, 0)) {
        if (igIsPopupOpen_Str("", ImGuiPopupFlags_AnyPopupId))
            return; /* ask once the other dialog is closed */
        igOpenPopup_Str(id, 0);
        s_uid = t->uid;
    }
    ImGuiViewport *vp = igGetMainViewport();
    igSetNextWindowPos(v2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.4f), ImGuiCond_Appearing,
                       v2(0.5f, 0.5f));
    igSetNextWindowSize(v2(S(460), 0), ImGuiCond_Always);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, v2(S(20), S(16)));
    if (igBeginPopupModal(id, NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        igPushFont(a->font_ui_bold, 16);
        igTextUnformatted("The log file already exists", NULL);
        igPopFont();
        igDummy(v2(1, S(4)));
        igPushTextWrapPos(0);
        igPushStyleColor_U32(ImGuiCol_Text, a->theme.fg_dim);
        igTextUnformatted(t->slog.path, NULL);
        igPopStyleColor(1);
        igDummy(v2(1, S(4)));
        igTextUnformatted("Overwrite it, add to the end of it, or don't log this session?", NULL);
        igPopTextWrapPos();
        igDummy(v2(1, S(10)));
        float bw = S(110), gap = S(8);
        igSetCursorPosX(igGetWindowWidth() - igGetStyle()->WindowPadding.x - bw * 3 - gap * 2);
        int choice = -1;
        if (ui_button("Overwrite", NULL, v2(bw, S(28)), false))
            choice = 0;
        igSameLine(0, gap);
        if (ui_button("Append", NULL, v2(bw, S(28)), true))
            choice = 1;
        igSameLine(0, gap);
        if (ui_button("Don't Log", NULL, v2(bw, S(28)), false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false))
            choice = 2;
        if (choice >= 0) {
            app_tab_log_resolve(a, t, choice);
            s_uid = 0;
            igCloseCurrentPopup();
        }
        igEndPopup();
    }
    igPopStyleVar(1);
}
