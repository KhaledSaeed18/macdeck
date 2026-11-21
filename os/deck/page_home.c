// Home: the overview the deck returns to when idle — the four headline Mac metrics and
// the pinned shortcuts (buttons flagged "home" by the host, else the first four).
#include "deck.h"

static Rect left(void) { Rect a = content_area(); return (Rect){a.x, a.y, a.w / 2 - 8, a.h}; }
static Rect right(void) { Rect a = content_area(); return (Rect){a.x + a.w / 2 + 16, a.y, a.w / 2 - 16, a.h}; }

static void render(int ox) {
    for (int i = 0; i < MAX_TILES; i++) {
        Tile *t = &tiles[i];
        ui_tile(shift(grid_cell(left(), 2, 2, 16, i), ox), t->set ? t->label : NULL, t->value, t->detail,
                t->level, connected ? 0 : TILE_STALE);
    }
    for (int i = 0; i < HOME_BUTTONS; i++) draw_mac_button(shift(grid_cell(right(), 2, 2, 16, i), ox), home_button(i), i);
}

static int hit(int x, int y) {
    for (int i = 0; i < HOME_BUTTONS; i++)
        if (home_button(i) >= 0 && in_rect(grid_cell(right(), 2, 2, 16, i), x, y)) return i;
    return -1;
}

static void press(int i) { mac_button_press(home_button(i)); }

const Page page_home = {"Home", render, hit, press};
