// Mac: a control centre for the Mac, like the controls in the macOS menu bar.
//
//   +--------------------------------+ +--------------------------------+
//   | MAC BATTERY     Power adapter  | | Wi-Fi                    [  o] |
//   | 87%                            | | Connected, 192.168.1.20        |
//   | ==========================     | +--------------------------------+
//   | Charging, 0:42 to full         | | AirDrop  [Off|Contacts|Everyone]|
//   | Health 96%  197 cycles         | +---------------+----------------+
//   +--------------------------------+ | Keep Awake    | Dark Mode      |
//   | SOUND                      50% | | [o  ]  Off    | [  o]  Dark    |
//   | [=========|                  ] | +---------------+----------------+
//   | [ Mute ] [ Quieter ] [ Louder ]| |          Lock Screen           |
//   +--------------------------------+ +--------------------------------+
//
// Tapping the volume bar sets the volume where it was tapped (16 steps, like the volume keys).
// Every control asks the Mac through mac_set() (MACSET, host/src/mac.ts); the page shows the
// change at once and fades it until the Mac confirms. All inner geometry is derived from the
// panel rectangles, so the same functions serve drawing (shifted while swiping) and hit tests.
#include <stdio.h>

#include "deck.h"

#define STEPS 16                    // volume steps, as on the Mac's keyboard
enum {                              // control indices
    C_VOL0 = 0,                     // C_VOL0 + 0..STEPS: a step of the volume bar
    C_MUTE = C_VOL0 + STEPS + 1, C_DOWN, C_UP, C_WIFI,
    C_AIRDROP0,                     // + AIRDROP_*: a segment
    C_AWAKE = C_AIRDROP0 + 3, C_DARK, C_LOCK,
};

static int live(void) { return connected && mac.have; }
static int busy(int k) { return mac.pending[k] && now() < mac.pending[k]; }
static int is_pressed(int c) { return button_state(c, live()) == BTN_PRESSED; }

// ---------------------------------------------------------------- layout (unshifted; ox added when drawing)
static Rect col(int right) { Rect a = content_area(); int w = (a.w - 16) / 2; return (Rect){a.x + right * (w + 16), a.y, w, a.h}; }
static Rect battery_panel(void) { return grid_cell(col(0), 1, 2, 16, 0); }
static Rect sound_panel(void) { return grid_cell(col(0), 1, 2, 16, 1); }
static Rect row(int i) { return grid_cell(col(1), 1, 4, 16, i); }      // right column, 4 rows
static Rect half(int i) { return grid_cell(row(2), 2, 1, 16, i); }     // row 2 holds two half tiles

static Rect vol_bar(Rect s) { return (Rect){s.x + 16, s.y + 64, s.w - 32, 40}; }
static Rect sound_button(Rect s, int i) { return grid_cell((Rect){s.x + 16, s.y + 124, s.w - 32, 76}, 3, 1, 16, i); }
static Rect airdrop_segs(Rect t) { return (Rect){t.x + 140, t.y + (t.h - 52) / 2, t.w - 152, 52}; }
static Rect airdrop_seg(Rect t, int i) { return grid_cell(inset(airdrop_segs(t), 4), 3, 1, 4, i); }

static int step_volume(int step) { return (step * 100 + STEPS / 2) / STEPS; }
static int volume_step(int vol) { return (vol * STEPS + 50) / 100; }

// ---------------------------------------------------------------- drawing
static uint32_t ink(void) { return live() ? C_TEXT : C_DIM; }
static void tile_bg(Rect r, int ctrl) { fill_r(r, is_pressed(ctrl) ? mix(C_PANEL, C_TEXT, 14) : C_PANEL); }

