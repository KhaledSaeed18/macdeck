#include "gfx.h"

#include <stdlib.h>
#include <string.h>

int W, H;
static uint32_t *back;
static int roff, goff, boff, aoff, alen;
static int clip_x0, clip_x1;

int gfx_init_buffer(int w, int h, int r_off, int g_off, int b_off, int a_off, int a_len) {
    W = w; H = h;
    roff = r_off; goff = g_off; boff = b_off; aoff = a_off; alen = a_len;
    back = malloc((size_t)W * H * 4);
    gfx_noclip();
    return back ? 0 : -1;
}
uint32_t *gfx_frame(void) { return back; }
void gfx_clip(int x0, int x1) { clip_x0 = x0 < 0 ? 0 : x0; clip_x1 = x1 > W ? W : x1; }
void gfx_noclip(void) { clip_x0 = 0; clip_x1 = W; }

// ---------------------------------------------------------------- primitives
static uint32_t px(uint32_t hex) {
    return (hex >> 16 & 255) << roff | (hex >> 8 & 255) << goff | (hex & 255) << boff | (alen ? 255u << aoff : 0);
}
uint32_t mix(uint32_t a, uint32_t b, int pct) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        int ca = a >> s & 255, cb = b >> s & 255;
        out |= (uint32_t)(ca + (cb - ca) * pct / 100) << s;
    }
    return out;
}
void fill(int x, int y, int w, int h, uint32_t hex) {
    int x0 = x < clip_x0 ? clip_x0 : x, x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > H ? H : y + h;
    uint32_t c = px(hex);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = back + (size_t)j * W;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}
