// MacDeck deck — the single userspace program of MacDeck OS (SM-T210, 1024x600 fb).
//
// Pages (swipe left/right, dots in the bottom bar):
//   0  Deck    — four metric tiles and six buttons, defined by the Mac host
//   1  Tablet  — battery, power, display and system info + local controls
// The bottom bar shows the status line, page dots and the tablet battery on every page.
//
// Power management (the backlight dominates consumption: ~-426 mA at 255, -98 mA at 80,
// +78 mA (charging) at 0 on a 250 mA USB port): configurable brightness, dim after idle,
// screen off after idle or when the Mac has been gone for a while, no rendering while
// off, first touch after dim/off only wakes, power/home keys toggle/wake, CPU governor.
//
// Mac link: TCP 127.0.0.1:8765 via adb forward, MacDeck protocol v1 (protocol/PROTOCOL.md).
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#ifndef DECK_PREVIEW
#include <linux/fb.h>
#include <linux/input.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font_spleen.h"

#define BUILD "deck-2.1"
#define PORT 8765
#define MAX_LINE 512
#define LINK_TIMEOUT 5.0
#define PAGES 2
#define TOP 56          // top bar height
#define BOTTOM 40       // bottom bar height
#define DIM_LEVEL 10    // backlight while dimmed

// ---------------------------------------------------------------- logging / time
static FILE *logf;
static void logmsg(const char *fmt, ...) {
    if (!logf) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(logf, "[%5ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }

// ---------------------------------------------------------------- sysfs helpers
static int read_str(const char *path, char *out, size_t cap) {
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) { out[0] = 0; return -1; }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    return 0;
}
static long read_long(const char *path, long fallback) {
    char b[32];
    return read_str(path, b, sizeof b) == 0 ? strtol(b, NULL, 10) : fallback;
}
static int write_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int ok = write(fd, val, strlen(val)) == (ssize_t)strlen(val);
    close(fd);
    return ok ? 0 : -1;
}

// ---------------------------------------------------------------- drawing
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fbmem;
static uint32_t *back;  // offscreen frame, W*H
static int W, H, fbfd = -1;
static int clip_x0, clip_x1;  // horizontal clip for sliding page content

static uint32_t rgb(uint32_t hex) {
    uint32_t r = hex >> 16 & 255, g = hex >> 8 & 255, b = hex & 255;
    return r << vi.red.offset | g << vi.green.offset | b << vi.blue.offset |
           (vi.transp.length ? 255u << vi.transp.offset : 0);
}
static void fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x, x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = back + (size_t)j * W;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}
static void frame(int x, int y, int w, int h, int t, uint32_t c) {
    fill(x, y, w, t, c); fill(x, y + h - t, w, t, c); fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}
static int text_w(const Font *f, const char *s) { return (int)strlen(s) * f->w; }
static void text(const Font *f, int x, int y, const char *s, uint32_t c, int maxx) {
    for (; *s; s++, x += f->w) {
        if (maxx > 0 && x + f->w > maxx) break;
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = f->data + (size_t)(ch - 32) * f->h * f->stride;
        for (int r = 0; r < f->h; r++) {
            int py = y + r;
            if (py < 0 || py >= H) continue;
            for (int b = 0; b < f->w; b++)
                if (g[r * f->stride + b / 8] & (0x80 >> (b % 8))) {
                    int px = x + b;
                    if (px >= clip_x0 && px < clip_x1) back[(size_t)py * W + px] = c;
                }
        }
    }
}
static void text_center(const Font *f, int x, int y, int w, const char *s, uint32_t c) {
    int tw = text_w(f, s);
    if (tw > w) tw = w;
    text(f, x + (w - tw) / 2, y, s, c, x + w);
}
static void present(void) {
    if (fi.line_length == (uint32_t)W * 4) memcpy(fbmem, back, (size_t)W * H * 4);
    else for (int y = 0; y < H; y++) memcpy(fbmem + (size_t)y * fi.line_length, back + (size_t)y * W, (size_t)W * 4);
}

#define C_BG 0x0f171e
#define C_PANEL 0x17222b
#define C_BAR 0x1d2a35
#define C_TEXT 0xe6edf2
#define C_DIM 0x97aab7
#define C_ACCENT 0x63dab4
#define C_WARN 0xf5a623
#define C_BAD 0xe6465a
#define C_LOCAL 0x2f4a5e   // tablet-page control buttons
#define C_INK 0x0b241d     // text on coloured buttons

