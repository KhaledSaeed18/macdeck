// Agents: the AI coding-agent sessions running on the Mac, one card each, sorted by the host:
// waiting for the user first, then working, then idle. Tapping a card brings that session's
// app or terminal to the front on the Mac. With more sessions than cards, the last card
// summarises the rest ("+N more").
#include <stdio.h>

#include "deck.h"

static Rect card(int i) { return grid_cell(content_area(), 2, 3, 16, i); }

static void draw_card(Rect r, const Agent *a, int pressed_now) {
    uint32_t c = agent_state_color(a->state);
    int right = r.x + r.w - 16;
    fill_r(r, pressed_now ? mix(C_PANEL, C_TEXT, 18) : C_PANEL);
    fill(r.x, r.y, 6, r.h, a->state == AGENT_IDLE ? C_MUTED : c);
    if (a->state == AGENT_WAITING && connected) frame_r(r, 2, C_WARN);
    text(&font_s, r.x + 22, r.y + 14, agent_state_name(a->state), c, 0);
    text_right(&font_s, right, r.y + 14, a->since, C_DIM);
    text(&font_m, r.x + 22, r.y + 46, a->title, connected ? C_TEXT : C_DIM, right);
    char foot[96];
    if (a->state == AGENT_WAITING && a->detail[0]) snprintf(foot, sizeof foot, "%s: %s", a->agent, a->detail);
    else if (a->where[0]) snprintf(foot, sizeof foot, "%s / %s", a->agent, a->where);
    else snprintf(foot, sizeof foot, "%s", a->agent);
    text(&font_s, r.x + 22, r.y + r.h - 38, foot, C_DIM, right);
}

static void draw_more(Rect r, int shown) {  // the sessions that didn't fit, by state
    int st[3] = {agent_counts.idle, agent_counts.working, agent_counts.waiting};
    for (int i = 0; i < shown; i++) st[agents[agent_slot(i)].state]--;
    char head[24], foot[64];
    snprintf(head, sizeof head, "+%d more", agent_counts.total - shown);
    snprintf(foot, sizeof foot, "%d working, %d idle", st[AGENT_WORKING] > 0 ? st[AGENT_WORKING] : 0,
             st[AGENT_IDLE] > 0 ? st[AGENT_IDLE] : 0);
    fill_r(r, mix(C_PANEL, C_BG, 50));
    text(&font_m, r.x + 22, r.y + 46, head, C_DIM, 0);
    text(&font_s, r.x + 22, r.y + r.h - 38, foot, C_DIM, r.x + r.w - 16);
}

static int shown_count(void) { int n = 0; while (agent_slot(n) >= 0) n++; return n; }

static void render(int ox) {
    int shown = shown_count();
    for (int i = 0; i < shown; i++)
        draw_card(shift(card(i), ox), &agents[agent_slot(i)], button_state(i, 1) == BTN_PRESSED);
    if (shown && agent_counts.total > shown && shown < MAX_AGENTS) draw_more(shift(card(shown), ox), shown);
    if (shown) return;
    Rect a = shift(content_area(), ox);
    text_center(&font_m, (Rect){a.x, a.y + a.h / 2 - 40, a.w, 32},
                connected ? "No agent sessions" : "Waiting for the Mac", C_TEXT);
    text_center(&font_s, (Rect){a.x, a.y + a.h / 2 + 8, a.w, 24},
                "Claude Code, Codex, opencode, Copilot and Qwen sessions appear here", C_DIM);
}

static int hit(int x, int y) {
    for (int i = 0; agent_slot(i) >= 0; i++)
        if (in_rect(card(i), x, y)) return i;
    return -1;
}

static void press(int i) { agent_focus(agent_slot(i)); }

const Page page_agents = {"Agents", render, hit, press};
