/*
 * touch.c - the on-screen controls (touch.h): where they sit, which finger holds what.
 *
 * Sizes and places are fractions of the screen's height u (a landscape screen: 720 or 1080 pixels on most phones),
 * so they keep their size on the panel whatever its shape; a wider phone only spreads the two sides further apart.
 * A finger keeps what it first touched: the stick, wherever it lands in the stick's area (the stick's base moves
 * there: no need to find it by feel), or the nearest button within reach. On the face buttons a finger may slide
 * from one to the next. Called from the main thread (fingers) and the game's present (touch_view): under a lock.
 */
#include "touch.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FINGERS 10
#define NONE (-1)

/* the layout, in units of u (the screen's height) from the left / right / top / bottom edges */
#define FACE_R    0.060f       /* A B X Y: radius */
#define FACE_GAP  0.118f       /* centre to each of them */
#define FACE_X    0.245f       /* the four's centre, from the right edge */
#define FACE_Y    0.290f       /*   from the bottom */
#define PILL_W    0.190f       /* L R ZL ZR */
#define PILL_H    0.085f
#define PILL_X    0.150f       /* centre from the side */
#define PILL_Y1   0.095f       /* ZL / ZR from the top */
#define PILL_Y2   0.200f       /* L / R */
#define SMALL_R   0.040f       /* - menu + */
#define SMALL_GAP 0.110f
#define SMALL_Y   0.070f
#define BASE_R    0.150f       /* the stick: its ring, its knob, its rest place */
#define KNOB_R    0.068f
#define BASE_X    0.270f
#define BASE_Y    0.300f
#define STICK_AREA_X 0.45f     /* the stick's area: the left part of the screen (a fraction of its width) ... */
#define STICK_AREA_TOP 0.27f   /*   below the shoulder buttons (u) */
#define REACH     1.35f        /* a button counts this far out (times its size): fingers are not exact */

static const uint64_t bits[TS_COUNT] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_MINUS, BTN_PLUS,
                                         0, 0, 0 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t id; int what; float x, y; } fingers[MAX_FINGERS];   /* what: a TS_ control, or NONE */
static int nfingers;
static float base_x, base_y, knob_x, knob_y;          /* the held stick, in pixels (of the last finger's screen) */
static float stick_x, stick_y;                         /* and where it points: -1..1, y up */
static int stick_held, mode = -1, shown, present = -1;

static void read_mode(void) {
    if (mode >= 0) return;
    const char *m = getenv("LAA_TOUCH");
    mode = m && *m == '1' ? 2 : m && *m == '0' ? 0 : 1;   /* 2 always, 1 auto, 0 never */
}

static int visible(void) {
    read_mode();
    return mode == 2 || (mode == 1 && shown);
}

typedef struct { float x, y, hw, hh; int round; } Place;   /* centre, half sizes */

