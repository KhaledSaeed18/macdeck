// Host-side preview: renders any deck page with sample data to a PPM image on the Mac,
// so layout changes can be checked without flashing the tablet. Built from the same
// modules as the deck except main.c and input.c (see the Makefile `preview` target).
//   build/deck-preview out.ppm [page] [pressed-control] [disconnected]
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "deck.h"

static const char *out_path;

void present(void) {  // write the frame (BGRA, as on the device) as RGB PPM
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    uint32_t *p = gfx_frame();
    for (int i = 0; i < W * H; i++) { fputc(p[i] >> 16 & 255, f); fputc(p[i] >> 8 & 255, f); fputc(p[i] & 255, f); }
    fclose(f);
}

static void feed(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void feed(const char *fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    model_handle(line);
}

int main(int argc, char **argv) {
    out_path = argc > 1 ? argv[1] : "deck-preview.ppm";
    gfx_init_buffer(1024, 600, 16, 8, 0, 24, 8);
    page = argc > 2 ? atoi(argv[2]) : 0;
    pressed = argc > 3 ? atoi(argv[3]) : -1;
    connected = !(argc > 4);
    tab = (Tablet){.battery = 28, .current_ma = -98, .temp_dc = 285, .mv = 3712, .freq_mhz = 624,
                   .mem_free_mb = 798, .mem_total_mb = 832, .uptime = 4980};
    copy(tab.status, sizeof tab.status, "Discharging");

    feed("CLOCK|14:05|Sat 21 Nov");
    feed("TILE|0|CPU|23%%|10 cores ld 2.1|23");
    feed("TILE|1|MEMORY|11.2G|of 16.0G used|70");
    feed("TILE|2|DISK FREE|21.8G|of 228G|90");
    feed("TILE|3|NETWORK|1.2M/s|up 84.0K/s|-1");
    const char *b[][3] = {{"ghostty", "Ghostty", "63dab4"}, {"claude", "Claude", "e8a87c"}, {"chrome", "Chrome", "4287f5"},
                          {"calculator", "Calculator", "8fd3ff"}, {"activity", "Activity", "f5a623"},
                          {"finder", "Finder", "b48cf2"}, {"sleep-display", "Screen Off", "e6465a"}};
    for (int i = 0; i < 7; i++) feed("BUTTON|%d|%s|%s|%s%s", i, b[i][0], b[i][1], b[i][2], i < 4 ? "|home" : "");
    for (int i = 0; i < HISTORY; i++) {  // two minutes of plausible history
        feed("SAMPLE|cpu|%.1f", 22 + 14 * sin(i / 9.0) + 9 * sin(i / 2.3));
        feed("SAMPLE|mem|%.1f", 68 + 3 * sin(i / 20.0));
        feed("SAMPLE|net_down|%.0f", 300000 + 900000 * pow(sin(i / 13.0), 2));
        feed("SAMPLE|net_up|%.0f", 40000 + 60000 * pow(sin(i / 7.0), 2));
    }
    feed("AGENT|0|Claude|waiting|Refactor the parser|my-app|12s|permission");
    feed("AGENT|1|Claude|working|Add the settings page|my-app|3m|");
    feed("AGENT|2|Codex|working|Fix flaky upload test|backend|48s|");
    feed("AGENT|3|Claude|idle|Write release notes|docs-site|25m|");
    toast("Opened Calculator", 4);
    pages_render();
    printf("wrote %s (%s)\n", out_path, page_at(page)->title);
    return 0;
}
