// Agents: the AI coding-agent sessions running on the Mac (Claude Code, Codex), one card
// each, sorted by the host: waiting for the user first, then working, then idle.
#include <stdio.h>

#include "deck.h"

static Rect card(int i) { return grid_cell(content_area(), 2, 3, 16, i); }

static void draw_card(Rect r, const Agent *a) {
    static const char *const names[] = {"IDLE", "WORKING", "NEEDS YOU"};
    static const uint32_t colors[] = {C_DIM, C_ACCENT, C_WARN};
    uint32_t c = connected ? colors[a->state] : C_DIM;
    int right = r.x + r.w - 16;
    fill_r(r, C_PANEL);
    fill(r.x, r.y, 6, r.h, a->state == AGENT_IDLE ? C_MUTED : c);
    if (a->state == AGENT_WAITING && connected) frame_r(r, 2, C_WARN);
    text(&font_s, r.x + 22, r.y + 14, names[a->state], c, 0);
    text_right(&font_s, right, r.y + 14, a->since, C_DIM);
    text(&font_m, r.x + 22, r.y + 46, a->title, connected ? C_TEXT : C_DIM, right);
    char foot[96];
    if (a->state == AGENT_WAITING && a->detail[0]) snprintf(foot, sizeof foot, "%s: %s", a->agent, a->detail);
    else if (a->where[0]) snprintf(foot, sizeof foot, "%s / %s", a->agent, a->where);
    else snprintf(foot, sizeof foot, "%s", a->agent);
    text(&font_s, r.x + 22, r.y + r.h - 38, foot, C_DIM, right);
}

static void render(int ox) {
    int shown = 0;
    for (int i = 0; i < MAX_AGENTS; i++)
        if (agents[i].set) draw_card(shift(card(shown++), ox), &agents[i]);
    if (shown) return;
    Rect a = shift(content_area(), ox);
    text_center(&font_m, (Rect){a.x, a.y + a.h / 2 - 40, a.w, 32},
                connected ? "No agent sessions" : "Waiting for the Mac", C_TEXT);
    text_center(&font_s, (Rect){a.x, a.y + a.h / 2 + 8, a.w, 24},
                "Claude Code and Codex sessions on the Mac appear here", C_DIM);
}

static int hit(int x, int y) { (void)x; (void)y; return -1; }
static void press(int i) { (void)i; }

const Page page_agents = {"Agents", render, hit, press};
