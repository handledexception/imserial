/* The app icon: a DE-9 serial port (D-shell and 9 pins) on a violet tile.
 * tools/gen_app_icon.py draws the same thing into the .ico/.png/.svg files;
 * keep the two in sync. Geometry is in a 64x64 design space. */
#include "ui/widgets.h"

#include <math.h>

#define ICON_R 0x6D
#define ICON_G 0x28
#define ICON_B 0xD9

/* Small sizes (<= 24 px) use this pixel-grid drawing on a full-bleed tile:
 * exact at 16 px, centred 1:1 up to 23 px, scaled at 24. */
static const char *const k_small[16] = {
    "................", "................", "................", "................",
    "................", "..############..", "..#..........#..", "..#.##.##.##.#..",
    "...#........#...", "...#.##..##.#...", "....########....", "................",
    "................", "................", "................", "................",
};

static const float k_shell[4][2] = {{13.5f, 21.5f}, {50.5f, 21.5f}, {45.75f, 43.5f}, {18.25f, 43.5f}};
static const float k_pins[9][2] = {{20, 29},   {26, 29}, {32, 29}, {38, 29}, {44, 29},
                                   {23, 36.5f}, {29, 36.5f}, {35, 36.5f}, {41, 36.5f}};

static float sd_round_rect(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    float qx = fabsf(px - (x0 + x1) * 0.5f) - ((x1 - x0) * 0.5f - r);
    float qy = fabsf(py - (y0 + y1) * 0.5f) - ((y1 - y0) * 0.5f - r);
    return sqrtf(maxf(qx, 0) * maxf(qx, 0) + maxf(qy, 0) * maxf(qy, 0)) + minf(maxf(qx, qy), 0) - r;
}

/* Signed distance to a polygon (negative inside). */
static float sd_poly(float px, float py, const float (*v)[2], int n)
{
    float d = (px - v[0][0]) * (px - v[0][0]) + (py - v[0][1]) * (py - v[0][1]);
    float s = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = v[j][0] - v[i][0], ey = v[j][1] - v[i][1];
        float wx = px - v[i][0], wy = py - v[i][1];
        float h = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
        float bx = wx - ex * h, by = wy - ey * h;
        d = minf(d, bx * bx + by * by);
        bool c1 = py >= v[i][1], c2 = py < v[j][1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
            s = -s;
    }
    return s * sqrtf(d);
}

void app_icon_render(uint8_t *rgba, int pitch, int n)
{
    enum { SS = 4 };
    bool small = n <= 24;
    float half = n <= 40 ? 2.0f : 1.6f, pr = n <= 40 ? 2.6f : 2.4f;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int tile = 0, ink = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float fx = ((float)x + (sx + 0.5f) / SS) * 64.0f / (float)n;
                    float fy = ((float)y + (sy + 0.5f) / SS) * 64.0f / (float)n;
                    float d = small ? sd_round_rect(fx, fy, 0, 0, 64, 64, 12) : sd_round_rect(fx, fy, 4, 4, 60, 60, 13);
                    if (d > 0)
                        continue;
                    tile++;
                    if (small) {
                        int cx, cy;
                        if (n < 24) {
                            int off = (n - 16) / 2;
                            cx = x - off, cy = y - off;
                        } else {
                            cx = (int)(fx / 4), cy = (int)(fy / 4);
                        }
                        ink += cx >= 0 && cx < 16 && cy >= 0 && cy < 16 && k_small[cy][cx] == '#';
                    } else {
                        bool hit = fabsf(sd_poly(fx, fy, k_shell, 4)) <= half;
                        for (int i = 0; i < 9 && !hit; i++) {
                            float dx = fx - k_pins[i][0], dy = fy - k_pins[i][1];
                            hit = dx * dx + dy * dy <= pr * pr;
                        }
                        ink += hit;
                    }
                }
            }
            float a = (float)tile / (SS * SS), w = tile ? (float)ink / (float)tile : 0;
            uint8_t *p = rgba + y * pitch + x * 4;
            p[0] = (uint8_t)(ICON_R + (255 - ICON_R) * w + 0.5f);
            p[1] = (uint8_t)(ICON_G + (255 - ICON_G) * w + 0.5f);
            p[2] = (uint8_t)(ICON_B + (255 - ICON_B) * w + 0.5f);
            p[3] = (uint8_t)(a * 255 + 0.5f);
        }
    }
}

void ui_draw_app_logo(ImDrawList *dl, ImVec2 center, float size)
{
    /* Whole-pixel cells keep the 16x16 drawing sharp at any UI scale. */
    float cell = maxf(1.0f, floorf(size / 16.0f)), n = cell * 16;
    ImVec2 o = v2(floorf(center.x - n * 0.5f), floorf(center.y - n * 0.5f));
    ImDrawList_AddRectFilled(dl, o, v2(o.x + n, o.y + n), COL32(ICON_R, ICON_G, ICON_B, 255), 3 * cell, 0);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (k_small[y][x] == '#')
                ImDrawList_AddRectFilled(dl, v2(o.x + x * cell, o.y + y * cell),
                                         v2(o.x + (x + 1) * cell, o.y + (y + 1) * cell), COL32(255, 255, 255, 255), 0, 0);
}
