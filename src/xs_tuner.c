/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tuner thread (CLAUDE.md §5.2, §5.3). Structure follows FFmpeg
 * fftools/ffplay.c read_thread() / decode_interrupt_cb() @ n8.1.3: one
 * thread owns the AVFormatContext; the interrupt callback aborts any
 * blocking call as soon as a newer command arrives or a deadline passes.
 *
 * Locking: t->lock guards the mailbox (pending source, gen, quit) and the
 * published status. The tuner never holds it across a blocking call, so the
 * emulation thread only ever waits for a few field copies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libavutil/time.h>
#include <rthreads/rthreads.h>

#include "xs_tuner.h"

/* Give up on a source that hasn't opened in this long... */
#define XS_OPEN_TIMEOUT_US (15 * 1000000LL)
/* ...or that delivers no packet for this long once open. http's own
 * rw_timeout (10 s) normally fires first; this also covers hls reloads. */
#define XS_DATA_TIMEOUT_US (20 * 1000000LL)
/* Retry a failed source after 2, 4, 8 ... up to 30 s. */
#define XS_RETRY_MIN_US    (2 * 1000000LL)
#define XS_RETRY_MAX_US    (30 * 1000000LL)
/* Queue limits: a live HLS start can deliver several segments at once. */
#define XS_QUEUE_SECONDS   20
#define XS_VQUEUE_BYTES    (128u << 20)
/* A stream's timestamps going back by more than this, or forward by more
 * than XS_GAP_MAX_US, is a discontinuity (HLS EXT-X-DISCONTINUITY, ad
 * splice, encoder restart, wrap) and gets joined: §5.2. A smaller forward
 * jump is a real gap (segments the server let expire) and is kept, so the
 * clock steps over the missing stretch. */
#define XS_BACK_MAX_US     1000000LL
#define XS_GAP_MAX_US      10000000LL
#define XS_MAX_STREAMS     64
#define XS_MAX_AUDIO       XS_MAX_TRACKS
#define XS_EPOCHS          16

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
   unsigned    max_height;
   bool        step_down;   /* xs_tuner_step_down() asked for a lower variant */
   char        audio_lang[8];

   /* published status (lock) */
   xs_tuner_status_t status;

   /* pipeline (own locks) */
   xs_pktq_t  *vq;
   xs_aring_t *aring[XS_MAX_TRACKS];
   xs_video_t *video;

   /* tuner thread only */
   unsigned    run_gen;     /* generation being played */
   unsigned    var_height;  /* chosen variant's video height */
   bool        var_lower;   /* a lower variant exists */
   unsigned    serial;      /* last attempt */
   int64_t     deadline;    /* av_gettime_relative() limit, 0 = none */
   bool        timed_out;
   /* Timeline (D-023): each discontinuity opens an epoch with its own
    * offset; every stream moves through the same epochs, so all of them get
    * the offset worked out by the first stream to cross. */
   int64_t     epoch_off_us[XS_EPOCHS];
   unsigned    epochs;      /* epochs opened (ids are 0 .. epochs-1) */
   unsigned    stream_epoch[XS_MAX_STREAMS];
   int64_t     last_end_us[XS_MAX_STREAMS];
   int64_t     max_end_us;  /* furthest point any stream reached */
   bool        rejoin;      /* reconnected: continue after max_end_us */
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

static bool step_down_asked(xs_tuner_t *t)
{
   bool s;
   slock_lock(t->lock);
   s = t->step_down;
   slock_unlock(t->lock);
   return s;
}

static int stop_cb(void *opaque)
{
   return superseded(opaque) || step_down_asked(opaque);
}

static int interrupt_cb(void *opaque)
{
   xs_tuner_t *t = opaque;
   if (superseded(t) || step_down_asked(t))
      return 1;
   if (t->deadline && av_gettime_relative() > t->deadline)
   {
      t->timed_out = true;
      return 1;
   }
   return 0;
}

typedef struct
{
   bool     has_audio, has_video, live;
   unsigned height;
   bool     can_step_down;
} media_info_t;

