/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Presenter (CLAUDE.md §5.5, D-007, D-008): runs on the emulation thread,
 * once per retro_run, and never waits. Buffers a tune up to the start
 * target, then plays: the audio ring is the clock, the newest due frame is
 * the picture, and a slow controller nudges the playback rate so the buffer
 * floor stays near the target whatever the source and display clocks do.
 */
#ifndef XS_PRESENT_H
#define XS_PRESENT_H

#include <stddef.h>
#include <stdint.h>

#include "xs_ctx.h"
#include "xs_tuner.h"

typedef struct
{
   const uint32_t *data;
   unsigned width, height;
   size_t   pitch;
   float    aspect;
   bool     dupe;      /* same picture as last tick */
} xs_picture_t;

typedef struct xs_present xs_present_t;

xs_present_t *xs_present_new(xs_ctx_t *ctx, xs_tuner_t *tuner);
void xs_present_free(xs_present_t *p);

/* Channel name for the cards ("" = none). */
void xs_present_set_name(xs_present_t *p, const char *name);

/* One tick: frames stereo frames into audio, and the picture to show. */
void xs_present_tick(xs_present_t *p, double fps, int16_t *audio, unsigned frames,
                     xs_picture_t *pic);

/* The last tick showed a card (tuning, no signal ...), not the stream. */
bool xs_present_card_showing(const xs_present_t *p);

/* Log a summary line (also done every minute while playing). */
void xs_present_log_stats(xs_present_t *p);

#endif
