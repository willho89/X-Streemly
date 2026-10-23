/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Audio ring and decoder (see xs_audio.h).
 *
 * Ring layout: a circular PCM buffer plus a circular list of chunk records
 * (timestamp, sample count, source duration). One lock; the writer waits
 * when either is full, the reader never waits.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <rthreads/rthreads.h>

#include "xs_audio.h"

#define XS_ARING_CHUNKS  8192
#define XS_ARING_POLL_US 50000
/* Chunks further apart than this are a jump in the audio. */
#define XS_ARING_JUMP_US 5000

typedef struct
{
   int64_t  pts_us;
   int64_t  src_us;
   unsigned frames;
} chunk_t;

struct xs_aring
{
   slock_t *lock;
   scond_t *cond;          /* space freed */
   unsigned serial;

   int16_t *pcm;           /* stereo frames */
   unsigned cap;           /* frames */
   unsigned rpos, used;    /* frames */

   chunk_t  chunks[XS_ARING_CHUNKS];
   unsigned chead, ccount;
   unsigned head_off;      /* frames already read from the head chunk */
   int64_t  level_us;      /* source time of all unread audio */
   int64_t  read_end_us;   /* where the last read stopped, for jumps across reads */

   double   ratio;
};

/* ---- ring ------------------------------------------------------------- */

xs_aring_t *xs_aring_new(unsigned seconds)
{
   xs_aring_t *r = calloc(1, sizeof(*r));
   if (!r)
      return NULL;
   r->cap   = seconds * XS_SAMPLE_RATE;
   r->pcm   = malloc((size_t)r->cap * 2 * sizeof(int16_t));
   r->lock  = slock_new();
   r->cond  = scond_new();
   r->ratio = 1.0;
   r->read_end_us = INT64_MIN;
   if (!r->pcm || !r->lock || !r->cond)
   {
      xs_aring_free(r);
      return NULL;
   }
   return r;
}

void xs_aring_free(xs_aring_t *r)
{
   if (!r)
      return;
   if (r->cond)
      scond_free(r->cond);
   if (r->lock)
      slock_free(r->lock);
   free(r->pcm);
   free(r);
}

void xs_aring_flush(xs_aring_t *r, unsigned serial)
{
   slock_lock(r->lock);
   r->serial   = serial;
   r->rpos     = r->used = 0;
   r->chead    = r->ccount = 0;
   r->head_off = 0;
   r->level_us = 0;
   r->ratio    = 1.0;
   r->read_end_us = INT64_MIN;
   scond_broadcast(r->cond);
   slock_unlock(r->lock);
}

bool xs_aring_write(xs_aring_t *r, unsigned serial, const int16_t *pcm, unsigned frames,
                    int64_t pts_us, int64_t src_us, xs_abort_fn stop, void *arg)
{
   unsigned wpos, first;
   chunk_t *c;

   if (!frames)
      return true;
   if (frames > r->cap / 2)
      frames = r->cap / 2;
   slock_lock(r->lock);
   while (r->serial == serial &&
          (r->used + frames > r->cap || r->ccount == XS_ARING_CHUNKS))
   {
      bool give_up;
      scond_wait_timeout(r->cond, r->lock, XS_ARING_POLL_US);
      slock_unlock(r->lock);
      give_up = stop && stop(arg);
      slock_lock(r->lock);
      if (give_up)
      {
         slock_unlock(r->lock);
         return false;
      }
   }
   if (r->serial != serial)   /* flushed for a newer source meanwhile */
   {
      slock_unlock(r->lock);
      return true;
   }
   wpos  = (r->rpos + r->used) % r->cap;
   first = r->cap - wpos < frames ? r->cap - wpos : frames;
   memcpy(r->pcm + 2 * wpos, pcm, (size_t)first * 2 * sizeof(int16_t));
   if (first < frames)
      memcpy(r->pcm, pcm + 2 * first, (size_t)(frames - first) * 2 * sizeof(int16_t));
   r->used += frames;

   c = &r->chunks[(r->chead + r->ccount) % XS_ARING_CHUNKS];
   c->pts_us = pts_us;
   c->src_us = src_us;
   c->frames = frames;
   r->ccount++;
   r->level_us += src_us;
   slock_unlock(r->lock);
   return true;
}

static int64_t head_consumed_us(const xs_aring_t *r)
{
   const chunk_t *c = &r->chunks[r->chead];
   return c->src_us * (int64_t)r->head_off / (int64_t)c->frames;
}

int64_t xs_aring_level_us(xs_aring_t *r, unsigned serial)
{
   int64_t level = 0;
   slock_lock(r->lock);
   if (r->serial == serial && r->ccount)
      level = r->level_us - head_consumed_us(r);
   slock_unlock(r->lock);
   return level;
}

