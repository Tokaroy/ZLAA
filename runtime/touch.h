/*
 * touch.h - on-screen controls, for phones without a controller: a stick on the left, A B X Y on the right, the
 * shoulder buttons in the top corners, - and + with ZLAA's menu between them at the top. The host hands it the
 * screen's fingers (main.c); it gives the buttons and the stick they hold (touch_input, merged with the
 * controller's) and what to draw (touch_view, render.c).
 *
 * LAA_TOUCH: unset = Auto (shown while there is no controller, or once the screen is touched; hidden again when a
 * controller button is pressed), 1 = always, 0 = never.
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include "input.h"

enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

/* A finger (id: any number that stays the same while it is down) at x, y (0..1 of the screen's width and height),
 * on a screen w x h pixels. Returns 1 if it tapped the menu button (open ZLAA's pause menu). */
int touch_finger(int type, uint64_t id, float x, float y, int w, int h);
/* A controller was used (Auto hides the controls), or is connected at all (Auto shows them while there is none) */
void touch_controller_used(void);
void touch_controller_present(int present);
/* All fingers lifted, nothing held (the pause menu opened, the app went to the background) */
void touch_release(void);
/* What the fingers hold: buttons OR'ed into s, the stick replacing s's left stick while it is held */
void touch_input(InputState *s);

/* What to draw, on a screen w x h pixels: each control's sprite, centre and whether it is pressed */
enum { TS_FACE_A, TS_FACE_B, TS_FACE_X, TS_FACE_Y, TS_L, TS_R, TS_ZL, TS_ZR, TS_MINUS, TS_PLUS, TS_MENU, TS_BASE,
       TS_KNOB, TS_COUNT };
typedef struct { int sprite, x, y, pressed; } TouchItem;
typedef struct {
    int count;
    TouchItem items[TS_COUNT];
    int face_r, small_r, pill_w, pill_h, base_r, knob_r;   /* the sprites' sizes in pixels, for this screen */
} TouchView;
/* 0 if the controls are hidden; else 1 and v filled */
int touch_view(int w, int h, TouchView *v);

#endif
