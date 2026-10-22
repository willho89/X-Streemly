/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tuner thread (CLAUDE.md §5.2). Structure follows FFmpeg fftools/ffplay.c
 * read_thread() / decode_interrupt_cb() @ n8.1.3: one thread owns the
 * AVFormatContext; the interrupt callback aborts any blocking call as soon
 * as a newer command arrives or a deadline passes.
 *
 * Locking: t->lock guards the mailbox (pending source, gen, quit) and the
 * published status. The tuner never holds it across a blocking call, so the
 * emulation thread only ever waits for a few field copies.
 *
 * M0: open, log stream info, read and count packets. Decoding is M1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/time.h>
#include <rthreads/rthreads.h>

#include "xs_tuner.h"

/* Give up on a source that hasn't opened in this long... */
#define XS_OPEN_TIMEOUT_US (15 * 1000000LL)
/* ...or that delivers no packet for this long once open. http's own
 * rw_timeout (10 s) normally fires first; this also covers hls reloads. */
#define XS_DATA_TIMEOUT_US (20 * 1000000LL)

struct xs_tuner
{
   xs_ctx_t   *ctx;
   sthread_t  *thread;
   slock_t    *lock;
   scond_t    *cond;

   /* mailbox (lock) */
   bool        quit;
   unsigned    gen;         /* bumped by every command */
   bool        have_source; /* the command for gen is a tune */
   xs_source_t pending;

   /* published status (lock) */
   xs_tuner_status_t status;

   /* tuner thread only */
   unsigned    run_gen;     /* generation being played */
   int64_t     deadline;    /* av_gettime_relative() limit, 0 = none */
   bool        timed_out;
};

/* ---- helpers ------------------------------------------------------------ */

static bool superseded(xs_tuner_t *t)
{
   bool s;
   slock_lock(t->lock);
   s = t->quit || t->gen != t->run_gen;
   slock_unlock(t->lock);
   return s;
}

static int interrupt_cb(void *opaque)
{
   xs_tuner_t *t = opaque;
   if (superseded(t))
      return 1;
   if (t->deadline && av_gettime_relative() > t->deadline)
   {
      t->timed_out = true;
      return 1;
   }
   return 0;
}

static void publish(xs_tuner_t *t, xs_tuner_state_t state, const char *msg,
                    uint64_t packets, uint64_t bytes)
{
   slock_lock(t->lock);
   if (t->gen == t->run_gen)   /* a stale run never overwrites a newer tune */
   {
      t->status.state   = state;
      t->status.gen     = t->run_gen;
      t->status.packets = packets;
      t->status.bytes   = bytes;
      if (msg)
         snprintf(t->status.msg, sizeof(t->status.msg), "%s", msg);
   }
   slock_unlock(t->lock);
}

static const char *scan_name(enum AVFieldOrder fo)
{
   switch (fo)
   {
      case AV_FIELD_PROGRESSIVE: return "p";
      case AV_FIELD_TT:
      case AV_FIELD_BT:          return "i tff";   /* top field displayed first */
      case AV_FIELD_BB:
      case AV_FIELD_TB:          return "i bff";
      default:                   return "?";
   }
}

/* Log what the source offers (CLAUDE.md M0) and build a one-line summary of
 * the first video and audio streams for the OSD. */