// ---------------------------------------------------------------- state
typedef struct { char label[32], value[24], detail[40]; int level, set; } Tile;
typedef struct { char id[32], label[32]; uint32_t color; } Button;
static Tile tiles[4];
static Button buttons[6];
static char clock_time[16] = "--:--", clock_date[32] = "", status[128] = "Waiting for the Mac...";
static int connected = 0, dirty = 1, page = 0, pressed = -1;  // pressed = button slot on current page
static double last_rx = 0, status_at = 0, disconnected_at = 0;
static float drag = 0;  // horizontal page offset in pixels while swiping / animating

// Tablet info (sampled from sysfs)
typedef struct {
    int battery, current_ma, temp_dc, mv, freq_mhz, mem_free_mb, mem_total_mb;
    char status[24], health[16], governor[16];
    long uptime;
} Tablet;
static Tablet tab = {.battery = -1};

// Settings (persisted by the Mac host via SETTING messages)
static const int levels[] = {10, 30, 60, 80, 120, 160, 255};
static const int dim_choices[] = {15, 30, 60, 300, 0};     // seconds, 0 = never
static const int off_choices[] = {60, 300, 900, 1800, 0};
static int brightness = 80, dim_after = 60, off_after = 300, cpu_saver = 0;

// Screen state
enum { SCREEN_ON, SCREEN_DIM, SCREEN_OFF };
static int screen = SCREEN_ON;
static double last_activity = 0;

static void set_status(const char *s) { copy(status, sizeof status, s); status_at = now(); dirty = 1; }

// ---------------------------------------------------------------- layout
static void tile_rect(int i, int *x, int *y, int *w, int *h) { *x = 16 + (i % 2) * 254; *y = TOP + 16 + (i / 2) * 246; *w = 238; *h = 230; }
static void button_rect(int i, int *x, int *y, int *w, int *h) { *x = 540 + (i % 2) * 242; *y = TOP + 16 + (i / 2) * 164; *w = 226; *h = 148; }

// ---------------------------------------------------------------- tablet info / power
static void sample_tablet(void) {
    Tablet t = tab;
    const char *B = "/sys/class/power_supply/battery/";
    char p[96];
#define BP(f) (snprintf(p, sizeof p, "%s%s", B, f), p)
    t.battery = (int)read_long(BP("capacity"), -1);
    t.current_ma = (int)read_long(BP("current_now"), 0);
    t.temp_dc = (int)read_long(BP("temp"), 0);
    t.mv = (int)(read_long(BP("voltage_now"), 0) / 1000);
    read_str(BP("status"), t.status, sizeof t.status);
    read_str(BP("health"), t.health, sizeof t.health);
#undef BP
    t.freq_mhz = (int)(read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1000);
    read_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", t.governor, sizeof t.governor);
    char up[64];
    t.uptime = read_str("/proc/uptime", up, sizeof up) == 0 ? strtol(up, NULL, 10) : 0;
    FILE *m = fopen("/proc/meminfo", "r");
    if (m) {
        char line[128]; long total = 0, fr = 0, cached = 0, buffers = 0;
        while (fgets(line, sizeof line, m)) {
            sscanf(line, "MemTotal: %ld", &total); sscanf(line, "MemFree: %ld", &fr);
            sscanf(line, "Cached: %ld", &cached); sscanf(line, "Buffers: %ld", &buffers);
        }
        fclose(m);
        t.mem_total_mb = (int)(total / 1024); t.mem_free_mb = (int)((fr + cached + buffers) / 1024);
    }
    // only the visible fields trigger a redraw
    if (t.battery != tab.battery || strcmp(t.status, tab.status) ||
        (page == 1 && (t.current_ma != tab.current_ma || t.freq_mhz != tab.freq_mhz || t.uptime / 60 != tab.uptime / 60 ||
                       t.temp_dc / 5 != tab.temp_dc / 5 || t.mem_free_mb != tab.mem_free_mb)))
        dirty = 1;
    tab = t;
}

static void set_backlight(int level) {
    char b[8]; snprintf(b, sizeof b, "%d", level);
    if (write_str("/sys/class/backlight/panel/brightness", b) != 0) logmsg("backlight write failed: %s", strerror(errno));
}

