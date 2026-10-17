#include "ui/theme.h"

#include <string.h>

/* "#RRGGBB" or "#RRGGBBAA" -> ImU32 */
static ImU32 hex(const char *s)
{
    unsigned v[4] = {0, 0, 0, 255};
    size_t n = strlen(s + 1) / 2;
    for (size_t i = 0; i < n && i < 4; i++) {
        unsigned b = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[1 + i * 2 + k];
            b = b * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
        }
        v[i] = b;
    }
    return COL32(v[0], v[1], v[2], v[3]);
}

ImU32 col_alpha(ImU32 c, float a)
{
    float alpha = (float)((c >> 24) & 0xFF) * a;
    return (c & 0x00FFFFFFu) | ((ImU32)clampf(alpha, 0, 255) << 24);
}

ImU32 col_lerp(ImU32 a, ImU32 b, float t)
{
    ImU32 r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float x = (float)((a >> sh) & 0xFF), y = (float)((b >> sh) & 0xFF);
        r |= (ImU32)(x + (y - x) * t + 0.5f) << sh;
    }
    return r;
}

/* Flattens a translucent color over an opaque background. */
static ImU32 over(ImU32 fg, ImU32 bg)
{
    float a = (float)(fg >> 24) / 255.0f;
    return col_lerp(bg | 0xFF000000u, fg | 0xFF000000u, a);
}