bool xs_aring_clock(xs_aring_t *r, unsigned serial, int64_t *clock_us)
{
   bool ok = false;
   slock_lock(r->lock);
   if (r->serial == serial && r->ccount)
   {
      *clock_us = r->chunks[r->chead].pts_us + head_consumed_us(r);
      ok = true;
   }
   slock_unlock(r->lock);
   return ok;
}

unsigned xs_aring_read(xs_aring_t *r, unsigned serial, int16_t *out, unsigned frames, int *jump)
{
   unsigned done = 0;
   int64_t prev_end;
   *jump = -1;
   slock_lock(r->lock);
   prev_end = r->read_end_us;
   if (r->serial != serial)
   {
      slock_unlock(r->lock);
      return 0;
   }
   while (done < frames && r->ccount)
   {
      chunk_t *c = &r->chunks[r->chead];
      unsigned n = c->frames - r->head_off, first;
      if (prev_end != INT64_MIN && *jump < 0)
      {
         int64_t at = c->pts_us + (r->head_off ? head_consumed_us(r) : 0);   /* where we read from */
         if (at - prev_end > XS_ARING_JUMP_US || prev_end - at > XS_ARING_JUMP_US)
            *jump = (int)done;
      }
      prev_end = c->pts_us + c->src_us;   /* where this chunk ends */
      if (n > frames - done)
         n = frames - done;
      first = r->cap - r->rpos < n ? r->cap - r->rpos : n;
      memcpy(out + 2 * done, r->pcm + 2 * r->rpos, (size_t)first * 2 * sizeof(int16_t));
      if (first < n)
         memcpy(out + 2 * (done + first), r->pcm, (size_t)(n - first) * 2 * sizeof(int16_t));
      r->rpos = (r->rpos + n) % r->cap;
      r->used -= n;
      done += n;
      r->head_off += n;
      if (r->head_off == c->frames)
      {
         r->level_us -= c->src_us;
         r->chead = (r->chead + 1) % XS_ARING_CHUNKS;
         r->ccount--;
         r->head_off = 0;
      }
   }
   if (done)
   {
      /* exactly where reading stopped (mid-chunk or at its end) */
      r->read_end_us = r->ccount ? r->chunks[r->chead].pts_us + head_consumed_us(r) : prev_end;
      scond_broadcast(r->cond);
   }
   slock_unlock(r->lock);
   return done;
}

void xs_aring_discard_until(xs_aring_t *r, unsigned serial, int64_t pts_us)
{
   bool freed = false;
   slock_lock(r->lock);
   while (r->serial == serial && r->ccount)
   {
      chunk_t *c = &r->chunks[r->chead];
      int64_t pos = c->pts_us + head_consumed_us(r);
      unsigned n;
      if (pos >= pts_us)
         break;
      if (c->pts_us + c->src_us <= pts_us)
         n = c->frames - r->head_off;   /* all of it */
      else
      {
         n = (unsigned)((pts_us - pos) * (int64_t)c->frames / (c->src_us ? c->src_us : 1));
         if (!n)
            break;
      }
      r->rpos = (r->rpos + n) % r->cap;
      r->used -= n;
      r->head_off += n;
      freed = true;
      if (r->head_off >= c->frames)
      {
         r->level_us -= c->src_us;
         r->chead = (r->chead + 1) % XS_ARING_CHUNKS;
         r->ccount--;
         r->head_off = 0;
      }
      else
         break;
   }
   if (freed)
      scond_broadcast(r->cond);
   slock_unlock(r->lock);
}

/* ---- loudness levelling ------------------------------------------------ */

#define XS_AGC_TARGET   0.1     /* RMS, about -20 dBFS: broadcast dialogue level */
#define XS_AGC_GATE     0.01    /* ticks quieter than -40 dBFS don't count */
#define XS_AGC_MIN_GAIN 0.25    /* -12 dB */
#define XS_AGC_MAX_GAIN 4.0     /* +12 dB */
#define XS_AGC_TAU_S    3.0     /* loudness estimate */
#define XS_AGC_FAST_S   1.0     /* the same over the first seconds of a channel */
#define XS_AGC_DOWN_S   0.5     /* gain falling */
#define XS_AGC_UP_S     4.0     /* gain rising */
#define XS_AGC_LIMIT    0.89    /* -1 dBFS: soft limiting above this */

void xs_agc_reset(xs_agc_t *a)
{
   a->ms       = 0.0;
   a->gain     = 1.0;
   a->settle_s = 0.0;
}

static int16_t agc_limit(double x)
{
   double ax = fabs(x);
   if (ax > XS_AGC_LIMIT)   /* soft knee from -1 dBFS to full scale */
   {
      double over = (ax - XS_AGC_LIMIT) / (1.0 - XS_AGC_LIMIT);
      ax = XS_AGC_LIMIT + (1.0 - XS_AGC_LIMIT) * over / (1.0 + over);
      x  = x < 0 ? -ax : ax;
   }
   return (int16_t)lrint(x * 32767.0);
}