static void apply_cpu(void) {
    const char *C = "/sys/devices/system/cpu/cpu0/cpufreq/";
    char p[96];
    // auto: interactive governor, full range. saver: conservative, capped at 624 MHz.
    snprintf(p, sizeof p, "%sscaling_governor", C);
    int ok = write_str(p, cpu_saver ? "conservative" : "interactive") == 0;
    snprintf(p, sizeof p, "%sscaling_max_freq", C);
    ok &= write_str(p, cpu_saver ? "624000" : "1205000") == 0;
    logmsg("cpu mode %s: %s", cpu_saver ? "saver" : "auto", ok ? "applied" : "not permitted (not root?)");
}

static void send_line(const char *fmt, ...);
static void set_screen(int s, const char *why) {
    if (s == screen) return;
    static const char *names[] = {"on", "dim", "off"};
    logmsg("screen %s -> %s (%s)", names[screen], names[s], why);
    if (s == SCREEN_OFF) {
        set_backlight(0);
    } else {
        if (screen == SCREEN_OFF) { dirty = 1; drag = 0; pressed = -1; }
        set_backlight(s == SCREEN_DIM ? (brightness < DIM_LEVEL ? brightness : DIM_LEVEL) : brightness);
    }
    screen = s;
    send_line("SCREEN|%s", names[s]);
}
static void activity(void) { last_activity = now(); }

// ---------------------------------------------------------------- settings
static const char *fmt_secs(int s, char *b, size_t cap) {
    if (s == 0) snprintf(b, cap, "never");
    else if (s < 60) snprintf(b, cap, "%ds", s);
    else snprintf(b, cap, "%dm", s / 60);
    return b;
}
static void apply_setting(const char *key, const char *val, int from_user) {
    int v = atoi(val);
    if (!strcmp(key, "brightness") && v >= 1 && v <= 255) {
        brightness = v;
        if (screen == SCREEN_ON) set_backlight(brightness);
    } else if (!strcmp(key, "dim") && v >= 0) dim_after = v;
    else if (!strcmp(key, "off") && v >= 0) off_after = v;
    else if (!strcmp(key, "cpu")) { cpu_saver = !strcmp(val, "saver"); apply_cpu(); }
    else return;
    dirty = 1;
    if (from_user) send_line("SETTING|%s|%s", key, val);
}
static void user_setting_int(const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); apply_setting(key, b, 1); }
static int next_choice(const int *c, int n, int cur, int dir) {
    int i = 0;
    while (i < n && c[i] != cur) i++;
    if (i == n) return c[0];
    i += dir;
    return c[i < 0 ? 0 : i >= n ? n - 1 : i];
}
static int cycle_choice(const int *c, int n, int cur) {
    for (int i = 0; i < n; i++) if (c[i] == cur) return c[(i + 1) % n];
    return c[0];
}

// Tablet page buttons (local actions; never sent to the Mac)
static void tablet_button_label(int i, char *b, size_t cap) {
    char t[16];
    switch (i) {
    case 0: snprintf(b, cap, "Dimmer"); break;
    case 1: snprintf(b, cap, "Brighter"); break;
    case 2: snprintf(b, cap, "Dim: %s", fmt_secs(dim_after, t, sizeof t)); break;
    case 3: snprintf(b, cap, "Off: %s", fmt_secs(off_after, t, sizeof t)); break;
    case 4: snprintf(b, cap, "CPU: %s", cpu_saver ? "saver" : "auto"); break;
    case 5: snprintf(b, cap, "Screen off"); break;
    }
}
static void tablet_button(int i) {
    char t[16], msg[64];
    switch (i) {
    case 0: case 1:
        user_setting_int("brightness", next_choice(levels, 7, brightness, i ? 1 : -1));
        snprintf(msg, sizeof msg, "Brightness %d", brightness); set_status(msg); break;
    case 2: user_setting_int("dim", cycle_choice(dim_choices, 5, dim_after));
        snprintf(msg, sizeof msg, "Dim after %s", fmt_secs(dim_after, t, sizeof t)); set_status(msg); break;
    case 3: user_setting_int("off", cycle_choice(off_choices, 5, off_after));
        snprintf(msg, sizeof msg, "Screen off after %s", fmt_secs(off_after, t, sizeof t)); set_status(msg); break;
    case 4: apply_setting("cpu", cpu_saver ? "auto" : "saver", 1);
        set_status(cpu_saver ? "CPU saver: conservative, max 624 MHz" : "CPU auto: interactive, up to 1205 MHz"); break;
    case 5: set_screen(SCREEN_OFF, "button"); break;
    }
}

