/* Persists UI state and open tabs to settings.json in the user config dir
 * ($XDG_CONFIG_HOME/imserial or %APPDATA%\imserial). */
#include "app/app.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool config_path(char *out, size_t outlen, bool create_dir)
{
    char dir[1024];
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base)
        return false;
    snprintf(dir, sizeof dir, "%s\\imserial", base);
    snprintf(out, outlen, "%s\\settings.json", dir);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/imserial", xdg);
    else if (home)
        snprintf(dir, sizeof dir, "%s/.config/imserial", home);
    else
        return false;
    snprintf(out, outlen, "%s/settings.json", dir);
#endif
    if (create_dir)
        SDL_CreateDirectory(dir);
    return true;
}

static double num(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool boolean(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static void str(const cJSON *o, const char *k, char *out, size_t outlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out, outlen, "%s", v->valuestring);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    memcpy(d, s, n);
    return d;
}

void settings_load(App *a)
{
    char path[1100];
    if (!config_path(path, sizeof path, false))
        return;
    size_t size = 0;
    char *text = SDL_LoadFile(path, &size);
    if (!text)
        return;
    cJSON *root = cJSON_ParseWithLength(text, size);
    SDL_free(text);
    if (!root)
        return;

    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsString(theme))
        a->dark = strcmp(theme->valuestring, "light") != 0;
    a->font_size = clampf((float)num(root, "uiFontSize", a->font_size), UI_FONT_MIN, UI_FONT_MAX);
    a->mono_size = clampf((float)num(root, "terminalFontSize", a->mono_size), MONO_FONT_MIN, MONO_FONT_MAX);
    a->zoom = clampf((float)num(root, "zoom", a->zoom), ZOOM_MIN, ZOOM_MAX);
    str(root, "terminalFont", a->mono_font_path, sizeof a->mono_font_path);
    {
        char builtin[32] = "";
        str(root, "terminalBuiltinFont", builtin, sizeof builtin);
        a->mono_builtin = !strcmp(builtin, "jetbrains-mono") ? MONO_BUILTIN_JETBRAINS : MONO_BUILTIN_PLEX;
    }
    char size_mode[16] = "";
    str(root, "consoleSize", size_mode, sizeof size_mode);
    a->console_fixed = strcmp(size_mode, "fixed") == 0;
    a->console_cols = (int)clampf((float)num(root, "consoleColumns", a->console_cols), CONSOLE_COLS_MIN, CONSOLE_COLS_MAX);
    a->console_rows = (int)clampf((float)num(root, "consoleRows", a->console_rows), CONSOLE_ROWS_MIN, CONSOLE_ROWS_MAX);
    {
        char mode[16] = "", existing[16] = "";
        str(root, "logMode", mode, sizeof mode);
        a->logcfg.mode = !strcmp(mode, "printable") ? LOG_PRINTABLE : !strcmp(mode, "all") ? LOG_ALL : LOG_NONE;
        str(root, "logPath", a->logcfg.path, sizeof a->logcfg.path);
        str(root, "logExisting", existing, sizeof existing);
        a->logcfg.existing = !strcmp(existing, "overwrite") ? LOG_EXISTING_OVERWRITE
                             : !strcmp(existing, "append")  ? LOG_EXISTING_APPEND
                                                            : LOG_EXISTING_ASK;
        a->logcfg.flush = boolean(root, "logFlush", a->logcfg.flush);
        a->logcfg.header = boolean(root, "logHeader", a->logcfg.header);
        a->logcfg.timestamps = boolean(root, "logTimestamps", a->logcfg.timestamps);
    }
    a->scrollback_lines =
        (int)clampf((float)num(root, "scrollbackLines", a->scrollback_lines), SCROLLBACK_MIN, SCROLLBACK_MAX);
    a->sidebar_visible = boolean(root, "sidebarVisible", true);
    a->sidebar_w = (float)num(root, "sidebarWidth", 280);
    a->sect_conn_open = boolean(root, "sectionConnection", true);
    a->sect_term_open = boolean(root, "sectionTerminal", true);
    const cJSON *win = cJSON_GetObjectItemCaseSensitive(root, "window");
    if (cJSON_IsObject(win)) {
        a->win_w = (int)num(win, "w", a->win_w);
        a->win_h = (int)num(win, "h", a->win_h);
        a->win_x = (int)num(win, "x", a->win_x);
        a->win_y = (int)num(win, "y", a->win_y);
        a->win_maximized = boolean(win, "maximized", false);
    }

    const cJSON *tabs = cJSON_GetObjectItemCaseSensitive(root, "tabs");
    const cJSON *jt;
    cJSON_ArrayForEach(jt, tabs)
    {
        Tab *t = app_new_tab(a, NULL);
        if (!t)
            break;
        str(jt, "portId", t->target.id, sizeof t->target.id);
        str(jt, "portPath", t->target.path, sizeof t->target.path);
        str(jt, "portName", t->target.name, sizeof t->target.name);
        t->cfg.baud = (uint32_t)num(jt, "baud", 115200);
        t->cfg.data_bits = (uint8_t)num(jt, "dataBits", 8);
        t->cfg.stop_bits = (uint8_t)num(jt, "stopBits", 1);
        t->cfg.parity = (SerialParity)num(jt, "parity", 0);
        t->cfg.flow = (SerialFlow)num(jt, "flow", 0);
        t->cfg.dtr = boolean(jt, "dtr", true);
        t->cfg.rts = boolean(jt, "rts", true);
        t->auto_reconnect = boolean(jt, "autoReconnect", true);
        t->timestamps = boolean(jt, "timestamps", false);
        t->local_echo = boolean(jt, "localEcho", false);
        t->batch_redraws = boolean(jt, "batchRedraws", true);
        t->nl_mode = (TermNewlineMode)num(jt, "newlineMode", 0);
        t->tb.nl_mode = t->nl_mode;
        t->tx_eol = (TxEol)num(jt, "txEol", EOL_LF);
        t->want_connected = boolean(jt, "connected", false);
        const cJSON *h;
        cJSON_ArrayForEach(h, cJSON_GetObjectItemCaseSensitive(jt, "history"))
        {
            if (cJSON_IsString(h) && t->history_count < HISTORY_MAX)
                t->history[t->history_count++] = dup_str(h->valuestring);
        }
    }
    int active = (int)num(root, "activeTab", 0);
    a->active = active >= 0 && active < a->ntabs ? active : 0;
    cJSON_Delete(root);
    a->settings_dirty = false;
}

