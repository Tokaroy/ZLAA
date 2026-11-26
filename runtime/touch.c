/*
 * touch.c - the on-screen controls (touch.h): where they sit, which finger holds what.
 *
 * Sizes and places are fractions of the screen's height u (a landscape screen: 720 or 1080 pixels on most phones),
 * so they keep their size on the panel whatever its shape; a wider phone only spreads the two sides further apart.
 * The player can move and resize each control (touch_edit): its offset from its default place (in u) and its size
 * (times the default) are kept per control; A B X Y can be moved and resized as one (around their middle).
 * A finger keeps what it first touched: the nearest button within reach, or else the d-pad / stick when it lands in
 * the movement area (the left part of the screen, or around the control wherever the player put it). The d-pad's
 * direction is from its middle, so a thumb that drifts off it still steers; the stick's base moves to where the
 * finger landed (unless LAA_TOUCH_FIXED). On the face buttons a finger may slide from one to the next. Called from
 * the main thread (fingers) and the game's present (touch_view): under a lock.
 */
#include "touch.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FINGERS 10
#define NONE (-1)

/* the default layout, in units of u (the screen's height) from the left / right / top / bottom edges */
/* (the d-pad and A B X Y are the same size, as high, as far in from their sides: FACE_GAP + FACE_R = DPAD_R) */
#define FACE_R    0.052f       /* A B X Y: radius */
#define FACE_GAP  0.105f       /* centre to each of them */
#define FACE_X    0.210f       /* the four's centre, from the right edge */
#define FACE_Y    0.235f       /*   from the bottom */
#define PILL_W    0.190f       /* L R ZL ZR */
#define PILL_H    0.085f
#define PILL_X    0.150f       /* centre from the side */
#define PILL_Y1   0.095f       /* ZL / ZR from the top */
#define PILL_Y2   0.200f       /* L / R */
#define SMALL_R   0.040f       /* - menu + */
#define SMALL_GAP 0.110f
#define SMALL_Y   0.070f
#define BASE_R    0.150f       /* the stick: its ring, its knob */
#define KNOB_R    0.068f
#define MOVE_X    FACE_X       /* the d-pad's middle and the stick's rest place, from the left and bottom */
#define MOVE_Y    FACE_Y
#define DPAD_R    (FACE_GAP + FACE_R)   /* the d-pad: middle to the end of an arm; an arm's width (a button's); */
#define ARM_W     (2 * FACE_R)          /*   no direction this near the middle */
#define DPAD_DEAD 0.20f        /*   (of DPAD_R) */
#define AREA_X    0.45f        /* the movement area: the left part of the screen (of its width) ... */
#define AREA_TOP  0.27f        /*   below the shoulder buttons (u); and within AREA_R of the control */
#define AREA_R    2.0f         /*   (times its size), wherever it is */
#define WIDE_W    0.170f       /* the editor's toolbar, across the middle: Reset, Undo, Redo, smaller, bigger, ABXY, */
#define TOOL_GAP  0.035f       /*   Done */
#define UNDO_MAX  32           /* changes Undo can take back (and Redo bring again) */
#define SCALE_MIN 0.5f         /* the player's sizes: half to twice the default, 10% a step */
#define SCALE_MAX 2.0f
#define SCALE_STEP 1.1f
#define REACH     1.35f        /* a button counts this far out (times its size): fingers are not exact */

/* the controls the player can move: the movement control (d-pad or stick), then the buttons TS_FACE_A..TS_MENU */
#define SLOTS (1 + TS_MENU + 1)
#define SLOT(c) ((c) == TS_BASE || (c) == TS_DPAD ? 0 : (c) + 1)
#define GRAB 100               /* a finger's `what` while editing: GRAB + the slot it drags */

static const uint64_t bits[TS_COUNT] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_MINUS, BTN_PLUS };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t id;
    int what;                  /* a TS_ control, GRAB + a slot, or NONE */
    float x0, y0;              /* (where it landed, in pixels: a drag's start) */
} fingers[MAX_FINGERS];
static int nfingers;
static float base_x, base_y, knob_x, knob_y;          /* the held stick, in pixels */
static float stick_x, stick_y;                         /* and where it points: -1..1, y up */
static int stick_held, dpad_x, dpad_y;                 /* the d-pad's direction: -1, 0, 1 each (y up) */
static int mode = -1, shown, present = -1, use_stick, fixed_stick, editing;
static int xbox_style;                                 /* LAA_TOUCH_SWAP=1: A and B, X and Y trade places */
static float opacity = 1;                              /* LAA_TOUCH_OPACITY (percent): how solid they are drawn */
static float off[SLOTS][2];                            /* each control's offset from its default place (u) */
static float scl[SLOTS];                               /*   and its size, times the default */
static int grouped = 1;                                /* A B X Y move and resize as one */
static int selected = NONE;                            /* (editing: the slot the toolbar's sizes act on) */
static float drag_from[SLOTS][2];                      /* (editing: the offsets when the drag began) */

