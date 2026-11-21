// State received from the Mac, protocol parsing (protocol/PROTOCOL.md) and the toast line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck.h"

Tile tiles[MAX_TILES];
Button buttons[MAX_BUTTONS];
char clock_time[16] = "--:--", clock_date[32] = "";
int connected = 0, dirty = 1;

// ---------------------------------------------------------------- sample history
#define MAX_SERIES 16
static Series series[MAX_SERIES];
static int nseries;

static Series *series_get(const char *key, int create) {
    for (int i = 0; i < nseries; i++) if (!strcmp(series[i].key, key)) return &series[i];
    if (!create || nseries == MAX_SERIES) return NULL;
    Series *s = &series[nseries++];
    copy(s->key, sizeof s->key, key);
    return s;
}
const Series *series_find(const char *key) { return series_get(key, 0); }
static void series_push(Series *s, float v) {
    s->v[s->head] = v;
    s->head = (s->head + 1) % HISTORY;
    if (s->n < HISTORY) s->n++;
}
int series_linear(const Series *s, float *out) {
    if (!s) return 0;
    int start = (s->head - s->n + HISTORY) % HISTORY;
    for (int i = 0; i < s->n; i++) out[i] = s->v[(start + i) % HISTORY];
    return s->n;
}
float series_max(const Series *s) {
    float m = 0;
    for (int i = 0; s && i < s->n; i++) if (s->v[i] > m) m = s->v[i];
    return m;
}

int home_button(int i) {  // i-th button flagged "home", else the i-th defined button
    int seen = 0, any_flag = 0;
    for (int s = 0; s < MAX_BUTTONS; s++) any_flag |= buttons[s].id[0] && buttons[s].home;
    for (int s = 0; s < MAX_BUTTONS; s++) {
        if (!buttons[s].id[0] || (any_flag && !buttons[s].home)) continue;
        if (seen++ == i) return s;
    }
    return -1;
}

// ---------------------------------------------------------------- toast
static char toast_buf[128];
static double toast_until;
void toast(const char *t, double secs) { copy(toast_buf, sizeof toast_buf, t); toast_until = now() + secs; dirty = 1; }
const char *toast_text(void) { return toast_until && now() < toast_until ? toast_buf : NULL; }
int toast_expired(double t) {
    if (!toast_until || t < toast_until) return 0;
    toast_until = 0; toast_buf[0] = 0;
    return 1;
}

// ---------------------------------------------------------------- protocol
static int split(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) if (*p == '|') { *p = 0; f[n++] = p + 1; }
    return n;
}
static int set_text(char *dst, size_t cap, const char *src) {
    if (!strcmp(dst, src)) return 0;
    copy(dst, cap, src);
    return 1;
}
#define SET(dst, src) set_text(dst, sizeof dst, src)

static int handle_tile(char **f) {
    int s = atoi(f[1]), lv = atoi(f[5]);
    if (s < 0 || s >= MAX_TILES) return 0;
    Tile *t = &tiles[s];
    int changed = SET(t->label, f[2]) | SET(t->value, f[3]) | SET(t->detail, f[4]) | (lv != t->level) | !t->set;
    t->level = lv; t->set = 1;
    return changed;
}
static int handle_button(char **f, int n) {
    int s = atoi(f[1]);
    if (s < 0 || s >= MAX_BUTTONS) return 0;
    Button *b = &buttons[s];
    copy(b->id, sizeof b->id, f[2]); copy(b->label, sizeof b->label, f[3]);
    b->color = (unsigned)strtoul(f[4], NULL, 16);
    b->home = n >= 6 && !strcmp(f[5], "home");
    return 1;
}

void model_handle(char *line) {
    char *f[8];
    int n = split(line, f, 8), changed = 0;
    const char *type = f[0];
    if (!strcmp(type, "PING") && n >= 2) link_send("PONG|%s", f[1]);
    else if (!strcmp(type, "CLOCK") && n >= 3) changed = SET(clock_time, f[1]) | SET(clock_date, f[2]);
    else if (!strcmp(type, "TILE") && n >= 6) changed = handle_tile(f);
    else if (!strcmp(type, "BUTTON") && n >= 5) changed = handle_button(f, n);
    else if (!strcmp(type, "SAMPLE") && n >= 3) {
        Series *s = series_get(f[1], 1);
        if (s) { series_push(s, (float)atof(f[2])); changed = 1; }
    } else if (!strcmp(type, "STATUS") && n >= 2) {
        double secs = n >= 3 ? atof(f[2]) : 4;
        toast(f[1], secs > 0 && secs <= 60 ? secs : 4);
    } else if (!strcmp(type, "SETTING") && n >= 3) apply_setting(f[1], f[2], 0);
    else if (!strcmp(type, "WAKE")) { power_activity(); set_screen(SCREEN_ON, "mac"); }
    if (changed && screen != SCREEN_OFF) dirty = 1;
}
