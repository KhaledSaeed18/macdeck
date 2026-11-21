// Host-side preview: renders the deck UI with sample data into a PPM image on the Mac,
// so layout changes can be checked without flashing the tablet.
//   cc -O2 -Ios/deck -o build/deck-preview os/deck/preview.c && build/deck-preview out.ppm [pressed-slot] [disconnected]
// It compiles deck.c with DECK_PREVIEW, which swaps the Linux fb/input headers for the
// minimal stubs below and renames deck's main().
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
    layout();
    connected = !(argc > 3);
    pressed = argc > 2 ? atoi(argv[2]) : -1;
    char lines[][MAX_LINE] = {
        "CLOCK|14:05|Sat 21 Nov",
        "TILE|0|CPU|23%|8 cores - load 2.1|23",
        "TILE|1|MEMORY|11.2G|of 16.0 GB used|70",
        "TILE|2|DISK FREE|21.8G|of 228 GB|90",
        "TILE|3|NETWORK|1.2M|down 1.2M/s up 84K/s|-1",
        "BUTTON|0|calculator|Calculator|63dab4",
        "BUTTON|1|terminal|Ghostty|4287f5",
        "BUTTON|2|cursor|Cursor|f5a623",
        "BUTTON|3|finder|Finder|8fd3ff",
        "BUTTON|4|lock|Lock Mac|e6465a",
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
