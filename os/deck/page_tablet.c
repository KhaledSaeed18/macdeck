// Tablet: battery, power, display and system readings plus the tablet's own controls.
// These buttons act locally; changed settings are reported to the host to be persisted.
#include <stdio.h>
#include <string.h>

#include "deck.h"

static const int levels[] = {10, 30, 60, 80, 120, 160, 255};
static const int dim_choices[] = {15, 30, 60, 300, 0};      // seconds, 0 = never
static const int off_choices[] = {60, 300, 900, 1800, 0};
#define LEN(a) (int)(sizeof a / sizeof a[0])

static Rect left(void) { Rect a = content_area(); return (Rect){a.x, a.y, a.w / 2 - 8, a.h}; }
static Rect right(void) { Rect a = content_area(); return (Rect){a.x + a.w / 2 + 16, a.y, a.w / 2 - 16, a.h}; }

static int step(const int *c, int n, int cur, int dir) {  // neighbour in an ordered list
    int i = 0;
    while (i < n && c[i] != cur) i++;
    if (i == n) return c[0];
    i += dir;
    return c[i < 0 ? 0 : i >= n ? n - 1 : i];
}
static int cycle(const int *c, int n, int cur) {
    for (int i = 0; i < n; i++) if (c[i] == cur) return c[(i + 1) % n];
    return c[0];
}
static void set_int(const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); apply_setting(key, b, 1); }

// ---------------------------------------------------------------- controls
enum { DIMMER, BRIGHTER, DIM_AFTER, OFF_AFTER, CPU_MODE, SCREEN_OFF_NOW, NCONTROLS };

static void label(int i, char *b, size_t cap) {
    char t[16];
    switch (i) {
    case DIMMER: snprintf(b, cap, "Dimmer"); break;
    case BRIGHTER: snprintf(b, cap, "Brighter"); break;
    case DIM_AFTER: snprintf(b, cap, "Dim: %s", fmt_secs(dim_after, t, sizeof t)); break;
    case OFF_AFTER: snprintf(b, cap, "Off: %s", fmt_secs(off_after, t, sizeof t)); break;
    case CPU_MODE: snprintf(b, cap, "CPU: %s", cpu_saver ? "saver" : "auto"); break;
    case SCREEN_OFF_NOW: snprintf(b, cap, "Screen off"); break;
    }
}

static void press(int i) {
    char t[16], msg[64];
    switch (i) {
    case DIMMER: case BRIGHTER:
        set_int("brightness", step(levels, LEN(levels), brightness, i == BRIGHTER ? 1 : -1));
        snprintf(msg, sizeof msg, "Brightness %d", brightness);
        break;
    case DIM_AFTER:
        set_int("dim", cycle(dim_choices, LEN(dim_choices), dim_after));
        snprintf(msg, sizeof msg, "Dim after %s", fmt_secs(dim_after, t, sizeof t));
        break;
    case OFF_AFTER:
        set_int("off", cycle(off_choices, LEN(off_choices), off_after));
        snprintf(msg, sizeof msg, "Screen off after %s", fmt_secs(off_after, t, sizeof t));
        break;
    case CPU_MODE:
        apply_setting("cpu", cpu_saver ? "auto" : "saver", 1);
        snprintf(msg, sizeof msg, "%s", cpu_saver ? "CPU saver: max 624 MHz" : "CPU auto: up to 1205 MHz");
        break;
    case SCREEN_OFF_NOW: set_screen(SCREEN_OFF, "button"); return;
    default: return;
    }
    toast(msg, 4);
}

// ---------------------------------------------------------------- page
static void render(int ox) {
    char v[24], d[40], t[16];
    Rect l = left();
    if (tab.battery >= 0) snprintf(v, sizeof v, "%d%%", tab.battery); else copy(v, sizeof v, "?");
    ui_tile(shift(grid_cell(l, 2, 2, 16, 0), ox), "BATTERY", v, tab.status[0] ? tab.status : "unknown", tab.battery, TILE_INVERT);
    snprintf(v, sizeof v, "%+dmA", tab.current_ma);
    snprintf(d, sizeof d, "%d.%02dV %d.%dC", tab.mv / 1000, tab.mv % 1000 / 10, tab.temp_dc / 10, tab.temp_dc % 10);
    ui_tile(shift(grid_cell(l, 2, 2, 16, 1), ox), "POWER", v, d, LEVEL_NONE, 0);
    snprintf(v, sizeof v, "%d", screen == SCREEN_ON ? brightness : 0);
    snprintf(d, sizeof d, "dim %s off ", fmt_secs(dim_after, t, sizeof t));
    strncat(d, fmt_secs(off_after, t, sizeof t), sizeof d - strlen(d) - 1);
    ui_tile(shift(grid_cell(l, 2, 2, 16, 2), ox), "BRIGHTNESS", v, d, brightness * 100 / 255, 0);
    snprintf(v, sizeof v, "%dMHz", tab.freq_mhz);
    snprintf(d, sizeof d, "up %ld:%02ld %dM free", tab.uptime / 3600, tab.uptime / 60 % 60, tab.mem_free_mb);
    ui_tile(shift(grid_cell(l, 2, 2, 16, 3), ox), "CPU / SYSTEM", v, d, LEVEL_NONE, 0);
    for (int i = 0; i < NCONTROLS; i++) {
        char b[32];
        label(i, b, sizeof b);
        ui_button(shift(grid_cell(right(), 2, 3, 16, i), ox), b, i == SCREEN_OFF_NOW ? C_BAD : C_LOCAL, button_state(i, 1));
    }
}

static int hit(int x, int y) {
    for (int i = 0; i < NCONTROLS; i++) if (in_rect(grid_cell(right(), 2, 3, 16, i), x, y)) return i;
    return -1;
}

const Page page_tablet = {"Tablet", render, hit, press};
