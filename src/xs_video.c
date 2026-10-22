/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Video thread (see xs_video.h). Decode loop and early frame drop follow
 * ffplay's video_thread / get_video_frame (fftools/ffplay.c @ n8.1.3);
 * deinterlacing follows DeeVee D-021 (bwdif at field rate, only pictures
 * flagged interlaced).
 *
 * Slots move FREE -> FILLING (video thread) -> READY -> SHOWN (presenter)
 * -> FREE. The video thread never touches a READY or SHOWN slot's pixels,
 * so the presenter can hand SHOWN memory to the frontend without a copy.
 */
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/cpu.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <rthreads/rthreads.h>

#include "xs_video.h"

#define XS_MAX_SLOTS   8
#define XS_MIN_SLOTS   3
/* Bytes of converted frames to keep ready: ~7 1080p frames. */
#define XS_RING_BUDGET (64u << 20)
#define XS_POLL_US     50000
/* A frame that ends this long before the clock is dropped before conversion. */
#define XS_LATE_US     20000
/* Worker threads for swscale and the deinterlacer. With the decoder at half
 * the CPUs this keeps the pipeline from oversubscribing the machine, where
 * a preempted lock holder would stall the emulation thread. */
#define XS_AUX_THREADS 2

typedef enum { SLOT_FREE = 0, SLOT_FILLING, SLOT_READY, SLOT_SHOWN } slot_state_t;

typedef struct
{
   slot_state_t state;
   uint32_t    *buf;
   size_t       cap;      /* bytes */
   unsigned     serial;
   uint64_t     seq;
   xs_frame_t   f;
} slot_t;

struct xs_video
{
   xs_ctx_t  *ctx;
   xs_pktq_t *q;
   sthread_t *thread;
   slock_t   *lock;
   scond_t   *cond;       /* slot released / flush / quit */
   bool       quit;

   /* lock: configuration from the tuner */
   unsigned           cfg_serial;
   AVCodecParameters *cfg_par;
   AVRational         cfg_tb, cfg_fr;
   unsigned           accept_serial;

   /* lock: from the presenter */
   unsigned   clock_serial;
   int64_t    clock_us;
   bool       clock_valid;
   xs_deint_t deint;
   unsigned   max_height;

   /* lock: ring */
   slot_t   slots[XS_MAX_SLOTS];
   unsigned allowed;      /* slots in use allowed for the current frame size */
   uint64_t next_seq;
   xs_video_stats_t stats;

   /* video thread only */
   AVCodecContext  *avctx;
   unsigned         dec_serial;
   AVRational       tb, fr;
   AVFrame         *frame, *filt;
   AVFilterGraph   *graph;
   AVFilterContext *src, *sink;
   int              g_w, g_h, g_fmt;
   xs_deint_t       g_deint;
   SwsContext      *sws;
   int64_t          next_pts_us;
   bool             logged_first;
   double           conv_total_ms, dec_total_ms;
   uint64_t         dec_packets;
};

/* ---- ring (call with v->lock held) ------------------------------------ */

static unsigned slots_in_use(const xs_video_t *v)
{
   unsigned i, n = 0;
   for (i = 0; i < XS_MAX_SLOTS; i++)
      n += v->slots[i].state != SLOT_FREE;
   return n;
}

static int find_free(const xs_video_t *v)
{
   unsigned i;
   if (slots_in_use(v) >= v->allowed)
      return -1;
   for (i = 0; i < XS_MAX_SLOTS; i++)
      if (v->slots[i].state == SLOT_FREE)
         return (int)i;
   return -1;
}

/* Oldest ready slot of serial; drops ready slots of other serials. */
static int ready_head(xs_video_t *v, unsigned serial)
{
   unsigned i;
   int best = -1;
   for (i = 0; i < XS_MAX_SLOTS; i++)
   {
      slot_t *s = &v->slots[i];
      if (s->state != SLOT_READY)
         continue;
      if (s->serial != serial)
      {
         s->state = SLOT_FREE;
         continue;
      }
      if (best < 0 || s->seq < v->slots[best].seq)
         best = (int)i;
   }
   return best;
}

/* ---- API ---------------------------------------------------------------- */

static void video_thread(void *arg);

xs_video_t *xs_video_new(xs_ctx_t *ctx, xs_pktq_t *q)
{
   xs_video_t *v = calloc(1, sizeof(*v));
   if (!v)
      return NULL;
   v->ctx     = ctx;
   v->q       = q;
   v->allowed = XS_MIN_SLOTS;
   v->lock    = slock_new();
   v->cond    = scond_new();
   v->frame   = av_frame_alloc();
   v->filt    = av_frame_alloc();
   if (v->lock && v->cond && v->frame && v->filt)
      v->thread = sthread_create(video_thread, v);
   if (!v->thread)
   {
      XS_ERROR(ctx, "can't start the video thread\n");
      xs_video_free(v);
      return NULL;
   }
   return v;
}

