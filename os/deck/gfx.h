// Drawing layer: an offscreen 32-bpp frame copied to the framebuffer, primitives,
// text, and the reusable widgets every page is built from.
#pragma once
#include <stdint.h>

#include "font_spleen.h"

// Palette (0xRRGGBB; converted to the framebuffer's pixel format at draw time)
#define C_BG     0x0f171e
#define C_PANEL  0x17222b
#define C_BAR    0x1d2a35
#define C_TEXT   0xe6edf2
#define C_DIM    0x97aab7
#define C_MUTED  0x3a4650
#define C_ACCENT 0x63dab4
#define C_WARN   0xf5a623
#define C_BAD    0xe6465a
#define C_BLUE   0x4287f5
#define C_LOCAL  0x2f4a5e  // tablet-page control buttons
#define C_INK    0x0b241d  // text on coloured buttons

typedef struct { int x, y, w, h; } Rect;

// ---- frame
extern int W, H;
int  gfx_init_buffer(int w, int h, int r_off, int g_off, int b_off, int a_off, int a_len);
uint32_t *gfx_frame(void);            // the offscreen frame (W*H pixels)
void gfx_clip(int x0, int x1);        // horizontal clip for sliding page content
void gfx_noclip(void);

// ---- primitives
uint32_t mix(uint32_t a, uint32_t b, int pct);  // blend b over a, pct 0..100 (hex colours)
void fill(int x, int y, int w, int h, uint32_t hex);
void fill_r(Rect r, uint32_t hex);
void shade_r(Rect r, int pct);      // darken what is already drawn (scrim behind a sheet)
void frame_r(Rect r, int t, uint32_t hex);
void line(int x0, int y0, int x1, int y1, int t, uint32_t hex);
int  text_w(const Font *f, const char *s);
void text(const Font *f, int x, int y, const char *s, uint32_t hex, int maxx);
void text_center(const Font *f, Rect r, const char *s, uint32_t hex);
void text_right(const Font *f, int right, int y, const char *s, uint32_t hex);

// ---- layout
Rect inset(Rect r, int d);
Rect shift(Rect r, int dx);
Rect grid_cell(Rect area, int cols, int rows, int gap, int i);
int  in_rect(Rect r, int x, int y);

// ---- widgets
enum { LEVEL_NONE = -1 };
enum { TILE_INVERT = 1, TILE_STALE = 2 };   // invert: low level is bad (battery)
uint32_t level_color(int level, int invert);
void ui_tile(Rect r, const char *label, const char *value, const char *detail, int level, int flags);
enum { BTN_NORMAL, BTN_PRESSED, BTN_DISABLED };
void ui_button(Rect r, const char *label, uint32_t color, int state);
void ui_panel(Rect r, const char *label, const char *value, uint32_t value_color);
void ui_meter(Rect r, int level, uint32_t color);
// On/off switch: a track with a square knob; `busy` fades it while a change is in flight.
void ui_switch(Rect r, int on, int busy);
// ---- icons: pixel art, '#' = ink (icons.c)
typedef struct { int w, h; const char *rows[16]; } Icon;
extern const Icon icon_off, icon_person, icon_people;
void icon(const Icon *ic, int x, int y, int scale, uint32_t hex);
// Segmented control: n labelled choices with `sel` filled; `pressed` (or -1) is highlighted.
void ui_segments(Rect r, const char *const *labels, int n, int sel, int pressed, int busy);
// Area/line chart of up to 4 series (n samples each, oldest first) on a 0..max scale.
void ui_chart(Rect r, const float *const *series, const int *counts, const uint32_t *colors, int nseries, float max);
