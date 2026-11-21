// Host-side preview: renders the deck UI with sample data into a PPM image on the Mac,
// so layout changes can be checked without flashing the tablet.
//   make preview                      (writes build/preview.png and build/preview-tablet.png)
//   build/deck-preview out.ppm [page] [pressed-slot] [disconnected]
// It compiles deck.c with DECK_PREVIEW, which swaps the Linux fb/input headers for the
// minimal stubs below and leaves out deck's input handling and main().
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DECK_PREVIEW 1
struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo { uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, activate;
                           struct fb_bitfield red, green, blue, transp; };
struct fb_fix_screeninfo { char id[16]; uint32_t line_length, smem_len; };
#include "deck.c"

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "deck-preview.ppm";
    W = 1024; H = 600;
    vi.red = (struct fb_bitfield){16, 8, 0}; vi.green = (struct fb_bitfield){8, 8, 0};
    vi.blue = (struct fb_bitfield){0, 8, 0}; vi.transp = (struct fb_bitfield){24, 8, 0};
    fi.line_length = W * 4;
    back = malloc((size_t)W * H * 4); fbmem = malloc((size_t)W * H * 4);
    page = argc > 2 ? atoi(argv[2]) : 0;
    pressed = argc > 3 ? atoi(argv[3]) : -1;
    connected = !(argc > 4);
    tab = (Tablet){.battery = 28, .current_ma = -98, .temp_dc = 285, .mv = 3712, .freq_mhz = 624,
                   .mem_free_mb = 798, .mem_total_mb = 832, .uptime = 4980};
    copy(tab.status, sizeof tab.status, "Discharging");
    char lines[][MAX_LINE] = {
        "CLOCK|14:05|Sat 21 Nov",
        "TILE|0|CPU|23%|10 cores ld 2.1|23",
        "TILE|1|MEMORY|11.2G|of 16.0G used|70",
        "TILE|2|DISK FREE|21.8G|of 228G|90",
        "TILE|3|NETWORK|1.2M/s|up 84.0K/s|-1",
        "BUTTON|0|ghostty|Ghostty|63dab4",
        "BUTTON|1|claude|Claude|e8a87c",
        "BUTTON|2|chrome|Chrome|4287f5",
        "BUTTON|3|calculator|Calculator|8fd3ff",
        "BUTTON|4|activity|Activity|f5a623",
        "BUTTON|5|sleep-display|Screen Off|e6465a",
        "STATUS|Opened Calculator",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) handle(lines[i]);
    render();
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = ((uint32_t *)fbmem)[i];
        fputc(p >> 16 & 255, f); fputc(p >> 8 & 255, f); fputc(p & 255, f);
    }
    fclose(f);
    printf("wrote %s\n", out);
    return 0;
}