static void log_stream_info(xs_tuner_t *t, AVFormatContext *fmt, int64_t open_us,
                            char *summary, size_t len)
{
   xs_ctx_t *ctx = t->ctx;
   unsigned i, j;
   int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
   size_t n = 0;

   XS_INFO(ctx, "opened in %lld ms: format %s, %s, %u program(s), %u stream(s)\n",
           (long long)(open_us / 1000), fmt->iformat->name,
           fmt->duration == AV_NOPTS_VALUE ? "live" : "fixed length",
           fmt->nb_programs, fmt->nb_streams);

   for (i = 0; i < fmt->nb_programs; i++)
   {
      const AVProgram *p = fmt->programs[i];
      const AVDictionaryEntry *bw = av_dict_get(p->metadata, "variant_bitrate", NULL, 0);
      char list[128];
      size_t ln = 0;
      list[0] = '\0';
      for (j = 0; j < p->nb_stream_indexes && ln < sizeof(list); j++)
         ln += (size_t)snprintf(list + ln, sizeof(list) - ln, "%s#%u", j ? "," : "",
                                p->stream_index[j]);
      XS_INFO(ctx, "  program %d: bandwidth %s, streams %s\n", p->id,
              bw ? bw->value : "?", list);
   }

   for (i = 0; i < fmt->nb_streams; i++)
   {
      const AVStream *st = fmt->streams[i];
      const AVCodecParameters *par = st->codecpar;
      const char *codec = avcodec_get_name(par->codec_id);

      if (par->codec_type == AVMEDIA_TYPE_VIDEO)
      {
         AVRational fr = av_guess_frame_rate(fmt, (AVStream *)st, NULL);
         XS_INFO(ctx, "  stream #%u video: %s %dx%d SAR %d:%d %.3f fps scan %s, %lld kb/s%s\n",
                 i, codec, par->width, par->height,
                 par->sample_aspect_ratio.num, par->sample_aspect_ratio.den,
                 fr.den ? av_q2d(fr) : 0.0, scan_name(par->field_order),
                 (long long)(par->bit_rate / 1000), (int)i == vi ? " [selected]" : "");
         if ((int)i == vi)
            n += (size_t)snprintf(summary + n, len - n, "%s %dx%d%s%.4g",
                                  codec, par->width, par->height,
                                  par->field_order == AV_FIELD_PROGRESSIVE ||
                                  par->field_order == AV_FIELD_UNKNOWN ? " " : " i",
                                  fr.den ? av_q2d(fr) : 0.0);
      }
      else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
      {
         char layout[64];
         av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
         XS_INFO(ctx, "  stream #%u audio: %s %d Hz %s, %lld kb/s%s\n",
                 i, codec, par->sample_rate, layout, (long long)(par->bit_rate / 1000),
                 (int)i == ai ? " [selected]" : "");
         if ((int)i == ai && n < len)
            n += (size_t)snprintf(summary + n, len - n, "%s%s %d kHz %dch", n ? ", " : "",
                                  codec, par->sample_rate / 1000, par->ch_layout.nb_channels);
      }
      else
         XS_INFO(ctx, "  stream #%u %s: %s\n", i,
                 av_get_media_type_string(par->codec_type) ? av_get_media_type_string(par->codec_type) : "data",
                 codec);
   }
   if (vi < 0)
      XS_WARN(ctx, "no video stream\n");
   if (!n)
      snprintf(summary, len, "%s", fmt->iformat->name);
}

/* ---- one source ----------------------------------------------------------- */

static void play_source(xs_tuner_t *t, const xs_source_t *src)
{
   xs_ctx_t *ctx = t->ctx;
   AVIOInterruptCB icb = { interrupt_cb, t };
   AVFormatContext *fmt = NULL;
   AVPacket *pkt = NULL;
   char summary[256], msg[256];
   uint64_t packets = 0, bytes = 0;
   int64_t start = av_gettime_relative(), first_pkt = 0;
   int ret;

   publish(t, XS_TUNER_CONNECTING, "Tuning", 0, 0);
   XS_INFO(ctx, "tuning %s\n", src->url);

   t->timed_out = false;
   t->deadline  = start + XS_OPEN_TIMEOUT_US;
   ret = xs_source_open(ctx, src, &icb, &fmt);
   if (ret < 0)
   {
      if (superseded(t))
      {
         XS_DEBUG(ctx, "tune aborted after %lld ms\n", (long long)((av_gettime_relative() - start) / 1000));
         return;
      }
      snprintf(msg, sizeof(msg), "No signal (%s)", t->timed_out ? "timed out" : av_err2str(ret));
      XS_WARN(ctx, "open failed: %s\n", msg);
      publish(t, XS_TUNER_FAILED, msg, 0, 0);
      return;
   }

   summary[0] = '\0';
   log_stream_info(t, fmt, av_gettime_relative() - start, summary, sizeof(summary));
   publish(t, XS_TUNER_PLAYING, summary, 0, 0);

   pkt = av_packet_alloc();
   for (;;)
   {
      t->deadline = av_gettime_relative() + XS_DATA_TIMEOUT_US;
      ret = pkt ? av_read_frame(fmt, pkt) : AVERROR(ENOMEM);
      if (ret == AVERROR(EAGAIN))
      {
         av_usleep(10000);
         continue;
      }
      if (ret < 0)
         break;
      if (!packets)
      {
         first_pkt = av_gettime_relative();
         XS_INFO(ctx, "first packet %lld ms after tune\n", (long long)((first_pkt - start) / 1000));
      }
      packets++;
      bytes += (uint64_t)pkt->size;
      av_packet_unref(pkt);
      if (!(packets & 63))
         publish(t, XS_TUNER_PLAYING, NULL, packets, bytes);
   }

   XS_INFO(ctx, "read %llu packets, %llu KB in %.1f s\n", (unsigned long long)packets,
           (unsigned long long)(bytes / 1024),
           first_pkt ? (av_gettime_relative() - first_pkt) / 1e6 : 0.0);
   if (superseded(t))
      ;   /* channel change or unload: nothing to report */
   else if (ret == AVERROR_EOF)
   {
      XS_INFO(ctx, "end of stream\n");
      publish(t, XS_TUNER_ENDED, "End of stream", packets, bytes);
   }
   else
   {
      snprintf(msg, sizeof(msg), "Signal lost (%s)", t->timed_out ? "timed out" : av_err2str(ret));
      XS_WARN(ctx, "%s\n", msg);
      publish(t, XS_TUNER_FAILED, msg, packets, bytes);
   }
   av_packet_free(&pkt);
   avformat_close_input(&fmt);
}

