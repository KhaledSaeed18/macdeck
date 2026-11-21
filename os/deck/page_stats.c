// Stats: live history charts of the Mac metrics (~2 minutes at one sample per second).
// Headline values come from the TILE messages; history comes from SAMPLE messages.
#include <stdio.h>

#include "deck.h"

typedef struct {
    int tile;                   // TILE slot with the headline value and detail line
    const char *keys[2];        // SAMPLE series drawn in the chart (NULL = meter panel)
    uint32_t colors[2];
    float fixed_max;            // chart scale; 0 = auto-scale (rates)
} StatPanel;

// Tile order is defined by the host: 0 CPU, 1 memory, 2 disk, 3 network (host/src/metrics.ts).
static const StatPanel panels[] = {
    {0, {"cpu", NULL}, {C_ACCENT, 0}, 100},
    {1, {"mem", NULL}, {C_BLUE, 0}, 100},
    {3, {"net_down", "net_up"}, {C_ACCENT, C_WARN}, 0},
    {2, {NULL, NULL}, {0, 0}, 0},
};
#define NPANELS (int)(sizeof panels / sizeof panels[0])

static void fmt_rate(float bps, char *b, size_t cap) {
    const char *u = "BKMG";
    int i = 0;
    while (bps >= 1000 && i < 3) { bps /= 1024; i++; }
    snprintf(b, cap, bps >= 100 || i == 0 ? "%.0f%c/s" : "%.1f%c/s", bps, u[i]);
}

static void chart_panel(Rect r, const StatPanel *p, const Tile *t) {
    Rect chart = {r.x + 16, r.y + 52, r.w - 32, r.h - 52 - 44};
    static float buf[2][HISTORY];
    const float *data[2];
    int counts[2], n = 0;
    float max = p->fixed_max;
    for (int k = 0; k < 2 && p->keys[k]; k++) {
        const Series *s = series_find(p->keys[k]);
        counts[n] = series_linear(s, buf[n]);
        data[n] = buf[n];
        if (!p->fixed_max && series_max(s) > max) max = series_max(s);
        n++;
    }
    if (!p->fixed_max) max = max < 1024 ? 1024 : max * 1.15f;
    ui_chart(chart, data, counts, p->colors, n, max);
    char scale[24];
    if (p->fixed_max) snprintf(scale, sizeof scale, "%.0f%%", p->fixed_max);
    else fmt_rate(max, scale, sizeof scale);
    text_right(&font_s, chart.x + chart.w - 6, chart.y + 4, scale, C_DIM);
    // footer: detail on the left, legend on the right for multi-series charts
    text(&font_s, r.x + 16, r.y + r.h - 34, t->detail, C_DIM, r.x + r.w / 2 + 40);
    if (n > 1) {
        int x = r.x + r.w - 16;
        const char *names[2] = {"down", "up"};
        for (int k = n - 1; k >= 0; k--) {
            x -= text_w(&font_s, names[k]);
            text(&font_s, x, r.y + r.h - 34, names[k], p->colors[k], 0);
            fill(x - 18, r.y + r.h - 25, 12, 4, p->colors[k]);
            x -= 34;
        }
    }
}

static void meter_panel(Rect r, const Tile *t) {
    text(&font_l, r.x + 16, r.y + 60, t->value, connected ? C_TEXT : C_DIM, r.x + r.w - 16);
    ui_meter((Rect){r.x + 16, r.y + r.h - 76, r.w - 32, 14}, t->level, connected ? level_color(t->level, 0) : C_DIM);
    char used[16];
    snprintf(used, sizeof used, "%d%% used", t->level);
    text(&font_s, r.x + 16, r.y + r.h - 46, t->detail, C_DIM, 0);
    text_right(&font_s, r.x + r.w - 16, r.y + r.h - 46, used, C_DIM);
}

static void render(int ox) {
    for (int i = 0; i < NPANELS; i++) {
        Rect r = shift(grid_cell(content_area(), 2, 2, 16, i), ox);
        const StatPanel *p = &panels[i];
        const Tile *t = &tiles[p->tile];
        if (!t->set) { ui_panel(r, "-", NULL, C_DIM); continue; }
        int chart = p->keys[0] != NULL;
        ui_panel(r, t->label, chart ? t->value : NULL, connected ? C_TEXT : C_DIM);
        if (chart) chart_panel(r, p, t); else meter_panel(r, t);
    }
}

static int hit(int x, int y) { (void)x; (void)y; return -1; }
static void press(int i) { (void)i; }

const Page page_stats = {"Stats", render, hit, press};