typedef struct { float off[SLOTS][2], scl[SLOTS]; int grouped; } Layout;
static Layout undo[UNDO_MAX], redo[UNDO_MAX], drag_before;   /* (editing: the layouts before each change, oldest
                                                                 first; and those Undo left, newest last) */
static int nundo, nredo;

static Layout layout_now(void) {
    Layout l;
    memcpy(l.off, off, sizeof off);
    memcpy(l.scl, scl, sizeof scl);
    l.grouped = grouped;
    return l;
}

/* After a change: remember the layout before it, if it changed anything (the oldest goes when there are too many) */
static void changed_from(const Layout *before) {
    Layout now = layout_now();
    if (memcmp(&now, before, sizeof now) == 0) return;
    if (nundo == UNDO_MAX) memmove(undo, undo + 1, sizeof undo[0] * --nundo);
    undo[nundo++] = *before;
    nredo = 0;                                         /* (a new change: what was undone is gone) */
}

static void layout_set(const Layout *l) {
    memcpy(off, l->off, sizeof off);
    memcpy(scl, l->scl, sizeof scl);
    grouped = l->grouped;
}

static void layout_default(void) {
    memset(off, 0, sizeof off);
    for (int k = 0; k < SLOTS; k++) scl[k] = 1;
    grouped = 1;
}

static int is_face_slot(int k) { return k >= SLOT(TS_FACE_A) && k <= SLOT(TS_FACE_Y); }

static void read_mode(void) {
    if (mode >= 0) return;
    const char *m = getenv("LAA_TOUCH"), *st = getenv("LAA_TOUCH_STICK"), *fx = getenv("LAA_TOUCH_FIXED");
    mode = m && *m == '1' ? 2 : m && *m == '0' ? 0 : 1;   /* 2 always, 1 auto, 0 never */
    use_stick = st && *st == '1';
    fixed_stick = fx && *fx == '1';
    const char *sw = getenv("LAA_TOUCH_SWAP");
    xbox_style = sw && *sw == '1';
    const char *op = getenv("LAA_TOUCH_OPACITY");     /* (percent, 20..100) */
    opacity = op && *op ? fminf(fmaxf((float)atof(op), 20.0f), 100.0f) / 100.0f : 1.0f;
    if (editing) return;                               /* (the layout being edited stays) */
    layout_default();
    const char *l = getenv("LAA_TOUCH_LAYOUT");        /* "g;x,y,s;x,y,s;...": grouped, then each slot's place, size */
    if (!l || !*l) return;
    grouped = *l != '0';
    l = strchr(l, ';');
    for (int k = 0; l && k < SLOTS; k++) {
        float x, y, z;
        if (sscanf(++l, "%f,%f,%f", &x, &y, &z) != 3) break;
        if (fabsf(x) < 4 && fabsf(y) < 2) off[k][0] = x, off[k][1] = y;
        if (z >= SCALE_MIN && z <= SCALE_MAX) scl[k] = z;
        l = strchr(l, ';');
    }
}

static int visible(void) {
    read_mode();
    return editing || mode == 2 || (mode == 1 && shown);
}

typedef struct { float x, y, hw, hh; int round; } Place;   /* centre, half sizes */