static void publish(xs_tuner_t *t, xs_tuner_state_t state, const char *msg,
                    const media_info_t *mi, uint64_t packets, uint64_t bytes)
{
   slock_lock(t->lock);
   if (t->gen == t->run_gen)   /* a stale run never overwrites a newer tune */
   {
      t->status.state   = state;
      t->status.gen     = t->run_gen;
      t->status.serial  = t->serial;
      t->status.packets = packets;
      t->status.bytes   = bytes;
      if (mi)
      {
         t->status.has_audio = mi->has_audio;
         t->status.has_video = mi->has_video;
         t->status.live      = mi->live;
         t->status.height    = mi->height;
         t->status.can_step_down = mi->can_step_down;
      }
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

/* A source with no known end. FFmpeg reports no duration for MPEG-TS over
 * HTTP, so an input of known size (Content-Length; not hls, whose pb is just
 * the playlist) counts as a file. Live HTTP streams are sent without one. */
static bool is_live(const AVFormatContext *fmt)
{
   if (fmt->duration != AV_NOPTS_VALUE)
      return false;
   if (strcmp(fmt->iformat->name, "hls") && fmt->pb && avio_size(fmt->pb) > 0)
      return false;
   return true;
}

/* Log what the source offers (M0). */
static void log_stream_info(xs_tuner_t *t, AVFormatContext *fmt, int64_t open_us)
{
   xs_ctx_t *ctx = t->ctx;
   unsigned i, j;

   XS_INFO(ctx, "opened in %lld ms: format %s, %s, %u program(s), %u stream(s)\n",
           (long long)(open_us / 1000), fmt->iformat->name,
           is_live(fmt) ? "live" : "fixed length",
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
         XS_INFO(ctx, "  stream #%u video: %s %dx%d SAR %d:%d %.3f fps scan %s, %lld kb/s\n",
                 i, codec, par->width, par->height,
                 par->sample_aspect_ratio.num, par->sample_aspect_ratio.den,
                 fr.den ? av_q2d(fr) : 0.0, scan_name(par->field_order),
                 (long long)(par->bit_rate / 1000));
      }
      else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
      {
         char layout[64];
         av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
         XS_INFO(ctx, "  stream #%u audio: %s %d Hz %s, %lld kb/s\n",
                 i, codec, par->sample_rate, layout, (long long)(par->bit_rate / 1000));
      }
      else
         XS_INFO(ctx, "  stream #%u %s: %s\n", i,
                 av_get_media_type_string(par->codec_type) ? av_get_media_type_string(par->codec_type) : "data",
                 codec);
   }
}

static int64_t program_bandwidth(const AVProgram *p)
{
   const AVDictionaryEntry *bw = av_dict_get(p->metadata, "variant_bitrate", NULL, 0);
   return bw ? strtoll(bw->value, NULL, 10) : 0;
}

/* Tallest video stream in a program (or in the whole file when p is NULL). */
static int tallest_video(const AVFormatContext *fmt, const AVProgram *p)
{
   unsigned i, n = p ? p->nb_stream_indexes : fmt->nb_streams;
   int best = -1;
   for (i = 0; i < n; i++)
   {
      int idx = p ? (int)p->stream_index[i] : (int)i;
      const AVCodecParameters *par = fmt->streams[idx]->codecpar;
      if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
          (best < 0 || par->height > fmt->streams[best]->codecpar->height))
         best = idx;
   }
   return best;
}

/* §5.3: the highest-resolution variant not above max_height (ties: highest
 * bandwidth), else the smallest. Every other variant's streams are
 * discarded, so the hls demuxer stops downloading them. Returns the video
 * stream index, or -1. */
