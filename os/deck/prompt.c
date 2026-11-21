// Agent prompts: a coding agent on the Mac asks for permission, asks a question or wants a plan
// approved, and the deck shows it as a card over whatever page is open (protocol: ASK...).
//
//   permission / plan                         question
//   +-------------------------------------+   +-------------------------------------+
//   | [NEEDS YOU]  Claude  my-app  1 of 2 |   | [NEEDS YOU]  Claude  my-app  Q 1/2  |
//   | Run a command                       |   | Which database should we use?       |
//   | +---------------------------------+ |   | +---------------+ +---------------+ |
//   | | npm test -- --watch=false       | |   | | Postgres      | | SQLite        | |
//   | +---------------------------------+ |   | | Relational... | | File-based... | |
//   | [Allow][Always][Deny][On Mac]       |   | [On Mac]                     [Done] |
//   +-------------------------------------+   +-------------------------------------+
//
// The host owns the wording and the options; the deck only draws them and sends back the index
// (or indexes, for a multi-choice question), or "mac" to answer on the Mac instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck.h"

#define MAX_PROMPTS 4
#define MAX_OPTS 6
#define MAX_TEXT 6
enum { K_PERMISSION, K_QUESTION, K_PLAN };
enum { P_OPT = PROMPT_CTRL, P_MAC = PROMPT_CTRL + 50, P_DONE, P_NONE };  // P_NONE: inside the card

typedef struct { char icon[12], label[40], detail[160], style[8]; } Opt;
typedef struct {
    char id[12], agent[16], where[40], title[200];
    char text[MAX_TEXT][256];
    Opt opt[MAX_OPTS];
    int kind, ntext, nopt, step, steps, multi, ready;
    unsigned sel;                                  // multi-choice selection, one bit per option
} Prompt;

static Prompt q[MAX_PROMPTS];
static int nq;

static Prompt *find(const char *id) {
    for (int i = 0; i < nq; i++) if (!strcmp(q[i].id, id)) return &q[i];
    return NULL;
}
static void drop(Prompt *p) {
    int i = (int)(p - q);
    memmove(&q[i], &q[i + 1], (size_t)(nq - i - 1) * sizeof *q);
    nq--; pressed = -1; dirty = 1;
}
static Prompt *shown(void) {
    for (int i = 0; i < nq; i++) if (q[i].ready) return &q[i];
    return NULL;
}
static int waiting(void) { int n = 0; for (int i = 0; i < nq; i++) n += q[i].ready; return n; }

int prompt_visible(void) { return connected && shown() != NULL; }
void prompt_clear(void) { nq = 0; dirty = 1; }

// ---------------------------------------------------------------- protocol
int prompt_handle(char **f, int n) {
    const char *type = f[0];
    Prompt *p = n >= 2 ? find(f[1]) : NULL;
    if (!strcmp(type, "ASK") && n >= 9)  {            // start, or replace with the next question
        if (!p) {
            if (nq == MAX_PROMPTS) return 0;
            p = &q[nq++];
        }
        memset(p, 0, sizeof *p);
        copy(p->id, sizeof p->id, f[1]);
        p->kind = !strcmp(f[2], "question") ? K_QUESTION : !strcmp(f[2], "plan") ? K_PLAN : K_PERMISSION;
        copy(p->agent, sizeof p->agent, f[3]); copy(p->where, sizeof p->where, f[4]); copy(p->title, sizeof p->title, f[5]);
        p->step = atoi(f[6]); p->steps = atoi(f[7]); p->multi = atoi(f[8]) == 1;
        return 0;
    }
    if (!p) return 0;
    if (!strcmp(type, "ASKTEXT") && n >= 3 && p->ntext < MAX_TEXT) copy(p->text[p->ntext++], sizeof p->text[0], f[2]);
    else if (!strcmp(type, "ASKOPT") && n >= 6 && p->nopt < MAX_OPTS) {
        Opt *o = &p->opt[p->nopt++];
        copy(o->icon, sizeof o->icon, f[2]); copy(o->label, sizeof o->label, f[3]);
        copy(o->detail, sizeof o->detail, f[4]); copy(o->style, sizeof o->style, f[5]);
    } else if (!strcmp(type, "ASKSHOW")) {
        p->ready = 1; picker_open = 0; pressed = -1;
        power_activity(); set_screen(SCREEN_ON, "prompt");
        logmsg("prompt %s shown (%d waiting)", p->id, waiting());
        return 1;
    } else if (!strcmp(type, "ASKDONE")) { drop(p); return 1; }
    return 0;
}

