// Spike: draw directly on the SM-T210 framebuffer and read the touchscreen, with no Android.
// Usage: fbtouch [seconds]   (default 30)
// Prints framebuffer + touch info, draws four "buttons", then for N seconds paints a dot
// at every touch and reports which button was hit. Coordinates are in raw panel space.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
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

static uint32_t rgb(int r, int g, int b) {
    return ((uint32_t)r >> (8 - vi.red.length)) << vi.red.offset |
           ((uint32_t)g >> (8 - vi.green.length)) << vi.green.offset |
           ((uint32_t)b >> (8 - vi.blue.length)) << vi.blue.offset |
           (vi.transp.length ? (0xffu >> (8 - vi.transp.length)) << vi.transp.offset : 0);
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    int bpp = vi.bits_per_pixel / 8;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= (int)vi.yres) continue;
        uint8_t *row = fbmem + (size_t)(j + vi.yoffset) * fi.line_length;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= (int)vi.xres) continue;
            if (bpp == 4) ((uint32_t *)row)[i] = c;
            else if (bpp == 2) ((uint16_t *)row)[i] = (uint16_t)c;
        }
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    int fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open fb0"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi)) { perror("ioctl fb"); return 1; }
    printf("FB id=%s xres=%u yres=%u virt=%ux%u off=%u,%u bpp=%u line_length=%u smem_len=%u\n",
           fi.id, vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual, vi.xoffset, vi.yoffset,
           vi.bits_per_pixel, fi.line_length, fi.smem_len);
    printf("FB rgba offsets r=%u/%u g=%u/%u b=%u/%u a=%u/%u  physical %umm x %umm\n",
           vi.red.offset, vi.red.length, vi.green.offset, vi.green.length, vi.blue.offset, vi.blue.length,
           vi.transp.offset, vi.transp.length, vi.width, vi.height);
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED) { perror("mmap"); return 1; }

    // Draw into the buffer currently on screen, then also show buffer 0.
    int W = vi.xres, H = vi.yres;
    struct { int x, y, w, h, r, g, b; const char *name; } btn[4] = {
        {W / 16, H / 12, W * 3 / 8, H / 3, 99, 218, 180, "TL-green"},
        {W * 9 / 16, H / 12, W * 3 / 8, H / 3, 66, 135, 245, "TR-blue"},
        {W / 16, H * 7 / 12, W * 3 / 8, H / 3, 245, 166, 35, "BL-orange"},
        {W * 9 / 16, H * 7 / 12, W * 3 / 8, H / 3, 230, 70, 90, "BR-red"},
    };
    for (int pass = 0; pass < 2; pass++) {
        rect(0, 0, W, H, rgb(15, 23, 30));
        for (int i = 0; i < 4; i++) rect(btn[i].x, btn[i].y, btn[i].w, btn[i].h, rgb(btn[i].r, btn[i].g, btn[i].b));
        if (pass == 0 && vi.yoffset != 0) { vi.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &vi); } else break;
    }
    ioctl(fd, FBIOPAN_DISPLAY, &vi);
    printf("DRAWN background + 4 buttons\n");

    int in = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (in < 0) { perror("open event0"); return 1; }
    char name[64] = "";
    ioctl(in, EVIOCGNAME(sizeof name), name);
    struct input_absinfo ax = {0}, ay = {0};
    ioctl(in, EVIOCGABS(ABS_MT_POSITION_X), &ax);
    ioctl(in, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
    printf("TOUCH %s x=%d..%d y=%d..%d\n", name, ax.minimum, ax.maximum, ay.minimum, ay.maximum);
    fflush(stdout);

    int x = -1, y = -1, down = 0, touches = 0;
    double end = now() + seconds;
    while (now() < end) {
        struct pollfd p = {in, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        struct input_event ev;
        while (read(in, &ev, sizeof ev) == sizeof ev) {
            if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) x = ev.value;
            else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) y = ev.value;
            else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) down = ev.value >= 0;
            else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
            else if (ev.type == EV_SYN && ev.code == SYN_REPORT && down && x >= 0 && y >= 0) {
                int sx = ax.maximum > 0 ? x * (W - 1) / ax.maximum : x;
                int sy = ay.maximum > 0 ? y * (H - 1) / ay.maximum : y;
                rect(sx - 6, sy - 6, 12, 12, rgb(255, 255, 255));
                const char *hit = "none";
                for (int i = 0; i < 4; i++)
                    if (sx >= btn[i].x && sx < btn[i].x + btn[i].w && sy >= btn[i].y && sy < btn[i].y + btn[i].h) hit = btn[i].name;
                if (touches++ % 5 == 0) { printf("TOUCH raw=%d,%d screen=%d,%d button=%s\n", x, y, sx, sy, hit); fflush(stdout); }
            }
        }
    }
    printf("DONE touch reports=%d\n", touches);
    return 0;
}
