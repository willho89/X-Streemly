/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bounded packet queue between the tuner and the video thread. Every packet
 * carries the serial of the source attempt it belongs to, like ffplay's
 * PacketQueue (fftools/ffplay.c @ n8.1.3); the consumer flushes its decoder
 * when the serial changes.
 */
#ifndef XS_PKTQ_H
#define XS_PKTQ_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/packet.h>

#include "xs_ctx.h"

typedef struct xs_pktq xs_pktq_t;

/* Full once it holds max_dur_us of media or max_bytes. */
xs_pktq_t *xs_pktq_new(int64_t max_dur_us, size_t max_bytes);
void xs_pktq_free(xs_pktq_t *q);

/* Drop everything queued. */
void xs_pktq_flush(xs_pktq_t *q);
/* Wake and fail every xs_pktq_get (shutdown). */
void xs_pktq_abort(xs_pktq_t *q);

/* Move pkt into the queue, waiting while it is full until stop(arg) says to
 * give up. dur_us is the packet's duration (0 if unknown). Returns false if
 * it gave up (pkt is then unreferenced). */
bool xs_pktq_put(xs_pktq_t *q, AVPacket *pkt, unsigned serial, int64_t dur_us,
                 xs_abort_fn stop, void *arg);

/* Take the oldest packet, waiting up to timeout_us. 1 = got one, 0 = timed
 * out, -1 = aborted. */
int xs_pktq_get(xs_pktq_t *q, AVPacket *pkt, unsigned *serial, int64_t timeout_us);

void xs_pktq_stats(xs_pktq_t *q, unsigned *count, int64_t *dur_us, size_t *bytes);

#endif