static void answer(Prompt *p, const char *value) {
    link_send("ANSWER|%s|%s", p->id, value);
    logmsg("prompt %s answered %s", p->id, value);
    toast(!strcmp(value, "mac") ? "Answer it on the Mac" : "Sent", 4);
    if (p->step < p->steps) p->ready = 0;            // the next question replaces it
    else drop(p);
    dirty = 1;
}

// ---------------------------------------------------------------- layout
static Rect card(void) { return content_area(); }
static int title_lines(const Prompt *p) { Rect c = card(); return (int)strlen(p->title) * font_m.w > c.w - 40 ? 2 : 1; }
static int body_top(const Prompt *p) { return card().y + 52 + title_lines(p) * 36 + 10; }
static Rect button_row(void) { Rect c = card(); return (Rect){c.x + 20, c.y + c.h - 20 - 92, c.w - 40, 92}; }
static Rect choice_bar(void) { Rect c = card(); return (Rect){c.x + 20, c.y + c.h - 20 - 64, c.w - 40, 64}; }

static int nbuttons(const Prompt *p) { return p->nopt + 1; }     // the options, then "On Mac"
static Rect button(const Prompt *p, int i) { return grid_cell(button_row(), nbuttons(p), 1, 12, i); }

static Rect tiles_area(const Prompt *p) { int y = body_top(p); Rect b = choice_bar(); return (Rect){b.x, y, b.w, b.y - 12 - y}; }
static int tile_rows(const Prompt *p) { return (p->nopt + 1) / 2; }
static Rect tile(const Prompt *p, int i) { return grid_cell(tiles_area(p), p->nopt == 1 ? 1 : 2, tile_rows(p), 12, i); }
static Rect mac_choice(void) { Rect b = choice_bar(); return (Rect){b.x, b.y, 280, b.h}; }
static Rect done_choice(void) { Rect b = choice_bar(); return (Rect){b.x + b.w - 280, b.y, 280, b.h}; }

// ---------------------------------------------------------------- drawing
static uint32_t style_color(const char *s) {
    return !strcmp(s, "accent") ? C_ACCENT : !strcmp(s, "blue") ? C_BLUE : !strcmp(s, "bad") ? C_BAD : C_LOCAL;
}
static const Icon *icon_named(const char *n) {
    static const struct { const char *name; const Icon *icon; } table[] = {
        {"check", &icon_check}, {"always", &icon_always}, {"cross", &icon_cross},
        {"laptop", &icon_laptop}, {"auto", &icon_auto}, {"back", &icon_back},
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) if (!strcmp(table[i].name, n)) return table[i].icon;
    return NULL;
}
static void icon_button(Rect r, const char *label, const char *icon, uint32_t color, int ctrl) {
    uint16_t bits[16];
    const Icon *ic = icon_named(icon);
    if (ic) icon_to_bits(ic, bits);
    ui_button_icon(r, label, ic ? bits : NULL, color, pressed == ctrl ? BTN_PRESSED : BTN_NORMAL);
}

static void header(const Prompt *p, Rect c) {
    const char *pill = p->kind == K_QUESTION ? "QUESTION" : p->kind == K_PLAN ? "PLAN" : "PERMISSION";
    int pw = text_w(&font_s, pill) + 20;
    fill(c.x + 20, c.y + 14, pw, 28, C_WARN);
    text(&font_s, c.x + 30, c.y + 16, pill, C_INK, 0);
    char who[64];
    snprintf(who, sizeof who, "%s  %s", p->agent, p->where);
    text(&font_s, c.x + 20 + pw + 16, c.y + 16, who, C_DIM, c.x + c.w - 200);
    char right[32] = "";
    if (p->steps > 1) snprintf(right, sizeof right, "Question %d of %d", p->step, p->steps);
    else if (waiting() > 1) snprintf(right, sizeof right, "1 of %d waiting", waiting());
    text_right(&font_s, c.x + c.w - 20, c.y + 16, right, C_DIM);
}