static Place place(int c, float w, float u) {
    float cx = w - FACE_X * u, cy = u - FACE_Y * u, f = FACE_R * u, g = FACE_GAP * u;
    float pw = PILL_W * u / 2, ph = PILL_H * u / 2, s = SMALL_R * u, sg = SMALL_GAP * u;
    Place p;
    int spot = c <= TS_FACE_Y && xbox_style ? c ^ 1 : c;   /* (Xbox style: A at the bottom, B right, X left, Y top) */
    switch (spot) {
    case TS_FACE_A: p = (Place){ cx + g, cy, f, f, 1 }; break;
    case TS_FACE_B: p = (Place){ cx, cy + g, f, f, 1 }; break;
    case TS_FACE_X: p = (Place){ cx, cy - g, f, f, 1 }; break;
    case TS_FACE_Y: p = (Place){ cx - g, cy, f, f, 1 }; break;
    case TS_ZL: p = (Place){ PILL_X * u, PILL_Y1 * u, pw, ph, 0 }; break;
    case TS_L: p = (Place){ PILL_X * u, PILL_Y2 * u, pw, ph, 0 }; break;
    case TS_ZR: p = (Place){ w - PILL_X * u, PILL_Y1 * u, pw, ph, 0 }; break;
    case TS_R: p = (Place){ w - PILL_X * u, PILL_Y2 * u, pw, ph, 0 }; break;
    case TS_MINUS: p = (Place){ w / 2 - sg, SMALL_Y * u, s, s, 1 }; break;
    case TS_MENU: p = (Place){ w / 2, SMALL_Y * u, s, s, 1 }; break;
    case TS_PLUS: p = (Place){ w / 2 + sg, SMALL_Y * u, s, s, 1 }; break;
    case TS_RESET: case TS_UNDO: case TS_REDO: case TS_SMALLER: case TS_BIGGER: case TS_GROUP: case TS_DONE: {
        static const int order[] = { TS_RESET, TS_UNDO, TS_REDO, TS_SMALLER, TS_BIGGER, TS_GROUP, TS_DONE };
        float x = w / 2 - (5 * WIDE_W + 4 * SMALL_R + 6 * TOOL_GAP) * u / 2;   /* (the toolbar, in this order) */
        for (int k = 0; k < 7; k++) {
            float half = (order[k] == TS_SMALLER || order[k] == TS_BIGGER ? SMALL_R : WIDE_W / 2) * u;
            if (order[k] == c) return (Place){ x + half, u / 2, half, half == SMALL_R * u ? half : ph, half == SMALL_R * u };
            x += 2 * half + TOOL_GAP * u;
        }
        return (Place){ 0, 0, 1, 1, 0 };
    }
    default: {                                         /* the d-pad / the stick's rest place */
        float r = (use_stick ? BASE_R : DPAD_R) * u;
        p = (Place){ MOVE_X * u, u - MOVE_Y * u, r, r, 1 };
    }
    }
    p.x += off[SLOT(c)][0] * u;
    p.y += off[SLOT(c)][1] * u;
    p.hw *= scl[SLOT(c)];
    p.hh *= scl[SLOT(c)];
    return p;
}

/* how far outside the control's reach the point is, as a fraction of its size (< 1: within reach) */
static float reach(const Place *p, float x, float y) {
    float dx = fabsf(x - p->x) / (p->hw * REACH), dy = fabsf(y - p->y) / (p->hh * REACH);
    return p->round ? sqrtf(dx * dx + dy * dy) : (dx > dy ? dx : dy);
}

/* the button under a point (the nearest within reach), from first..last */
static int button_at(float x, float y, float w, float u, int first, int last) {
    int best = NONE;
    float best_d = 1;
    for (int c = first; c <= last; c++) {
        Place p = place(c, w, u);
        float d = reach(&p, x, y);
        if (d < best_d) best_d = d, best = c;
    }
    return best;
}

/* is the point in the movement area: near the control, or the left part of the screen while the control is there */
static int in_move_area(float x, float y, float w, float u) {
    Place p = place(TS_DPAD, w, u);
    float dx = x - p.x, dy = y - p.y;
    if (dx * dx + dy * dy < AREA_R * AREA_R * p.hw * p.hw) return 1;
    return p.x < AREA_X * w && x < AREA_X * w && y > AREA_TOP * u;
}

static void dpad_aim(float px, float py, float w, float u) {   /* eight directions (45 degrees each), none near the middle */
    Place p = place(TS_DPAD, w, u);
    float dx = px - p.x, dy = py - p.y, d = sqrtf(dx * dx + dy * dy);
    if (d < DPAD_DEAD * p.hw) { dpad_x = dpad_y = 0; return; }
    float edge = d * 0.3827f;                          /* (sin 22.5 degrees: the eight sectors' borders) */
    dpad_x = dx > edge ? 1 : dx < -edge ? -1 : 0;
    dpad_y = dy < -edge ? 1 : dy > edge ? -1 : 0;
}

static void move_knob(float x, float y, float u) {
    float dx = x - base_x, dy = y - base_y, d = sqrtf(dx * dx + dy * dy), max = BASE_R * u * scl[0];
    if (d > max) dx *= max / d, dy *= max / d;
    knob_x = base_x + dx;
    knob_y = base_y + dy;
    float m = sqrtf(dx * dx + dy * dy) / max;          /* (a small dead zone in the middle, then all the way) */
    float k = m < 0.12f ? 0 : (m - 0.12f) / (0.88f * m);
    stick_x = dx / max * k;
    stick_y = -dy / max * k;
}

