// MacDeck deck — the single userspace program of MacDeck OS (SM-T210, 1024x600 fb).
// Renders a top bar (title, link state, clock), four metric tiles, six buttons and a
// status line, all defined by the Mac host over the MacDeck protocol v1
// (protocol/PROTOCOL.md). Listens on 127.0.0.1:8765; the Mac connects via adb forward.
// Draws into an offscreen buffer and copies it to fb0 only when something changed.
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

#define BUILD "deck-1.2"
#define PORT 8765
#define MAX_LINE 512
#define LINK_TIMEOUT 5.0

// ---------------------------------------------------------------- logging
static FILE *logf;
static void logmsg(const char *fmt, ...) {
    if (!logf) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(logf, "[%5ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// ---------------------------------------------------------------- drawing
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fbmem;
static uint32_t *back;  // offscreen frame, W*H
static int W, H, fbfd = -1;

static uint32_t rgb(uint32_t hex) {
    uint32_t r = hex >> 16 & 255, g = hex >> 8 & 255, b = hex & 255;
    return r << vi.red.offset | g << vi.green.offset | b << vi.blue.offset |
           (vi.transp.length ? 255u << vi.transp.offset : 0);
}
static void fill(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = back + (size_t)(y + j) * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}
static int text_w(const Font *f, const char *s) { return (int)strlen(s) * f->w; }
static void text(const Font *f, int x, int y, const char *s, uint32_t c, int maxw) {
    for (; *s; s++, x += f->w) {
        if (maxw > 0 && x + f->w > maxw) break;
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = f->data + (size_t)(ch - 32) * f->h * f->stride;
        for (int r = 0; r < f->h; r++) {
            int py = y + r;
            if (py < 0 || py >= H) continue;
            for (int b = 0; b < f->w; b++)
                if (g[r * f->stride + b / 8] & (0x80 >> (b % 8))) {
                    int px = x + b;
                    if (px >= 0 && px < W) back[(size_t)py * W + px] = c;
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

// ---------------------------------------------------------------- state
typedef struct { char label[32], value[24], detail[40]; int level, set; } Tile;
typedef struct { char id[32], label[32]; uint32_t color; int x, y, w, h; } Button;
static Tile tiles[4];
static Button buttons[6];
static char clock_time[16] = "--:--", clock_date[32] = "", status[128] = "Waiting for the Mac...";
static int connected = 0, pressed = -1, dirty = 1;
static double last_rx = 0, status_at = 0;

#define C_BG 0x0f171e
#define C_PANEL 0x17222b
#define C_BAR 0x1d2a35
#define C_TEXT 0xe6edf2
#define C_DIM 0x97aab7
#define C_ACCENT 0x63dab4
#define C_WARN 0xf5a623
#define C_BAD 0xe6465a

static void layout(void) {
    int bx = 540, by = 72, bw = 226, bh = 148, gap = 16;
    for (int i = 0; i < 6; i++) {
        buttons[i].x = bx + (i % 2) * (bw + gap);
        buttons[i].y = by + (i / 2) * (bh + gap);
        buttons[i].w = bw; buttons[i].h = bh;
    }
}

static void render(void) {
    fill(0, 0, W, H, rgb(C_BG));
    // top bar
    fill(0, 0, W, 56, rgb(C_BAR));
    text(&font_m, 20, 12, "MACDECK", rgb(C_ACCENT), 0);
    const char *link = connected ? "Mac connected" : "Mac not connected";
    fill(200, 22, 12, 12, rgb(connected ? C_ACCENT : C_BAD));
    text(&font_s, 220, 16, link, rgb(connected ? C_TEXT : C_DIM), 0);
    text(&font_m, W - 20 - text_w(&font_m, clock_time), 12, clock_time, rgb(C_TEXT), 0);
    if (clock_date[0]) text(&font_s, W - 40 - text_w(&font_m, clock_time) - text_w(&font_s, clock_date), 16, clock_date, rgb(C_DIM), 0);
    // tiles (2x2, left side)
    for (int i = 0; i < 4; i++) {
        int x = 16 + (i % 2) * 254, y = 72 + (i / 2) * 246, w = 238, h = 230;
        fill(x, y, w, h, rgb(C_PANEL));
        Tile *t = &tiles[i];
        if (!t->set) { text(&font_s, x + 16, y + 14, "-", rgb(C_DIM), 0); continue; }
        text(&font_s, x + 16, y + 14, t->label, rgb(C_DIM), x + w - 8);
        uint32_t vc = !connected ? C_DIM : t->level >= 90 ? C_BAD : t->level >= 75 ? C_WARN : C_TEXT;
        text(&font_l, x + 16, y + 56, t->value, rgb(vc), x + w - 8);
        if (t->level >= 0) {
            fill(x + 16, y + 140, w - 32, 10, rgb(C_BAR));
            int lv = t->level > 100 ? 100 : t->level;
            fill(x + 16, y + 140, (w - 32) * lv / 100, 10, rgb(connected ? (t->level >= 90 ? C_BAD : t->level >= 75 ? C_WARN : C_ACCENT) : C_DIM));
        }
        text(&font_s, x + 16, y + 170, t->detail, rgb(C_DIM), x + w - 8);
    }
    // buttons (2x3, right side)
    for (int i = 0; i < 6; i++) {
        Button *b = &buttons[i];
        if (!b->id[0]) { fill(b->x, b->y, b->w, b->h, rgb(C_PANEL)); continue; }
        uint32_t c = i == pressed ? 0xffffff : connected ? b->color : 0x3a4650;
        fill(b->x, b->y, b->w, b->h, rgb(c));
        text_center(&font_m, b->x + 8, b->y + (b->h - font_m.h) / 2, b->w - 16, b->label, rgb(i == pressed || connected ? 0x0b241d : C_DIM));
    }
    // status bar
    fill(0, H - 40, W, 40, rgb(C_BAR));
    text(&font_s, 20, H - 32, status, rgb(now() - status_at < 4 ? C_TEXT : C_DIM), W - 20);
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
static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }

static void handle(char *line) {
    char *f[8];
    int n = split(line, f, 8);
    if (!strcmp(f[0], "PING") && n >= 2) { send_line("PONG|%s", f[1]); return; }
    if (!strcmp(f[0], "CLOCK") && n >= 3) { copy(clock_time, sizeof clock_time, f[1]); copy(clock_date, sizeof clock_date, f[2]); }
    else if (!strcmp(f[0], "TILE") && n >= 6) {
        int s = atoi(f[1]);
        if (s < 0 || s > 3) return;
        copy(tiles[s].label, sizeof tiles[s].label, f[2]); copy(tiles[s].value, sizeof tiles[s].value, f[3]);
        copy(tiles[s].detail, sizeof tiles[s].detail, f[4]); tiles[s].level = atoi(f[5]); tiles[s].set = 1;
    } else if (!strcmp(f[0], "BUTTON") && n >= 5) {
        int s = atoi(f[1]);
        if (s < 0 || s > 5) return;
        copy(buttons[s].id, sizeof buttons[s].id, f[2]); copy(buttons[s].label, sizeof buttons[s].label, f[3]);
        buttons[s].color = (uint32_t)strtoul(f[4], NULL, 16);
    } else if (!strcmp(f[0], "STATUS") && n >= 2) { copy(status, sizeof status, f[1]); status_at = now(); }
    else return;
    dirty = 1;
}

static void drop_client(const char *why) {
    if (client < 0) return;
    logmsg("client dropped: %s", why);
    close(client); client = -1; rxlen = 0;
    connected = 0; copy(status, sizeof status, "Mac disconnected - waiting..."); status_at = now(); dirty = 1;
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

// ---------------------------------------------------------------- main
#ifndef DECK_PREVIEW
static void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { if (write(fd, val, strlen(val)) < 0) {} close(fd); }
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

static int open_retry(const char *path, int flags) {
    for (int i = 0; i < 50; i++) {  // ueventd may still be creating /dev nodes
        int fd = open(path, flags);
        if (fd >= 0) return fd;
        usleep(100000);
    }
    return -1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    logf = fopen(argc > 1 ? argv[1] : "/tmp/deck.log", "a");
    logmsg("%s starting, pid %d uid %d", BUILD, getpid(), getuid());
    write_file("/sys/class/backlight/panel/brightness", "160");

    {   // record how we were booted (normal vs charger/LPM) for diagnostics
        char cmd[2048] = ""; FILE *c = fopen("/proc/cmdline", "r");
        if (c) { size_t n = fread(cmd, 1, sizeof cmd - 1, c); cmd[n] = 0; fclose(c); }
        const char *m = strstr(cmd, "androidboot.mode="), *l = strstr(cmd, "lpm_boot=");
        logmsg("boot: %.40s %.12s recovery=%d", m ? m : "androidboot.mode=(none)", l ? l : "", strstr(cmd, "recovery_mode=1") != NULL);
    }

    fbfd = open_retry("/dev/graphics/fb0", O_RDWR);
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
    layout();

    int in = open_retry("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    // Touch coordinate mapping. Samsung's stock kernel reports the panel's native portrait
    // axes (x 0..598, y 0..1022) while fb0 is landscape 1024x600; TWRP's kernel reports
    // landscape. Detect from the axis ranges; MACDECK_TOUCH=none|rot|rotinv overrides.
    struct input_absinfo ax = {0}, ay = {0};
    if (in >= 0) { ioctl(in, EVIOCGABS(ABS_MT_POSITION_X), &ax); ioctl(in, EVIOCGABS(ABS_MT_POSITION_Y), &ay); }
    int xmax = ax.maximum > 0 ? ax.maximum : W - 1, ymax = ay.maximum > 0 ? ay.maximum : H - 1;
    const char *mode = getenv("MACDECK_TOUCH");
    if (!mode) mode = (xmax < ymax) != (W < H) ? "rot" : "none";
    logmsg("touch fd %d x<=%d y<=%d mapping=%s", in, xmax, ymax, mode);

    loopback_up();
    int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    while (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { logmsg("bind: %s", strerror(errno)); sleep(1); }
    listen(srv, 2);
    logmsg("listening on 127.0.0.1:%d", PORT);

    int tx = -1, ty = -1, down = 0, wasdown = 0, start_btn = -1;
    double last_render = 0;
    for (;;) {
        struct pollfd p[3] = {{srv, POLLIN, 0}, {in, POLLIN, 0}, {client, POLLIN, 0}};
        poll(p, client >= 0 ? 3 : 2, 100);
        if (p[0].revents & POLLIN) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                if (client >= 0) drop_client("replaced by new connection");
                client = c; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                connected = 1; last_rx = now(); rxlen = 0; dirty = 1;
                copy(status, sizeof status, "Mac connected"); status_at = now();
                logmsg("client connected");
                send_line("HELLO|macdeck/1|%s|%dx%d", BUILD, W, H);
            }
        }
        if (client >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) read_client();
        if (client >= 0 && now() - last_rx > LINK_TIMEOUT) drop_client("heartbeat timeout");
        if (in >= 0 && (p[1].revents & POLLIN)) {
            struct input_event ev;
            while (read(in, &ev, sizeof ev) == sizeof ev) {
                if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) tx = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) ty = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) down = ev.value >= 0;
                else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
                else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    int sx, sy, hit = -1;
                    if (!strcmp(mode, "rot")) { sx = ty * (W - 1) / ymax; sy = (xmax - tx) * (H - 1) / xmax; }
                    else if (!strcmp(mode, "rotinv")) { sx = (ymax - ty) * (W - 1) / ymax; sy = tx * (H - 1) / xmax; }
                    else { sx = tx * (W - 1) / xmax; sy = ty * (H - 1) / ymax; }
                    sx = sx < 0 ? 0 : sx > W - 1 ? W - 1 : sx; sy = sy < 0 ? 0 : sy > H - 1 ? H - 1 : sy;
                    for (int i = 0; i < 6; i++) {
                        Button *b = &buttons[i];
                        if (b->id[0] && sx >= b->x && sx < b->x + b->w && sy >= b->y && sy < b->y + b->h) hit = i;
                    }
                    if (down && !wasdown) { start_btn = hit; logmsg("touch raw=%d,%d screen=%d,%d button=%d", tx, ty, sx, sy, hit); }
                    int show = down && hit == start_btn ? hit : -1;
                    if (show != pressed) { pressed = show; dirty = 1; }
                    if (!down && wasdown) {
                        if (hit >= 0 && hit == start_btn) {
                            logmsg("press %s", buttons[hit].id);
                            if (connected) { send_line("PRESS|%s", buttons[hit].id); copy(status, sizeof status, "Sent to Mac..."); }
                            else copy(status, sizeof status, "Mac not connected");
                            status_at = now();
                        }
                        start_btn = -1; pressed = -1; dirty = 1;
                    }
                    wasdown = down;
                }
            }
        }
        double t = now();
        if (status_at && t - status_at > 4 && t - status_at < 4.2) dirty = 1;  // fade status to dim
        if (dirty && t - last_render > 0.03) { render(); last_render = t; }
    }
}
#endif  // DECK_PREVIEW