// ---------------------------------------------------------------- rendering
static uint32_t level_color(int level, int inverted) {
    if (inverted) return level <= 15 ? C_BAD : level <= 30 ? C_WARN : C_ACCENT;
    return level >= 90 ? C_BAD : level >= 75 ? C_WARN : C_ACCENT;
}

static void draw_tile(int i, int ox, const char *label, const char *value, const char *detail, int level, int inverted, int live) {
    int x, y, w, h; tile_rect(i, &x, &y, &w, &h); x += ox;
    fill(x, y, w, h, rgb(C_PANEL));
    if (!label) { text(&font_s, x + 16, y + 14, "-", rgb(C_DIM), 0); return; }
    text(&font_s, x + 16, y + 14, label, rgb(C_DIM), x + w - 8);
    uint32_t vc = !live ? C_DIM : level >= 0 && level_color(level, inverted) != C_ACCENT ? level_color(level, inverted) : C_TEXT;
    text(&font_l, x + 16, y + 56, value, rgb(vc), x + w - 8);
    if (level >= 0) {
        int lv = level > 100 ? 100 : level;
        fill(x + 16, y + 140, w - 32, 10, rgb(C_BAR));
        fill(x + 16, y + 140, (w - 32) * lv / 100, 10, rgb(live ? level_color(level, inverted) : C_DIM));
    }
    text(&font_s, x + 16, y + 170, detail, rgb(C_DIM), x + w - 8);
}

static void draw_button(int i, int ox, const char *label, uint32_t color, int enabled) {
    int x, y, w, h; button_rect(i, &x, &y, &w, &h); x += ox;
    if (!label) { fill(x, y, w, h, rgb(C_PANEL)); return; }
    int down = i == pressed && ox == 0;
    fill(x, y, w, h, rgb(down ? 0xffffff : enabled ? color : 0x3a4650));
    uint32_t ink = down ? C_INK : !enabled ? C_DIM : color == C_LOCAL ? C_TEXT : C_INK;
    text_center(&font_m, x + 8, y + (h - font_m.h) / 2, w - 16, label, rgb(ink));
}

static void render_deck(int ox) {
    for (int i = 0; i < 4; i++) {
        Tile *t = &tiles[i];
        draw_tile(i, ox, t->set ? t->label : NULL, t->value, t->detail, t->level, 0, connected);
    }
    for (int i = 0; i < 6; i++)
        draw_button(i, ox, buttons[i].id[0] ? buttons[i].label : NULL, buttons[i].color, connected);
}

static void render_tablet(int ox) {
    char v[24], d[40], t[16];
    if (tab.battery >= 0) snprintf(v, sizeof v, "%d%%", tab.battery); else copy(v, sizeof v, "?");
    snprintf(d, sizeof d, "%s", tab.status[0] ? tab.status : "unknown");
    draw_tile(0, ox, "BATTERY", v, d, tab.battery, 1, 1);
    snprintf(v, sizeof v, "%+dmA", tab.current_ma);
    snprintf(d, sizeof d, "%d.%02dV %d.%dC", tab.mv / 1000, tab.mv % 1000 / 10, tab.temp_dc / 10, tab.temp_dc % 10);
    draw_tile(1, ox, "POWER", v, d, -1, 0, 1);
    snprintf(v, sizeof v, "%d", screen == SCREEN_ON ? brightness : 0);
    snprintf(d, sizeof d, "dim %s off ", fmt_secs(dim_after, t, sizeof t));
    fmt_secs(off_after, t, sizeof t); strncat(d, t, sizeof d - strlen(d) - 1);
    draw_tile(2, ox, "BRIGHTNESS", v, d, brightness * 100 / 255, 0, 1);
    snprintf(v, sizeof v, "%dMHz", tab.freq_mhz);
    snprintf(d, sizeof d, "up %ld:%02ld %dM free", tab.uptime / 3600, tab.uptime / 60 % 60, tab.mem_free_mb);
    draw_tile(3, ox, "CPU / SYSTEM", v, d, -1, 0, 1);
    for (int i = 0; i < 6; i++) {
        char label[32]; tablet_button_label(i, label, sizeof label);
        draw_button(i, ox, label, i == 5 ? C_BAD : C_LOCAL, 1);
    }
}