static void draw_battery(Rect r) {
    fill_r(r, C_PANEL);
    text(&font_s, r.x + 16, r.y + 14, "MAC BATTERY", C_DIM, 0);
    if (!mac.have) { text(&font_l, r.x + 16, r.y + 44, "-", C_DIM, 0); return; }
    text_right(&font_s, r.x + r.w - 16, r.y + 14, mac.batt_source, C_DIM);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", mac.batt_pct);
    uint32_t bar = !live() ? C_DIM : mac.batt_charging ? C_ACCENT : level_color(mac.batt_pct, 1);
    uint32_t num = !live() ? C_DIM : mac.batt_charging || bar != C_ACCENT ? bar : C_TEXT;  // plain white when fine
    text(&font_l, r.x + 16, r.y + 44, pct, num, 0);
    ui_meter((Rect){r.x + 16, r.y + 120, r.w - 32, 16}, mac.batt_pct, bar);
    text(&font_s, r.x + 16, r.y + 150, mac.batt_line, mac.batt_charging && live() ? C_ACCENT : ink(), r.x + r.w - 16);
    text(&font_s, r.x + 16, r.y + 184, mac.batt_detail, C_DIM, r.x + r.w - 16);
}

static void draw_sound(Rect r) {
    fill_r(r, C_PANEL);
    text(&font_s, r.x + 16, r.y + 14, "SOUND", C_DIM, 0);
    int muted = mac.val[MAC_MUTE], vol = mac.val[MAC_VOLUME];
    char v[16];
    snprintf(v, sizeof v, "%d%%", vol);
    if (mac.have) text_right(&font_m, r.x + r.w - 16, r.y + 10, muted ? "Muted" : v, !live() ? C_DIM : muted ? C_WARN : C_TEXT);
    // the bar fills to the volume; while a step is pressed it previews that step instead
    Rect b = vol_bar(r);
    fill_r(b, C_BAR);
    int preview = pressed >= C_VOL0 && pressed <= C_VOL0 + STEPS && is_pressed(pressed);
    int shown = preview ? step_volume(pressed - C_VOL0) : vol;
    uint32_t c = !live() || muted ? C_MUTED : preview ? mix(C_ACCENT, C_TEXT, 35) : C_ACCENT;
    if (busy(MAC_VOLUME) && !preview) c = mix(c, C_BAR, 35);
    int fw = b.w * shown / 100;
    fill(b.x, b.y, fw, b.h, c);
    fill(b.x + (fw >= 6 ? fw - 6 : 0), b.y - 4, 6, b.h + 8, live() ? C_TEXT : C_DIM);  // knob
    const char *labels[3] = {muted ? "Unmute" : "Mute", "Quieter", "Louder"};
    for (int i = 0; i < 3; i++)
        ui_button(sound_button(r, i), labels[i], i == 0 && muted ? C_WARN : C_LOCAL, button_state(C_MUTE + i, live()));
}

static void draw_wifi(Rect r) {
    tile_bg(r, C_WIFI);
    text(&font_m, r.x + 20, r.y + 16, "Wi-Fi", ink(), 0);
    text(&font_s, r.x + 20, r.y + 62, mac.have ? mac.wifi_detail : "-", C_DIM, r.x + r.w - 116);
    ui_switch((Rect){r.x + r.w - 20 - 76, r.y + (r.h - 38) / 2, 76, 38}, mac.val[MAC_WIFI], busy(MAC_WIFI) || !live());
}

static void draw_airdrop(Rect r) {
    static const char *const modes[3] = {"Off", "Contacts", "Everyone"};
    fill_r(r, C_PANEL);
    text(&font_m, r.x + 20, r.y + (r.h - 32) / 2, "AirDrop", ink(), 0);
    int pressed_seg = -1;
    for (int i = 0; i < 3; i++) if (is_pressed(C_AIRDROP0 + i)) pressed_seg = i;
    ui_segments(airdrop_segs(r), modes, 3, mac.val[MAC_AIRDROP], pressed_seg, busy(MAC_AIRDROP) || !live());
}

static void draw_half(Rect r, int ctrl, const char *title, const char *detail, int key) {
    tile_bg(r, ctrl);
    text(&font_m, r.x + 16, r.y + 12, title, ink(), r.x + r.w - 8);
    ui_switch((Rect){r.x + 16, r.y + 56, 64, 34}, mac.val[key], busy(key) || !live());
    text(&font_s, r.x + 96, r.y + 61, detail, C_DIM, r.x + r.w - 8);
}

