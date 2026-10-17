/* Color themes derived from VS Code's "2026 Dark" and "2026 Light" default
 * themes (extensions/theme-defaults/themes/2026-*.json). */
#pragma once
#include "ui/imgui_c.h"
#include "term/termbuf.h"

typedef struct {
    bool dark;
    ImU32 bg;           /* titleBar/sideBar/statusBar background (window base) */
    ImU32 editor_bg;    /* terminal surface */
    ImU32 tabs_bg;      /* editorGroupHeader.tabsBackground */
    ImU32 fg, fg_dim, fg_disabled, icon;
    ImU32 border;       /* widget.border / panel.border */
    ImU32 accent, accent_hover, accent_fg; /* button.* */
    ImU32 secondary_bg, secondary_hover, secondary_fg;
    ImU32 focus;
    ImU32 input_bg, input_border, placeholder;
    ImU32 list_hover, list_active, list_inactive, list_active_fg;
    ImU32 tab_active_bg, tab_active_fg, tab_inactive_fg, tab_top, tab_hover_bg;
    ImU32 menu_bg, menu_sel, widget_bg, widget_border;
    ImU32 scroll, scroll_hover, scroll_active;
    ImU32 cmd_bg, cmd_border, cmd_hover;
    ImU32 badge_bg, badge_fg;
    ImU32 selection;
    ImU32 search_match, search_match_current;
    ImU32 term_fg, term_cursor;
    ImU32 ok, warn, error, info;
    ImU32 close_hover;
    ImU32 status_hover;
    ImU32 toolbar_hover;
    ImU32 shadow;
    ImU32 ansi[16];
} Theme;

void theme_build(Theme *t, bool dark);
void theme_apply_style(const Theme *t, ImGuiStyle *style);
/* Resolves a terminal color (default/palette/RGB) to a draw color. */
ImU32 theme_term_color(const Theme *t, TermColor c, bool foreground);
ImU32 col_alpha(ImU32 c, float alpha_mul);
ImU32 col_lerp(ImU32 a, ImU32 b, float t);