/* Editing: resize the selected control (or A B X Y together, their places spread or drawn in around their middle) */
static void resize(float f, float w, float u) {
    if (selected == NONE) return;
    int group = grouped && is_face_slot(selected), first = group ? SLOT(TS_FACE_A) : selected;
    int last = group ? SLOT(TS_FACE_Y) : selected;
    for (int k = first; k <= last; k++) if (scl[k] * f < SCALE_MIN - 0.001f || scl[k] * f > SCALE_MAX + 0.001f) return;
    float mx = 0, my = 0;
    for (int k = first; k <= last; k++) {
        Place p = place(k - 1, w, u);
        mx += p.x / (last - first + 1);
        my += p.y / (last - first + 1);
    }
    for (int k = first; k <= last; k++) {
        Place p = place(k == 0 ? TS_DPAD : k - 1, w, u);
        scl[k] *= f;
        if (group) {
            off[k][0] += (p.x - mx) * (f - 1) / u;
            off[k][1] += (p.y - my) * (f - 1) / u;
        }
    }
}

/* Editing: which control a finger landed on (a slot), the toolbar first; NONE for none */
static int edit_target(float px, float py, float w, float u) {
    int b = button_at(px, py, w, u, TS_RESET, TS_REDO);
    if (b != NONE) return b;
    b = button_at(px, py, w, u, TS_FACE_A, TS_MENU);
    if (b != NONE) return GRAB + SLOT(b);
    Place p = place(TS_DPAD, w, u);
    return reach(&p, px, py) < 1 ? GRAB + 0 : NONE;
}