static int select_variant(xs_tuner_t *t, AVFormatContext *fmt, unsigned max_height)
{
   const AVProgram *pick = NULL, *smallest = NULL;
   unsigned i, j;
   int vi, ph;

   t->var_lower = false;
   if (fmt->nb_programs < 2)
      return tallest_video(fmt, fmt->nb_programs ? fmt->programs[0] : NULL);

   for (i = 0; i < fmt->nb_programs; i++)
   {
      const AVProgram *p = fmt->programs[i];
      int v = tallest_video(fmt, p), h;
      if (v < 0)
         continue;
      h = fmt->streams[v]->codecpar->height;
      if (!smallest || h < fmt->streams[tallest_video(fmt, smallest)]->codecpar->height)
         smallest = p;
      if (max_height && h > (int)max_height)
         continue;
      if (!pick)
         pick = p;
      else
      {
         int ph = fmt->streams[tallest_video(fmt, pick)]->codecpar->height;
         if (h > ph || (h == ph && program_bandwidth(p) > program_bandwidth(pick)))
            pick = p;
      }
   }
   if (!pick)
      pick = smallest;
   if (!pick)
      return tallest_video(fmt, NULL);

   for (i = 0; i < fmt->nb_streams; i++)
   {
      bool keep = false;
      for (j = 0; j < pick->nb_stream_indexes; j++)
         keep |= pick->stream_index[j] == i;
      if (!keep)
         fmt->streams[i]->discard = AVDISCARD_ALL;
   }
   vi = tallest_video(fmt, pick);
   ph = vi >= 0 ? fmt->streams[vi]->codecpar->height : 0;
   for (i = 0; i < fmt->nb_programs; i++)
   {
      int v = tallest_video(fmt, fmt->programs[i]);
      if (v >= 0 && fmt->streams[v]->codecpar->height < ph)
         t->var_lower = true;
   }
   XS_INFO(t->ctx, "variant: program %d, %dx%d, %lld kb/s (of %u)%s\n", pick->id,
           vi >= 0 ? fmt->streams[vi]->codecpar->width : 0,
           vi >= 0 ? fmt->streams[vi]->codecpar->height : 0,
           (long long)(program_bandwidth(pick) / 1000), fmt->nb_programs,
           max_height ? " within the resolution cap" : "");
   return vi;
}

static void timeline_reset(xs_tuner_t *t)
{
   int i;
   t->epochs = 1;
   t->epoch_off_us[0] = 0;
   for (i = 0; i < XS_MAX_STREAMS; i++)
   {
      t->stream_epoch[i] = 0;
      t->last_end_us[i]  = AV_NOPTS_VALUE;
   }
   t->max_end_us = AV_NOPTS_VALUE;
}

static unsigned open_epoch(xs_tuner_t *t, int64_t off_us)
{
   unsigned id = t->epochs++;
   t->epoch_off_us[id % XS_EPOCHS] = off_us;
   return id;
}

static bool continues(int64_t delta_us)
{
   return delta_us >= -XS_BACK_MAX_US && delta_us <= XS_GAP_MAX_US;
}

/* One continuous timeline (§5.2, D-023). Streams are interleaved, so after
 * a discontinuity one stream's new timestamps can arrive while another's
 * old ones are still coming: a single shared offset would flip back and
 * forth. Instead a stream that jumps moves to the next epoch: it adopts
 * that epoch's offset if another stream already opened it and the offset
 * fits, else it opens it from its own continuity. */
static void unwrap(xs_tuner_t *t, const AVStream *st, AVPacket *pkt)
{
   int idx = pkt->stream_index;
   int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
   int64_t ts_us, dur_us, off_us, off;

   if (ts == AV_NOPTS_VALUE || idx < 0 || idx >= XS_MAX_STREAMS)
      return;
   ts_us  = av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q);
   dur_us = pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 0;
   if (t->rejoin)
   {
      /* After a reconnect: carry on right after what is already buffered,
       * whatever the new connection's timestamps are. */
      int i;
      unsigned e = open_epoch(t, t->max_end_us - ts_us);
      t->rejoin = false;
      for (i = 0; i < XS_MAX_STREAMS; i++)
      {
         t->stream_epoch[i] = e;
         t->last_end_us[i]  = AV_NOPTS_VALUE;
      }
      XS_INFO(t->ctx, "timeline: reconnected, joined at %.3f s\n", t->max_end_us / 1e6);
   }
   else if (t->last_end_us[idx] != AV_NOPTS_VALUE)
   {
      unsigned e = t->stream_epoch[idx];
      int64_t delta = ts_us + t->epoch_off_us[e % XS_EPOCHS] - t->last_end_us[idx];
      if (!continues(delta))
      {
         unsigned next = e + 1;
         if (next < t->epochs &&
             continues(ts_us + t->epoch_off_us[next % XS_EPOCHS] - t->last_end_us[idx]))
            XS_DEBUG(t->ctx, "timeline: stream #%d follows into epoch %u\n", idx, next);
         else
         {
            next = open_epoch(t, t->last_end_us[idx] - ts_us);
            XS_INFO(t->ctx, "timeline: discontinuity of %+.3f s on stream #%d, joined (epoch %u)\n",
                    delta / 1e6, idx, next);
         }
         t->stream_epoch[idx] = next;
      }
   }
   off_us = t->epoch_off_us[t->stream_epoch[idx] % XS_EPOCHS];
   t->last_end_us[idx] = ts_us + off_us + dur_us;
   if (t->max_end_us == AV_NOPTS_VALUE || t->last_end_us[idx] > t->max_end_us)
      t->max_end_us = t->last_end_us[idx];
   off = av_rescale_q(off_us, AV_TIME_BASE_Q, st->time_base);
   if (pkt->pts != AV_NOPTS_VALUE)
      pkt->pts += off;
   if (pkt->dts != AV_NOPTS_VALUE)
      pkt->dts += off;
}