static void render_battery_badge(void) {
    // right-aligned to the same 20 px margin as the clock in the top bar
    char b[24];
    int charging = !strcmp(tab.status, "Charging") || !strcmp(tab.status, "Full");
    if (tab.battery >= 0) snprintf(b, sizeof b, "%d%%%s", tab.battery, charging ? " +" : "");
    else copy(b, sizeof b, "--");
    int tx = W - 20 - text_w(&font_s, b), x = tx - 12 - 37, y = H - BOTTOM + 12;
    uint32_t c = tab.battery < 0 ? C_DIM : level_color(tab.battery, 1);
    frame(x, y, 34, 16, 2, rgb(C_DIM));
    fill(x + 34, y + 5, 3, 6, rgb(C_DIM));
    if (tab.battery > 0) fill(x + 3, y + 3, 28 * (tab.battery > 100 ? 100 : tab.battery) / 100, 10, rgb(c));
    text(&font_s, tx, H - BOTTOM + 8, b, rgb(C_TEXT), W);
}

static void render(void) {
    clip_x0 = 0; clip_x1 = W;
    fill(0, 0, W, H, rgb(C_BG));
    // top bar
    fill(0, 0, W, TOP, rgb(C_BAR));
    text(&font_m, 20, 12, "MACDECK", rgb(C_ACCENT), 0);
    int lx = 20 + text_w(&font_m, "MACDECK") + 24;  // link indicator right after the title
    fill(lx, 22, 12, 12, rgb(connected ? C_ACCENT : C_BAD));
    text(&font_s, lx + 20, 16, connected ? "Mac connected" : "Mac not connected", rgb(connected ? C_TEXT : C_DIM), 0);
    text(&font_m, W - 20 - text_w(&font_m, clock_time), 12, clock_time, rgb(C_TEXT), 0);
    if (clock_date[0]) text(&font_s, W - 40 - text_w(&font_m, clock_time) - text_w(&font_s, clock_date), 16, clock_date, rgb(C_DIM), 0);
    // page content (slides with drag)
    int base = (int)drag;
    for (int p = 0; p < PAGES; p++) {
        int ox = (p - page) * W + base;
        if (ox <= -W || ox >= W) continue;
        if (p == 0) render_deck(ox); else render_tablet(ox);
    }
    // bottom bar: status, page dots, battery
    fill(0, H - BOTTOM, W, BOTTOM, rgb(C_BAR));
    text(&font_s, 20, H - BOTTOM + 8, status, rgb(now() - status_at < 4 ? C_TEXT : C_DIM), W / 2 - 60);
    for (int p = 0; p < PAGES; p++) {
        int cx = W / 2 - (PAGES - 1) * 12 + p * 24;
        fill(cx - 5, H - BOTTOM / 2 - 5, 10, 10, rgb(p == page ? C_ACCENT : 0x3a4650));
    }
    render_battery_badge();
    present();
    dirty = 0;
}

// ---------------------------------------------------------------- protocol
static int client = -1;
static char rxbuf[MAX_LINE * 2];
static size_t rxlen;

static void send_line(const char *fmt, ...) {
    if (client < 0) return;
    char line[MAX_LINE];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    if (send(client, line, n, MSG_NOSIGNAL) != n) logmsg("send failed: %s", strerror(errno));
}

static int split(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) if (*p == '|') { *p = 0; f[n++] = p + 1; }
    return n;
}
#define SET_IF_CHANGED(dst, src) do { if (strcmp(dst, src)) { copy(dst, sizeof dst, src); changed = 1; } } while (0)