void xs_video_free(xs_video_t *v)
{
   unsigned i;
   if (!v)
      return;
   if (v->thread)
   {
      slock_lock(v->lock);
      v->quit = true;
      scond_broadcast(v->cond);
      slock_unlock(v->lock);
      xs_pktq_abort(v->q);
      sthread_join(v->thread);
   }
   avfilter_graph_free(&v->graph);
   sws_free_context(&v->sws);
   avcodec_free_context(&v->avctx);
   avcodec_parameters_free(&v->cfg_par);
   av_frame_free(&v->frame);
   av_frame_free(&v->filt);
   for (i = 0; i < XS_MAX_SLOTS; i++)
      free(v->slots[i].buf);
   if (v->cond)
      scond_free(v->cond);
   if (v->lock)
      slock_free(v->lock);
   free(v);
}

void xs_video_configure(xs_video_t *v, unsigned serial, const AVStream *st, AVRational frame_rate)
{
   slock_lock(v->lock);
   avcodec_parameters_free(&v->cfg_par);
   v->cfg_par = avcodec_parameters_alloc();
   if (v->cfg_par)
      avcodec_parameters_copy(v->cfg_par, st->codecpar);
   v->cfg_serial = serial;
   v->cfg_tb     = st->time_base;
   v->cfg_fr     = frame_rate;
   slock_unlock(v->lock);
}

void xs_video_flush(xs_video_t *v, unsigned serial)
{
   unsigned i;
   slock_lock(v->lock);
   v->accept_serial = serial;
   v->clock_valid   = false;
   for (i = 0; i < XS_MAX_SLOTS; i++)
      if (v->slots[i].state == SLOT_READY)
         v->slots[i].state = SLOT_FREE;
   scond_broadcast(v->cond);
   slock_unlock(v->lock);
}

void xs_video_set_options(xs_video_t *v, xs_deint_t deint, unsigned max_height)
{
   slock_lock(v->lock);
   v->deint      = deint;
   v->max_height = max_height;
   slock_unlock(v->lock);
}

void xs_video_set_clock(xs_video_t *v, unsigned serial, int64_t clock_us)
{
   slock_lock(v->lock);
   v->clock_serial = serial;
   v->clock_us     = clock_us;
   v->clock_valid  = true;
   slock_unlock(v->lock);
}

bool xs_video_head_pts(xs_video_t *v, unsigned serial, int64_t *pts_us)
{
   int h;
   slock_lock(v->lock);
   h = ready_head(v, serial);
   if (h >= 0)
      *pts_us = v->slots[h].f.pts_us;
   slock_unlock(v->lock);
   return h >= 0;
}

bool xs_video_advance(xs_video_t *v, unsigned serial, int64_t until_us, xs_frame_t *out,
                      unsigned *dropped)
{
   int pick = -1, h;
   unsigned i;
   bool changed = false;

   slock_lock(v->lock);
   /* Walk ready frames in order while they are due; the last due one wins. */
   while ((h = ready_head(v, serial)) >= 0 && v->slots[h].f.pts_us <= until_us)
   {
      if (pick >= 0)
      {
         v->slots[pick].state = SLOT_FREE;
         if (dropped)
            (*dropped)++;
      }
      pick = h;
      v->slots[h].state = SLOT_FILLING;   /* hide from ready_head meanwhile */
   }
   if (pick >= 0)
   {
      for (i = 0; i < XS_MAX_SLOTS; i++)
         if (v->slots[i].state == SLOT_SHOWN)
            v->slots[i].state = SLOT_FREE;
      v->slots[pick].state = SLOT_SHOWN;
      changed = true;
      scond_broadcast(v->cond);
   }
   for (i = 0; i < XS_MAX_SLOTS; i++)
      if (v->slots[i].state == SLOT_SHOWN)
      {
         *out = v->slots[i].f;
         break;
      }
   slock_unlock(v->lock);
   return changed;
}

bool xs_video_current(xs_video_t *v, xs_frame_t *out)
{
   unsigned i;
   bool ok = false;
   slock_lock(v->lock);
   for (i = 0; i < XS_MAX_SLOTS; i++)
      if (v->slots[i].state == SLOT_SHOWN)
      {
         *out = v->slots[i].f;
         ok = true;
         break;
      }
   slock_unlock(v->lock);
   return ok;
}