void theme_build(Theme *t, bool dark)
{
    memset(t, 0, sizeof *t);
    t->dark = dark;
    if (dark) {
        t->bg = hex("#191A1B");
        t->editor_bg = hex("#121314");
        t->tabs_bg = hex("#191A1B");
        t->fg = hex("#BFBFBF");
        t->fg_dim = hex("#8C8C8C");
        t->fg_disabled = hex("#555555");
        t->icon = hex("#8C8C8C");
        t->border = hex("#2A2B2C");
        t->accent = hex("#297AA0");
        t->accent_hover = hex("#2B7DA3");
        t->accent_fg = hex("#FFFFFF");
        t->secondary_bg = hex("#FFFFFF0C");
        t->secondary_hover = hex("#FFFFFF16");
        t->secondary_fg = hex("#CCCCCC");
        t->focus = hex("#3994BCB3");
        t->input_bg = hex("#191A1B");
        t->input_border = hex("#333536");
        t->placeholder = hex("#555555");
        t->list_hover = hex("#FFFFFF14");
        t->list_active = hex("#FFFFFF22");
        t->list_inactive = hex("#2C2D2E");
        t->list_active_fg = hex("#EDEDED");
        t->tab_active_bg = hex("#121314");
        t->tab_active_fg = hex("#BFBFBF");
        t->tab_inactive_fg = hex("#8C8C8C");
        t->tab_top = hex("#3994BC");
        t->tab_hover_bg = hex("#FFFFFF0A");
        t->menu_bg = hex("#202122");
        t->menu_sel = hex("#3994BC26");
        t->widget_bg = hex("#202122");
        t->widget_border = hex("#2A2B2C");
        t->scroll = hex("#A8A9AA40");
        t->scroll_hover = hex("#A8A9AA70");
        t->scroll_active = hex("#A8A9AA9C");
        t->cmd_bg = hex("#1E1F20");
        t->cmd_border = hex("#2E3031");
        t->cmd_hover = hex("#242526");
        t->badge_bg = hex("#307E9F");
        t->badge_fg = hex("#FFFFFF");
        t->selection = hex("#3994BC55");
        t->search_match = hex("#CCA70040");
        t->search_match_current = hex("#CCA70099");
        t->term_fg = hex("#CCCCCC");
        t->term_cursor = hex("#BFBFBF");
        t->ok = hex("#73C991");
        t->warn = hex("#CCA700");
        t->error = hex("#F48771");
        t->info = hex("#48A0C7");
        t->close_hover = hex("#E81123");
        t->status_hover = hex("#FFFFFF12");
        t->toolbar_hover = hex("#FFFFFF14");
        t->shadow = hex("#00000060");
        static const char *ansi[16] = {"#000000", "#CD3131", "#0DBC79", "#E5E510", "#2472C8", "#BC3FBC",
                                       "#11A8CD", "#E5E5E5", "#666666", "#F14C4C", "#23D18B", "#F5F543",
                                       "#3B8EEA", "#D670D6", "#29B8DB", "#E5E5E5"};
        for (int i = 0; i < 16; i++)
            t->ansi[i] = hex(ansi[i]);
    } else {
        t->bg = hex("#FAFAFD");
        t->editor_bg = hex("#FFFFFF");
        t->tabs_bg = hex("#FAFAFD");
        t->fg = hex("#202020");
        t->fg_dim = hex("#606060");
        t->fg_disabled = hex("#A0A0A0");
        t->icon = hex("#606060");
        t->border = hex("#E2E2E5");
        t->accent = hex("#0069CC");
        t->accent_hover = hex("#0063C1");
        t->accent_fg = hex("#FFFFFF");
        t->secondary_bg = hex("#EAEAEA");
        t->secondary_hover = hex("#E0E0E2");
        t->secondary_fg = hex("#202020");
        t->focus = hex("#0069CC");
        t->input_bg = hex("#FFFFFF");
        t->input_border = hex("#D8D8D8");
        t->placeholder = hex("#999999");
        t->list_hover = hex("#00000010");
        t->list_active = hex("#0069CC1F");
        t->list_inactive = hex("#DADADA99");
        t->list_active_fg = hex("#202020");
        t->tab_active_bg = hex("#FFFFFF");
        t->tab_active_fg = hex("#202020");
        t->tab_inactive_fg = hex("#606060");
        t->tab_top = hex("#0069CC");
        t->tab_hover_bg = hex("#0000000A");
        t->menu_bg = hex("#FFFFFF");
        t->menu_sel = hex("#0069CC1A");
        t->widget_bg = hex("#FAFAFD");
        t->widget_border = hex("#E4E5E6");
        t->scroll = hex("#64646440");
        t->scroll_hover = hex("#64646480");
        t->scroll_active = hex("#646464C0");
        t->cmd_bg = hex("#FFFFFF");
        t->cmd_border = hex("#D8D8D8AA");
        t->cmd_hover = hex("#F3F3F5");
        t->badge_bg = hex("#0069CC");
        t->badge_fg = hex("#FFFFFF");
        t->selection = hex("#0069CC33");
        t->search_match = hex("#BF880040");
        t->search_match_current = hex("#BF880090");
        t->term_fg = hex("#3B3B3B");
        t->term_cursor = hex("#202020");
        t->ok = hex("#388A34");
        t->warn = hex("#BF8803");
        t->error = hex("#C72E0F");
        t->info = hex("#0069CC");
        t->close_hover = hex("#E81123");
        t->status_hover = hex("#E3E3E5");
        t->toolbar_hover = hex("#0000001A");
        t->shadow = hex("#00000022");
        static const char *ansi[16] = {"#000000", "#CD3131", "#00BC00", "#949800", "#0451A5", "#BC05BC",
                                       "#0598BC", "#555555", "#666666", "#CD3131", "#14CE14", "#B5BA00",
                                       "#0451A5", "#BC05BC", "#0598BC", "#A5A5A5"};
        for (int i = 0; i < 16; i++)
            t->ansi[i] = hex(ansi[i]);
    }
}

