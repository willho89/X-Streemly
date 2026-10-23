/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Presenter (see xs_present.h).
 *
 * Clock: the timestamp of the next audio sample to be played. Each tick
 * emits exactly the tick's samples and shows the newest frame due by the
 * middle of the tick. Without audio, the clock advances by the tick.
 *
 * Rate control (D-007): live sources deliver in bursts (HLS segments), so
 * the buffer level is a saw-tooth; its *floor* (minimum over the last 20 s)
 * is what must stay near the target. A proportional controller sets the
 * playback ratio (at most ±0.5 %, inaudible), applied by the audio
 * resampler; see ffplay check_external_clock_speed() for the idea.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/time.h>

#include "xs_osd.h"
#include "xs_present.h"

#define XS_FADE          64
#define XS_CARD_W        640
#define XS_CARD_H        360
#define XS_FLOOR_BUCKETS 20          /* seconds of floor history */
#define XS_RATIO_MAX     0.005
#define XS_KP            0.002       /* ratio per second of floor error */
#define XS_STATS_EVERY_S 60
#define XS_NO_SIGNAL_S   1.0         /* stall this long before saying so */
/* Live HLS starts several segments back from the live edge (CNBC UK: 18 s
 * of 6 s segments, fetched one after another over the first seconds), far
 * more than the rate controller can work off. When a full window's floor is
 * this far over target, skip ahead to target + KEEP. The floor spans at
 * least one segment arrival, so the skip can't cut into the wait for the
 * next one (the instantaneous level could). */
#define XS_CATCHUP_EXCESS_US 3000000
#define XS_CATCHUP_KEEP_US   500000
/* Adaptive jitter buffer (D-022). Live HLS arrives a segment at a time,
 * and FFmpeg finds each new segment up to half a target duration late, so
 * the floor target grows with half the burst size seen (the level's
 * peak-to-trough over the window) and by a second after every underrun. */
#define XS_UNDERRUN_STEP_US  1000000
/* Two underruns this close together on a live HLS stream with a lower
 * variant: the variant needs more bandwidth than there is, step down. */
#define XS_STEP_DOWN_WITHIN_S 60.0
#define XS_EXTRA_MAX_US      10000000

typedef enum { P_WAIT = 0, P_BUFFERING, P_PLAYING } pstate_t;

struct xs_present
{
   xs_ctx_t   *ctx;
   xs_tuner_t *tuner;
   char        name[256];
   char        idle_title[64], idle_detail[256];

   unsigned  gen, serial;
   unsigned  gen_seen;        /* the tune timed by tune_us */
   pstate_t  state;
   bool      has_audio, has_video;
   unsigned  track, tracks;   /* the audio track played (its ring), of how many */
   xs_agc_t  agc;
   bool      have_frame;      /* a stream frame is on screen */
   bool      started;         /* this serial played at least once */
   bool      pictured;        /* first picture of this tune shown */
   int64_t   tune_us;         /* when this tune or attempt began */
   int64_t   clock_us;
   unsigned  fade_in;
   uint64_t  ticks;
   uint64_t  state_ticks;     /* ticks in the current state */

   /* controller */
   int64_t   bucket_min[XS_FLOOR_BUCKETS];
   int64_t   bucket_max[XS_FLOOR_BUCKETS];
   int64_t   burst_us;        /* peak-to-trough over the window */
   int64_t   extra_us;        /* added after underruns (this tune) */
   unsigned  bucket;
   unsigned  buckets_full;    /* seconds of history since (re)start/skip */
   uint64_t  bucket_tick;
   double    ratio;
   int64_t   floor_us;

   /* stats */
   unsigned  underruns, shown, dropped;
   uint64_t  last_underrun_tick;
   uint64_t  last_stats_tick;
   int64_t   last_stats_us;

   /* card */
   uint32_t  card[XS_CARD_W * XS_CARD_H];
   char      card_key[512];
   bool      card_on_screen;
};

xs_present_t *xs_present_new(xs_ctx_t *ctx, xs_tuner_t *tuner)
{
   xs_present_t *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   p->ctx   = ctx;
   p->tuner = tuner;
   p->ratio = 1.0;
   xs_agc_reset(&p->agc);
   xs_present_set_idle(p, "NO CONTENT", "LOAD A PLAYLIST OR A .STRM FILE");
   return p;
}

void xs_present_free(xs_present_t *p)
{
   free(p);
}

void xs_present_set_name(xs_present_t *p, const char *name)
{
   snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
   p->card_key[0] = '\0';
}