unsigned xs_video_ready_count(xs_video_t *v, unsigned serial)
{
   unsigned i, n = 0;
   slock_lock(v->lock);
   for (i = 0; i < XS_MAX_SLOTS; i++)
      n += v->slots[i].state == SLOT_READY && v->slots[i].serial == serial;
   slock_unlock(v->lock);
   return n;
}

void xs_video_get_stats(xs_video_t *v, xs_video_stats_t *out)
{
   slock_lock(v->lock);
   *out = v->stats;
   slock_unlock(v->lock);
}

/* ---- video thread ------------------------------------------------------- */

static bool open_decoder(xs_video_t *v, unsigned serial)
{
   AVCodecParameters *par = NULL;
   const AVCodec *codec;
   int ret;

   slock_lock(v->lock);
   if (v->cfg_serial == serial && v->cfg_par)
   {
      par = avcodec_parameters_alloc();
      if (par)
         avcodec_parameters_copy(par, v->cfg_par);
      v->tb = v->cfg_tb;
      v->fr = v->cfg_fr;
   }
   slock_unlock(v->lock);
   if (!par)
      return false;

   avcodec_free_context(&v->avctx);
   avfilter_graph_free(&v->graph);
   v->dec_serial   = serial;
   v->next_pts_us  = AV_NOPTS_VALUE;
   v->logged_first = false;

   codec = avcodec_find_decoder(par->codec_id);
   if (!codec || !(v->avctx = avcodec_alloc_context3(codec)) ||
       avcodec_parameters_to_context(v->avctx, par) < 0)
   {
      XS_WARN(v->ctx, "no decoder for video codec %s\n", avcodec_get_name(par->codec_id));
      avcodec_free_context(&v->avctx);
      avcodec_parameters_free(&par);
      return true;   /* serial taken; its packets are dropped */
   }
   avcodec_parameters_free(&par);
   v->avctx->pkt_timebase = v->tb;
   v->avctx->thread_count = av_cpu_count() / 2 > 1 ? av_cpu_count() / 2 : 1;
   v->avctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
   if ((ret = avcodec_open2(v->avctx, codec, NULL)) < 0)
   {
      XS_WARN(v->ctx, "can't open the %s decoder: %s\n", codec->name, av_err2str(ret));
      avcodec_free_context(&v->avctx);
   }
   return true;
}

/* bwdif/yadif at field rate, only on pictures flagged interlaced. */
static bool setup_graph(xs_video_t *v, const AVFrame *f, xs_deint_t deint)
{
   char args[256], filt[96];
   AVFilterInOut *in = NULL, *out = NULL;
   int ret;

   if (v->graph && v->g_w == f->width && v->g_h == f->height && v->g_fmt == f->format &&
       v->g_deint == deint)
      return true;
   avfilter_graph_free(&v->graph);
   if (deint == XS_DEINT_OFF)
      return false;

   v->graph = avfilter_graph_alloc();
   if (!v->graph)
      return false;
   v->graph->nb_threads = XS_AUX_THREADS;
   snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            f->width, f->height, f->format, v->tb.num, v->tb.den,
            f->sample_aspect_ratio.num ? f->sample_aspect_ratio.num : 1,
            f->sample_aspect_ratio.den ? f->sample_aspect_ratio.den : 1);
   snprintf(filt, sizeof(filt), "%s=mode=send_field:parity=auto:deint=interlaced",
            deint == XS_DEINT_YADIF ? "yadif" : "bwdif");
   ret = avfilter_graph_create_filter(&v->src, avfilter_get_by_name("buffer"), "in", args, NULL, v->graph);
   if (ret >= 0)
      ret = avfilter_graph_create_filter(&v->sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, v->graph);
   if (ret >= 0)
   {
      out = avfilter_inout_alloc();
      in  = avfilter_inout_alloc();
      if (!out || !in)
         ret = AVERROR(ENOMEM);
   }
   if (ret >= 0)
   {
      out->name = av_strdup("in");  out->filter_ctx = v->src;  out->pad_idx = 0; out->next = NULL;
      in->name  = av_strdup("out"); in->filter_ctx  = v->sink; in->pad_idx  = 0; in->next  = NULL;
      ret = avfilter_graph_parse_ptr(v->graph, filt, &in, &out, NULL);
   }
   if (ret >= 0)
      ret = avfilter_graph_config(v->graph, NULL);
   avfilter_inout_free(&in);
   avfilter_inout_free(&out);
   if (ret < 0)
   {
      XS_WARN(v->ctx, "deinterlace filter: %s\n", av_err2str(ret));
      avfilter_graph_free(&v->graph);
      return false;
   }
   v->g_w = f->width;
   v->g_h = f->height;
   v->g_fmt = f->format;
   v->g_deint = deint;
   XS_INFO(v->ctx, "deinterlacing with %s at field rate\n", deint == XS_DEINT_YADIF ? "yadif" : "bwdif");
   return true;
}

