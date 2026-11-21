// Power management and tablet readings.
//
// Measured on the SM-T210 over a 250 mA USB port, the backlight dominates consumption:
// 255 ≈ -426 mA, 160 ≈ -190 mA, 80 ≈ -98 mA, 30 ≈ -56 mA, off ≈ +78 mA (charging).
// Hence: brightness 80 by default, dim after idle, backlight off after idle or when the Mac
// has been gone a while, and nothing is rendered while off. Without Android the CPU
// governor is 'userspace' pinned at 1205 MHz, so it is set explicitly here.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "deck.h"

#define DIM_LEVEL 10                 // backlight while dimmed
#define MAC_GONE_OFF 120             // seconds without the Mac (and no touches) before screen off
#define BACKLIGHT "/sys/class/backlight/panel/brightness"
#define BATTERY "/sys/class/power_supply/battery/"
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

Tablet tab = {.battery = -1};
int brightness = 80, dim_after = 60, off_after = 300, cpu_saver = 0, screen = SCREEN_ON;
static double last_activity, last_sample, disconnected_since;
static int was_connected;

// ---------------------------------------------------------------- sysfs
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

void tablet_sample(void) {
    Tablet t = tab;
    t.battery = (int)read_long(BATTERY "capacity", -1);
    t.current_ma = (int)read_long(BATTERY "current_now", 0);
    t.temp_dc = (int)read_long(BATTERY "temp", 0);
    t.mv = (int)(read_long(BATTERY "voltage_now", 0) / 1000);
    read_str(BATTERY "status", t.status, sizeof t.status);
    t.freq_mhz = (int)(read_long(CPUFREQ "scaling_cur_freq", 0) / 1000);
    char up[64];
    t.uptime = read_str("/proc/uptime", up, sizeof up) == 0 ? strtol(up, NULL, 10) : 0;
    FILE *m = fopen("/proc/meminfo", "r");
    if (m) {
        char line[128];
        long total = 0, fr = 0, cached = 0, buffers = 0;
        while (fgets(line, sizeof line, m)) {
            sscanf(line, "MemTotal: %ld", &total); sscanf(line, "MemFree: %ld", &fr);
            sscanf(line, "Cached: %ld", &cached); sscanf(line, "Buffers: %ld", &buffers);
        }
        fclose(m);
        t.mem_total_mb = (int)(total / 1024);
        t.mem_free_mb = (int)((fr + cached + buffers) / 1024);
    }
    // Redraw only for visible changes: the battery badge is on every page, the rest on Tablet.
    int on_tablet = page_at(page) == &page_tablet;
    if (t.battery != tab.battery || strcmp(t.status, tab.status) ||
        (on_tablet && (t.current_ma != tab.current_ma || t.freq_mhz != tab.freq_mhz || t.uptime / 60 != tab.uptime / 60 ||
                       t.temp_dc / 5 != tab.temp_dc / 5 || t.mem_free_mb != tab.mem_free_mb)))
        dirty = 1;
    tab = t;
}

// ---------------------------------------------------------------- backlight / CPU
static void set_backlight(int level) {
    char b[8];
    snprintf(b, sizeof b, "%d", level);
    if (write_str(BACKLIGHT, b) != 0) logmsg("backlight write failed: %s", strerror(errno));
}

static void apply_cpu(void) {
    // auto: interactive governor, full range. saver: conservative, capped at 624 MHz.
    int ok = write_str(CPUFREQ "scaling_governor", cpu_saver ? "conservative" : "interactive") == 0;
    ok &= write_str(CPUFREQ "scaling_max_freq", cpu_saver ? "624000" : "1205000") == 0;
    logmsg("cpu mode %s: %s", cpu_saver ? "saver" : "auto", ok ? "applied" : "not permitted (not root?)");
}

// ---------------------------------------------------------------- screen
const char *screen_name(int s) { static const char *n[] = {"on", "dim", "off"}; return n[s]; }

void set_screen(int s, const char *why) {
    if (s == screen) return;
    logmsg("screen %s -> %s (%s)", screen_name(screen), screen_name(s), why);
    if (s == SCREEN_OFF) set_backlight(0);
    else {
        if (screen == SCREEN_OFF) { dirty = 1; drag = 0; pressed = -1; }
        set_backlight(s == SCREEN_DIM && brightness > DIM_LEVEL ? DIM_LEVEL : brightness);
    }
    if (s != SCREEN_ON) picker_open = 0;
    if (s != SCREEN_ON && page != 0) { page = 0; drag = 0; pressed = -1; dirty = 1; }  // idle: back to Home
    screen = s;
    link_send("SCREEN|%s", screen_name(s));
}

void power_activity(void) { last_activity = now(); }

void power_init(void) {
    set_backlight(brightness);
    apply_cpu();
    last_activity = disconnected_since = now();
    tablet_sample();
}

void power_tick(double t) {
    if (t - last_sample >= (screen == SCREEN_OFF ? 30 : 2)) { tablet_sample(); last_sample = t; }
    if (connected != was_connected) { was_connected = connected; if (!connected) disconnected_since = t; }
    double idle = t - last_activity;
    if (screen == SCREEN_ON && dim_after && idle > dim_after) set_screen(SCREEN_DIM, "idle");
    if (screen != SCREEN_OFF && off_after && idle > off_after) set_screen(SCREEN_OFF, "idle");
    if (screen != SCREEN_OFF && !connected && t - disconnected_since > MAC_GONE_OFF && idle > MAC_GONE_OFF)
        set_screen(SCREEN_OFF, "mac gone");
}

// ---------------------------------------------------------------- settings
const char *fmt_secs(int s, char *b, size_t cap) {
    if (s == 0) snprintf(b, cap, "never");
    else if (s < 60) snprintf(b, cap, "%ds", s);
    else snprintf(b, cap, "%dm", s / 60);
    return b;
}

void apply_setting(const char *key, const char *val, int from_user) {
    int v = atoi(val);
    if (!strcmp(key, "brightness") && v >= 1 && v <= 255) {
        brightness = v;
        if (screen == SCREEN_ON) set_backlight(brightness);
    } else if (!strcmp(key, "dim") && v >= 0) dim_after = v;
    else if (!strcmp(key, "off") && v >= 0) off_after = v;
    else if (!strcmp(key, "cpu")) { cpu_saver = !strcmp(val, "saver"); apply_cpu(); }
    else return;
    dirty = 1;
    if (from_user) link_send("SETTING|%s|%s", key, val);
}