void xs_agc_process(xs_agc_t *a, int16_t *s, unsigned frames, unsigned rate)
{
   double sum = 0.0, ms, want, from = a->gain, dt, tau, step;
   unsigned i;
   if (!frames || !rate)
      return;
   dt = (double)frames / rate;
   for (i = 0; i < frames * 2; i++)
      sum += (double)s[i] * s[i];
   ms = sum / (frames * 2) / (32768.0 * 32768.0);
   if (ms > XS_AGC_GATE * XS_AGC_GATE)
   {
      tau = a->settle_s < 2 * XS_AGC_FAST_S ? XS_AGC_FAST_S : XS_AGC_TAU_S;
      a->ms = a->ms > 0.0 ? a->ms + (ms - a->ms) * (1.0 - exp(-dt / tau)) : ms;
      a->settle_s += dt;
   }
   if (a->ms > 0.0)
   {
      want = XS_AGC_TARGET / sqrt(a->ms);
      if (want < XS_AGC_MIN_GAIN)
         want = XS_AGC_MIN_GAIN;
      if (want > XS_AGC_MAX_GAIN)
         want = XS_AGC_MAX_GAIN;
      tau  = want < a->gain ? XS_AGC_DOWN_S : XS_AGC_UP_S;
      a->gain += (want - a->gain) * (1.0 - exp(-dt / tau));
   }
   /* Ramp across the tick so gain changes don't step. */
   step = (a->gain - from) / frames;
   for (i = 0; i < frames; i++)
   {
      double g = from + step * (i + 1);
      s[2 * i]     = agc_limit(s[2 * i] * g / 32768.0);
      s[2 * i + 1] = agc_limit(s[2 * i + 1] * g / 32768.0);
   }
}

int64_t xs_aring_skip(xs_aring_t *r, unsigned serial, int64_t us)
{
   int64_t dropped = 0;
   slock_lock(r->lock);
   while (r->serial == serial && r->ccount > 1 && dropped < us)
   {
      chunk_t *c = &r->chunks[r->chead];
      unsigned n = c->frames - r->head_off;
      dropped += c->src_us - head_consumed_us(r);
      r->rpos = (r->rpos + n) % r->cap;
      r->used -= n;
      r->level_us -= c->src_us;
      r->chead = (r->chead + 1) % XS_ARING_CHUNKS;
      r->ccount--;
      r->head_off = 0;
   }
   if (dropped)
      scond_broadcast(r->cond);
   slock_unlock(r->lock);
   return dropped;
}

void xs_aring_set_ratio(xs_aring_t *r, double ratio)
{
   slock_lock(r->lock);
   r->ratio = ratio;
   slock_unlock(r->lock);
}

double xs_aring_ratio(xs_aring_t *r)
{
   double ratio;
   slock_lock(r->lock);
   ratio = r->ratio;
   slock_unlock(r->lock);
   return ratio;
}

/* ---- decoder (tuner thread) ------------------------------------------- */

struct xs_adec
{
   xs_ctx_t        *ctx;
   AVCodecContext  *avctx;
   AVFrame         *frame;
   SwrContext      *swr;
   AVChannelLayout  swr_layout;
   int              swr_rate;
   int              swr_fmt;
   int16_t         *buf;
   int              buf_frames;
   int64_t          next_pts_us;  /* for frames without a timestamp */
   double           comp_acc;     /* fractional compensation samples */
};

xs_adec_t *xs_adec_open(xs_ctx_t *ctx, const AVStream *st)
{
   const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
   xs_adec_t *d;
   AVDictionary *opts = NULL;
   int ret;

   if (!codec)
   {
      XS_WARN(ctx, "no decoder for audio codec %s\n", avcodec_get_name(st->codecpar->codec_id));
      return NULL;
   }
   d = calloc(1, sizeof(*d));
   if (!d)
      return NULL;
   d->ctx   = ctx;
   d->avctx = avcodec_alloc_context3(codec);
   d->frame = av_frame_alloc();
   d->next_pts_us = AV_NOPTS_VALUE;
   if (!d->avctx || !d->frame || avcodec_parameters_to_context(d->avctx, st->codecpar) < 0)
   {
      xs_adec_free(d);
      return NULL;
   }
   d->avctx->pkt_timebase = st->time_base;
   /* AC-3 / E-AC-3 fold down themselves, honouring the stream's own centre
    * and surround mix levels (DeeVee D-014). Others ignore the option. */
   av_dict_set(&opts, "downmix", "stereo", 0);
   ret = avcodec_open2(d->avctx, codec, &opts);
   av_dict_free(&opts);
   if (ret < 0)
   {
      XS_WARN(ctx, "can't open the %s decoder: %s\n", codec->name, av_err2str(ret));
      xs_adec_free(d);
      return NULL;
   }
   return d;
}