static void handle(char *line) {
    char *f[8];
    int n = split(line, f, 8), changed = 0;
    if (!strcmp(f[0], "PING") && n >= 2) { send_line("PONG|%s", f[1]); return; }
    if (!strcmp(f[0], "CLOCK") && n >= 3) { SET_IF_CHANGED(clock_time, f[1]); SET_IF_CHANGED(clock_date, f[2]); }
    else if (!strcmp(f[0], "TILE") && n >= 6) {
        int s = atoi(f[1]);
        if (s < 0 || s > 3) return;
        Tile *t = &tiles[s];
        SET_IF_CHANGED(t->label, f[2]); SET_IF_CHANGED(t->value, f[3]); SET_IF_CHANGED(t->detail, f[4]);
        int lv = atoi(f[5]);
        if (lv != t->level || !t->set) { t->level = lv; t->set = 1; changed = 1; }
        if (page != 0) changed = 0;  // not visible
    } else if (!strcmp(f[0], "BUTTON") && n >= 5) {
        int s = atoi(f[1]);
        if (s < 0 || s > 5) return;
        copy(buttons[s].id, sizeof buttons[s].id, f[2]); copy(buttons[s].label, sizeof buttons[s].label, f[3]);
        buttons[s].color = (uint32_t)strtoul(f[4], NULL, 16);
        changed = 1;
    } else if (!strcmp(f[0], "STATUS") && n >= 2) { set_status(f[1]); return; }
    else if (!strcmp(f[0], "SETTING") && n >= 3) { apply_setting(f[1], f[2], 0); return; }
    else if (!strcmp(f[0], "WAKE")) { activity(); set_screen(SCREEN_ON, "mac"); return; }
    if (changed && screen != SCREEN_OFF) dirty = 1;
}

static void drop_client(const char *why) {
    if (client < 0) return;
    logmsg("client dropped: %s", why);
    close(client); client = -1; rxlen = 0;
    connected = 0; disconnected_at = now(); set_status("Mac disconnected - waiting...");
}

static void read_client(void) {
    char tmp[1024];
    ssize_t r = recv(client, tmp, sizeof tmp, 0);
    if (r <= 0) { drop_client(r == 0 ? "closed by peer" : strerror(errno)); return; }
    last_rx = now();
    for (ssize_t i = 0; i < r; i++) {
        char c = tmp[i];
        if (c == '\n') {
            rxbuf[rxlen] = 0;
            if (rxlen && rxbuf[rxlen - 1] == '\r') rxbuf[rxlen - 1] = 0;
            if (rxlen <= MAX_LINE) handle(rxbuf);
            rxlen = 0;
        } else if (rxlen < sizeof rxbuf - 1) rxbuf[rxlen++] = c;
        else rxlen = sizeof rxbuf;  // overlong: discard until newline
    }
}

// ---------------------------------------------------------------- input
#ifndef DECK_PREVIEW
// Find input devices by name, not number: in Samsung's low-power charging mode the touch
// driver is not registered and event0 becomes the power key.
static int open_input(const char *want, int retries) {
    for (int attempt = 0; attempt <= retries; attempt++) {
        for (int i = 0; i < 16; i++) {
            char path[32], name[64] = "";
            snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            if (strstr(name, want)) { logmsg("input %s: %s (%s)", want, path, name); return fd; }
            close(fd);
        }
        if (attempt < retries) usleep(100000);  // ueventd may still be creating /dev nodes
    }
    logmsg("input %s: not found", want);
    return -1;
}

// Bring the loopback interface up (stock Android's init.rc does `ifup lo`; without it
// 127.0.0.1 accepts binds but every connection fails). Needs root; harmless otherwise.
static void loopback_up(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    if (s >= 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP)) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        logmsg("lo was down, bringing it up: %s", ioctl(s, SIOCSIFFLAGS, &ifr) == 0 ? "ok" : strerror(errno));
    }
    if (s >= 0) close(s);
}

// Touch gesture state
static int tx = -1, ty = -1, down = 0, wasdown = 0, start_btn = -1, swiping = 0, swallow = 0;
static int down_x, down_y;
static double anim_from, anim_start;  // release animation of `drag` back to 0

static int hit_button(int sx, int sy) {
    for (int i = 0; i < 6; i++) {
        if (page == 0 && !buttons[i].id[0]) continue;
        int x, y, w, h; button_rect(i, &x, &y, &w, &h);
        if (sx >= x && sx < x + w && sy >= y && sy < y + h) return i;
    }
    return -1;
}

static void press(int i) {
    if (page == 1) { tablet_button(i); return; }
    logmsg("press %s", buttons[i].id);
    if (connected) { send_line("PRESS|%s", buttons[i].id); set_status("Sent to Mac..."); }
    else set_status("Mac not connected");
}