static void set_state(xs_present_t *p, pstate_t s)
{
   p->state = s;
   p->state_ticks = 0;
}

/* This attempt has delivered (or is delivering) media. A short file can be
 * read to its end before the presenter ever sees it playing. */
static bool has_media(const xs_tuner_status_t *st)
{
   return st->state == XS_TUNER_PLAYING ||
          ((st->state == XS_TUNER_ENDED || st->state == XS_TUNER_FAILED) && st->packets);
}

static void clear_window(xs_present_t *p)
{
   unsigned i;
   for (i = 0; i < XS_FLOOR_BUCKETS; i++)
   {
      p->bucket_min[i] = INT64_MAX;
      p->bucket_max[i] = INT64_MIN;
   }
   p->buckets_full = 0;
}

/* Buffer to reach before (re)starting: the option plus what underruns
 * taught us. */
static int64_t start_target(const xs_present_t *p)
{
   return (int64_t)p->ctx->opt.buffer_ms * 1000 + p->extra_us;
}

/* What the floor is steered to while playing (live). */
static int64_t floor_target(const xs_present_t *p)
{
   return start_target(p) + p->burst_us / 2;
}

static void reset(xs_present_t *p, const xs_tuner_status_t *st)
{
   p->gen        = st->gen;
   p->serial     = st->serial;
   p->has_audio  = st->has_audio;
   p->has_video  = st->has_video;
   /* The viewer's track choice lasts for the tune, across reopens. */
   if (st->gen != p->gen_seen || p->track >= st->audio_tracks)
      p->track = st->audio_default < st->audio_tracks ? st->audio_default : 0;
   p->tracks     = st->audio_tracks;
   p->have_frame = false;
   p->started    = false;
   if (st->gen != p->gen_seen)
   {
      p->gen_seen = st->gen;
      p->pictured = false;
      p->tune_us  = av_gettime_relative();
      xs_agc_reset(&p->agc);   /* a new channel: its own loudness */
   }
   p->ratio      = 1.0;
   p->floor_us   = 0;
   clear_window(p);
   p->burst_us = 0;
   p->extra_us = 0;
   set_state(p, has_media(st) ? P_BUFFERING : P_WAIT);
}

static void fade(int16_t *s, unsigned frames, bool in)
{
   unsigned i;
   for (i = 0; i < frames; i++)
   {
      int g = in ? (int)i : (int)(frames - 1 - i);
      s[2 * i]     = (int16_t)(s[2 * i] * g / (int)frames);
      s[2 * i + 1] = (int16_t)(s[2 * i + 1] * g / (int)frames);
   }
}

/* Draw the card if its text changed; true if it was redrawn. */
static bool card(xs_present_t *p, const char *title, const char *detail, xs_picture_t *pic)
{
   char key[sizeof(p->card_key)];
   bool redraw;
   snprintf(key, sizeof(key), "%s|%s|%s", title, p->name, detail ? detail : "");
   redraw = strcmp(key, p->card_key) != 0 || !p->card_on_screen;
   if (redraw)
   {
      snprintf(p->card_key, sizeof(p->card_key), "%s", key);
      xs_osd_card(p->card, XS_CARD_W, XS_CARD_H, XS_CARD_W * sizeof(uint32_t),
                  p->ctx->opt.osd ? title : NULL, p->ctx->opt.osd ? p->name : NULL,
                  p->ctx->opt.osd ? detail : NULL);
   }
   pic->data   = p->card;
   pic->width  = XS_CARD_W;
   pic->height = XS_CARD_H;
   pic->pitch  = XS_CARD_W * sizeof(uint32_t);
   pic->aspect = 16.0f / 9.0f;
   pic->dupe   = !redraw;
   p->card_on_screen = true;
   p->have_frame = false;
   return redraw;
}

/* "No signal (Server returned 404 Not Found)" -> "Server returned 404 Not Found" */
static const char *reason(const char *msg)
{
   const char *paren = strchr(msg, '(');
   static char buf[256];
   size_t n;
   if (!paren)
      return msg;
   snprintf(buf, sizeof(buf), "%s", paren + 1);
   n = strlen(buf);
   if (n && buf[n - 1] == ')')
      buf[n - 1] = '\0';
   return buf;
}

/* Once a second: floor over the window, catch-up skip, rate. Returns true
 * if it skipped (the caller fades). */