static int wait_slot(xs_video_t *v, unsigned serial)
{
   int s;
   slock_lock(v->lock);
   while ((s = find_free(v)) < 0 && !v->quit && v->accept_serial == serial)
      scond_wait_timeout(v->cond, v->lock, XS_POLL_US);
   if (v->quit || v->accept_serial != serial)
      s = -1;
   if (s >= 0)
      v->slots[s].state = SLOT_FILLING;
   slock_unlock(v->lock);
   return s;
}

/* Convert one picture into a slot and publish it. */
static void emit(xs_video_t *v, AVFrame *f, int64_t pts_us, int64_t dur_us)
{
   unsigned serial = v->dec_serial, max_h, out_w, out_h;
   AVRational sar = f->sample_aspect_ratio;
   AVFrame *dst;
   slot_t *s;
   size_t pitch, need;
   int64_t t0;
   int idx, ret;
   bool late;

   slock_lock(v->lock);
   v->stats.decoded++;
   late = v->clock_valid && v->clock_serial == serial && pts_us + dur_us < v->clock_us - XS_LATE_US;
   if (late)
      v->stats.dropped_early++;
   max_h = v->max_height;
   slock_unlock(v->lock);
   if (late)
      return;

   out_w = (unsigned)f->width;
   out_h = (unsigned)f->height;
   if (max_h && out_h > max_h)
   {
      out_w = ((unsigned)((uint64_t)out_w * max_h / out_h) + 1) & ~1u;
      out_h = max_h;
   }
   pitch = (size_t)out_w * sizeof(uint32_t);
   need  = pitch * out_h;

   slock_lock(v->lock);
   {
      unsigned n = (unsigned)(XS_RING_BUDGET / need);
      v->allowed = n < XS_MIN_SLOTS ? XS_MIN_SLOTS : n > XS_MAX_SLOTS ? XS_MAX_SLOTS : n;
   }
   slock_unlock(v->lock);
   if ((idx = wait_slot(v, serial)) < 0)
      return;
   s = &v->slots[idx];
   if (s->cap < need)
   {
      uint32_t *nb = realloc(s->buf, need);
      if (!nb)
      {
         slock_lock(v->lock);
         s->state = SLOT_FREE;
         slock_unlock(v->lock);
         return;
      }
      s->buf = nb;
      s->cap = need;
   }

   /* Unflagged streams: HD is BT.709, SD BT.601; limited range. */
   if (f->colorspace == AVCOL_SPC_UNSPECIFIED)
      f->colorspace = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
   if (f->color_range == AVCOL_RANGE_UNSPECIFIED)
      f->color_range = AVCOL_RANGE_MPEG;

   t0 = av_gettime_relative();
   ret = AVERROR(ENOMEM);
   if (!v->sws && (v->sws = sws_alloc_context()))
   {
      v->sws->flags   = SWS_BILINEAR;
      v->sws->threads = XS_AUX_THREADS;
   }
   if (v->sws && (dst = av_frame_alloc()))
   {
      dst->width  = (int)out_w;
      dst->height = (int)out_h;
      dst->format = AV_PIX_FMT_BGR0;   /* XRGB8888, little endian */
      dst->data[0]     = (uint8_t *)s->buf;
      dst->linesize[0] = (int)pitch;
      dst->colorspace      = AVCOL_SPC_RGB;
      dst->color_range     = AVCOL_RANGE_JPEG;
      dst->color_primaries = f->color_primaries;
      dst->color_trc       = f->color_trc;
      /* Undeinterlaced pictures stay woven: swscale then converts each
       * field on its own (right for interlaced 4:2:0 chroma). */
      dst->flags = f->flags & (AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
      ret = sws_scale_frame(v->sws, dst, f);
      av_frame_free(&dst);
   }
   v->conv_total_ms += (av_gettime_relative() - t0) / 1000.0;

   slock_lock(v->lock);
   if (ret < 0 || v->accept_serial != serial)
   {
      if (ret < 0)
         XS_WARN(v->ctx, "convert: %s\n", av_err2str(ret));
      s->state = SLOT_FREE;
   }
   else
   {
      if (!sar.num || !sar.den)
         sar = (AVRational){ 1, 1 };
      s->f.data   = s->buf;
      s->f.width  = out_w;
      s->f.height = out_h;
      s->f.pitch  = pitch;
      s->f.aspect = (float)((double)f->width * sar.num / ((double)f->height * sar.den));
      s->f.pts_us = pts_us;
      s->f.slot   = (unsigned)idx;
      s->serial   = serial;
      s->seq      = v->next_seq++;
      s->state    = SLOT_READY;
      v->stats.converted++;
      v->stats.convert_ms = v->conv_total_ms / (double)v->stats.converted;
   }
   slock_unlock(v->lock);
}

static int64_t frame_dur_us(const xs_video_t *v, const AVFrame *f, AVRational tb)
{
   if (f->duration > 0)
      return av_rescale_q(f->duration, tb, AV_TIME_BASE_Q);
   if (v->fr.num > 0 && v->fr.den > 0)
      return av_rescale_q(1, av_inv_q(v->fr), AV_TIME_BASE_Q);
   return 40000;
}

static void handle_frame(xs_video_t *v, AVFrame *f)
{
   xs_deint_t deint;
   int64_t pts_us, dur_us = frame_dur_us(v, f, v->tb);
   bool interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;

   if (f->best_effort_timestamp != AV_NOPTS_VALUE)
      pts_us = av_rescale_q(f->best_effort_timestamp, v->tb, AV_TIME_BASE_Q);
   else if (v->next_pts_us != AV_NOPTS_VALUE)
      pts_us = v->next_pts_us;
   else
      return;   /* nothing to place it by yet */
   v->next_pts_us = pts_us + dur_us;

   if (!v->logged_first)
   {
      v->logged_first = true;
      XS_INFO(v->ctx, "video: %s %dx%d %s, %s%s, %d decoder threads\n", v->avctx->codec->name,
              f->width, f->height, av_get_pix_fmt_name(f->format) ? av_get_pix_fmt_name(f->format) : "?",
              interlaced ? "interlaced " : "progressive",
              interlaced ? ((f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? "tff" : "bff") : "",
              v->avctx->thread_count);
   }

   slock_lock(v->lock);
   deint = v->deint;
   slock_unlock(v->lock);

   /* Once a stream has shown interlaced pictures, keep everything going
    * through the filter (it passes progressive ones once) so the frame
    * order and timing stay consistent. */
   if ((interlaced || v->graph) && setup_graph(v, f, deint))
   {
      AVRational otb = av_buffersink_get_time_base(v->sink);
      f->pts = av_rescale_q(pts_us, AV_TIME_BASE_Q, v->tb);
      if (av_buffersrc_add_frame_flags(v->src, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
         return;
      while (av_buffersink_get_frame(v->sink, v->filt) >= 0)
      {
         if (v->filt->pts != AV_NOPTS_VALUE)
            emit(v, v->filt, av_rescale_q(v->filt->pts, otb, AV_TIME_BASE_Q),
                 frame_dur_us(v, v->filt, otb));
         av_frame_unref(v->filt);
      }
      return;
   }
   emit(v, f, pts_us, dur_us);
}

static void video_thread(void *arg)
{
   xs_video_t *v = arg;
   AVPacket *pkt = av_packet_alloc();
   unsigned serial;

   sthread_setname("xs-video");
   while (pkt)
   {
      int r = xs_pktq_get(v->q, pkt, &serial, XS_POLL_US);
      bool stale;
      int64_t t0;
      int ret;

      if (r < 0)
         break;
      if (r == 0)
         continue;
      if (serial != v->dec_serial && !open_decoder(v, serial))
      {
         av_packet_unref(pkt);   /* no configuration for it (already superseded) */
         continue;
      }
      slock_lock(v->lock);
      stale = v->accept_serial != serial;
      slock_unlock(v->lock);
      if (stale || !v->avctx)
      {
         av_packet_unref(pkt);
         continue;
      }

      t0 = av_gettime_relative();
      ret = avcodec_send_packet(v->avctx, pkt);
      av_packet_unref(pkt);
      if (ret < 0 && ret != AVERROR(EAGAIN))
         XS_DEBUG(v->ctx, "video decode: %s\n", av_err2str(ret));
      while (avcodec_receive_frame(v->avctx, v->frame) >= 0)
      {
         v->dec_total_ms += (av_gettime_relative() - t0) / 1000.0;
         handle_frame(v, v->frame);
         av_frame_unref(v->frame);
         t0 = av_gettime_relative();
      }
      v->dec_total_ms += (av_gettime_relative() - t0) / 1000.0;
      v->dec_packets++;
      slock_lock(v->lock);
      v->stats.decode_ms = v->dec_total_ms / (double)v->dec_packets;
      slock_unlock(v->lock);
   }
   av_packet_free(&pkt);
}