static void render(int ox) {
    draw_battery(shift(battery_panel(), ox));
    draw_sound(shift(sound_panel(), ox));
    draw_wifi(shift(row(0), ox));
    draw_airdrop(shift(row(1), ox));
    draw_half(shift(half(0), ox), C_AWAKE, "Keep Awake", mac.val[MAC_AWAKE] && mac.awake_detail[0] ? mac.awake_detail : mac.val[MAC_AWAKE] ? "On" : "Off", MAC_AWAKE);
    draw_half(shift(half(1), ox), C_DARK, "Dark Mode", mac.val[MAC_DARK] ? "Dark" : "Light", MAC_DARK);
    ui_button(shift(row(3), ox), "Lock Screen", C_LOCAL, button_state(C_LOCK, live()));
}

// ---------------------------------------------------------------- talking to the Mac
static const char *const mac_keys[MAC_NKEYS] = {"wifi", "airdrop", "awake", "dark", "volume", "mute"};
static const char *const airdrop_names[3] = {"off", "contacts", "everyone"};

void mac_set(int k, int v) {
    char b[16];
    if (k == MAC_AIRDROP) copy(b, sizeof b, airdrop_names[v]);
    else if (k == MAC_VOLUME) snprintf(b, sizeof b, "%d", v);
    else copy(b, sizeof b, v ? "on" : "off");
    link_send("MACSET|%s|%s", mac_keys[k], b);
    logmsg("mac %s=%s", mac_keys[k], b);
    mac.val[k] = v; mac.pending[k] = now() + 4;
    if (k == MAC_VOLUME) { mac.val[MAC_MUTE] = 0; mac.pending[MAC_MUTE] = now() + 4; }  // setting a volume unmutes
    dirty = 1;
}

// ---------------------------------------------------------------- touch
static int hit(int x, int y) {
    Rect s = sound_panel(), b = vol_bar(s);
    if (in_rect((Rect){b.x - 8, b.y - 12, b.w + 16, b.h + 24}, x, y)) {  // a little extra margin for fingers
        int step = ((x - b.x) * STEPS + b.w / 2) / b.w;
        return C_VOL0 + (step < 0 ? 0 : step > STEPS ? STEPS : step);
    }
    for (int i = 0; i < 3; i++) if (in_rect(sound_button(s, i), x, y)) return C_MUTE + i;
    if (in_rect(row(0), x, y)) return C_WIFI;
    for (int i = 0; i < 3; i++) if (in_rect(airdrop_seg(row(1), i), x, y)) return C_AIRDROP0 + i;
    if (in_rect(half(0), x, y)) return C_AWAKE;
    if (in_rect(half(1), x, y)) return C_DARK;
    if (in_rect(row(3), x, y)) return C_LOCK;
    return -1;
}

static void press(int i) {
    if (!live()) { toast(connected ? "Waiting for the Mac..." : "Mac not connected", 4); return; }
    int vol = mac.val[MAC_VOLUME];
    if (i >= C_VOL0 && i <= C_VOL0 + STEPS) mac_set(MAC_VOLUME, step_volume(i - C_VOL0));
    else if (i == C_MUTE) mac_set(MAC_MUTE, !mac.val[MAC_MUTE]);
    else if (i == C_DOWN) mac_set(MAC_VOLUME, step_volume(volume_step(vol) > 0 ? volume_step(vol) - 1 : 0));
    else if (i == C_UP) mac_set(MAC_VOLUME, step_volume(volume_step(vol) < STEPS ? volume_step(vol) + 1 : STEPS));
    else if (i == C_WIFI) mac_set(MAC_WIFI, !mac.val[MAC_WIFI]);
    else if (i >= C_AIRDROP0 && i < C_AIRDROP0 + 3) mac_set(MAC_AIRDROP, i - C_AIRDROP0);
    else if (i == C_AWAKE) mac_set(MAC_AWAKE, !mac.val[MAC_AWAKE]);
    else if (i == C_DARK) mac_set(MAC_DARK, !mac.val[MAC_DARK]);
    else if (i == C_LOCK) { link_send("MACSET|lock|now"); toast("Locking the Mac...", 4); }
}

const Page page_mac = {"Mac", render, hit, press};
