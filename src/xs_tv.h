/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The TV (CLAUDE.md §5.9, §5.10): remote-control input, channel changing,
 * and what is drawn over the picture. Runs on the emulation thread; tuning
 * goes through the tuner, so nothing here waits on the network.
 */
#ifndef XS_TV_H
#define XS_TV_H

#include "xs_chan.h"
#include "xs_ctx.h"
#include "xs_osd.h"
#include "xs_present.h"
#include "xs_tuner.h"

typedef struct xs_tv xs_tv_t;

/* Takes over *chan. */
xs_tv_t *xs_tv_new(xs_ctx_t *ctx, xs_tuner_t *tuner, xs_present_t *present, xs_chan_t *chan,
                   bool bitmasks);
void xs_tv_free(xs_tv_t *tv);

/* Tune the lineup's current channel (load, reset). */
void xs_tv_start(xs_tv_t *tv);

/* One tick: read input, change channel, update the overlay. pic_h is the
 * height of the picture on screen (the overlay is laid out for it). */
void xs_tv_tick(xs_tv_t *tv, retro_input_state_t input_cb, double fps, unsigned pic_h);

const xs_overlay_t *xs_tv_overlay(const xs_tv_t *tv);

#endif
