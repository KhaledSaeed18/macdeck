// Touchscreen gestures (tap, press highlight, horizontal swipe between pages) and keys.
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "deck.h"

#define SWIPE_START 30     // px of horizontal travel before a touch becomes a swipe
#define SETTLE_SECS 0.18

static int touch_fd = -1, key_fds[2] = {-1, -1};
static int xmax, ymax;
static enum { MAP_NONE, MAP_ROT, MAP_ROTINV } mapping;

// Gesture state
static int tx = -1, ty = -1, down, wasdown, start_ctrl = -1, swiping, swallow, down_x, down_y;
static double settle_from, settle_start;

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

void input_init(void) {
    touch_fd = open_input("touch", 50);
    key_fds[0] = open_input("88pm80x_on", 0);  // power key
    key_fds[1] = open_input("gpio-keys", 0);   // home / volume
    // Samsung's stock kernel reports the panel's native portrait axes (x 0..598, y 0..1022)
    // while fb0 is landscape 1024x600; TWRP's kernel reports landscape. Detect from the axis
    // ranges; MACDECK_TOUCH=none|rot|rotinv overrides.
    struct input_absinfo ax = {0}, ay = {0};
    if (touch_fd >= 0) { ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &ax); ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay); }
    xmax = ax.maximum > 0 ? ax.maximum : W - 1;
    ymax = ay.maximum > 0 ? ay.maximum : H - 1;
    const char *m = getenv("MACDECK_TOUCH");
    mapping = m ? (!strcmp(m, "rot") ? MAP_ROT : !strcmp(m, "rotinv") ? MAP_ROTINV : MAP_NONE)
                : (xmax < ymax) != (W < H) ? MAP_ROT : MAP_NONE;
    logmsg("touch x<=%d y<=%d mapping=%d", xmax, ymax, mapping);
}

int input_pollfds(struct pollfd *p) {
    int fds[3] = {touch_fd, key_fds[0], key_fds[1]};
    for (int i = 0; i < 3; i++) p[i] = (struct pollfd){fds[i], POLLIN, 0};  // poll() ignores fd -1
    return 3;
}

// ---------------------------------------------------------------- gestures
static void to_screen(int *sx, int *sy) {
    int x, y;
    switch (mapping) {
    case MAP_ROT: x = ty * (W - 1) / ymax; y = (xmax - tx) * (H - 1) / xmax; break;
    case MAP_ROTINV: x = (ymax - ty) * (W - 1) / ymax; y = tx * (H - 1) / xmax; break;
    default: x = tx * (W - 1) / xmax; y = ty * (H - 1) / ymax;
    }
    *sx = x < 0 ? 0 : x > W - 1 ? W - 1 : x;
    *sy = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
}

// Control under a point: chrome (page squares, the picker) first; while the picker is open
// nothing on the page underneath reacts.
static int ctrl_at(int x, int y) {
    int c = chrome_hit(x, y);
    return c >= 0 || picker_open ? c : page_at(page)->hit(x, y);
}

static void finger_down(int sx, int sy) {
    power_activity();
    if (screen != SCREEN_ON) { swallow = 1; set_screen(SCREEN_ON, "touch"); return; }  // wake only
    swallow = 0; swiping = 0; down_x = sx; down_y = sy; settle_start = 0;
    start_ctrl = ctrl_at(sx, sy);
    if (start_ctrl >= 0) { pressed = start_ctrl; dirty = 1; }
}

static void finger_move(int sx, int sy) {
    int dx = sx - down_x, dy = sy - down_y;
    if (!swiping && !picker_open && abs(dx) > SWIPE_START && abs(dx) > abs(dy)) { swiping = 1; pressed = -1; dirty = 1; }
    if (swiping) {
        drag = (float)dx; dirty = 1;  // pages wrap around, so there is no end to resist at
        return;
    }
    int show = ctrl_at(sx, sy) == start_ctrl ? start_ctrl : -1;
    if (show != pressed) { pressed = show; dirty = 1; }
}

static void finger_up(int sx, int sy) {
    if (swiping) {
        int step = drag < -W / 6 ? 1 : drag > W / 6 ? -1 : 0;  // past the last page comes Home, and back
        drag += step * W;
        page = (page + step + page_count()) % page_count();
        settle_from = drag; settle_start = now();
        logmsg("page %d (%s)", page, page_at(page)->title);
    } else if (start_ctrl >= 0 && ctrl_at(sx, sy) == start_ctrl) {
        if (start_ctrl >= CHROME_CTRL) chrome_press(start_ctrl);
        else page_at(page)->press(start_ctrl);
    }
    pressed = -1; dirty = 1;
}

static void touch_frame(void) {
    int sx, sy;
    to_screen(&sx, &sy);
    if (down && !wasdown) finger_down(sx, sy);
    else if (down && !swallow) finger_move(sx, sy);
    else if (!down && wasdown) { if (!swallow) finger_up(sx, sy); start_ctrl = -1; swiping = 0; swallow = 0; }
    wasdown = down;
}

static void read_touch(void) {
    struct input_event ev;
    while (read(touch_fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) tx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) ty = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) down = ev.value >= 0;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
        else if (ev.type == EV_SYN && ev.code == SYN_REPORT && tx >= 0 && ty >= 0) touch_frame();
    }
}

static void read_keys(int fd) {  // power key toggles the screen; home/volume keys wake it
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY || ev.value != 1) continue;
        power_activity();
        if (ev.code == KEY_POWER && screen == SCREEN_ON) set_screen(SCREEN_OFF, "power key");
        else set_screen(SCREEN_ON, "key");
    }
}

void input_handle(const struct pollfd *p, int n) {
    if (n > 0 && p[0].fd >= 0 && (p[0].revents & POLLIN)) read_touch();
    for (int i = 1; i < n; i++)
        if (p[i].fd >= 0 && (p[i].revents & POLLIN)) read_keys(p[i].fd);
}

int input_tick(double t) {  // ease the page back into place after a swipe
    if (!settle_start) return 0;
    double k = (t - settle_start) / SETTLE_SECS;
    if (k >= 1) { drag = 0; settle_start = 0; }
    else drag = (float)(settle_from * (1 - k) * (1 - k));
    dirty = 1;
    return settle_start != 0;
}
