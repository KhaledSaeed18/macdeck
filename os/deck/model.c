// State received from the Mac, protocol parsing (protocol/PROTOCOL.md) and the toast line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck.h"

Tile tiles[MAX_TILES];
Button buttons[MAX_BUTTONS];
Agent agents[MAX_AGENTS];
AgentCounts agent_counts;
MacState mac;
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

int agent_slot(int i) {  // agent cards are drawn in slot order, skipping empty slots
    for (int s = 0, seen = 0; s < MAX_AGENTS; s++)
        if (agents[s].set && seen++ == i) return s;
    return -1;
}

static int handle_agent(char **f, int n) {  // AGENT|slot|agent|state|title|where|since[|detail[|id]]
    int s = atoi(f[1]);
    if (s < 0 || s >= MAX_AGENTS) return 0;
    Agent *a = &agents[s];
    if (n < 7 || !f[2][0]) { int was = a->set; *a = (Agent){0}; return was; }
    int st = !strcmp(f[3], "working") ? AGENT_WORKING : !strcmp(f[3], "waiting") ? AGENT_WAITING : AGENT_IDLE;
    int changed = SET(a->agent, f[2]) | SET(a->title, f[4]) | SET(a->where, f[5]) | SET(a->since, f[6]) |
                  SET(a->detail, n >= 8 ? f[7] : "") | (st != a->state) | !a->set;
    copy(a->id, sizeof a->id, n >= 9 ? f[8] : "");
    a->state = st; a->set = 1;
    return changed;
}

// ---------------------------------------------------------------- Mac page state
static int mac_value(int k, int v) {  // while the user's change is in flight, only its confirmation counts
    if (mac.pending[k] && now() < mac.pending[k] && v != mac.val[k]) return 0;
    int changed = v != mac.val[k] || mac.pending[k];  // also redraw when a pending change is confirmed
    mac.pending[k] = 0; mac.val[k] = v;
    return changed;
}

static int handle_mac(char **f, int n) {  // MAC|battery|pct|charging|source|line|detail, MAC|sound|vol|muted, ...
    const char *k = f[1];
    int on = n >= 3 && !strcmp(f[2], "on"), changed = !mac.have;
    mac.have = 1;
    if (!strcmp(k, "battery") && n >= 7) {
        changed |= mac.batt_pct != atoi(f[2]) || mac.batt_charging != atoi(f[3]);
        mac.batt_pct = atoi(f[2]); mac.batt_charging = atoi(f[3]);
        changed |= SET(mac.batt_source, f[4]) | SET(mac.batt_line, f[5]) | SET(mac.batt_detail, f[6]);
    } else if (!strcmp(k, "sound") && n >= 4) changed |= mac_value(MAC_VOLUME, atoi(f[2])) | mac_value(MAC_MUTE, atoi(f[3]));
    else if (!strcmp(k, "wifi") && n >= 4) changed |= mac_value(MAC_WIFI, on) | SET(mac.wifi_detail, f[3]);
    else if (!strcmp(k, "airdrop") && n >= 3)
        changed |= mac_value(MAC_AIRDROP, !strcmp(f[2], "everyone") ? AIRDROP_EVERYONE : !strcmp(f[2], "off") ? AIRDROP_OFF : AIRDROP_CONTACTS);
    else if (!strcmp(k, "awake") && n >= 4) changed |= mac_value(MAC_AWAKE, on) | SET(mac.awake_detail, f[3]);
    else if (!strcmp(k, "dark") && n >= 3) changed |= mac_value(MAC_DARK, on);
    return changed;
}

void model_handle(char *line) {
    char *f[10];
    int n = split(line, f, 10), changed = 0;
    const char *type = f[0];
    if (!strcmp(type, "PING") && n >= 2) link_send("PONG|%s", f[1]);
    else if (!strcmp(type, "CLOCK") && n >= 3) changed = SET(clock_time, f[1]) | SET(clock_date, f[2]);
    else if (!strcmp(type, "TILE") && n >= 6) changed = handle_tile(f);
    else if (!strcmp(type, "BUTTON") && n >= 5) changed = handle_button(f, n);
    else if (!strcmp(type, "AGENT") && n >= 3) changed = handle_agent(f, n);
    else if (!strcmp(type, "MAC") && n >= 3) changed = handle_mac(f, n);
    else if (!strcmp(type, "AGENTS") && n >= 5) {
        AgentCounts c = {atoi(f[1]), atoi(f[2]), atoi(f[3]), atoi(f[4])};
        changed = memcmp(&c, &agent_counts, sizeof c) != 0;
        agent_counts = c;
    }
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