void fill_r(Rect r, uint32_t hex) { fill(r.x, r.y, r.w, r.h, hex); }
void frame_r(Rect r, int t, uint32_t hex) {
    fill(r.x, r.y, r.w, t, hex); fill(r.x, r.y + r.h - t, r.w, t, hex);
    fill(r.x, r.y, t, r.h, hex); fill(r.x + r.w - t, r.y, t, r.h, hex);
}
void line(int x0, int y0, int x1, int y1, int t, uint32_t hex) {  // Bresenham, t px thick
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        fill(x0, y0, t, t, hex);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
int text_w(const Font *f, const char *s) { return (int)strlen(s) * f->w; }
void text(const Font *f, int x, int y, const char *s, uint32_t hex, int maxx) {
    uint32_t c = px(hex);
    for (; *s; s++, x += f->w) {
        if (maxx > 0 && x + f->w > maxx) break;
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = f->data + (size_t)(ch - 32) * f->h * f->stride;
        for (int r = 0; r < f->h; r++) {
            int py = y + r;
            if (py < 0 || py >= H) continue;
            for (int b = 0; b < f->w; b++)
                if (g[r * f->stride + b / 8] & (0x80 >> (b % 8))) {
                    int pxl = x + b;
                    if (pxl >= clip_x0 && pxl < clip_x1) back[(size_t)py * W + pxl] = c;
                }
        }
    }
}
void text_center(const Font *f, Rect r, const char *s, uint32_t hex) {
    int tw = text_w(f, s);
    if (tw > r.w) tw = r.w;
    text(f, r.x + (r.w - tw) / 2, r.y + (r.h - f->h) / 2, s, hex, r.x + r.w);
}
void text_right(const Font *f, int right, int y, const char *s, uint32_t hex) { text(f, right - text_w(f, s), y, s, hex, 0); }

// ---------------------------------------------------------------- layout
Rect inset(Rect r, int d) { return (Rect){r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }
Rect shift(Rect r, int dx) { r.x += dx; return r; }
Rect grid_cell(Rect a, int cols, int rows, int gap, int i) {
    int cw = (a.w - (cols - 1) * gap) / cols, ch = (a.h - (rows - 1) * gap) / rows;
    return (Rect){a.x + (i % cols) * (cw + gap), a.y + (i / cols) * (ch + gap), cw, ch};
}
int in_rect(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// ---------------------------------------------------------------- widgets
uint32_t level_color(int level, int invert) {
    if (invert) return level <= 15 ? C_BAD : level <= 30 ? C_WARN : C_ACCENT;
    return level >= 90 ? C_BAD : level >= 75 ? C_WARN : C_ACCENT;
}

void ui_meter(Rect r, int level, uint32_t color) {
    int lv = level < 0 ? 0 : level > 100 ? 100 : level;
    fill_r(r, C_BAR);
    fill(r.x, r.y, r.w * lv / 100, r.h, color);
}

// Label, big value, optional level bar and a detail line; matches the original deck tiles.
void ui_tile(Rect r, const char *label, const char *value, const char *detail, int level, int flags) {
    fill_r(r, C_PANEL);
    int right = r.x + r.w - 8;
    if (!label) { text(&font_s, r.x + 16, r.y + 14, "-", C_DIM, 0); return; }
    int stale = flags & TILE_STALE, inv = flags & TILE_INVERT;
    text(&font_s, r.x + 16, r.y + 14, label, C_DIM, right);
    uint32_t lc = level >= 0 ? level_color(level, inv) : C_ACCENT;
    text(&font_l, r.x + 16, r.y + r.h / 2 - 59, value, stale ? C_DIM : lc == C_ACCENT ? C_TEXT : lc, right);
    if (level >= 0) ui_meter((Rect){r.x + 16, r.y + r.h - 90, r.w - 32, 10}, level, stale ? C_DIM : lc);
    if (detail) text(&font_s, r.x + 16, r.y + r.h - 60, detail, C_DIM, right);
}

void ui_button(Rect r, const char *label, uint32_t color, int state) {
    if (!label) { fill_r(r, C_PANEL); return; }
    uint32_t bg = state == BTN_PRESSED ? 0xffffff : state == BTN_DISABLED ? C_MUTED : color;
    uint32_t ink = state == BTN_PRESSED ? C_INK : state == BTN_DISABLED ? C_DIM : color == C_LOCAL ? C_TEXT : C_INK;
    fill_r(r, bg);
    text_center(&font_m, inset(r, 8), label, ink);
}

// Panel with a small label on the left and a medium value on the right of its header.
void ui_panel(Rect r, const char *label, const char *value, uint32_t value_color) {
    fill_r(r, C_PANEL);
    text(&font_s, r.x + 16, r.y + 14, label, C_DIM, r.x + r.w / 2);
    if (value) text_right(&font_m, r.x + r.w - 16, r.y + 10, value, value_color);
}

void ui_chart(Rect r, const float *const *series, const int *counts, const uint32_t *colors, int nseries, float max) {
    fill_r(r, mix(C_PANEL, C_BG, 40));
    for (int g = 1; g < 4; g++) fill(r.x, r.y + r.h * g / 4, r.w, 1, C_BAR);  // grid lines
    if (max <= 0) max = 1;
    for (int s = 0; s < nseries; s++) {
        int n = counts[s];
        if (n < 2) continue;
        uint32_t area = mix(mix(C_PANEL, C_BG, 40), colors[s], nseries > 1 ? 18 : 28);
        int prev_x = 0, prev_y = 0;
        for (int x = 0; x < r.w; x++) {
            float t = (float)x * (n - 1) / (r.w - 1);
            int i = (int)t;
            float v = i + 1 < n ? series[s][i] + (series[s][i + 1] - series[s][i]) * (t - i) : series[s][n - 1];
            if (v < 0) v = 0;
            if (v > max) v = max;
            int y = r.y + r.h - 1 - (int)(v / max * (r.h - 2));
            if (s == 0 || nseries == 1) fill(r.x + x, y, 1, r.y + r.h - y, area);
            if (x) line(r.x + prev_x, prev_y, r.x + x, y, 2, colors[s]);
            prev_x = x; prev_y = y;
        }
    }
}