void xs_adec_free(xs_adec_t *d)
{
   if (!d)
      return;
   swr_free(&d->swr);
   av_channel_layout_uninit(&d->swr_layout);
   av_frame_free(&d->frame);
   avcodec_free_context(&d->avctx);
   free(d->buf);
   free(d);
}

static bool setup_swr(xs_adec_t *d, const AVFrame *f)
{
   AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
   if (d->swr && f->sample_rate == d->swr_rate && f->format == d->swr_fmt &&
       !av_channel_layout_compare(&f->ch_layout, &d->swr_layout))
      return true;

   swr_free(&d->swr);
   if (swr_alloc_set_opts2(&d->swr, &stereo, AV_SAMPLE_FMT_S16, XS_SAMPLE_RATE,
                           &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0)
      return false;
   /* Normalise the downmix matrix so nothing can clip (DeeVee D-014). */
   av_opt_set_double(d->swr, "rematrix_maxval", 1.0, 0);
   if (swr_init(d->swr) < 0)
   {
      swr_free(&d->swr);
      return false;
   }
   av_channel_layout_uninit(&d->swr_layout);
   av_channel_layout_copy(&d->swr_layout, &f->ch_layout);
   d->swr_rate = f->sample_rate;
   d->swr_fmt  = f->format;
   d->comp_acc = 0;
   {
      char layout[64];
      av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout));
      XS_INFO(d->ctx, "audio: %s %d Hz %s %s -> 48000 Hz stereo\n", d->avctx->codec->name,
              f->sample_rate, layout, av_get_sample_fmt_name(f->format));
   }
   return true;
}

static bool convert_frame(xs_adec_t *d, const AVFrame *f, xs_aring_t *r, unsigned serial,
                          xs_abort_fn stop, void *arg)
{
   double ratio = xs_aring_ratio(r);
   int64_t pts_us, delay_us;
   double expected;
   int out_max, got, comp = 0;
   uint8_t *out[1];

   if (!setup_swr(d, f))
      return true;   /* undecodable format: skip */

   pts_us = f->best_effort_timestamp != AV_NOPTS_VALUE
          ? av_rescale_q(f->best_effort_timestamp, d->avctx->pkt_timebase, AV_TIME_BASE_Q)
          : d->next_pts_us;
   /* Samples still inside the resampler come out first. */
   delay_us = swr_get_delay(d->swr, AV_TIME_BASE);

   /* Rate correction (D-007): produce fewer samples (ratio > 1) or more for
    * the same stretch of source, spread over this frame. */
   expected = (double)f->nb_samples * XS_SAMPLE_RATE / f->sample_rate;
   d->comp_acc += expected * (1.0 / ratio - 1.0);
   comp = (int)lrint(d->comp_acc);
   if (comp)
   {
      d->comp_acc -= comp;
      if (swr_set_compensation(d->swr, comp, (int)expected) < 0)
         comp = 0;
   }

   out_max = swr_get_out_samples(d->swr, f->nb_samples) + 64;
   if (out_max > d->buf_frames)
   {
      int16_t *nb = realloc(d->buf, (size_t)out_max * 2 * sizeof(int16_t));
      if (!nb)
         return true;
      d->buf = nb;
      d->buf_frames = out_max;
   }
   out[0] = (uint8_t *)d->buf;
   got = swr_convert(d->swr, out, out_max, (const uint8_t **)f->extended_data, f->nb_samples);
   if (pts_us != AV_NOPTS_VALUE)
      d->next_pts_us = pts_us + (int64_t)f->nb_samples * AV_TIME_BASE / f->sample_rate;
   if (got <= 0 || pts_us == AV_NOPTS_VALUE)
      return true;
   /* Source time covered: output samples scaled back by the correction. */
   return xs_aring_write(r, serial, d->buf, (unsigned)got, pts_us - delay_us,
                         (int64_t)((double)got * AV_TIME_BASE / XS_SAMPLE_RATE *
                                   (expected + comp ? expected / (expected + comp) : 1.0)),
                         stop, arg);
}

bool xs_adec_packet(xs_adec_t *d, const AVPacket *pkt, xs_aring_t *r, unsigned serial,
                    xs_abort_fn stop, void *arg)
{
   int ret = avcodec_send_packet(d->avctx, pkt);
   if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
      XS_DEBUG(d->ctx, "audio decode: %s\n", av_err2str(ret));
   while ((ret = avcodec_receive_frame(d->avctx, d->frame)) >= 0)
   {
      bool ok = convert_frame(d, d->frame, r, serial, stop, arg);
      av_frame_unref(d->frame);
      if (!ok)
         return false;
   }
   return true;
}
