// MacDeck OS v0 — first on-device "deck" process. Started by init; no Android framework.
// Draws a dark background, four buttons and an animated heartbeat bar straight to fb0,
// reads the touchscreen, and lights up a button while it is pressed. Runs forever.
// Logs to /tmp/deck0.log (tmpfs) so it can be inspected over adb.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fbmem;
static int W, H;
static FILE *logf;

static void logmsg(const char *fmt, ...) {
    if (!logf) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(logf, "[%5ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}

static uint32_t rgb(int r, int g, int b) {
    return ((uint32_t)r >> (8 - vi.red.length)) << vi.red.offset |
           ((uint32_t)g >> (8 - vi.green.length)) << vi.green.offset |
           ((uint32_t)b >> (8 - vi.blue.length)) << vi.blue.offset |
           (vi.transp.length ? (0xffu >> (8 - vi.transp.length)) << vi.transp.offset : 0);
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = (uint32_t *)(fbmem + (size_t)j * fi.line_length);
        for (int i = x; i < x + w; i++) row[i] = c;
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { if (write(fd, val, strlen(val)) < 0) {} close(fd); }
}

typedef struct { int x, y, w, h, r, g, b; const char *name; } Button;
static Button btn[4];

static void draw_button(int i, int pressed) {
    Button *b = &btn[i];
    int r = pressed ? 255 : b->r, g = pressed ? 255 : b->g, bl = pressed ? 255 : b->b;
    rect(b->x, b->y, b->w, b->h, rgb(r, g, bl));
}

int main(void) {
    logf = fopen("/tmp/deck0.log", "a");
    logmsg("deck0 starting, pid %d", getpid());
    write_file("/sys/class/backlight/panel/brightness", "160");

    int fd = -1;
    for (int i = 0; i < 50 && fd < 0; i++) {  // ueventd may still be creating nodes
        fd = open("/dev/graphics/fb0", O_RDWR);
        if (fd < 0) usleep(100000);
    }
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi)) { logmsg("fb0 unavailable"); return 1; }
    W = vi.xres; H = vi.yres;
    logmsg("fb0 %s %dx%d bpp=%d line=%d virt=%dx%d", fi.id, W, H, vi.bits_per_pixel, fi.line_length, vi.xres_virtual, vi.yres_virtual);
    if (vi.bits_per_pixel != 32) { logmsg("unsupported bpp"); return 1; }
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED) { logmsg("mmap failed"); return 1; }
    vi.yoffset = 0; vi.activate = FB_ACTIVATE_NOW;
    ioctl(fd, FBIOPAN_DISPLAY, &vi);
    ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);

    int top = 56, gap = 24, bw = (W - 3 * gap) / 2, bh = (H - top - 3 * gap) / 2;
    Button init[4] = {
        {gap, top + gap, bw, bh, 99, 218, 180, "green"},
        {2 * gap + bw, top + gap, bw, bh, 66, 135, 245, "blue"},
        {gap, top + 2 * gap + bh, bw, bh, 245, 166, 35, "orange"},
        {2 * gap + bw, top + 2 * gap + bh, bw, bh, 230, 70, 90, "red"},
    };
    memcpy(btn, init, sizeof btn);
    rect(0, 0, W, H, rgb(15, 23, 30));
    for (int i = 0; i < 4; i++) draw_button(i, 0);

    int in = -1;
    for (int i = 0; i < 50 && in < 0; i++) {
        in = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
        if (in < 0) usleep(100000);
    }
    struct input_absinfo ax = {0}, ay = {0};
    if (in >= 0) { ioctl(in, EVIOCGABS(ABS_MT_POSITION_X), &ax); ioctl(in, EVIOCGABS(ABS_MT_POSITION_Y), &ay); }
    logmsg("touch fd=%d x<=%d y<=%d", in, ax.maximum, ay.maximum);

    int x = -1, y = -1, down = 0, wasdown = 0, pressed = -1, taps = 0;
    double start = now(), last_beat = 0;
    for (;;) {
        double t = now();
        if (t - last_beat > 0.05) {  // heartbeat: a block sweeping across the top bar
            last_beat = t;
            rect(0, 0, W, top, rgb(23, 34, 43));
            int pos = (int)((t - start) * 300) % (W + 120) - 120;
            rect(pos, 16, 120, top - 32, rgb(99, 218, 180));
            for (int i = 0; i < taps % 40; i++) rect(W - 12 - i * 14, top - 10, 8, 6, rgb(183, 199, 210));  // tap counter dots
        }
        struct pollfd p = {in, POLLIN, 0};
        if (in < 0 || poll(&p, 1, 30) <= 0) continue;
        struct input_event ev;
        while (read(in, &ev, sizeof ev) == sizeof ev) {
            if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) x = ev.value;
            else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) y = ev.value;
            else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) down = ev.value >= 0;
            else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
            else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                int sx = x > W - 1 ? W - 1 : x, sy = y > H - 1 ? H - 1 : y;
                int hit = -1;
                if (down) for (int i = 0; i < 4; i++)
                    if (sx >= btn[i].x && sx < btn[i].x + btn[i].w && sy >= btn[i].y && sy < btn[i].y + btn[i].h) hit = i;
                if (hit != pressed) {
                    if (pressed >= 0) draw_button(pressed, 0);
                    if (hit >= 0) draw_button(hit, 1);
                    pressed = hit;
                }
                if (down && !wasdown) { taps++; logmsg("tap %d at %d,%d button=%s", taps, sx, sy, hit >= 0 ? btn[hit].name : "none"); }
                wasdown = down;
            }
        }
    }
}