static void tuner_thread(void *arg)
{
   xs_tuner_t *t = arg;
   xs_source_t src;

   sthread_setname("xs-tuner");
   slock_lock(t->lock);
   for (;;)
   {
      while (!t->quit && t->gen == t->run_gen)
         scond_wait(t->cond, t->lock);
      if (t->quit)
         break;
      t->run_gen = t->gen;
      if (!t->have_source)
      {
         t->status.state = XS_TUNER_IDLE;   /* stop */
         t->status.gen   = t->run_gen;
         continue;
      }
      src = t->pending;
      slock_unlock(t->lock);
      play_source(t, &src);
      slock_lock(t->lock);
   }
   slock_unlock(t->lock);
}

/* ---- API (emulation thread) --------------------------------------------- */

xs_tuner_t *xs_tuner_new(xs_ctx_t *ctx)
{
   xs_tuner_t *t = calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   t->ctx  = ctx;
   t->lock = slock_new();
   t->cond = scond_new();
   if (t->lock && t->cond)
      t->thread = sthread_create(tuner_thread, t);
   if (!t->thread)
   {
      XS_ERROR(ctx, "can't start the tuner thread\n");
      if (t->cond)
         scond_free(t->cond);
      if (t->lock)
         slock_free(t->lock);
      free(t);
      return NULL;
   }
   return t;
}

void xs_tuner_free(xs_tuner_t *t)
{
   int64_t start;
   if (!t)
      return;
   start = av_gettime_relative();
   slock_lock(t->lock);
   t->quit = true;
   scond_signal(t->cond);
   slock_unlock(t->lock);
   sthread_join(t->thread);
   XS_DEBUG(t->ctx, "tuner stopped in %lld ms\n", (long long)((av_gettime_relative() - start) / 1000));
   scond_free(t->cond);
   slock_free(t->lock);
   free(t);
}

unsigned xs_tuner_tune(xs_tuner_t *t, const xs_source_t *src)
{
   unsigned gen;
   slock_lock(t->lock);
   t->pending     = *src;
   t->have_source = true;
   gen = ++t->gen;
   t->status.state = XS_TUNER_CONNECTING;
   t->status.gen   = gen;
   t->status.packets = t->status.bytes = 0;
   snprintf(t->status.msg, sizeof(t->status.msg), "Tuning");
   scond_signal(t->cond);
   slock_unlock(t->lock);
   return gen;
}

void xs_tuner_stop(xs_tuner_t *t)
{
   slock_lock(t->lock);
   t->have_source = false;
   t->gen++;
   scond_signal(t->cond);
   slock_unlock(t->lock);
}

void xs_tuner_get_status(xs_tuner_t *t, xs_tuner_status_t *out)
{
   slock_lock(t->lock);
   *out = t->status;
   slock_unlock(t->lock);
}