void settings_save(App *a)
{
    char path[1100];
    a->settings_dirty = false;
    if (!config_path(path, sizeof path, true))
        return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "theme", a->dark ? "dark" : "light");
    cJSON_AddNumberToObject(root, "uiFontSize", a->font_size);
    cJSON_AddNumberToObject(root, "terminalFontSize", a->mono_size);
    cJSON_AddNumberToObject(root, "zoom", a->zoom);
    cJSON_AddStringToObject(root, "terminalFont", a->mono_font_path);
    cJSON_AddStringToObject(root, "terminalBuiltinFont",
                            a->mono_builtin == MONO_BUILTIN_JETBRAINS ? "jetbrains-mono" : "plex-mono");
    cJSON_AddStringToObject(root, "consoleSize", a->console_fixed ? "fixed" : "fit");
    cJSON_AddNumberToObject(root, "consoleColumns", a->console_cols);
    cJSON_AddNumberToObject(root, "consoleRows", a->console_rows);
    cJSON_AddNumberToObject(root, "scrollbackLines", a->scrollback_lines);
    static const char *const log_modes[] = {"none", "printable", "all"};
    static const char *const log_existing[] = {"ask", "overwrite", "append"};
    cJSON_AddStringToObject(root, "logMode", log_modes[a->logcfg.mode % 3]);
    cJSON_AddStringToObject(root, "logPath", a->logcfg.path);
    cJSON_AddStringToObject(root, "logExisting", log_existing[a->logcfg.existing % 3]);
    cJSON_AddBoolToObject(root, "logFlush", a->logcfg.flush);
    cJSON_AddBoolToObject(root, "logHeader", a->logcfg.header);
    cJSON_AddBoolToObject(root, "logTimestamps", a->logcfg.timestamps);
    cJSON_AddBoolToObject(root, "sidebarVisible", a->sidebar_visible);
    cJSON_AddNumberToObject(root, "sidebarWidth", a->sidebar_w);
    cJSON_AddBoolToObject(root, "sectionConnection", a->sect_conn_open);
    cJSON_AddBoolToObject(root, "sectionTerminal", a->sect_term_open);
    cJSON_AddNumberToObject(root, "activeTab", a->active);

    if (a->window) {
        SDL_WindowFlags fl = SDL_GetWindowFlags(a->window);
        a->win_maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
        if (!a->win_maximized && !(fl & SDL_WINDOW_MINIMIZED)) {
            SDL_GetWindowPosition(a->window, &a->win_x, &a->win_y);
            SDL_GetWindowSize(a->window, &a->win_w, &a->win_h);
        }
    }
    cJSON *win = cJSON_AddObjectToObject(root, "window");
    cJSON_AddNumberToObject(win, "x", a->win_x);
    cJSON_AddNumberToObject(win, "y", a->win_y);
    cJSON_AddNumberToObject(win, "w", a->win_w);
    cJSON_AddNumberToObject(win, "h", a->win_h);
    cJSON_AddBoolToObject(win, "maximized", a->win_maximized);

    cJSON *tabs = cJSON_AddArrayToObject(root, "tabs");
    for (int i = 0; i < a->ntabs; i++) {
        const Tab *t = a->tabs[i];
        cJSON *jt = cJSON_CreateObject();
        cJSON_AddStringToObject(jt, "portId", t->target.id);
        cJSON_AddStringToObject(jt, "portPath", t->target.path);
        cJSON_AddStringToObject(jt, "portName", t->target.name);
        cJSON_AddNumberToObject(jt, "baud", t->cfg.baud);
        cJSON_AddNumberToObject(jt, "dataBits", t->cfg.data_bits);
        cJSON_AddNumberToObject(jt, "stopBits", t->cfg.stop_bits);
        cJSON_AddNumberToObject(jt, "parity", t->cfg.parity);
        cJSON_AddNumberToObject(jt, "flow", t->cfg.flow);
        cJSON_AddBoolToObject(jt, "dtr", t->cfg.dtr);
        cJSON_AddBoolToObject(jt, "rts", t->cfg.rts);
        cJSON_AddBoolToObject(jt, "autoReconnect", t->auto_reconnect);
        cJSON_AddBoolToObject(jt, "timestamps", t->timestamps);
        cJSON_AddBoolToObject(jt, "localEcho", t->local_echo);
        cJSON_AddBoolToObject(jt, "batchRedraws", t->batch_redraws);
        cJSON_AddNumberToObject(jt, "newlineMode", t->nl_mode);
        cJSON_AddNumberToObject(jt, "txEol", t->tx_eol);
        cJSON_AddBoolToObject(jt, "connected", t->want_connected);
        cJSON *h = cJSON_AddArrayToObject(jt, "history");
        int start = t->history_count > 50 ? t->history_count - 50 : 0;
        for (int k = start; k < t->history_count; k++)
            cJSON_AddItemToArray(h, cJSON_CreateString(t->history[k]));
        cJSON_AddItemToArray(tabs, jt);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;
    char tmp[1110];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (SDL_SaveFile(tmp, text, strlen(text)))
        SDL_RenamePath(tmp, path);
    cJSON_free(text);
}
