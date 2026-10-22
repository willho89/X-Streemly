/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Video thread (CLAUDE.md §5.4, D-001, D-005): decode, deinterlace,
 * convert to XRGB8888 at native resolution into a ring of frame slots. The
 * presenter takes frames by PTS and hands the slot's memory straight to the
 * frontend; a slot stays reserved while it is the picture on screen.
 */
#ifndef XS_VIDEO_H
#define XS_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>

#include "xs_ctx.h"
#include "xs_pktq.h"

typedef struct xs_video xs_video_t;

typedef struct
{
   const uint32_t *data;
   unsigned width, height;
   size_t   pitch;        /* bytes */
   float    aspect;       /* display aspect ratio */
   int64_t  pts_us;
   unsigned slot;         /* internal */
} xs_frame_t;

typedef struct
{
   uint64_t decoded;      /* pictures out of the decoder (after deinterlace) */
   uint64_t dropped_early;/* late before conversion */
   uint64_t converted;
   double   convert_ms;   /* mean swscale time per converted frame */
   double   decode_ms;    /* mean send+receive time per packet */
} xs_video_stats_t;

/* Starts the thread; it takes packets from q. */
xs_video_t *xs_video_new(xs_ctx_t *ctx, xs_pktq_t *q);
void xs_video_free(xs_video_t *v);

/* Tuner: the stream that packets with this serial belong to. Call before
 * queueing its first packet. */
void xs_video_configure(xs_video_t *v, unsigned serial, const AVStream *st, AVRational frame_rate);
/* Drop every ready frame (not the one on screen); accept only serial now. */
void xs_video_flush(xs_video_t *v, unsigned serial);

/* Presenter. */
void xs_video_set_options(xs_video_t *v, xs_deint_t deint, unsigned max_height);
/* Presentation clock, for dropping late frames before conversion. */
void xs_video_set_clock(xs_video_t *v, unsigned serial, int64_t clock_us);
/* Oldest ready frame's PTS. */
bool xs_video_head_pts(xs_video_t *v, unsigned serial, int64_t *pts_us);
/* Make the newest ready frame with pts <= until_us the one on screen,
 * releasing the previous one and any older ready frames. True if the
 * picture changed; *out is the picture on screen. */
bool xs_video_advance(xs_video_t *v, unsigned serial, int64_t until_us, xs_frame_t *out,
                      unsigned *dropped);
/* Picture on screen, if any (still valid until the next advance/flush). */
bool xs_video_current(xs_video_t *v, xs_frame_t *out);
unsigned xs_video_ready_count(xs_video_t *v, unsigned serial);
void xs_video_get_stats(xs_video_t *v, xs_video_stats_t *out);

#endif