static bool controller(xs_present_t *p, double fps, int64_t level_us)
{
   unsigned i;
   int64_t floor_us = INT64_MAX, peak_us = INT64_MIN, target;
   double err, ratio;

   if (level_us < p->bucket_min[p->bucket])
      p->bucket_min[p->bucket] = level_us;
   if (level_us > p->bucket_max[p->bucket])
      p->bucket_max[p->bucket] = level_us;
   if ((double)(p->ticks - p->bucket_tick) < fps)
      return false;
   /* once a second */
   p->bucket_tick = p->ticks;
   for (i = 0; i < XS_FLOOR_BUCKETS; i++)
   {
      if (p->bucket_min[i] < floor_us)
         floor_us = p->bucket_min[i];
      if (p->bucket_max[i] > peak_us)
         peak_us = p->bucket_max[i];
   }
   p->bucket = (p->bucket + 1) % XS_FLOOR_BUCKETS;
   p->bucket_min[p->bucket] = INT64_MAX;
   p->bucket_max[p->bucket] = INT64_MIN;
   p->floor_us = floor_us;
   if (p->buckets_full < XS_FLOOR_BUCKETS)
      p->buckets_full++;
   /* The burst size only grows within a tune: a quiet spell must not talk
    * the buffer down before the next late segment. */
   if (p->buckets_full >= XS_FLOOR_BUCKETS / 2 && peak_us - floor_us > p->burst_us)
      p->burst_us = peak_us - floor_us > XS_EXTRA_MAX_US ? XS_EXTRA_MAX_US : peak_us - floor_us;
   target = floor_target(p);

   if (p->buckets_full >= XS_FLOOR_BUCKETS && floor_us - target > XS_CATCHUP_EXCESS_US)
   {
      int64_t skipped = xs_aring_skip(xs_tuner_aring(p->tuner, p->track), p->serial,
                                      floor_us - target - XS_CATCHUP_KEEP_US);
      XS_INFO(p->ctx, "caught up with the live edge: skipped %.2f s (floor was %.2f s, target %.2f s)\n",
              skipped / 1e6, floor_us / 1e6, target / 1e6);
      clear_window(p);
      p->burst_us = 0;   /* the window held the backlog: learn it afresh */
      return true;
   }
   if (p->buckets_full < XS_FLOOR_BUCKETS / 2)
      return false;   /* too little history to steer by */

   err   = (floor_us - target) / 1e6;
   ratio = 1.0 + err * XS_KP;
   if (ratio > 1.0 + XS_RATIO_MAX)
      ratio = 1.0 + XS_RATIO_MAX;
   if (ratio < 1.0 - XS_RATIO_MAX)
      ratio = 1.0 - XS_RATIO_MAX;
   p->ratio = ratio;
   {
      unsigned i;
      for (i = 0; i < XS_MAX_TRACKS; i++)
         xs_aring_set_ratio(xs_tuner_aring(p->tuner, i), ratio);
   }
   return false;
}

void xs_present_set_idle(xs_present_t *p, const char *title, const char *detail)
{
   snprintf(p->idle_title, sizeof(p->idle_title), "%s", title ? title : "");
   snprintf(p->idle_detail, sizeof(p->idle_detail), "%s", detail ? detail : "");
}

bool xs_present_has_picture(const xs_present_t *p)
{
   return p->pictured && p->gen_seen == p->gen;
}

bool xs_present_card_showing(const xs_present_t *p)
{
   return p->card_on_screen;
}

void xs_present_log_stats(xs_present_t *p)
{
   xs_video_stats_t vs;
   xs_aring_t *ar = xs_tuner_aring(p->tuner, p->track);
   int64_t now = av_gettime_relative();
   double tick_rate = p->last_stats_us && now > p->last_stats_us
                    ? (p->ticks - p->last_stats_tick) * 1e6 / (double)(now - p->last_stats_us) : 0.0;
   xs_video_get_stats(xs_tuner_video(p->tuner), &vs);
   XS_INFO(p->ctx, "stats: %s, %.2f ticks/s, buffer %.2f s (floor %.2f s, target %.2f s), rate %+.3f%%, "
           "shown %u, dropped %u + %llu early, underruns %u, convert %.2f ms/frame, decode %.2f ms/packet; "
           "retro_run mean %.2f max %.2f ms (incl. frontend callbacks), longest gap %.1f ms\n",
           p->state == P_PLAYING ? "playing" : p->state == P_BUFFERING ? "buffering" : "waiting", tick_rate,
           xs_aring_level_us(ar, p->serial) / 1e6, p->floor_us == INT64_MAX ? 0.0 : p->floor_us / 1e6,
           floor_target(p) / 1e6,
           (p->ratio - 1.0) * 100.0, p->shown, p->dropped, (unsigned long long)vs.dropped_early,
           p->underruns, vs.convert_ms, vs.decode_ms,
           p->ctx->run.runs ? p->ctx->run.run_sum_us / 1000.0 / p->ctx->run.runs : 0.0,
           p->ctx->run.run_max_us / 1000.0, p->ctx->run.gap_max_us / 1000.0);
   memset(&p->ctx->run, 0, sizeof(p->ctx->run));
   p->last_stats_tick = p->ticks;
   p->last_stats_us   = now;
}

