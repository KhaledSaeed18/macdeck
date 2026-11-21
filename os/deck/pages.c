// Page registry, the chrome shared by every page (top bar, bottom bar) and the carousel
// that composes the current page with its neighbour while swiping.
#include <stdio.h>
#include <string.h>

#include "deck.h"

static const Page *const registry[] = {&page_home, &page_agents, &page_stats, &page_shortcuts, &page_mac, &page_tablet};
#define NPAGES (int)(sizeof registry / sizeof registry[0])

int page = 0, pressed = -1, picker_open = 0;
float drag = 0;
static int drawing;  // index of the page currently being drawn

int page_count(void) { return NPAGES; }
const Page *page_at(int i) { return registry[i]; }
Rect content_area(void) { return (Rect){16, TOP + 16, W - 32, H - TOP - BOTTOM - 32}; }

int button_state(int i, int enabled) {
    if (drawing == page && i == pressed && drag == 0) return BTN_PRESSED;
    return enabled ? BTN_NORMAL : BTN_DISABLED;
}

// ---------------------------------------------------------------- Mac buttons (Home + Shortcuts)
void draw_mac_button(Rect r, int slot, int ctrl) {
    if (slot < 0 || !buttons[slot].id[0]) { ui_button(r, NULL, 0, BTN_NORMAL); return; }
    const Button *b = &buttons[slot];
    ui_button_icon(r, b->label, b->has_icon ? b->icon : NULL, b->color, button_state(ctrl, connected));
}

void mac_button_press(int slot) {
    if (slot < 0 || !buttons[slot].id[0]) return;
    logmsg("press %s", buttons[slot].id);
    if (!connected) { toast("Mac not connected", 8); return; }
    link_send("PRESS|%s", buttons[slot].id);
    toast("Sent to Mac...", 4);
}

// ---------------------------------------------------------------- agent sessions (Home + Agents)
const char *agent_state_name(int st) { return st == AGENT_WAITING ? "NEEDS YOU" : st == AGENT_WORKING ? "WORKING" : "IDLE"; }
uint32_t agent_state_color(int st) {
    if (!connected) return C_DIM;
    return st == AGENT_WAITING ? C_WARN : st == AGENT_WORKING ? C_ACCENT : C_DIM;
}

void agent_focus(int slot) {
    if (slot < 0 || !agents[slot].set) return;
    if (!connected) { toast("Mac not connected", 8); return; }
    if (!agents[slot].id[0]) { toast("Update the Mac host to open sessions", 8); return; }
    logmsg("focus %s", agents[slot].id);
    link_send("FOCUS|%s", agents[slot].id);
    toast("Opening on the Mac...", 4);
}

void page_show(const Page *p) {
    for (int i = 0; i < NPAGES; i++)
        if (registry[i] == p) { page = i; drag = 0; pressed = -1; picker_open = 0; dirty = 1; logmsg("page %d (%s)", i, p->title); }
}

// ---------------------------------------------------------------- page picker
// The squares' touch zone is much larger than the squares, and reaches a little above the bar.
static Rect squares_zone(void) { return (Rect){W / 2 - 110, H - BOTTOM - 12, 220, BOTTOM + 12}; }
// Up to five pages sit in one row of tall tiles (number above title); more wrap into rows of
// shorter tiles with the number beside the title.
#define PICK_COLS (NPAGES <= 5 ? NPAGES : (NPAGES + 1) / 2)
#define PICK_ROWS ((NPAGES + PICK_COLS - 1) / PICK_COLS)
#define PICK_TILE_H (PICK_ROWS == 1 ? 168 : 96)
static Rect sheet(void) {
    int h = 48 + PICK_ROWS * PICK_TILE_H + (PICK_ROWS - 1) * 16 + 16;
    return (Rect){16, H - BOTTOM - 16 - h, W - 32, h};
}
static Rect pick_tile(int i) { Rect s = sheet(); return grid_cell((Rect){s.x + 16, s.y + 48, s.w - 32, s.h - 64}, PICK_COLS, PICK_ROWS, 16, i); }

int chrome_hit(int x, int y) {
    if (in_rect(squares_zone(), x, y)) return CTRL_SQUARES;
    if (!picker_open) return -1;
    for (int i = 0; i < NPAGES; i++)
        if (in_rect(pick_tile(i), x, y)) return CTRL_PICK + i;
    return CTRL_PICK_CLOSE;  // anywhere else while the picker is open closes it
}

void chrome_press(int c) {
    if (c == CTRL_SQUARES) picker_open = !picker_open;
    else if (c == CTRL_PICK_CLOSE) picker_open = 0;
    else if (c >= CTRL_PICK && c < CTRL_PICK + NPAGES) page_show(registry[c - CTRL_PICK]);
    if (c < CTRL_PICK) logmsg("picker %s", picker_open ? "open" : "closed");  // page jumps log in page_show
    dirty = 1;
}