// Command / file / plan text in a dark box; underneath, one line per option that explains itself
// (e.g. "Always: allow npm test in my-app from now on").
static void draw_body(const Prompt *p, Rect c) {
    Rect b = {c.x + 20, body_top(p), c.w - 40, button_row().y - 14 - body_top(p)};
    fill_r(b, C_BG);
    int legend = 0;
    for (int i = 0; i < p->nopt; i++) legend += p->opt[i].detail[0] != 0;
    int bottom = b.y + b.h - 10 - legend * 26;
    int y = b.y + 12, room = (bottom - y) / 26;
    for (int i = 0; i < p->ntext && room > 0; i++) {
        int used = text_wrap(&font_s, b.x + 16, y, b.w - 32, room, 26, p->text[i], i == 0 ? C_TEXT : C_DIM);
        y += used * 26 + 6; room = (bottom - y) / 26;
    }
    y = bottom;
    for (int i = 0; i < p->nopt; i++) {
        if (!p->opt[i].detail[0]) continue;
        char line[220];
        snprintf(line, sizeof line, "%s: %s", p->opt[i].label, p->opt[i].detail);
        text(&font_s, b.x + 16, y, line, style_color(p->opt[i].style) == C_LOCAL ? C_DIM : style_color(p->opt[i].style), b.x + b.w - 16);
        y += 26;
    }
}

static void draw_tiles(const Prompt *p) {
    for (int i = 0; i < p->nopt; i++) {
        Rect t = tile(p, i);
        int on = p->multi && (p->sel >> i & 1), down = pressed == P_OPT + i;
        fill_r(t, on ? C_ACCENT : down ? mix(C_BAR, C_TEXT, 18) : C_BAR);
        uint32_t ink = on ? C_INK : C_TEXT;
        text(&font_m, t.x + 16, t.y + 12, p->opt[i].label, ink, t.x + t.w - (p->multi ? 56 : 12));
        if (p->multi) {                                // a checkbox in the corner
            Rect box = {t.x + t.w - 44, t.y + 14, 28, 28};
            if (on) { uint16_t b[16]; icon_to_bits(&icon_check, b); fill_r(box, C_INK); icon_bits(b, box.x - 2, box.y - 2, 2, C_ACCENT); }
            else frame_r(box, 2, C_DIM);
        }
        text_wrap(&font_s, t.x + 16, t.y + 52, t.w - 32, (t.h - 60) / 24, 24, p->opt[i].detail, on ? C_INK : C_DIM);
    }
    icon_button(mac_choice(), "On Mac", "laptop", C_LOCAL, P_MAC);
    if (p->multi) {
        if (p->sel) icon_button(done_choice(), "Done", "check", C_ACCENT, P_DONE);
        else ui_button(done_choice(), "Pick one or more", C_LOCAL, BTN_DISABLED);
    }
}

void prompt_render(void) {
    Prompt *p = shown();
    if (!p) return;
    Rect c = card();
    shade_r((Rect){0, TOP, W, H - TOP - BOTTOM}, 30);
    fill_r(c, C_PANEL);
    frame_r(c, 3, C_WARN);
    header(p, c);
    text_wrap(&font_m, c.x + 20, c.y + 52, c.w - 40, 2, 36, p->title, C_TEXT);
    if (p->kind == K_QUESTION) { draw_tiles(p); return; }
    draw_body(p, c);
    for (int i = 0; i < p->nopt; i++) icon_button(button(p, i), p->opt[i].label, p->opt[i].icon, style_color(p->opt[i].style), P_OPT + i);
    icon_button(button(p, p->nopt), "On Mac", "laptop", C_LOCAL, P_MAC);
}

// ---------------------------------------------------------------- touch
int prompt_hit(int x, int y) {
    Prompt *p = shown();
    if (!p) return -1;
    if (p->kind == K_QUESTION) {
        for (int i = 0; i < p->nopt; i++) if (in_rect(tile(p, i), x, y)) return P_OPT + i;
        if (in_rect(mac_choice(), x, y)) return P_MAC;
        if (p->multi && p->sel && in_rect(done_choice(), x, y)) return P_DONE;
    } else {
        for (int i = 0; i < nbuttons(p); i++) if (in_rect(button(p, i), x, y)) return i < p->nopt ? P_OPT + i : P_MAC;
    }
    return P_NONE;                                     // the card is modal: nothing underneath reacts
}

void prompt_press(int c) {
    Prompt *p = shown();
    if (!p || c == P_NONE) return;
    if (c == P_MAC) { answer(p, "mac"); return; }
    if (c == P_DONE) {
        char v[32] = "";
        for (int i = 0; i < p->nopt; i++)
            if (p->sel >> i & 1) snprintf(v + strlen(v), sizeof v - strlen(v), "%s%d", v[0] ? "," : "", i);
        answer(p, v);
        return;
    }
    int i = c - P_OPT;
    if (i < 0 || i >= p->nopt) return;
    if (p->kind == K_QUESTION && p->multi) { p->sel ^= 1u << i; dirty = 1; return; }
    char v[8];
    snprintf(v, sizeof v, "%d", i);
    answer(p, v);
}
