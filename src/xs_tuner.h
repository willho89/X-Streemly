/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tuner thread (CLAUDE.md §5.2, D-001). Every call here is safe from the
 * emulation thread: none of them waits on the network. Only xs_tuner_free()
 * joins the thread, and the interrupt callback makes that prompt.
 */
#ifndef XS_TUNER_H
#define XS_TUNER_H

#include <stdint.h>

#include "xs_ctx.h"
#include "xs_source.h"

typedef enum
{
   XS_TUNER_IDLE = 0,
   XS_TUNER_CONNECTING,
   XS_TUNER_PLAYING,     /* open, packets flowing */
   XS_TUNER_FAILED,      /* see status.msg */
   XS_TUNER_ENDED        /* source reached its end (VOD, or a dead live feed) */
} xs_tuner_state_t;

typedef struct
{
   xs_tuner_state_t state;
   unsigned gen;          /* the tune() this status belongs to */
   char     msg[256];     /* PLAYING: stream summary; FAILED: reason */
   uint64_t packets;
   uint64_t bytes;
} xs_tuner_status_t;

xs_tuner_t *xs_tuner_new(xs_ctx_t *ctx);
/* Interrupts and joins the thread. */
void xs_tuner_free(xs_tuner_t *t);

/* Switch to src (copied). Returns the generation number of this tune. */
unsigned xs_tuner_tune(xs_tuner_t *t, const xs_source_t *src);
void xs_tuner_stop(xs_tuner_t *t);
void xs_tuner_get_status(xs_tuner_t *t, xs_tuner_status_t *out);

#endif