static ImVec4 to_v4(ImU32 c)
{
    return v4((float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f,
              (float)((c >> 24) & 0xFF) / 255.0f);
}

void theme_apply_style(const Theme *t, ImGuiStyle *s)
{
    s->WindowPadding = v2(10, 10);
    s->WindowRounding = 8;
    s->WindowBorderSize = 1;
    s->ChildRounding = 8;
    s->ChildBorderSize = 0;
    s->PopupRounding = 6;
    s->PopupBorderSize = 1;
    s->FramePadding = v2(8, 5);
    s->FrameRounding = 4;
    s->FrameBorderSize = 1;
    s->ItemSpacing = v2(8, 6);
    s->ItemInnerSpacing = v2(6, 4);
    s->IndentSpacing = 14;
    s->ScrollbarSize = 10;
    s->ScrollbarRounding = 6;
    s->ScrollbarPadding = 2;
    s->GrabMinSize = 24;
    s->GrabRounding = 4;
    s->TabRounding = 6;
    s->SelectableRounding = 4;
    s->MenuItemRounding = 4;
    s->SeparatorSize = 1;
    s->SelectableTextAlign = v2(0, 0.5f);

    ImVec4 *c = s->Colors;
    ImU32 frame = t->input_bg;
    c[ImGuiCol_Text] = to_v4(t->fg);
    c[ImGuiCol_TextDisabled] = to_v4(t->fg_disabled);
    c[ImGuiCol_WindowBg] = to_v4(t->bg);
    c[ImGuiCol_ChildBg] = to_v4(0);
    c[ImGuiCol_PopupBg] = to_v4(t->menu_bg);
    c[ImGuiCol_Border] = to_v4(t->border);
    c[ImGuiCol_BorderShadow] = to_v4(0);
    c[ImGuiCol_FrameBg] = to_v4(frame);
    c[ImGuiCol_FrameBgHovered] = to_v4(over(t->list_hover, frame));
    c[ImGuiCol_FrameBgActive] = to_v4(over(t->list_active, frame));
    c[ImGuiCol_TitleBg] = to_v4(t->bg);
    c[ImGuiCol_TitleBgActive] = to_v4(t->bg);
    c[ImGuiCol_TitleBgCollapsed] = to_v4(t->bg);
    c[ImGuiCol_MenuBarBg] = to_v4(t->bg);
    c[ImGuiCol_ScrollbarBg] = to_v4(0);
    c[ImGuiCol_ScrollbarGrab] = to_v4(t->scroll);
    c[ImGuiCol_ScrollbarGrabHovered] = to_v4(t->scroll_hover);
    c[ImGuiCol_ScrollbarGrabActive] = to_v4(t->scroll_active);
    c[ImGuiCol_CheckMark] = to_v4(t->accent_fg);
    c[ImGuiCol_CheckboxSelectedBg] = to_v4(t->accent);
    c[ImGuiCol_SliderGrab] = to_v4(t->accent);
    c[ImGuiCol_SliderGrabActive] = to_v4(t->accent_hover);
    c[ImGuiCol_Button] = to_v4(t->secondary_bg);
    c[ImGuiCol_ButtonHovered] = to_v4(t->secondary_hover);
    c[ImGuiCol_ButtonActive] = to_v4(t->list_active);
    c[ImGuiCol_Header] = to_v4(t->list_active);
    c[ImGuiCol_HeaderHovered] = to_v4(t->list_hover);
    c[ImGuiCol_HeaderActive] = to_v4(t->list_active);
    c[ImGuiCol_Separator] = to_v4(t->border);
    c[ImGuiCol_SeparatorHovered] = to_v4(t->tab_top);
    c[ImGuiCol_SeparatorActive] = to_v4(t->tab_top);
    c[ImGuiCol_ResizeGrip] = to_v4(0);
    c[ImGuiCol_ResizeGripHovered] = to_v4(0);
    c[ImGuiCol_ResizeGripActive] = to_v4(0);
    c[ImGuiCol_InputTextCursor] = to_v4(t->fg);
    c[ImGuiCol_Tab] = to_v4(0);
    c[ImGuiCol_TabHovered] = to_v4(t->tab_hover_bg);
    c[ImGuiCol_TabSelected] = to_v4(t->tab_active_bg);
    c[ImGuiCol_TabSelectedOverline] = to_v4(t->tab_top);
    c[ImGuiCol_TextLink] = to_v4(t->info);
    c[ImGuiCol_TextSelectedBg] = to_v4(t->selection);
    c[ImGuiCol_NavCursor] = to_v4(t->focus);
    c[ImGuiCol_ModalWindowDimBg] = to_v4(t->dark ? COL32(0, 0, 0, 90) : COL32(0, 0, 0, 40));
}

ImU32 theme_term_color(const Theme *t, TermColor c, bool fg)
{
    switch (TC_KIND(c)) {
    case 1: {
        unsigned i = c & 0xFF;
        if (i < 16)
            return t->ansi[i];
        if (i < 232) { /* 6x6x6 cube */
            i -= 16;
            static const unsigned lv[6] = {0, 95, 135, 175, 215, 255};
            return COL32(lv[(i / 36) % 6], lv[(i / 6) % 6], lv[i % 6], 255);
        }
        unsigned g = 8 + (i - 232) * 10;
        return COL32(g, g, g, 255);
    }
    case 2:
        return COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    default:
        return fg ? t->term_fg : t->editor_bg;
    }
}
