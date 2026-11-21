// MacDeck deck — the single userspace program of MacDeck OS (SM-T210, 1024x600 fb).
// Sets up the framebuffer, inputs and the Mac link, then runs one poll() loop.
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "deck.h"

#define PORT 8765

static struct fb_fix_screeninfo fi;
static uint8_t *fbmem;

void present(void) {
    uint32_t *back = gfx_frame();
    if (fi.line_length == (uint32_t)W * 4) memcpy(fbmem, back, (size_t)W * H * 4);
    else for (int y = 0; y < H; y++) memcpy(fbmem + (size_t)y * fi.line_length, back + (size_t)y * W, (size_t)W * 4);
}

static int fb_init(void) {
    int fd = -1;
    for (int i = 0; i < 50 && fd < 0; i++) { fd = open("/dev/graphics/fb0", O_RDWR); if (fd < 0) usleep(100000); }
    struct fb_var_screeninfo vi;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi) || vi.bits_per_pixel != 32) return -1;
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fbmem == MAP_FAILED) return -1;
    if (gfx_init_buffer(vi.xres, vi.yres, vi.red.offset, vi.green.offset, vi.blue.offset, vi.transp.offset, vi.transp.length)) return -1;
    vi.yoffset = 0; vi.activate = FB_ACTIVATE_NOW;
    ioctl(fd, FBIOPAN_DISPLAY, &vi);
    ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
    logmsg("fb0 %dx%d line=%u", W, H, fi.line_length);
    return 0;
}

static void log_boot_mode(void) {  // normal vs charger/LPM vs recovery, for diagnostics
    char cmd[2048] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (f) { size_t n = fread(cmd, 1, sizeof cmd - 1, f); cmd[n] = 0; fclose(f); }
    const char *m = strstr(cmd, "androidboot.mode=");
    logmsg("boot: %.40s recovery=%d", m ? m : "androidboot.mode=(none)", strstr(cmd, "recovery_mode=1") != NULL);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    log_open(argc > 1 ? argv[1] : "/tmp/deck.log");
    logmsg("%s starting, pid %d uid %d", BUILD, getpid(), getuid());
    log_boot_mode();
    power_init();
    if (fb_init()) { logmsg("fb0 unusable"); return 1; }
    input_init();
    int srv = link_listen(PORT);

    double last_render = 0, last_check = 0;
    for (;;) {
        double t = now();
        power_tick(t);
        if (t - last_check >= 1) { link_check(t); last_check = t; }
        if (toast_expired(t)) dirty = 1;
        int animating = input_tick(t);
        if (dirty && screen != SCREEN_OFF && t - last_render > 0.025) { pages_render(); last_render = t; }

        // Sleep until input arrives: 16 ms while animating or behind on a frame, else up to 1 s.
        struct pollfd p[5] = {{srv, POLLIN, 0}, {link_fd(), POLLIN, 0}};
        int n = 2 + input_pollfds(p + 2);
        poll(p, n, animating || (dirty && screen != SCREEN_OFF) ? 16 : 1000);
        if (p[0].revents & POLLIN) link_accept(srv);
        if (p[1].fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) link_read();
        input_handle(p + 2, n - 2);
    }
}
