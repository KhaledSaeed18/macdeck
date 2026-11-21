// Host-side preview: renders any deck page with sample data to a PPM image on the Mac,
// so layout changes can be checked without flashing the tablet. Built from the same
// modules as the deck except main.c and input.c (see the Makefile `preview` target).
//   build/deck-preview out.ppm [page] [pressed-control] [disconnected]
//   MACDECK_PICKER=1 draws the page picker open over the page
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// MACDECK_PROMPT=permission|question|multi|plan draws an agent prompt card over the page
static void sample_prompt(const char *kind) {
    if (!kind) return;
    if (!strcmp(kind, "permission")) {
        feed("ASK|p1|permission|Claude|my-app|Run a command|1|1|0");
        feed("ASKTEXT|p1|$ npm test -- --watch=false --reporter=dot && npm run lint -- --max-warnings=0");
        feed("ASKTEXT|p1|Run the unit tests once, then lint the project");
        feed("ASKOPT|p1|check|Allow||accent");
        feed("ASKOPT|p1|always|Always|allow npm test in my-app from now on|blue");
        feed("ASKOPT|p1|cross|Deny||bad");
        feed("ASK|p2|permission|Codex|backend|Edit a file|1|1|0");  // a second one waiting
        feed("ASKSHOW|p2");
    } else if (!strcmp(kind, "question") || !strcmp(kind, "multi")) {
        int multi = !strcmp(kind, "multi");
        feed("ASK|p1|question|Claude|my-app|%s|1|2|%d", multi ? "Which checks should run before each commit?" : "Which database should the new service use?", multi);
        feed("ASKOPT|p1|-|%s|%s|-", multi ? "Lint" : "Postgres", multi ? "ESLint with the project rules" : "Relational, the same as the main app; needs a running server");
        feed("ASKOPT|p1|-|%s|%s|-", multi ? "Type check" : "SQLite", multi ? "tsc --noEmit, about 20 s" : "A single file next to the service, nothing to run");
        feed("ASKOPT|p1|-|%s|%s|-", multi ? "Unit tests" : "Redis", multi ? "Only the tests for changed files" : "In memory, for caching only");
        if (multi) feed("ASKOPT|p1|-|Format|Prettier --check|-");
    } else if (!strcmp(kind, "plan")) {
        feed("ASK|p1|plan|Claude|my-app|Approve the plan?|1|1|0");
        feed("ASKTEXT|p1|Plan: add a settings page");
        feed("ASKTEXT|p1|1. Add a /settings route and page component");
        feed("ASKTEXT|p1|2. Store preferences in the existing user table");
        feed("ASKTEXT|p1|3. Add tests for the API and the page");
        feed("ASKOPT|p1|auto|Auto-accept|approve, and accept file edits without asking|accent");
        feed("ASKOPT|p1|check|Approve|approve, and ask before each edit|blue");
        feed("ASKOPT|p1|back|Keep planning||-");
    }
    feed("ASKSHOW|p1");
    if (!strcmp(kind, "multi")) { prompt_press(PROMPT_CTRL + 0); prompt_press(PROMPT_CTRL + 2); }  // two ticked
}

int main(int argc, char **argv) {
    out_path = argc > 1 ? argv[1] : "deck-preview.ppm";
    gfx_init_buffer(1024, 600, 16, 8, 0, 24, 8);
    page = argc > 2 ? atoi(argv[2]) : 0;
    pressed = argc > 3 ? atoi(argv[3]) : -1;
    connected = !(argc > 4);
    picker_open = getenv("MACDECK_PICKER") != NULL;
    tab = (Tablet){.battery = 28, .current_ma = -98, .temp_dc = 285, .mv = 3712, .freq_mhz = 624,
                   .mem_free_mb = 798, .mem_total_mb = 832, .uptime = 4980};
    copy(tab.status, sizeof tab.status, "Discharging");

    feed("CLOCK|14:05|Sat 21 Nov");
    feed("TILE|0|CPU|23%%|10 cores ld 2.1|23");
    feed("TILE|1|MEMORY|11.2G|of 16.0G used|70");
    feed("TILE|2|DISK FREE|21.8G|of 228G|90");
    feed("TILE|3|NETWORK down/s|99.9K|up 84.0K/s|-1");
    const char *b[][3] = {{"ghostty", "Ghostty", "63dab4"}, {"claude", "Claude", "e8a87c"}, {"chrome", "Chrome", "4287f5"},
                          {"calculator", "Calculator", "8fd3ff"}, {"activity", "Activity", "f5a623"},
                          {"finder", "Finder", "b48cf2"}, {"sleep-display", "Screen Off", "e6465a"}};
    for (int i = 0; i < 7; i++) feed("BUTTON|%d|%s|%s|%s%s", i, b[i][0], b[i][1], b[i][2], i < 4 ? "|home" : "");
    const char *icons[] = {"000007e01ff83ffc399c799e799e7ffe7ffe7ffe7ffe7ffe7ffe73ce61860000",
                           "00000180018003c003c007e01ff8ffffffff1ff807e003c003c0018001800000",
                           "000007e01ff8399c62466426c423ffffffffc42364266246399c1ff807e00000",
                           "00003ffc3ffc300c300c3ffc333c333c3ffc333c333c3ffc333c333c3ffc0000",
                           "00000006000600060066006600660666066606666666666666666666ffff0000",
                           "000000007e007f007ffe7ffe40027ffe7ffe7ffe7ffe7ffe7ffe7ffe00000000",
                           "000002000e001c003c007c007c007c007e007e007f803ff81ff00fe003800000"};  // ghost spark globe calculator chart folder moon (host/icons)
    for (int i = 0; i < 7; i++) feed("ICON|button|%d|%s", i, icons[i]);
    for (int i = 0; i < HISTORY; i++) {  // two minutes of plausible history
        feed("SAMPLE|cpu|%.1f", 22 + 14 * sin(i / 9.0) + 9 * sin(i / 2.3));
        feed("SAMPLE|mem|%.1f", 68 + 3 * sin(i / 20.0));
        feed("SAMPLE|net_down|%.0f", 300000 + 900000 * pow(sin(i / 13.0), 2));
        feed("SAMPLE|net_up|%.0f", 40000 + 60000 * pow(sin(i / 7.0), 2));
    }
    feed("AGENT|0|Claude|waiting|Refactor the parser|my-app|12s|permission|a1b2c3d4");
    feed("AGENT|1|Claude|working|Add the settings page|my-app|3m||e5f6a7b8");
    feed("AGENT|2|Codex|working|Fix flaky upload test|backend|48s||c9d0e1f2");
    feed("AGENT|3|Claude|idle|Write release notes|docs-site|25m||d4e5f6a7");
    feed("AGENT|4|opencode|idle|Tidy the README|tools|28m||b8c9d0e1");
    feed("AGENTS|7|1|2|4");  // 7 sessions: 5 cards + "+2 more"
    feed("MAC|battery|87|1|Power adapter|Charging, 0:42 to full|Health 96%%  197 cycles  30W adapter");
    feed("MAC|sound|50|0");
    feed("MAC|wifi|on|Connected, 192.168.1.20");
    feed("MAC|airdrop|contacts");
    feed("MAC|dark|on");
    toast("Opened Calculator", 4);
    sample_prompt(getenv("MACDECK_PROMPT"));
    pages_render();
    printf("wrote %s (%s)\n", out_path, page_at(page)->title);
    return 0;
}
