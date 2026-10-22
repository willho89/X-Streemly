/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Audio (CLAUDE.md §5.5, §5.6). Two halves:
 *  - xs_aring: decoded 48 kHz stereo s16 PCM as PTS-stamped chunks (DeeVee's
 *    chunk model), written by the tuner thread, read by the presenter. The
 *    samples handed to the frontend *are* the clock: the presentation time
 *    is the timestamp of the next unread sample.
 *  - xs_adec: decoder + swresample on the tuner thread. Applies the
 *    presenter's rate correction with swr_set_compensation (ffplay
 *    synchronize_audio / audio_decode_frame @ n8.1.3), D-007.
 */
#ifndef XS_AUDIO_H
#define XS_AUDIO_H

#include <stdint.h>

#include <libavformat/avformat.h>

#include "xs_ctx.h"

typedef struct xs_aring xs_aring_t;

xs_aring_t *xs_aring_new(unsigned seconds);
void xs_aring_free(xs_aring_t *r);

/* Drop everything; only data written with this serial is accepted after. */
void xs_aring_flush(xs_aring_t *r, unsigned serial);

/* Producer. pts_us: timeline time of the first sample; src_us: how much
 * source time the chunk covers (differs from frames/48 kHz while the rate is
 * being corrected). Waits while full until stop(arg). False = gave up. */
bool xs_aring_write(xs_aring_t *r, unsigned serial, const int16_t *pcm, unsigned frames,
                    int64_t pts_us, int64_t src_us, xs_abort_fn stop, void *arg);

/* Consumer (presenter). All take the serial the caller is presenting and
 * report nothing for any other. */
int64_t  xs_aring_level_us(xs_aring_t *r, unsigned serial);
/* Timestamp of the next unread sample. False if empty. */
bool     xs_aring_clock(xs_aring_t *r, unsigned serial, int64_t *clock_us);
/* Read up to frames stereo frames; returns how many were read. */
unsigned xs_aring_read(xs_aring_t *r, unsigned serial, int16_t *out, unsigned frames);
/* Drop about us of the oldest audio (whole chunks); returns what was dropped. */
int64_t  xs_aring_skip(xs_aring_t *r, unsigned serial, int64_t us);

/* Playback speed the presenter wants (1.0 = nominal, >1 drains the buffer). */
void   xs_aring_set_ratio(xs_aring_t *r, double ratio);
double xs_aring_ratio(xs_aring_t *r);

typedef struct xs_adec xs_adec_t;

xs_adec_t *xs_adec_open(xs_ctx_t *ctx, const AVStream *st);
void xs_adec_free(xs_adec_t *d);
/* Decode one packet (timestamps already on the tuner's timeline) into the
 * ring. False if it gave up waiting for space. */
bool xs_adec_packet(xs_adec_t *d, const AVPacket *pkt, xs_aring_t *r, unsigned serial,
                    xs_abort_fn stop, void *arg);

#endif
