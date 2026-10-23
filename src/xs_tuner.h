/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tuner thread (CLAUDE.md §5.2, D-001). Every call here is safe from the
 * emulation thread: none of them waits on the network. Only xs_tuner_free()
 * joins threads, and the interrupt callback makes that prompt.
 *
 * The tuner owns the media pipeline: the video packet queue, the video
 * thread and the audio ring. Each attempt to play a source has its own
 * serial; everything it produces is tagged with it, so the presenter can
 * tell a new attempt (re-buffer) from more of the same.
 */
#ifndef XS_TUNER_H
#define XS_TUNER_H

#include <stdint.h>

#include "xs_audio.h"
#include "xs_ctx.h"
#include "xs_source.h"
#include "xs_video.h"

/* Audio tracks decoded at once, so switching is instant (§M4). */
#define XS_MAX_TRACKS 4

typedef enum
{
   XS_TUNER_IDLE = 0,
   XS_TUNER_CONNECTING,
   XS_TUNER_PLAYING,     /* open, packets flowing */
   XS_TUNER_FAILED,      /* see status.msg; retrying unless it was fixed-length */
   XS_TUNER_ENDED        /* fixed-length source reached its end */
} xs_tuner_state_t;

typedef struct
{
   xs_tuner_state_t state;
   unsigned gen;          /* the tune() this status belongs to */
   unsigned serial;       /* the attempt; changes on every (re)open */
   char     msg[256];     /* PLAYING: stream summary; FAILED: reason */
   bool     has_audio, has_video;
   bool     live;
   unsigned height;         /* the chosen video's height */
   bool     can_step_down;  /* a lower HLS variant exists */
   unsigned audio_tracks;   /* decoded audio tracks, each into its own ring */
   unsigned audio_default;  /* the one to start with (Audio Language option) */
   char     audio_lang[XS_MAX_TRACKS][8];   /* language tags ("eng"), "" if none */
   bool     audio_described[XS_MAX_TRACKS];
   uint64_t packets;
   uint64_t bytes;
} xs_tuner_status_t;

xs_tuner_t *xs_tuner_new(xs_ctx_t *ctx);
/* Interrupts and joins the threads. */
void xs_tuner_free(xs_tuner_t *t);

/* Switch to src (copied). Returns the generation number of this tune. */
unsigned xs_tuner_tune(xs_tuner_t *t, const xs_source_t *src);
void xs_tuner_stop(xs_tuner_t *t);
void xs_tuner_get_status(xs_tuner_t *t, xs_tuner_status_t *out);
/* Variant choice and output cap (0 = native); applies from the next open. */
void xs_tuner_set_max_height(xs_tuner_t *t, unsigned max_height);
/* Preferred audio language (ISO 639 code, "" = the stream's default),
 * from the next open. */
void xs_tuner_set_audio_lang(xs_tuner_t *t, const char *lang);
/* The chosen HLS variant can't keep up: reopen with the next lower one,
 * keeping what is buffered (a reconnect). Ignored if there is none. */
void xs_tuner_step_down(xs_tuner_t *t);

/* The ring of audio track i (0 .. status.audio_tracks - 1). */
xs_aring_t *xs_tuner_aring(xs_tuner_t *t, unsigned i);
xs_video_t *xs_tuner_video(xs_tuner_t *t);

#endif