/* ---- audio tracks ----------------------------------------------------------- */

/* ISO 639-2 has bibliographic codes beside the terminology ones that
 * iptv-org and most muxers use: treat them as the same language. */
static bool same_language(const char *a, const char *b)
{
   static const char *pairs[][2] = {
      { "fre", "fra" }, { "ger", "deu" }, { "dut", "nld" }, { "chi", "zho" }, { "cze", "ces" },
      { "gre", "ell" }, { "per", "fas" }, { "rum", "ron" }, { "slo", "slk" }, { "alb", "sqi" },
      { "arm", "hye" }, { "baq", "eus" }, { "bur", "mya" }, { "geo", "kat" }, { "ice", "isl" },
      { "mac", "mkd" }, { "mao", "mri" }, { "may", "msa" }, { "tib", "bod" }, { "wel", "cym" },
   };
   unsigned i;
   if (!strcasecmp(a, b))
      return true;
   for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
      if ((!strcasecmp(a, pairs[i][0]) && !strcasecmp(b, pairs[i][1])) ||
          (!strcasecmp(a, pairs[i][1]) && !strcasecmp(b, pairs[i][0])))
         return true;
   return false;
}

static const char *stream_lang(const AVStream *st)
{
   const AVDictionaryEntry *e = av_dict_get(st->metadata, "language", NULL, 0);
   return e && e->value[0] ? e->value : "";
}

static bool described(const AVStream *st)
{
   return (st->disposition & (AV_DISPOSITION_VISUAL_IMPAIRED | AV_DISPOSITION_HEARING_IMPAIRED)) != 0;
}

/* The audio track to start with: the preferred language (not an
 * audio-description track if there's another), else FFmpeg's best. */
static unsigned pick_audio(const AVFormatContext *fmt, const int *cand, unsigned n, int best, const char *lang)
{
   unsigned i, pass;
   if (lang[0])
      for (pass = 0; pass < 2; pass++)
         for (i = 0; i < n; i++)
         {
            const AVStream *st = fmt->streams[cand[i]];
            if (same_language(stream_lang(st), lang) && (pass || !described(st)))
               return i;
         }
   for (i = 0; i < n; i++)
      if (cand[i] == best)
         return i;
   return 0;
}

static void audio_status(xs_tuner_t *t, const AVFormatContext *fmt, const int *cand, unsigned n, unsigned def)
{
   unsigned i;
   slock_lock(t->lock);
   if (t->gen == t->run_gen)
   {
      t->status.audio_tracks  = n;
      t->status.audio_default = def;
      for (i = 0; i < XS_MAX_TRACKS; i++)
      {
         snprintf(t->status.audio_lang[i], sizeof(t->status.audio_lang[i]), "%s",
                  i < n ? stream_lang(fmt->streams[cand[i]]) : "");
         t->status.audio_described[i] = i < n && described(fmt->streams[cand[i]]);
      }
   }
   slock_unlock(t->lock);
}

/* ---- one attempt ---------------------------------------------------------- */

typedef enum { RUN_ABORTED, RUN_FAILED, RUN_ENDED, RUN_STEP_DOWN } run_result_t;


/* One attempt. resume: a reconnect after this serial already delivered
 * media, so keep the serial and everything buffered, and join the new
 * connection's timeline onto it. *delivered: this attempt read packets. */