/* Show the stream picture on screen (or the newest due one). */
static void stream_picture(xs_present_t *p, int64_t until_us, xs_picture_t *pic, bool *have)
{
   xs_frame_t f;
   unsigned dropped = 0;
   bool changed = xs_video_advance(xs_tuner_video(p->tuner), p->serial, until_us, &f, &dropped);
   p->dropped += dropped;
   if (changed)
   {
      p->shown++;
      if (!p->pictured)
      {
         p->pictured = true;
         XS_INFO(p->ctx, "picture %.2f s after tune\n", (av_gettime_relative() - p->tune_us) / 1e6);
      }
   }
   *have = changed || (p->have_frame && xs_video_current(xs_tuner_video(p->tuner), &f));
   if (!*have)
      return;
   pic->data   = f.data;
   pic->width  = f.width;
   pic->height = f.height;
   pic->pitch  = f.pitch;
   pic->aspect = f.aspect;
   pic->dupe   = !changed && !p->card_on_screen;
   p->have_frame     = true;
   p->card_on_screen = false;
}

/* The tracks not being played are decoded all the same; keep them level
 * with the one that is, so a switch picks up at the same moment and they
 * never fill up and hold the tuner back. */
static void trim_other_tracks(xs_present_t *p, xs_aring_t *ar)
{
   int64_t pos;
   unsigned i;
   if (p->tracks < 2 || !xs_aring_clock(ar, p->serial, &pos))
      return;
   for (i = 0; i < p->tracks && i < XS_MAX_TRACKS; i++)
      if (i != p->track)
         xs_aring_discard_until(xs_tuner_aring(p->tuner, i), p->serial, pos);
}

void xs_present_next_audio(xs_present_t *p)
{
   if (p->tracks > 1)
   {
      p->track = (p->track + 1) % p->tracks;
      XS_INFO(p->ctx, "audio: track %u of %u\n", p->track + 1, p->tracks);
   }
}

unsigned xs_present_audio_track(const xs_present_t *p, unsigned *tracks)
{
   if (tracks)
      *tracks = p->tracks;
   return p->track;
}