static void touch_frame(const char *mode, int xmax, int ymax) {
    int sx, sy;
    if (!strcmp(mode, "rot")) { sx = ty * (W - 1) / ymax; sy = (xmax - tx) * (H - 1) / xmax; }
    else if (!strcmp(mode, "rotinv")) { sx = (ymax - ty) * (W - 1) / ymax; sy = tx * (H - 1) / xmax; }
    else { sx = tx * (W - 1) / xmax; sy = ty * (H - 1) / ymax; }
    sx = sx < 0 ? 0 : sx > W - 1 ? W - 1 : sx; sy = sy < 0 ? 0 : sy > H - 1 ? H - 1 : sy;

    if (down && !wasdown) {  // finger down
        activity();
        if (screen != SCREEN_ON) { swallow = 1; set_screen(SCREEN_ON, "touch"); }  // wake only
        else {
            swallow = 0; swiping = 0; down_x = sx; down_y = sy; anim_start = 0;
            start_btn = hit_button(sx, sy);
            if (start_btn >= 0) { pressed = start_btn; dirty = 1; }
        }
    } else if (down && !swallow) {  // finger moving
        int dx = sx - down_x, dy = sy - down_y;
        if (!swiping && abs(dx) > 30 && abs(dx) > abs(dy)) { swiping = 1; pressed = -1; dirty = 1; }
        if (swiping) {
            float d = (float)dx;
            if ((page == 0 && d > 0) || (page == PAGES - 1 && d < 0)) d /= 4;  // rubber band at the ends
            drag = d; dirty = 1;
        } else {
            int show = hit_button(sx, sy) == start_btn ? start_btn : -1;
            if (show != pressed) { pressed = show; dirty = 1; }
        }
    } else if (!down && wasdown) {  // finger up
        if (!swallow) {
            if (swiping) {
                int target = page;
                if (drag < -W / 6 && page < PAGES - 1) target = page + 1;
                if (drag > W / 6 && page > 0) target = page - 1;
                if (target != page) { drag += (target - page) * W; page = target; }
                anim_from = drag; anim_start = now();
                logmsg("page %d", page);
            } else if (start_btn >= 0 && hit_button(sx, sy) == start_btn) press(start_btn);
            pressed = -1; dirty = 1;
        }
        start_btn = -1; swiping = 0; swallow = 0;
    }
    wasdown = down;
}

static void read_touch(int in, const char *mode, int xmax, int ymax) {
    struct input_event ev;
    while (read(in, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) tx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) ty = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) down = ev.value >= 0;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
        else if (ev.type == EV_SYN && ev.code == SYN_REPORT && tx >= 0 && ty >= 0) touch_frame(mode, xmax, ymax);
    }
}