static run_result_t play_source(xs_tuner_t *t, const xs_source_t *src, unsigned max_height,
                                bool resume, bool *delivered)
{
   xs_ctx_t *ctx = t->ctx;
   AVIOInterruptCB icb = { interrupt_cb, t };
   AVFormatContext *fmt = NULL;
   AVPacket *pkt = NULL;
   xs_adec_t *adec[XS_MAX_TRACKS] = { NULL };
   media_info_t mi;
   char msg[256];
   uint64_t packets = 0, bytes = 0;
   int64_t start = av_gettime_relative(), first_pkt = 0;
   run_result_t result;
   int ret, vi, ai = -1, i;
   int cand[XS_MAX_AUDIO];
   unsigned ncand = 0, acur = 0, atrk = 0;

   memset(&mi, 0, sizeof(mi));

   *delivered = false;
   if (resume)
      t->rejoin = t->max_end_us != AV_NOPTS_VALUE;
   else
   {
      t->serial++;
      xs_pktq_flush(t->vq);
      for (i = 0; i < XS_MAX_TRACKS; i++)
         xs_aring_flush(t->aring[i], t->serial);
      xs_video_flush(t->video, t->serial);
      timeline_reset(t);
      t->rejoin = false;
      publish(t, XS_TUNER_CONNECTING, "Tuning", &mi, 0, 0);
   }
   XS_INFO(ctx, "tuning %s\n", src->url);

   t->timed_out = false;
   t->deadline  = start + XS_OPEN_TIMEOUT_US;
   ret = xs_source_open(ctx, src, &icb, &fmt);
   if (ret < 0)
   {
      if (superseded(t))
      {
         XS_DEBUG(ctx, "tune aborted after %lld ms\n", (long long)((av_gettime_relative() - start) / 1000));
         return RUN_ABORTED;
      }
      snprintf(msg, sizeof(msg), "No signal (%s)", t->timed_out ? "timed out" : av_err2str(ret));
      XS_WARN(ctx, "open failed: %s\n", msg);
      publish(t, XS_TUNER_FAILED, msg, &mi, 0, 0);
      return RUN_FAILED;
   }

   log_stream_info(t, fmt, av_gettime_relative() - start);
   vi = select_variant(t, fmt, max_height);
   if (vi >= 0)
   {
      AVStream *st = fmt->streams[vi];
      xs_video_configure(t->video, t->serial, st, av_guess_frame_rate(fmt, st, NULL));
      mi.has_video = true;
      t->var_height = (unsigned)st->codecpar->height;
      mi.height = t->var_height;
      mi.can_step_down = t->var_lower;
   }
   else
      XS_WARN(ctx, "no video stream\n");
   /* Audio tracks: those of the chosen variant that can be decoded (§M4).
    * Every one is decoded, each into its own ring, so switching tracks is
    * instant: the presenter just reads another ring. */
   for (i = 0; i < (int)fmt->nb_streams && ncand < XS_MAX_AUDIO; i++)
      if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && fmt->streams[i]->discard != AVDISCARD_ALL &&
          (adec[ncand] = xs_adec_open(ctx, fmt->streams[i])))
         cand[ncand++] = i;
   mi.has_audio = ncand > 0;
   if (ncand)
   {
      char lang[8];
      slock_lock(t->lock);
      snprintf(lang, sizeof(lang), "%s", t->audio_lang);
      slock_unlock(t->lock);
      acur = pick_audio(fmt, cand, ncand, av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0), lang);
      ai = cand[acur];
   }
   for (i = 0; i < (int)fmt->nb_streams; i++)
   {
      bool keep = i == vi;
      unsigned k;
      for (k = 0; k < ncand; k++)
         keep |= cand[k] == i;
      if (!keep)
         fmt->streams[i]->discard = AVDISCARD_ALL;
   }
   mi.live = is_live(fmt);

   if (vi >= 0)
   {
      const AVCodecParameters *par = fmt->streams[vi]->codecpar;
      AVRational fr = av_guess_frame_rate(fmt, fmt->streams[vi], NULL);
      snprintf(msg, sizeof(msg), "%s %dx%d %.4g fps%s%s", avcodec_get_name(par->codec_id),
               par->width, par->height, fr.den ? av_q2d(fr) : 0.0,
               ai >= 0 ? ", " : "", ai >= 0 ? avcodec_get_name(fmt->streams[ai]->codecpar->codec_id) : "");
   }
   else
      snprintf(msg, sizeof(msg), "%s (audio only)", ai >= 0 ? avcodec_get_name(fmt->streams[ai]->codecpar->codec_id) : "?");
   publish(t, XS_TUNER_PLAYING, msg, &mi, 0, 0);
   audio_status(t, fmt, cand, ncand, acur);
   if (ncand > 1)
      XS_INFO(ctx, "audio: %u tracks, starting with %u (%s%s)\n", ncand, acur + 1,
              stream_lang(fmt->streams[ai])[0] ? stream_lang(fmt->streams[ai]) : "no language tag",
              described(fmt->streams[ai]) ? ", described" : "");

   pkt = av_packet_alloc();
   for (;;)
   {
      AVStream *st;
      bool ok = true;

      t->deadline = av_gettime_relative() + XS_DATA_TIMEOUT_US;
      ret = pkt ? av_read_frame(fmt, pkt) : AVERROR(ENOMEM);
      if (ret == AVERROR(EAGAIN))
      {
         av_usleep(10000);
         continue;
      }
      if (ret < 0)
         break;
      if (pkt->stream_index != vi)
      {
         unsigned k;
         for (k = 0; k < ncand && cand[k] != pkt->stream_index; k++)
            ;
         if (k == ncand)
         {
            av_packet_unref(pkt);
            continue;
         }
         atrk = k;
      }
      if (!packets)
      {
         *delivered = true;
         first_pkt = av_gettime_relative();
         XS_INFO(ctx, "first packet %lld ms after tune\n", (long long)((first_pkt - start) / 1000));
      }
      packets++;
      bytes += (uint64_t)pkt->size;
      st = fmt->streams[pkt->stream_index];
      unwrap(t, st, pkt);

      if (pkt->stream_index == vi)
         ok = xs_pktq_put(t->vq, pkt, t->serial,
                          pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 0,
                          stop_cb, t);
      else
         ok = xs_adec_packet(adec[atrk], pkt, t->aring[atrk], t->serial, stop_cb, t);
      av_packet_unref(pkt);
      if (!ok)
         break;   /* superseded while waiting for space */
      if (!(packets & 63))
         publish(t, XS_TUNER_PLAYING, NULL, NULL, packets, bytes);
   }

   XS_INFO(ctx, "read %llu packets, %llu KB in %.1f s\n", (unsigned long long)packets,
           (unsigned long long)(bytes / 1024),
           first_pkt ? (av_gettime_relative() - first_pkt) / 1e6 : 0.0);
   if (superseded(t))
      result = RUN_ABORTED;   /* channel change or unload: nothing to report */
   else if (step_down_asked(t))
      result = RUN_STEP_DOWN;
   else
   {
      if (ret == AVERROR_EOF && !mi.live)
      {
         /* Let the decoders finish what they hold. Not on a live failure:
          * the reconnect continues with the same decoders. */
         for (i = 0; i < (int)ncand; i++)
            if (adec[i])
               xs_adec_packet(adec[i], NULL, t->aring[i], t->serial, stop_cb, t);
         if (vi >= 0 && pkt)
         {
            av_packet_unref(pkt);   /* empty packet = drain */
            xs_pktq_put(t->vq, pkt, t->serial, 0, stop_cb, t);
         }
         XS_INFO(ctx, "end of stream\n");
         publish(t, XS_TUNER_ENDED, "End of stream", NULL, packets, bytes);
         result = RUN_ENDED;
      }
      else
      {
         snprintf(msg, sizeof(msg), "Signal lost (%s)",
                  t->timed_out ? "timed out" : ret == AVERROR_EOF ? "stream ended" : av_err2str(ret));
         XS_WARN(ctx, "%s\n", msg);
         publish(t, XS_TUNER_FAILED, msg, NULL, packets, bytes);
         result = RUN_FAILED;
      }
   }
   av_packet_free(&pkt);
   for (i = 0; i < XS_MAX_TRACKS; i++)
      xs_adec_free(adec[i]);
   avformat_close_input(&fmt);
   return result;
}