void xs_present_tick(xs_present_t *p, double fps, int16_t *audio, unsigned frames,
                     xs_picture_t *pic)
{
   xs_tuner_status_t st;
   xs_aring_t *ar;
   xs_video_t *vid = xs_tuner_video(p->tuner);
   int64_t tick_us = (int64_t)(1e6 / fps);
   bool more_coming, have = false;

   p->ticks++;
   p->state_ticks++;
   memset(audio, 0, (size_t)frames * 2 * sizeof(int16_t));
   /* By wall clock, in every state: a frontend calling retro_run slowly
    * shows up as a low tick rate instead of as silence. */
   {
      int64_t now = av_gettime_relative();
      if (!p->last_stats_us)
         p->last_stats_us = now;
      else if (now - p->last_stats_us >= XS_STATS_EVERY_S * 1000000LL)
         xs_present_log_stats(p);
   }
   xs_tuner_get_status(p->tuner, &st);
   if (st.gen != p->gen || st.serial != p->serial ||
       (p->state == P_WAIT && has_media(&st)))
      reset(p, &st);
   more_coming = st.state == XS_TUNER_PLAYING;
   ar = xs_tuner_aring(p->tuner, p->track);

   if (p->state == P_WAIT)
   {
      switch (st.state)
      {
         case XS_TUNER_IDLE:   card(p, p->idle_title, p->idle_detail, pic); break;
         case XS_TUNER_FAILED: card(p, "NO SIGNAL", reason(st.msg), pic); break;
         case XS_TUNER_ENDED:  card(p, "END OF STREAM", NULL, pic); break;
         default:              card(p, "TUNING", NULL, pic); break;
      }
      return;
   }

   if (p->state == P_BUFFERING)
   {
      int64_t level = p->has_audio ? xs_aring_level_us(ar, p->serial) : 0;
      unsigned vready = p->has_video ? xs_video_ready_count(vid, p->serial) : 0;
      bool audio_ok = !p->has_audio || level >= start_target(p) ||
                      (!more_coming && level > 0);
      bool video_ok = !p->has_video || vready > 0 || (p->has_audio && !more_coming);

      if (audio_ok && video_ok && (p->has_audio ? level > 0 : vready > 0))
      {
         XS_INFO(p->ctx, "%s after %.2f s, buffer %.2f s\n", p->started ? "resumed" : "playing",
                 p->state_ticks / fps, level / 1e6);
         set_state(p, P_PLAYING);
         p->started = true;
         p->fade_in = XS_FADE;
         if (!p->has_audio)
            xs_video_head_pts(vid, p->serial, &p->clock_us);
      }
      else
      {
         if (p->have_frame)   /* stalled: hold the last picture */
         {
            stream_picture(p, INT64_MIN, pic, &have);
            if (have && p->state_ticks / fps < XS_NO_SIGNAL_S)
               return;
         }
         if (!more_coming && st.state == XS_TUNER_ENDED)
            card(p, "END OF STREAM", NULL, pic);
         else if (!more_coming && st.state == XS_TUNER_FAILED)
            card(p, "NO SIGNAL", reason(st.msg), pic);
         else
            card(p, p->started ? "NO SIGNAL" : "TUNING", p->started ? "BUFFERING" : st.msg, pic);
         return;
      }
   }

   /* P_PLAYING */
   if (p->has_audio)
   {
      unsigned got;
      if (!xs_aring_clock(ar, p->serial, &p->clock_us))
         got = 0;
      else
      {
         int jump;
         got = xs_aring_read(ar, p->serial, audio, frames, &jump);
         /* A jump inside the tick (reconnect, join): fade out to it and in
          * after it, so the cut doesn't click (DeeVee D-013's edge fades). */
         if (jump == 0)
            fade(audio, got < XS_FADE ? got : XS_FADE, true);
         else if (jump > 0)
         {
            unsigned j = (unsigned)jump, out = j < XS_FADE ? j : XS_FADE;
            unsigned in = got - j < XS_FADE ? got - j : XS_FADE;
            fade(audio + 2 * (j - out), out, false);
            fade(audio + 2 * j, in, true);
         }
      }
      if (p->fade_in && got)
      {
         unsigned n = got < p->fade_in ? got : p->fade_in;
         fade(audio, n, true);
         p->fade_in = 0;
      }
      if (got < frames)
      {
         fade(audio + 2 * (got > XS_FADE ? got - XS_FADE : 0), got > XS_FADE ? XS_FADE : got, false);
         p->underruns++;
         if (st.live && more_coming && st.can_step_down && p->last_underrun_tick &&
             (p->ticks - p->last_underrun_tick) / fps < XS_STEP_DOWN_WITHIN_S)
         {
            XS_INFO(p->ctx, "two stalls within %.0f s: asking for a lower variant\n", XS_STEP_DOWN_WITHIN_S);
            xs_tuner_step_down(p->tuner);
            p->extra_us = 0;   /* the buffer grew for the wrong variant */
            p->last_underrun_tick = 0;
         }
         else
         {
            if (st.live && more_coming && p->extra_us < XS_EXTRA_MAX_US)
               p->extra_us += XS_UNDERRUN_STEP_US;
            p->last_underrun_tick = p->ticks;
         }
         XS_WARN(p->ctx, "audio underrun (%u of %u samples)%s, buffer target now %.2f s\n", got, frames,
                 more_coming ? ", re-buffering" : "", floor_target(p) / 1e6);
         set_state(p, P_BUFFERING);
      }
      /* Nothing to correct on a file: it can't drift. */
      if (st.live && got == frames && controller(p, fps, xs_aring_level_us(ar, p->serial)))
      {
         fade(audio, frames, false);   /* the next tick fades in after the skip */
         p->fade_in = XS_FADE;
      }
   }
   else
      p->clock_us += tick_us;
   trim_other_tracks(p, ar);
   if (p->ctx->opt.level_audio && p->has_audio)
      xs_agc_process(&p->agc, audio, frames, XS_SAMPLE_RATE);

   if (p->has_video)
   {
      xs_video_set_clock(vid, p->serial, p->clock_us);
      stream_picture(p, p->clock_us + tick_us / 2, pic, &have);
      if (!p->has_audio && !xs_video_ready_count(vid, p->serial) && !more_coming)
         set_state(p, P_BUFFERING);
   }
   if (!have)
      card(p, p->has_video ? "TUNING" : "RADIO", p->has_video ? NULL : st.msg, pic);

}
