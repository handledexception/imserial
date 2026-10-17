/* UI regions. Each draws into the root window at the given rectangle. */
#pragma once
#include "app/app.h"
#include "ui/widgets.h"

void ui_titlebar(App *a, ImVec2 p0, ImVec2 p1);
void ui_sidebar(App *a, ImVec2 p0, ImVec2 p1);
/* Draws the tab strip and returns its height. */
float ui_tabs(App *a, ImVec2 p0, float width);
void ui_termview(App *a, Tab *t, ImVec2 p0, ImVec2 p1);
void ui_sendbar(App *a, Tab *t, ImVec2 p0, ImVec2 p1);
void ui_statusbar(App *a, ImVec2 p0, ImVec2 p1);
void ui_quickpick(App *a);
void ui_open_quickpick(App *a);
void ui_preferences(App *a);
void ui_log_prompt(App *a);

/* Shared bits */
ImU32 ui_link_color(const Theme *th, LinkState s);
void ui_link_icon(ImDrawList *dl, ImVec2 c, LinkState s, float size);
void termview_copy_selection(Tab *t);
void termview_paste(App *a, Tab *t);
void termview_select_all(Tab *t);
bool termview_sdl_event(App *a, const SDL_Event *e);
