// MacDeck deck — shared application state and module interfaces.
//
//   gfx.c        drawing primitives and widgets
//   model.c      state received from the Mac + protocol parsing
//   link.c       TCP link to the Mac (adb forward)
//   power.c      settings, screen state, backlight, CPU governor, tablet sysfs readings
//   input.c      touch gestures and keys
//   pages.c      page registry, chrome (top/bottom bars), carousel composition
//   page_*.c     one file per page
//   util.c       time, logging
//   main.c       framebuffer, startup and event loop
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#define BUILD "deck-3.7"
#define TOP 56          // top bar height
#define BOTTOM 40       // bottom bar height
#define MAX_TILES 4
#define MAX_BUTTONS 12
#define HOME_BUTTONS 3   // pinned shortcuts on Home, in one column
#define MAX_AGENTS 6    // agent session cards (Agents page)
#define HISTORY 120     // samples kept per series (~2 min at 1/s)

struct pollfd;

// ---------------------------------------------------------------- util (util.c)
double now(void);
void log_open(const char *path);
void logmsg(const char *fmt, ...);
void copy(char *dst, size_t cap, const char *src);

// ---------------------------------------------------------------- model (model.c)
typedef struct { char label[32], value[24], detail[40]; int level, set; } Tile;
typedef struct { char id[32], label[32]; unsigned color; int home, has_icon; uint16_t icon[16]; } Button;
typedef struct { char key[16]; float v[HISTORY]; int n, head; } Series;
enum { AGENT_IDLE, AGENT_WORKING, AGENT_WAITING };
typedef struct { char agent[16], title[48], where[40], since[12], detail[48], id[12]; int state, set; } Agent;
typedef struct { int total, waiting, working, idle; } AgentCounts;  // all sessions, shown or not

extern Tile tiles[MAX_TILES];
extern Button buttons[MAX_BUTTONS];
extern Agent agents[MAX_AGENTS];

// The Mac page's state (MAC messages). Switches and choices show the user's change at once
// ("optimistic") and keep it until the Mac reports the same value or the change times out.
enum { MAC_WIFI, MAC_AIRDROP, MAC_DARK, MAC_VOLUME, MAC_MUTE, MAC_NKEYS };
enum { AIRDROP_OFF, AIRDROP_CONTACTS, AIRDROP_EVERYONE };
typedef struct {
    int have;                                   // a MAC message has arrived since connecting
    int batt_pct, batt_charging;
    char batt_source[24], batt_line[48], batt_detail[64], wifi_detail[48];
    int val[MAC_NKEYS];                         // 0/1 switches, AIRDROP_*, volume 0..100
    double pending[MAC_NKEYS];                  // time until which val[k] is the user's change
} MacState;
extern MacState mac;
void mac_set(int key, int value);              // change a Mac control (optimistic + MACSET)
extern AgentCounts agent_counts;
extern char clock_time[16], clock_date[32];
extern int connected, dirty;

void model_handle(char *line);                 // one protocol line from the Mac
const Series *series_find(const char *key);
int  series_linear(const Series *s, float *out); // copies oldest..newest, returns count
float series_max(const Series *s);
int  home_button(int i);                       // button slot shown in home position i, or -1
int  agent_slot(int i);                        // slot of the i-th agent card, or -1

// Bottom-left status line: transient toasts; empty when nothing is happening.
void toast(const char *text, double secs);
const char *toast_text(void);                  // NULL when nothing to show
int  toast_expired(double t);                  // clears it and returns 1 when it just expired

// ---------------------------------------------------------------- link (link.c)
void link_send(const char *fmt, ...);
int  link_listen(int port);
void link_accept(int srv);
void link_read(void);
void link_check(double t);
int  link_fd(void);

// ---------------------------------------------------------------- power (power.c)
enum { SCREEN_ON, SCREEN_DIM, SCREEN_OFF };
typedef struct {
    int battery, current_ma, temp_dc, mv, freq_mhz, mem_free_mb, mem_total_mb;
    char status[24];
    long uptime;
} Tablet;
extern Tablet tab;
extern int brightness, dim_after, off_after, cpu_saver, screen;

void power_init(void);
void power_tick(double t);                     // idle dim/off, sysfs sampling
void power_activity(void);                     // user interaction happened
void set_screen(int s, const char *why);
void apply_setting(const char *key, const char *val, int from_user);
void tablet_sample(void);
const char *fmt_secs(int s, char *b, size_t cap);
const char *screen_name(int s);

// ---------------------------------------------------------------- pages (pages.c, page_*.c)
typedef struct {
    const char *title;
    void (*render)(int ox);        // draw the content area shifted by ox pixels
    int  (*hit)(int x, int y);     // control index under the point, or -1
    void (*press)(int i);          // activate control i
} Page;

extern const Page page_home, page_agents, page_stats, page_shortcuts, page_mac, page_tablet;
extern int page, pressed;          // current page; pressed control on it (-1 = none)
extern float drag;                 // horizontal offset while swiping / settling
int  page_count(void);
const Page *page_at(int i);
void pages_render(void);           // full frame: bars + visible pages, then present()
int  button_state(int i, int enabled);  // BTN_* for control i of the page being drawn
Rect content_area(void);
void present(void);                // copy the frame to the display (main.c / preview.c)

// ---------------------------------------------------------------- input (input.c)
void input_init(void);
int  input_pollfds(struct pollfd *p);          // adds touch/key fds, returns how many
void input_handle(const struct pollfd *p, int n);
int  input_tick(double t);                     // advances the page-settle animation; 1 while animating

// Shared page helpers
void mac_button_press(int slot);   // send a Mac button press (shared by Home and Shortcuts)
void draw_mac_button(Rect r, int slot, int ctrl);
const char *agent_state_name(int state);   // "NEEDS YOU", "WORKING", "IDLE"
uint32_t agent_state_color(int state);     // dimmed while the Mac is not connected
void agent_focus(int slot);                // ask the Mac to bring that session's app to the front
void page_show(const Page *p);             // switch to a page (e.g. Home -> Agents)

// Chrome controls, outside any page: the page squares in the bottom bar open the page picker,
// a sheet with one tile per page. Their indices start at CHROME_CTRL so they never collide
// with a page's own control indices.
enum { CHROME_CTRL = 1000, CTRL_SQUARES = CHROME_CTRL, CTRL_PICK_CLOSE, CTRL_PICK /* + page index */ };
extern int picker_open;
int  chrome_hit(int x, int y);             // chrome control under the point, or -1
void chrome_press(int c);

// ---------------------------------------------------------------- prompts (prompt.c)
// A coding agent asks for permission, a choice or a plan approval (ASK... messages). The card is
// modal and drawn over every page; its controls start at PROMPT_CTRL.
#define PROMPT_CTRL 2000
int  prompt_handle(char **f, int n);       // ASK, ASKTEXT, ASKOPT, ASKSHOW, ASKDONE; 1 = redraw
int  prompt_visible(void);
void prompt_render(void);
int  prompt_hit(int x, int y);             // >= PROMPT_CTRL while a card is shown, else -1
void prompt_press(int c);
void prompt_clear(void);                   // forget all prompts (new connection)
