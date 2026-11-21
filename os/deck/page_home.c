// Home: the summary the deck returns to when idle.
//
//   +------------------------------------+----------+
//   | AGENTS  counts                     | button 1 |
//   |  up to 4 sessions                  +----------+
//   +------------+------------+----------+ button 2 |
//   | CPU        | MEMORY     | NETWORK  +----------+
//   |            |            |          | button 3 |
//   +------------+------------+----------+----------+
//
// Buttons are the host's "home" pins (else the first three). Disk stays on the Stats page.
// The agents panel is a single tap target that opens the Agents page, where each session
// has its own card.
#include <stdio.h>

#include "deck.h"

#define COL_W 236          // right column: same width as the shortcut buttons elsewhere
#define ROWS 4             // agent sessions listed in the summary panel
enum { CTRL_PANEL = HOME_BUTTONS };  // control index of the agents panel (after the buttons)
static const int home_tiles[] = {0, 1, 3};  // TILE slots: CPU, memory, network

static Rect left(void) { Rect a = content_area(); return (Rect){a.x, a.y, a.w - COL_W - 16, a.h}; }
static Rect right(void) { Rect a = content_area(); return (Rect){a.x + a.w - COL_W, a.y, COL_W, a.h}; }
static Rect summary(void) { Rect l = left(); return (Rect){l.x, l.y, l.w, (l.h - 16) / 2}; }
static Rect tiles_area(void) { Rect l = left(), s = summary(); return (Rect){l.x, s.y + s.h + 16, l.w, l.h - s.h - 16}; }
static Rect row(int i) { Rect s = summary(); return (Rect){s.x + 8, s.y + 48 + i * 42, s.w - 16, 38}; }

// Header counts, right-aligned: "1 needs you  2 working  3 idle" (zero counts left out)
static void counts(int right_x, int y) {
    const int n[3] = {agent_counts.idle, agent_counts.working, agent_counts.waiting};
    const char *const names[3] = {"idle", "working", "needs you"};
    for (int st = AGENT_IDLE; st <= AGENT_WAITING; st++) {  // drawn right to left
        if (n[st] <= 0) continue;
        char b[24];
        snprintf(b, sizeof b, "%d %s", n[st], names[st]);
        text_right(&font_s, right_x, y, b, st == AGENT_IDLE ? C_DIM : agent_state_color(st));
        right_x -= text_w(&font_s, b) + 24;
    }
}

static void draw_summary(int ox) {
    Rect s = shift(summary(), ox);
    fill_r(s, button_state(CTRL_PANEL, 1) == BTN_PRESSED ? mix(C_PANEL, C_TEXT, 10) : C_PANEL);
    text(&font_s, s.x + 16, s.y + 14, "AGENTS", C_DIM, 0);
    if (connected) counts(s.x + s.w - 16, s.y + 14);
    int n = 0;
    for (; n < ROWS && agent_slot(n) >= 0; n++) {
        const Agent *a = &agents[agent_slot(n)];
        Rect r = shift(row(n), ox);
        fill(r.x, r.y + 4, 4, r.h - 8, a->state == AGENT_IDLE ? C_MUTED : agent_state_color(a->state));
        char meta[48];
        snprintf(meta, sizeof meta, "%s  %s", a->agent, a->since);
        int mx = r.x + r.w - 8 - text_w(&font_s, meta);
        text(&font_m, r.x + 16, r.y + 3, a->title, connected ? C_TEXT : C_DIM, mx - 16);
        text(&font_s, mx, r.y + 7, meta, a->state == AGENT_IDLE ? C_DIM : agent_state_color(a->state), 0);
    }
    if (n == 0)
        text_center(&font_m, (Rect){s.x, s.y + s.h / 2 - 8, s.w, 32},
                    connected ? "No agent sessions" : "Waiting for the Mac", C_DIM);
}

static void render(int ox) {
    draw_summary(ox);
    for (int i = 0; i < 3; i++) {
        Tile *t = &tiles[home_tiles[i]];
        ui_tile(shift(grid_cell(tiles_area(), 3, 1, 16, i), ox), t->set ? t->label : NULL, t->value, t->detail,
                t->level, connected ? 0 : TILE_STALE);
    }
    for (int i = 0; i < HOME_BUTTONS; i++)
        draw_mac_button(shift(grid_cell(right(), 1, HOME_BUTTONS, 16, i), ox), home_button(i), i);
}

static int hit(int x, int y) {
    for (int i = 0; i < HOME_BUTTONS; i++)
        if (home_button(i) >= 0 && in_rect(grid_cell(right(), 1, HOME_BUTTONS, 16, i), x, y)) return i;
    return in_rect(summary(), x, y) ? CTRL_PANEL : -1;
}

static void press(int i) {
    if (i < HOME_BUTTONS) mac_button_press(home_button(i));
    else page_show(&page_agents);
}

const Page page_home = {"Home", render, hit, press};
