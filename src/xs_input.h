/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Input (CLAUDE.md §5.9): RetroPad buttons and keyboard digits as TV
 * remote actions. Edge-triggered; the D-pad and L/R auto-repeat after
 * 400 ms, like DeeVee's dv_input.c.
 */
#ifndef XS_INPUT_H
#define XS_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include <libretro.h>

typedef enum
{
   XS_ACT_UP = 1,       /* D-pad up: channel up / list cursor up */
   XS_ACT_DOWN,
   XS_ACT_NEXT_GROUP,   /* R: next group / list page down */
   XS_ACT_PREV_GROUP,   /* L: previous group / list page up */
   XS_ACT_OK,           /* A: info banner / tune the list cursor */
   XS_ACT_BACK,         /* B: last channel / close the list */
   XS_ACT_LIST,         /* Select: channel list */
   XS_ACT_ENTER,        /* keyboard Enter: tune the digits typed */
   XS_ACT_FAV,          /* X: favourite on/off */
   XS_ACT_NEXT_FAV,     /* Y: next favourite */
   XS_ACT_DOT,          /* keyboard . or -: major.minor channel numbers */
   XS_ACT_DIGIT0        /* + 0..9: keyboard digits */
} xs_action_t;

#define XS_MAX_ACTIONS 16

typedef struct
{
   uint32_t prev;
   unsigned held[16];
   uint32_t keys_prev;   /* digits 0-9, Enter */
   bool     bitmasks;
} xs_input_t;

void xs_input_init(xs_input_t *in, bool bitmasks);
void xs_input_set_descriptors(retro_environment_t environ_cb);

/* Read this tick's input. Returns the number of actions in acts. *zapping
 * is true while a channel-change button is held. */
unsigned xs_input_poll(xs_input_t *in, retro_input_state_t state_cb, double fps,
                       unsigned *acts, bool *zapping);

#endif