/* Wait before a retry; false if a newer command arrived meanwhile. */
static bool retry_wait(xs_tuner_t *t, int64_t us)
{
   int64_t until = av_gettime_relative() + us;
   bool stop;
   slock_lock(t->lock);
   while (!(stop = t->quit || t->gen != t->run_gen) && av_gettime_relative() < until)
      scond_wait_timeout(t->cond, t->lock, until - av_gettime_relative());
   slock_unlock(t->lock);
   return !stop;
}

static void tuner_thread(void *arg)
{
   xs_tuner_t *t = arg;
   xs_source_t src;
   unsigned max_height;

   sthread_setname("xs-tuner");
   slock_lock(t->lock);
   for (;;)
   {
      int64_t backoff = XS_RETRY_MIN_US;
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
      max_height = t->max_height;
      t->step_down = false;
      slock_unlock(t->lock);
      bool resume = false;
      unsigned step_cap = 0;   /* from step-downs, for this tune */
      for (;;)
      {
         bool delivered;
         unsigned cap = step_cap && (!max_height || step_cap < max_height) ? step_cap : max_height;
         run_result_t r = play_source(t, &src, cap, resume, &delivered);
         if (r == RUN_STEP_DOWN)
         {
            /* Same stream, the next lower variant, nothing buffered lost. */
            step_cap = t->var_height > 1 ? t->var_height - 1 : 1;
            XS_INFO(t->ctx, "variant: %up can't keep up, stepping down\n", t->var_height);
            resume = true;
            slock_lock(t->lock);
            t->step_down = false;
            slock_unlock(t->lock);
            continue;
         }
         if (r != RUN_FAILED)
            break;
         /* Once a serial has delivered media, reconnects keep it (and what
          * is buffered); a lost connection is retried at once. */
         if (delivered)
            backoff = 0;
         resume = resume || delivered;
         XS_INFO(t->ctx, "retrying in %.1f s\n", backoff / 1e6);
         if (!retry_wait(t, backoff))
            break;
         backoff = backoff * 2 > XS_RETRY_MAX_US ? XS_RETRY_MAX_US :
                   backoff ? backoff * 2 : XS_RETRY_MIN_US;
         slock_lock(t->lock);
         max_height = t->max_height;
         slock_unlock(t->lock);
      }
      slock_lock(t->lock);
   }
   slock_unlock(t->lock);
}