static void read_keys(int fd) {  // power key toggles the screen; home/volume keys wake it
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY || ev.value != 1) continue;
        activity();
        if (ev.code == KEY_POWER && screen == SCREEN_ON) set_screen(SCREEN_OFF, "power key");
        else set_screen(SCREEN_ON, "key");
    }
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    logf = fopen(argc > 1 ? argv[1] : "/tmp/deck.log", "a");
    logmsg("%s starting, pid %d uid %d", BUILD, getpid(), getuid());
    {   // record how we were booted (normal vs charger/LPM) for diagnostics
        char cmd[2048] = "";
        read_str("/proc/cmdline", cmd, sizeof cmd);
        const char *m = strstr(cmd, "androidboot.mode="), *l = strstr(cmd, "lpm_boot=");
        logmsg("boot: %.40s %.12s recovery=%d", m ? m : "androidboot.mode=(none)", l ? l : "", strstr(cmd, "recovery_mode=1") != NULL);
    }
    set_backlight(brightness);
    apply_cpu();

    for (int i = 0; i < 50 && fbfd < 0; i++) { fbfd = open("/dev/graphics/fb0", O_RDWR); if (fbfd < 0) usleep(100000); }
    if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fi) || vi.bits_per_pixel != 32) {
        logmsg("fb0 unusable"); return 1;
    }
    W = vi.xres; H = vi.yres;
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    back = malloc((size_t)W * H * 4);
    if (fbmem == MAP_FAILED || !back) { logmsg("no memory"); return 1; }
    vi.yoffset = 0; vi.activate = FB_ACTIVATE_NOW;
    ioctl(fbfd, FBIOPAN_DISPLAY, &vi);
    ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
    logmsg("fb0 %dx%d line=%u", W, H, fi.line_length);

    int touch = open_input("touch", 50), onkey = open_input("88pm80x_on", 0), gpiokeys = open_input("gpio-keys", 0);
    // Touch coordinate mapping. Samsung's stock kernel reports the panel's native portrait
    // axes (x 0..598, y 0..1022) while fb0 is landscape 1024x600; TWRP's kernel reports
    // landscape. Detect from the axis ranges; MACDECK_TOUCH=none|rot|rotinv overrides.
    struct input_absinfo ax = {0}, ay = {0};
    if (touch >= 0) { ioctl(touch, EVIOCGABS(ABS_MT_POSITION_X), &ax); ioctl(touch, EVIOCGABS(ABS_MT_POSITION_Y), &ay); }
    int xmax = ax.maximum > 0 ? ax.maximum : W - 1, ymax = ay.maximum > 0 ? ay.maximum : H - 1;
    const char *mode = getenv("MACDECK_TOUCH");
    if (!mode) mode = (xmax < ymax) != (W < H) ? "rot" : "none";
    logmsg("touch x<=%d y<=%d mapping=%s", xmax, ymax, mode);

    loopback_up();
    int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    while (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { logmsg("bind: %s", strerror(errno)); sleep(1); }
    listen(srv, 2);
    logmsg("listening on 127.0.0.1:%d", PORT);

    double last_render = 0, last_sample = 0, last_ping_check = 0;
    activity(); disconnected_at = now();
    sample_tablet();
    for (;;) {
        double t = now();
        // ---- timers
        if (t - last_sample >= (screen == SCREEN_OFF ? 30 : 2)) { sample_tablet(); last_sample = t; }
        if (client >= 0 && t - last_ping_check >= 1) {
            last_ping_check = t;
            if (t - last_rx > LINK_TIMEOUT) drop_client("heartbeat timeout");
        }
        double idle = t - last_activity;
        if (screen == SCREEN_ON && dim_after && idle > dim_after) set_screen(SCREEN_DIM, "idle");
        if (screen != SCREEN_OFF && off_after && idle > off_after) set_screen(SCREEN_OFF, "idle");
        if (screen != SCREEN_OFF && !connected && t - disconnected_at > 120 && idle > 120) set_screen(SCREEN_OFF, "mac gone");
        if (status_at && t - status_at > 4) { dirty = 1; status_at = 0; }  // fade the status line
        if (anim_start) {  // settle the page slide
            double k = (t - anim_start) / 0.18;
            if (k >= 1) { drag = 0; anim_start = 0; }
            else drag = (float)(anim_from * (1 - k) * (1 - k));
            dirty = 1;
        }
        // ---- render (never while the screen is off)
        if (dirty && screen != SCREEN_OFF && t - last_render > 0.025) { render(); last_render = t; }

        // ---- wait for input: short timeout only while animating, otherwise up to 1 s
        int timeout = anim_start || (dirty && screen != SCREEN_OFF) ? 16 : 1000;
        struct pollfd p[5] = {{srv, POLLIN, 0}, {touch, POLLIN, 0}, {onkey, POLLIN, 0}, {gpiokeys, POLLIN, 0}, {client, POLLIN, 0}};
        poll(p, 5, timeout);  // negative fds are ignored by poll()
        if (p[0].revents & POLLIN) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                if (client >= 0) drop_client("replaced by new connection");
                client = c; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                connected = 1; last_rx = now(); rxlen = 0;
                logmsg("client connected");
                set_status("Mac connected");
                send_line("HELLO|macdeck/1|%s|%dx%d", BUILD, W, H);
                static const char *names[] = {"on", "dim", "off"};
                send_line("SCREEN|%s", names[screen]);
            }
        }
        if (client >= 0 && (p[4].revents & (POLLIN | POLLHUP | POLLERR))) read_client();
        if (touch >= 0 && (p[1].revents & POLLIN)) read_touch(touch, mode, xmax, ymax);
        if (onkey >= 0 && (p[2].revents & POLLIN)) read_keys(onkey);
        if (gpiokeys >= 0 && (p[3].revents & POLLIN)) read_keys(gpiokeys);
    }
}
#endif  // DECK_PREVIEW