static void draw_picker(void) {
    shade_r((Rect){0, TOP, W, H - TOP - BOTTOM}, 35);  // the page stays visible, dimmed
    Rect s = sheet();
    fill_r(s, C_BAR);
    frame_r(s, 2, C_MUTED);
    text(&font_s, s.x + 16, s.y + 14, "GO TO PAGE", C_DIM, 0);
    text_right(&font_s, s.x + s.w - 16, s.y + 14, "tap outside to close", C_DIM);
    for (int i = 0; i < NPAGES; i++) {
        Rect t = pick_tile(i);
        int cur = i == page;
        uint32_t ink = cur ? C_ACCENT : C_TEXT;
        fill_r(t, pressed == CTRL_PICK + i ? mix(C_PANEL, C_TEXT, 18) : C_PANEL);
        if (cur) frame_r(t, 3, C_ACCENT);
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        if (PICK_ROWS == 1) {
            text(&font_l, t.x + 16, t.y + 12, num, ink, 0);
            text(&font_m, t.x + 16, t.y + t.h - 48, registry[i]->title, ink, t.x + t.w - 8);
        } else {
            text(&font_l, t.x + 20, t.y + (t.h - 64) / 2, num, ink, 0);
            text(&font_m, t.x + 80, t.y + (t.h - 32) / 2, registry[i]->title, ink, t.x + t.w - 8);
        }
        if (registry[i] == &page_agents && connected && agent_counts.waiting > 0) {  // same amber as the badge
            snprintf(num, sizeof num, "%d", agent_counts.waiting);
            int w = text_w(&font_s, num) + 16;
            fill(t.x + t.w - 12 - w, t.y + 12, w, 28, C_WARN);
            text(&font_s, t.x + t.w - 12 - w + 8, t.y + 14, num, C_INK, 0);
        }
    }
}


// ---------------------------------------------------------------- chrome
static void needs_you_badge(int right) {  // amber pill while any agent session waits for the user
    if (!connected || agent_counts.waiting <= 0) return;
    char b[24];
    snprintf(b, sizeof b, "%d NEEDS YOU", agent_counts.waiting);
    int w = text_w(&font_s, b) + 20;
    fill(right - w, 14, w, 28, C_WARN);
    text(&font_s, right - w + 10, 16, b, C_INK, 0);
}

static void top_bar(void) {
    fill(0, 0, W, TOP, C_BAR);
    text(&font_m, 20, 12, "MACDECK", C_ACCENT, 0);
    int lx = 20 + text_w(&font_m, "MACDECK") + 24;
    fill(lx, 22, 12, 12, connected ? C_ACCENT : C_BAD);
    text(&font_s, lx + 20, 16, connected ? "Mac connected" : "Mac not connected", connected ? C_TEXT : C_DIM, 0);
    const char *title = registry[page]->title;
    text(&font_s, (W - text_w(&font_s, title)) / 2, 16, title, C_DIM, 0);
    text_right(&font_m, W - 20, 12, clock_time, C_TEXT);
    int date_x = W - 40 - text_w(&font_m, clock_time);
    if (clock_date[0]) { text_right(&font_s, date_x, 16, clock_date, C_DIM); date_x -= text_w(&font_s, clock_date); }
    needs_you_badge(date_x - 20);
}

static void battery_badge(void) {  // right-aligned to the same margin as the clock
    char b[24];
    int charging = !strcmp(tab.status, "Charging") || !strcmp(tab.status, "Full");
    if (tab.battery >= 0) snprintf(b, sizeof b, "%d%%%s", tab.battery, charging ? " +" : "");
    else copy(b, sizeof b, "--");
    int tx = W - 20 - text_w(&font_s, b), y = H - BOTTOM + 12;
    Rect body = {tx - 12 - 37, y, 34, 16};
    frame_r(body, 2, C_DIM);
    fill(body.x + 34, y + 5, 3, 6, C_DIM);
    if (tab.battery > 0) ui_meter((Rect){body.x + 3, y + 3, 28, 10}, tab.battery, level_color(tab.battery, 1));
    text(&font_s, tx, H - BOTTOM + 8, b, C_TEXT, 0);
}

static void bottom_bar(void) {
    fill(0, H - BOTTOM, W, BOTTOM, C_BAR);
    const char *t = toast_text();
    if (t) text(&font_s, 20, H - BOTTOM + 8, t, C_TEXT, W / 2 - 80);
    else if (!connected) text(&font_s, 20, H - BOTTOM + 8, "Waiting for the Mac...", C_DIM, W / 2 - 80);
    // page squares: a faint pill behind them marks them as a control (brighter while pressed or open)
    uint32_t pill = mix(C_BAR, C_TEXT, pressed == CTRL_SQUARES ? 22 : picker_open ? 14 : 5);
    fill(W / 2 - (NPAGES - 1) * 12 - 18, H - BOTTOM + 6, (NPAGES - 1) * 24 + 36, BOTTOM - 12, pill);
    for (int p = 0; p < NPAGES; p++)  // other squares stay a step brighter than the pill behind them
        fill(W / 2 - (NPAGES - 1) * 12 + p * 24 - 5, H - BOTTOM / 2 - 5, 10, 10, p == page ? C_ACCENT : mix(pill, C_TEXT, 20));
    battery_badge();
}

void pages_render(void) {
    gfx_noclip();
    fill(0, 0, W, H, C_BG);
    top_bar();
    for (int k = -1; k <= 1; k++) {  // the page and its neighbours, wrapping around the ends
        int ox = k * W + (int)drag;
        if (ox <= -W || ox >= W) continue;
        drawing = (page + k + NPAGES) % NPAGES;
        registry[drawing]->render(ox);
    }
    gfx_noclip();
    if (picker_open) draw_picker();
    bottom_bar();
    present();
    dirty = 0;
}