static Place place(int c, float w, float u) {
    float cx = w - FACE_X * u, cy = u - FACE_Y * u, f = FACE_R * u, g = FACE_GAP * u;
    float pw = PILL_W * u / 2, ph = PILL_H * u / 2, s = SMALL_R * u, sg = SMALL_GAP * u;
    switch (c) {
    case TS_FACE_A: return (Place){ cx + g, cy, f, f, 1 };
    case TS_FACE_B: return (Place){ cx, cy + g, f, f, 1 };
    case TS_FACE_X: return (Place){ cx, cy - g, f, f, 1 };
    case TS_FACE_Y: return (Place){ cx - g, cy, f, f, 1 };
    case TS_ZL: return (Place){ PILL_X * u, PILL_Y1 * u, pw, ph, 0 };
    case TS_L: return (Place){ PILL_X * u, PILL_Y2 * u, pw, ph, 0 };
    case TS_ZR: return (Place){ w - PILL_X * u, PILL_Y1 * u, pw, ph, 0 };
    case TS_R: return (Place){ w - PILL_X * u, PILL_Y2 * u, pw, ph, 0 };
    case TS_MINUS: return (Place){ w / 2 - sg, SMALL_Y * u, s, s, 1 };
    case TS_MENU: return (Place){ w / 2, SMALL_Y * u, s, s, 1 };
    case TS_PLUS: return (Place){ w / 2 + sg, SMALL_Y * u, s, s, 1 };
    default: return (Place){ BASE_X * u, u - BASE_Y * u, BASE_R * u, BASE_R * u, 1 };
    }
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

static void move_knob(float x, float y, float u) {
    float dx = x - base_x, dy = y - base_y, d = sqrtf(dx * dx + dy * dy), max = BASE_R * u;
    if (d > max) dx *= max / d, dy *= max / d;
    knob_x = base_x + dx;
    knob_y = base_y + dy;
    float m = sqrtf(dx * dx + dy * dy) / max;          /* (a small dead zone in the middle, then all the way) */
    float k = m < 0.12f ? 0 : (m - 0.12f) / (0.88f * m);
    stick_x = dx / max * k;
    stick_y = -dy / max * k;
}

int touch_finger(int type, uint64_t id, float x, float y, int w, int h) {
    float W = (float)w, u = (float)h, px = x * W, py = y * u;
    int tapped_menu = 0;
    pthread_mutex_lock(&lock);
    int i = 0;
    while (i < nfingers && fingers[i].id != id) i++;
    if (type == TOUCH_DOWN) {
        shown = 1;
        if (i == nfingers && nfingers < MAX_FINGERS) {
            int what = button_at(px, py, W, u, TS_FACE_A, TS_MENU);
            if (what == NONE && !stick_held && px < STICK_AREA_X * W && py > STICK_AREA_TOP * u) {
                what = TS_BASE;                      /* the stick, its base where the finger landed (on the screen) */
                float r = BASE_R * u;
                base_x = px < r ? r : px;
                base_y = py > u - r ? u - r : py;
                stick_held = 1;
                move_knob(px, py, u);
            }
            fingers[nfingers++] = (__typeof__(fingers[0])){ id, what, x, y };
        }
    } else if (i < nfingers) {
        fingers[i].x = x;
        fingers[i].y = y;
        int what = fingers[i].what;
        if (type == TOUCH_MOVE && what == TS_BASE) move_knob(px, py, u);
        else if (type == TOUCH_MOVE && what >= TS_FACE_A && what <= TS_FACE_Y) {   /* (sliding between them) */
            int now = button_at(px, py, W, u, TS_FACE_A, TS_FACE_Y);
            if (now != NONE) fingers[i].what = now;
        }
        if (type == TOUCH_UP) {
            if (what == TS_BASE) stick_held = 0;
            if (what == TS_MENU) tapped_menu = 1;
            fingers[i] = fingers[--nfingers];
        }
    }
    int on = visible();
    pthread_mutex_unlock(&lock);
    return tapped_menu && on;
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
    mode = -1;                                       /* (the setting may change in the menu: read again) */
    pthread_mutex_unlock(&lock);
}

void touch_input(InputState *s) {
    pthread_mutex_lock(&lock);
    if (visible()) {
        for (int i = 0; i < nfingers; i++)
            if (fingers[i].what >= 0) s->buttons |= bits[fingers[i].what];
        if (stick_held) {
            s->lx = (int32_t)lrintf(stick_x * 32767);
            s->ly = (int32_t)lrintf(stick_y * 32767);
        }
    }
    pthread_mutex_unlock(&lock);
}

int touch_view(int w, int h, TouchView *v) {
    float W = (float)w, u = (float)h;
    pthread_mutex_lock(&lock);
    int on = visible();
    if (on) {
        v->count = 0;
        for (int c = TS_FACE_A; c <= TS_MENU; c++) {
            Place p = place(c, W, u);
            int pressed = 0;
            for (int i = 0; i < nfingers; i++) pressed |= fingers[i].what == c;
            v->items[v->count++] = (TouchItem){ c, (int)lrintf(p.x), (int)lrintf(p.y), pressed };
        }
        Place rest = place(TS_BASE, W, u);
        float bx = stick_held ? base_x : rest.x, by = stick_held ? base_y : rest.y;
        v->items[v->count++] = (TouchItem){ TS_BASE, (int)lrintf(bx), (int)lrintf(by), stick_held };
        v->items[v->count++] = (TouchItem){ TS_KNOB, (int)lrintf(stick_held ? knob_x : bx),
                                            (int)lrintf(stick_held ? knob_y : by), stick_held };
        v->face_r = (int)lrintf(FACE_R * u);
        v->small_r = (int)lrintf(SMALL_R * u);
        v->pill_w = (int)lrintf(PILL_W * u);
        v->pill_h = (int)lrintf(PILL_H * u);
        v->base_r = (int)lrintf(BASE_R * u);
        v->knob_r = (int)lrintf(KNOB_R * u);
    }
    pthread_mutex_unlock(&lock);
    return on;
}
