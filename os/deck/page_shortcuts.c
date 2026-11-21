// Shortcuts: every button defined by the host, in a 4x3 grid (slots 0..11).
#include "deck.h"

#define COLS 4
#define ROWS 3

static Rect cell(int i) { return grid_cell(content_area(), COLS, ROWS, 16, i); }

static void render(int ox) {
    for (int i = 0; i < MAX_BUTTONS; i++) draw_mac_button(shift(cell(i), ox), i, i);
}

static int hit(int x, int y) {
    for (int i = 0; i < MAX_BUTTONS; i++)
        if (buttons[i].id[0] && in_rect(cell(i), x, y)) return i;
    return -1;
}

static void press(int i) { mac_button_press(i); }

const Page page_shortcuts = {"Shortcuts", render, hit, press};
