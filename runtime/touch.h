/*
 * touch.h - on-screen controls, for phones without a controller: a d-pad (or a stick) on the left, A B X Y on the
 * right, the shoulder buttons in the top corners, - and + with ZLAA's menu between them at the top. The host hands it
 * the screen's fingers (main.c); it gives the buttons and the stick they hold (touch_input, merged with the
 * controller's) and what to draw (touch_view, render.c).
 *
 * Settings (environment variables, from the menu):
 *   LAA_TOUCH         unset = Auto (shown while there is no controller, or once the screen is touched; hidden again
 *                     when a controller is used), 1 = always, 0 = never
 *   LAA_TOUCH_STICK   1: Link moves by a stick; unset: by a d-pad (eight directions, the stick pushed all the way)
 *   LAA_TOUCH_FIXED   1: the stick stays in its place; unset: it appears where the thumb lands
 *   LAA_TOUCH_LAYOUT  where the player put each control and how big (touch_edit; unset = the defaults)
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stddef.h>
#include <stdint.h>
#include "input.h"

enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };
enum { TOUCH_NOTHING, TOUCH_MENU_TAPPED, TOUCH_EDIT_DONE };

/* A finger (id: any number that stays the same while it is down) at x, y (0..1 of the screen's width and height),
 * on a screen w x h pixels. Returns TOUCH_MENU_TAPPED (open ZLAA's pause menu) or, while editing, TOUCH_EDIT_DONE
 * (Done tapped: save touch_layout_text, then touch_edit(0)). */
int touch_finger(int type, uint64_t id, float x, float y, int w, int h);
/* A controller was used (Auto hides the controls), or is connected at all (Auto shows them while there is none) */
void touch_controller_used(void);
void touch_controller_present(int present);
/* All fingers lifted, nothing held, the settings read again (the pause menu opened, the app went to the background) */
void touch_release(void);
/* What the fingers hold: buttons OR'ed into s, the stick replacing s's left stick while it is held (nothing while
 * editing) */
void touch_input(InputState *s);

/* Editing the layout: every control shown; a finger selects and drags the one it touches; a toolbar in the middle:
 * Reset (all back), smaller and bigger (the selected one), ABXY (the four face buttons as one, or each alone),
 * Done (the caller saves the layout). touch_layout_text: the layout as LAA_TOUCH_LAYOUT's text ("" = the
 * defaults). */
void touch_edit(int on);
int touch_editing(void);
void touch_layout_text(char *out, size_t size);

/* What to draw, on a screen w x h pixels: each control's sprite, centre and whether it is pressed */
/* (TS_ARM_V / TS_ARM_H: a pressed arm of the d-pad, up and right, mirrored for down and left; TS_RESET .. TS_GROUP:
 * the editor's toolbar) */
enum { TS_FACE_A, TS_FACE_B, TS_FACE_X, TS_FACE_Y, TS_L, TS_R, TS_ZL, TS_ZR, TS_MINUS, TS_PLUS, TS_MENU, TS_BASE,
       TS_KNOB, TS_DPAD, TS_ARM_V, TS_ARM_H, TS_RESET, TS_DONE, TS_SMALLER, TS_BIGGER, TS_GROUP, TS_COUNT };
typedef struct { int sprite, x, y, pressed, flip; } TouchItem;   /* flip: 1 mirrored left-right, 2 upside down */
typedef struct {
    int count;
    TouchItem items[TS_COUNT + 2];
    int face_r, small_r, pill_w, pill_h, base_r, knob_r;   /* the sprites' sizes in pixels, for this screen */
    int dpad_r, arm_w, wide_w;                             /* (the d-pad: middle to an arm's end, an arm's width;
                                                              the editor's wide buttons' width) */
    float scale[TS_COUNT];                                 /* each sprite's size, times the above (the player's) */
} TouchView;
/* 0 if the controls are hidden; else 1 and v filled */
int touch_view(int w, int h, TouchView *v);

#endif