/* ---- API (emulation thread) --------------------------------------------- */

xs_tuner_t *xs_tuner_new(xs_ctx_t *ctx)
{
   xs_tuner_t *t = calloc(1, sizeof(*t));
   bool rings = true;
   unsigned i;
   if (!t)
      return NULL;
   t->ctx   = ctx;
   t->lock  = slock_new();
   t->cond  = scond_new();
   t->vq    = xs_pktq_new(XS_QUEUE_SECONDS * 1000000LL, XS_VQUEUE_BYTES);
   for (i = 0; i < XS_MAX_TRACKS; i++)
      rings &= (t->aring[i] = xs_aring_new(XS_QUEUE_SECONDS + 4)) != NULL;
   if (t->vq)
      t->video = xs_video_new(ctx, t->vq);
   if (t->lock && t->cond && rings && t->video)
      t->thread = sthread_create(tuner_thread, t);
   if (!t->thread)
   {
      XS_ERROR(ctx, "can't start the tuner\n");
      xs_video_free(t->video);
      xs_pktq_free(t->vq);
      for (i = 0; i < XS_MAX_TRACKS; i++)
         xs_aring_free(t->aring[i]);
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
   unsigned i;
   if (!t)
      return;
   start = av_gettime_relative();
   slock_lock(t->lock);
   t->quit = true;
   scond_signal(t->cond);
   slock_unlock(t->lock);
   sthread_join(t->thread);
   xs_video_free(t->video);
   XS_DEBUG(t->ctx, "tuner stopped in %lld ms\n", (long long)((av_gettime_relative() - start) / 1000));
   xs_pktq_free(t->vq);
   for (i = 0; i < XS_MAX_TRACKS; i++)
      xs_aring_free(t->aring[i]);
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

void xs_tuner_set_audio_lang(xs_tuner_t *t, const char *lang)
{
   slock_lock(t->lock);
   snprintf(t->audio_lang, sizeof(t->audio_lang), "%s", lang ? lang : "");
   slock_unlock(t->lock);
}

void xs_tuner_step_down(xs_tuner_t *t)
{
   slock_lock(t->lock);
   if (t->status.can_step_down && t->status.gen == t->gen)
      t->step_down = true;
   slock_unlock(t->lock);
}

void xs_tuner_set_max_height(xs_tuner_t *t, unsigned max_height)
{
   slock_lock(t->lock);
   t->max_height = max_height;
   slock_unlock(t->lock);
}

xs_aring_t *xs_tuner_aring(xs_tuner_t *t, unsigned i) { return t->aring[i < XS_MAX_TRACKS ? i : 0]; }
xs_video_t *xs_tuner_video(xs_tuner_t *t) { return t->video; }