int touch_finger(int type, uint64_t id, float x, float y, int w, int h) {
    float W = (float)w, u = (float)h, px = x * W, py = y * u;
    int result = TOUCH_NOTHING;
    pthread_mutex_lock(&lock);
    read_mode();
    int i = 0;
    while (i < nfingers && fingers[i].id != id) i++;
    if (type == TOUCH_DOWN && i == nfingers && nfingers < MAX_FINGERS) {
        shown = 1;
        int what = NONE;
        if (editing) {
            what = edit_target(px, py, W, u);
            if (what >= GRAB) {
                selected = what - GRAB;
                memcpy(drag_from, off, sizeof off);
                drag_before = layout_now();
            }
        } else {
            what = button_at(px, py, W, u, TS_FACE_A, TS_MENU);
            int held = 0;
            for (int k = 0; k < nfingers; k++) held |= fingers[k].what == TS_DPAD || fingers[k].what == TS_BASE;
            if (what == NONE && !held && in_move_area(px, py, W, u)) {
                if (!use_stick) {
                    what = TS_DPAD;
                    dpad_aim(px, py, W, u);
                } else {
                    what = TS_BASE;
                    Place rest = place(TS_BASE, W, u);
                    float r = BASE_R * u * scl[0];     /* (floating: where the finger landed, kept on the screen) */
                    base_x = fixed_stick ? rest.x : px < r ? r : px > W - r ? W - r : px;
                    base_y = fixed_stick ? rest.y : py < r ? r : py > u - r ? u - r : py;
                    stick_held = 1;
                    move_knob(px, py, u);
                }
            }
        }
        fingers[nfingers++] = (__typeof__(fingers[0])){ id, what, px, py };
    } else if (i < nfingers) {
        int what = fingers[i].what;
        if (type == TOUCH_MOVE && what >= GRAB) {      /* dragging a control, or A B X Y (middles kept on the screen) */
            int k0 = what - GRAB, group = grouped && is_face_slot(k0);
            float dx = (px - fingers[i].x0) / u, dy = (py - fingers[i].y0) / u, lo = 0.03f * u;
            for (int pass = 0, tries = 0; pass < 2 && tries < 8; pass++)   /* (then pulled back as far as the */
                for (int k = group ? SLOT(TS_FACE_A) : k0; k <= (group ? SLOT(TS_FACE_Y) : k0); k++) {
                    if (pass == 0) { off[k][0] = drag_from[k][0] + dx; off[k][1] = drag_from[k][1] + dy; continue; }
                    Place p = place(k == 0 ? TS_DPAD : k - 1, W, u);
                    float fx = p.x < lo ? (lo - p.x) / u : p.x > W - lo ? -(p.x - (W - lo)) / u : 0;
                    float fy = p.y < lo ? (lo - p.y) / u : p.y > u - lo ? -(p.y - (u - lo)) / u : 0;
                    if (fx != 0 || fy != 0) {
                        dx += fx;
                        dy += fy;
                        pass = -1;                     /* (again from the start, with the corrected move; */
                        tries++;                       /*  a group too big for the screen stops trying) */
                        break;
                    }
                }
        } else if (type == TOUCH_MOVE && what == TS_BASE) move_knob(px, py, u);
        else if (type == TOUCH_MOVE && what == TS_DPAD) dpad_aim(px, py, W, u);
        else if (type == TOUCH_MOVE && what >= TS_FACE_A && what <= TS_FACE_Y) {   /* (sliding between them) */
            int now = button_at(px, py, W, u, TS_FACE_A, TS_FACE_Y);
            if (now != NONE) fingers[i].what = now;
        }
        if (type == TOUCH_UP) {
            int over = what >= TS_RESET && what <= TS_REDO ? button_at(px, py, W, u, TS_RESET, TS_REDO) : NONE;
            if (over != what) over = NONE;           /* (a toolbar button counts if the finger is let go on it) */
            Layout before = layout_now();
            if (what >= GRAB) changed_from(&drag_before);   /* (a drag that moved something) */
            if (over == TS_RESET) { layout_default(); selected = NONE; }
            if (over == TS_SMALLER) resize(1 / SCALE_STEP, W, u);
            if (over == TS_BIGGER) resize(SCALE_STEP, W, u);
            if (over == TS_GROUP) grouped = !grouped;
            if (over >= TS_RESET && over != TS_UNDO && over != TS_REDO && over != TS_DONE) changed_from(&before);
            if (over == TS_UNDO && nundo) {          /* (the layout now goes to Redo's list, and the other way) */
                redo[nredo++] = before;
                layout_set(&undo[--nundo]);
            }
            if (over == TS_REDO && nredo) {
                undo[nundo++] = before;
                layout_set(&redo[--nredo]);
            }
            if (over == TS_DONE) result = TOUCH_EDIT_DONE;
            if (what == TS_BASE) stick_held = 0;
            if (what == TS_DPAD) dpad_x = dpad_y = 0;
            if (what == TS_MENU && visible()) result = TOUCH_MENU_TAPPED;
            fingers[i] = fingers[--nfingers];
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

void touch_controller_used(void) {
    pthread_mutex_lock(&lock);
    shown = 0;
    pthread_mutex_unlock(&lock);
}

void touch_controller_present(int p) {
    pthread_mutex_lock(&lock);
    if (p != present) {
        if (!p) shown = 1;                           /* (none, or it was disconnected: the controls come up) */
        present = p;
    }
    pthread_mutex_unlock(&lock);
}

void touch_release(void) {
    pthread_mutex_lock(&lock);
    nfingers = 0;
    stick_held = 0;
    dpad_x = dpad_y = 0;
    mode = -1;                                       /* (the settings may change in the menu: read again) */
    pthread_mutex_unlock(&lock);
}

void touch_edit(int on) {
    pthread_mutex_lock(&lock);
    nfingers = 0;
    stick_held = 0;
    dpad_x = dpad_y = 0;
    mode = -1;
    editing = 0;
    read_mode();                                     /* (starting: the saved layout; ending: as touch_layout_text) */
    editing = on;
    selected = NONE;
    nundo = nredo = 0;
    pthread_mutex_unlock(&lock);
}

int touch_editing(void) {
    return editing;
}

void touch_layout_text(char *out, size_t size) {
    pthread_mutex_lock(&lock);
    int moved = !grouped;
    for (int k = 0; k < SLOTS; k++)
        moved |= fabsf(off[k][0]) > 0.001f || fabsf(off[k][1]) > 0.001f || fabsf(scl[k] - 1) > 0.001f;
    out[0] = 0;
    size_t n = moved ? (size_t)snprintf(out, size, "%d", grouped) : 0;
    for (int k = 0; moved && k < SLOTS && n < size; k++)
        n += (size_t)snprintf(out + n, size - n, ";%.3f,%.3f,%.3f", off[k][0], off[k][1], scl[k]);
    pthread_mutex_unlock(&lock);
}

void touch_input(InputState *s) {
    pthread_mutex_lock(&lock);
    if (visible() && !editing) {
        for (int i = 0; i < nfingers; i++)
            if (fingers[i].what >= 0 && fingers[i].what < TS_COUNT) s->buttons |= bits[fingers[i].what];
        if (stick_held) {
            s->lx = (int32_t)lrintf(stick_x * 32767);
            s->ly = (int32_t)lrintf(stick_y * 32767);
        } else if (dpad_x || dpad_y) {               /* (the stick all the way that way: diagonals as far) */
            float k = dpad_x && dpad_y ? 0.7071f : 1;
            s->lx = (int32_t)lrintf(dpad_x * k * 32767);
            s->ly = (int32_t)lrintf(dpad_y * k * 32767);
        }
    }
    pthread_mutex_unlock(&lock);
}

int touch_view(int w, int h, TouchView *v) {
    float W = (float)w, u = (float)h;
    pthread_mutex_lock(&lock);
    int on = visible();
    if (on) {
        int held[SLOTS + TS_COUNT] = { 0 };          /* (what each finger holds or drags: drawn pressed) */
        for (int i = 0; i < nfingers; i++) {
            int wh = fingers[i].what;
            if (wh >= GRAB) held[wh - GRAB == 0 ? TS_DPAD : wh - GRAB - 1] = 1;
            else if (wh >= 0) held[wh] = 1;
        }
        if (editing && selected != NONE) {           /* (the selected control, or A B X Y, shown pressed) */
            int group = grouped && is_face_slot(selected);
            for (int k = group ? SLOT(TS_FACE_A) : selected; k <= (group ? SLOT(TS_FACE_Y) : selected); k++)
                held[k == 0 ? TS_DPAD : k - 1] = 1;
        }
        held[TS_GROUP] |= grouped;
        v->count = 0;
        v->opacity = editing ? 1 : opacity;          /* (the editor shows them solid) */
        for (int c = TS_FACE_A; c <= TS_MENU; c++) {
            Place p = place(c, W, u);
            v->items[v->count++] = (TouchItem){ c, (int)lrintf(p.x), (int)lrintf(p.y), held[c], 0 };
        }
        Place rest = place(TS_DPAD, W, u);
        int cx = (int)lrintf(rest.x), cy = (int)lrintf(rest.y);
        if (use_stick) {
            int bx = stick_held ? (int)lrintf(base_x) : cx, by = stick_held ? (int)lrintf(base_y) : cy;
            int on_stick = stick_held || held[TS_DPAD];
            v->items[v->count++] = (TouchItem){ TS_BASE, bx, by, on_stick, 0 };   /* (the stick's knob: as its base) */
            v->items[v->count++] = (TouchItem){ TS_KNOB, stick_held ? (int)lrintf(knob_x) : bx,
                                                stick_held ? (int)lrintf(knob_y) : by, on_stick, 0 };
        } else {                                     /* the d-pad, and over it the arms pressed (half an arm out) */
            int half = (int)lrintf(DPAD_R * u * scl[0] / 2);
            v->items[v->count++] = (TouchItem){ TS_DPAD, cx, cy, held[TS_DPAD] && editing, 0 };
            if (dpad_y) v->items[v->count++] = (TouchItem){ TS_ARM_V, cx, cy - dpad_y * half, 1, dpad_y < 0 ? 2 : 0 };
            if (dpad_x) v->items[v->count++] = (TouchItem){ TS_ARM_H, cx + dpad_x * half, cy, 1, dpad_x < 0 ? 1 : 0 };
        }
        for (int c = TS_RESET; editing && c <= TS_REDO; c++) {
            Place p = place(c, W, u);
            v->items[v->count++] = (TouchItem){ c, (int)lrintf(p.x), (int)lrintf(p.y), held[c], 0 };
        }
        v->face_r = (int)lrintf(FACE_R * u);
        v->small_r = (int)lrintf(SMALL_R * u);
        v->pill_w = (int)lrintf(PILL_W * u);
        v->pill_h = (int)lrintf(PILL_H * u);
        v->base_r = (int)lrintf(BASE_R * u);
        v->knob_r = (int)lrintf(KNOB_R * u);
        v->dpad_r = (int)lrintf(DPAD_R * u);
        v->arm_w = (int)lrintf(ARM_W * u);
        v->wide_w = (int)lrintf(WIDE_W * u);
        for (int c = 0; c < TS_COUNT; c++)
            v->scale[c] = c <= TS_MENU ? scl[SLOT(c)] : c <= TS_ARM_H ? scl[0] : 1;
    }
    pthread_mutex_unlock(&lock);
    return on;
}
