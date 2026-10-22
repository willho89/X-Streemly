/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libretro entry points (CLAUDE.md §3). The only file allowed globals.
 *
 * Content -> source -> tuner; each retro_run asks the presenter for the
 * tick's audio and picture and hands them on (CLAUDE.md §5.5, D-008).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <libretro.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/time.h>
#include <rthreads/rthreads.h>

#include "xs_ctx.h"
#include "xs_present.h"
#include "xs_source.h"
#include "xs_tuner.h"

/* Declared at load: the tuning card's size, room for 1080p. A bigger stream
 * raises the maximum with SET_SYSTEM_AV_INFO (§5.4). */
#define XS_BASE_WIDTH   640
#define XS_BASE_HEIGHT  360
#define XS_MAX_WIDTH    1920
#define XS_MAX_HEIGHT   1080
#define XS_DEFAULT_FPS  60.0

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static xs_ctx_t   g_ctx_storage;
static xs_ctx_t  *g_ctx;
static bool       g_can_dupe;
static double     g_fps = XS_DEFAULT_FPS;
static double     g_audio_accum;
static xs_source_t g_source;
static bool       g_have_source;
static xs_present_t *g_present;

/* What the frontend currently believes. */
static struct retro_system_av_info g_av;

/* Last tuner status shown as a frontend message. */
static unsigned         g_osd_gen, g_osd_serial;
static unsigned         g_played_serial;   /* last serial seen playing */
static xs_tuner_state_t g_osd_state;

/* One tick of audio at the lowest supported display rate (23 Hz). */
#define XS_AUDIO_MAX_TICK 2100
static int16_t  g_audio[2 * XS_AUDIO_MAX_TICK];

/* ---- logging ------------------------------------------------------------ */

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void xs_log(xs_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...)
{
   char buf[2048];
   va_list va;
   retro_log_printf_t cb = (ctx && ctx->log_cb) ? ctx->log_cb : (log_cb ? log_cb : fallback_log);

   va_start(va, fmt);
   vsnprintf(buf, sizeof(buf), fmt, va);
   va_end(va);
   cb(level, "[X-Streemly] %s", buf);
}

/* FFmpeg logs from any thread, a line at a time or in pieces: assemble whole
 * lines (one buffer, under a lock) before handing them to the frontend. */
static slock_t *g_ff_log_lock;
static char     g_ff_line[2048];
static int      g_ff_prefix = 1;

static void ff_log(void *avcl, int level, const char *fmt, va_list vl)
{
   char part[1024];
   size_t n;
   enum retro_log_level rl;

   if (level > av_log_get_level() || !g_ff_log_lock)
      return;
   rl = level <= AV_LOG_ERROR ? RETRO_LOG_ERROR : level <= AV_LOG_WARNING ? RETRO_LOG_WARN :
        level <= AV_LOG_INFO ? RETRO_LOG_INFO : RETRO_LOG_DEBUG;

   slock_lock(g_ff_log_lock);
   av_log_format_line2(avcl, level, fmt, vl, part, sizeof(part), &g_ff_prefix);
   n = strlen(g_ff_line);
   snprintf(g_ff_line + n, sizeof(g_ff_line) - n, "%s", part);
   n = strlen(g_ff_line);
   if (n && (g_ff_line[n - 1] == '\n' || n == sizeof(g_ff_line) - 1))
   {
      xs_log(g_ctx, rl, "ffmpeg: %s%s", g_ff_line, g_ff_line[n - 1] == '\n' ? "" : "\n");
      g_ff_line[0] = '\0';
   }
   slock_unlock(g_ff_log_lock);
}

static void show_message(unsigned ms, bool alert, const char *fmt, ...)
{
   char buf[256];
   struct retro_message_ext msg;
   unsigned version = 0;
   va_list va;

   va_start(va, fmt);
   vsnprintf(buf, sizeof(buf), fmt, va);
   va_end(va);
   XS_DEBUG(g_ctx, "OSD: %s\n", buf);
   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) && version >= 1)
   {
      memset(&msg, 0, sizeof(msg));
      msg.msg      = buf;
      msg.duration = ms;
      msg.priority = alert ? 3 : 1;
      msg.level    = alert ? RETRO_LOG_WARN : RETRO_LOG_INFO;
      msg.target   = RETRO_MESSAGE_TARGET_OSD;
      msg.type     = alert ? RETRO_MESSAGE_TYPE_NOTIFICATION : RETRO_MESSAGE_TYPE_NOTIFICATION_ALT;
      msg.progress = -1;
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
   }
   else
   {
      struct retro_message old = { buf, (unsigned)(ms * 60 / 1000) };
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &old);
   }
}

/* ---- core options --------------------------------------------------------- */

static struct retro_core_option_v2_category g_opt_categories[] = {
   { "video", "Video", "Resolution and deinterlacing." },
   { "advanced", "Advanced", "Buffering and logging." },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition g_opt_defs[] = {
   {
      "xstreemly_max_resolution", "Max Resolution", NULL,
      "Highest picture height to use. Streams that offer several qualities (HLS) play the best "
      "one within this; others are scaled down to it. Applies at the next tune.",
      NULL, "video",
      { { "native", "Native" }, { "1080", "1080p" }, { "720", "720p" }, { "576", "576p" },
        { "480", "480p" }, { NULL, NULL } },
      "native"
   },
   {
      "xstreemly_deinterlace", "Deinterlace", NULL,
      "For interlaced broadcasts (1080i, 576i, 480i). Field-rate modes show every field, "
      "for smooth motion. Progressive pictures are never touched.",
      NULL, "video",
      { { "bwdif", "Bwdif, field rate" }, { "yadif", "Yadif, field rate" }, { "off", "Off" },
        { NULL, NULL } },
      "bwdif"
   },
   {
      "xstreemly_osd", "Show OSD", NULL,
      "Text on the tuning card and status messages.",
      NULL, "video",
      { { "on", "On" }, { "off", "Off" }, { NULL, NULL } },
      "on"
   },
   {
      "xstreemly_buffer", "Buffer", NULL,
      "How much to buffer before playing, and the least kept while playing. More rides out a "
      "shaky connection; less starts sooner.",
      NULL, "advanced",
      { { "500", "0.5 s" }, { "1000", "1 s" }, { "2000", "2 s" }, { "4000", "4 s" }, { NULL, NULL } },
      "1000"
   },
   {
      "xstreemly_verbose_log", "Verbose Logging", NULL,
      "Log FFmpeg's connection and stream details (every playlist and segment request).",
      NULL, "advanced",
      { { "off", "Off" }, { "on", "On" }, { NULL, NULL } },
      "off"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 g_opts_v2 = { g_opt_categories, g_opt_defs };

static void set_core_options(retro_environment_t cb)
{
   unsigned version = 0;
   if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
   {
      cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &g_opts_v2);
      return;
   }
   {
      /* Legacy frontends: "Desc; default|other|...". */
#define XS_NUM_OPTS (sizeof(g_opt_defs) / sizeof(g_opt_defs[0]) - 1)
      static char desc[XS_NUM_OPTS][1024];
      static struct retro_variable vars[XS_NUM_OPTS + 1];
      unsigned i;
      for (i = 0; i < XS_NUM_OPTS; i++)
      {
         const struct retro_core_option_v2_definition *d = &g_opt_defs[i];
         size_t n = (size_t)snprintf(desc[i], sizeof(desc[i]), "%s; %s", d->desc, d->default_value);
         int v;
         for (v = 0; d->values[v].value && n < sizeof(desc[i]); v++)
            if (strcmp(d->values[v].value, d->default_value))
               n += (size_t)snprintf(desc[i] + n, sizeof(desc[i]) - n, "|%s", d->values[v].value);
         vars[i].key   = d->key;
         vars[i].value = desc[i];
      }
      vars[XS_NUM_OPTS].key = vars[XS_NUM_OPTS].value = NULL;
      cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
   }
}

static const char *get_option(const char *key)
{
   struct retro_variable var = { key, NULL };
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return NULL;
}

static void read_options(xs_ctx_t *ctx)
{
   const char *v = get_option("xstreemly_verbose_log");
   ctx->opt.verbose = v && !strcmp(v, "on");
   av_log_set_level(ctx->opt.verbose ? AV_LOG_VERBOSE : AV_LOG_WARNING);

   v = get_option("xstreemly_max_resolution");
   ctx->opt.max_height = v && strcmp(v, "native") ? (unsigned)atoi(v) : 0;
   v = get_option("xstreemly_deinterlace");
   ctx->opt.deint = !v || !strcmp(v, "bwdif") ? XS_DEINT_BWDIF :
                    !strcmp(v, "yadif") ? XS_DEINT_YADIF : XS_DEINT_OFF;
   v = get_option("xstreemly_osd");
   ctx->opt.osd = !v || strcmp(v, "off");
   v = get_option("xstreemly_buffer");
   ctx->opt.buffer_ms = v ? (unsigned)atoi(v) : 1000;
   if (ctx->opt.buffer_ms < 200 || ctx->opt.buffer_ms > 10000)
      ctx->opt.buffer_ms = 1000;

   if (ctx->tuner)
   {
      xs_tuner_set_max_height(ctx->tuner, ctx->opt.max_height);
      xs_video_set_options(xs_tuner_video(ctx->tuner), ctx->opt.deint, ctx->opt.max_height);
   }
}

/* ---- libretro API ----------------------------------------------------- */

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = true;

   environ_cb = cb;
   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   set_core_options(cb);
   /* "Start Core" with no content: the iptv-org lineup (M3). */
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   g_ctx = &g_ctx_storage;
   memset(g_ctx, 0, sizeof(*g_ctx));
   g_ctx->log_cb = log_cb;
   g_ff_log_lock = slock_new();
   av_log_set_level(AV_LOG_WARNING);
   av_log_set_callback(ff_log);
   avformat_network_init();
}

RETRO_API void retro_deinit(void)
{
   avformat_network_deinit();
   av_log_set_callback(av_log_default_callback);
   if (g_ff_log_lock)
      slock_free(g_ff_log_lock);
   g_ff_log_lock = NULL;
   g_ctx = NULL;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "X-Streemly";
   info->library_version  = XS_VERSION;
   info->valid_extensions = "strm";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = XS_BASE_WIDTH;
   info->geometry.base_height  = XS_BASE_HEIGHT;
   info->geometry.max_width    = XS_MAX_WIDTH;
   info->geometry.max_height   = XS_MAX_HEIGHT;
   info->geometry.aspect_ratio = 16.0f / 9.0f;
   info->timing.fps            = g_fps;
   info->timing.sample_rate    = XS_SAMPLE_RATE;
   g_av = *info;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { }

RETRO_API void retro_reset(void)
{
   if (g_ctx && g_ctx->tuner && g_have_source)
      xs_tuner_tune(g_ctx->tuner, &g_source);   /* re-tune */
}

/* Display-rate output (D-008): frames are presented by PTS, so the core
 * runs at whatever the frontend's display runs at. */
static void query_display_rate(void)
{
   float rate = 0;
   g_fps = XS_DEFAULT_FPS;
   if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &rate) && rate >= 23.0f && rate <= 240.0f)
      g_fps = rate;
   XS_INFO(g_ctx, "output rate %.3f Hz\n", g_fps);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   char err[512];

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      XS_ERROR(g_ctx, "frontend lacks XRGB8888\n");
      return false;
   }
   g_can_dupe = false;
   environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe);
   read_options(g_ctx);
   query_display_rate();
   XS_INFO(g_ctx, "X-Streemly %s, %s\n", XS_VERSION, avformat_configuration()[0] ? "FFmpeg " LIBAVFORMAT_IDENT : "");

   g_audio_accum = 0;
   g_osd_gen     = 0;
   g_osd_serial  = 0;
   g_played_serial = 0;
   g_osd_state   = XS_TUNER_IDLE;
   g_have_source = false;

   if (game && game->path &&
       !xs_source_from_content(g_ctx, game->path, &g_source, err, sizeof(err)))
   {
      XS_ERROR(g_ctx, "%s\n", err);
      return false;
   }

   g_ctx->tuner = xs_tuner_new(g_ctx);
   if (!g_ctx->tuner)
      return false;
   g_present = xs_present_new(g_ctx, g_ctx->tuner);
   if (!g_present)
   {
      xs_tuner_free(g_ctx->tuner);
      g_ctx->tuner = NULL;
      return false;
   }
   read_options(g_ctx);   /* again, now that the tuner exists */

   if (!game || !game->path)
   {
      show_message(6000, false, "No content: load a .strm file to tune a stream");
      return true;
   }
   xs_present_set_name(g_present, g_source.name);
   g_have_source = true;
   xs_tuner_tune(g_ctx->tuner, &g_source);
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}

RETRO_API void retro_unload_game(void)
{
   if (g_present)
   {
      xs_present_log_stats(g_present);
      xs_present_free(g_present);
      g_present = NULL;
   }
   if (g_ctx && g_ctx->tuner)
   {
      xs_tuner_free(g_ctx->tuner);
      g_ctx->tuner = NULL;
   }
   g_have_source = false;
}

/* Tell the viewer what the tuner is doing when that changes. */
static void update_osd(void)
{
   xs_tuner_status_t st;
   if (!g_ctx->tuner)
      return;
   xs_tuner_get_status(g_ctx->tuner, &st);
   if (st.gen == g_osd_gen && st.serial == g_osd_serial && st.state == g_osd_state)
      return;
   g_osd_gen    = st.gen;
   g_osd_serial = st.serial;
   g_osd_state  = st.state;
   if (!g_ctx->opt.osd)
      return;
   switch (st.state)
   {
      case XS_TUNER_PLAYING:
         if (st.serial != g_played_serial)   /* not on a reconnect */
            show_message(4000, false, "%s", st.msg);
         g_played_serial = st.serial;
         break;
      case XS_TUNER_FAILED:
         /* A loss mid-stream reconnects behind the buffer; if the picture
          * does stall, the NO SIGNAL card says so. */
         if (st.serial != g_played_serial)
            show_message(6000, true, "%s", st.msg);
         break;
      default: break;
   }
}

/* Tell the frontend about a new picture size or shape (§5.4): a cheap
 * SET_GEOMETRY within the declared maximum, SET_SYSTEM_AV_INFO beyond it. */
static void update_geometry(const xs_picture_t *pic)
{
   struct retro_game_geometry *g = &g_av.geometry;
   if (pic->width == g->base_width && pic->height == g->base_height &&
       fabsf(pic->aspect - g->aspect_ratio) < 0.001f)
      return;
   g->base_width   = pic->width;
   g->base_height  = pic->height;
   g->aspect_ratio = pic->aspect;
   if (pic->width > g->max_width || pic->height > g->max_height)
   {
      g->max_width  = pic->width > g->max_width ? pic->width : g->max_width;
      g->max_height = pic->height > g->max_height ? pic->height : g->max_height;
      XS_INFO(g_ctx, "picture %ux%u: raising the maximum to %ux%u\n", pic->width, pic->height,
              g->max_width, g->max_height);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &g_av);
   }
   else
   {
      XS_DEBUG(g_ctx, "geometry %ux%u aspect %.4f\n", pic->width, pic->height, pic->aspect);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, g);
   }
}

RETRO_API void retro_run(void)
{
   xs_picture_t pic;
   bool updated = false, resized;
   unsigned n;
   struct retro_game_geometry before = g_av.geometry;
   static int64_t last_end;
   int64_t start = av_gettime_relative();

   if (last_end && start - last_end > g_ctx->run.gap_max_us)
      g_ctx->run.gap_max_us = start - last_end;
   input_poll_cb();
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      read_options(g_ctx);
   update_osd();

   g_audio_accum += XS_SAMPLE_RATE / g_fps;
   n = (unsigned)g_audio_accum;
   if (n > XS_AUDIO_MAX_TICK)
      n = XS_AUDIO_MAX_TICK;
   g_audio_accum -= n;

   xs_present_tick(g_present, g_fps, g_audio, n, &pic);
   update_geometry(&pic);
   resized = before.base_width != g_av.geometry.base_width ||
             before.base_height != g_av.geometry.base_height;

   /* Video first: the frame belongs to the start of this tick's audio. */
   if (pic.dupe && g_can_dupe && !resized)
      video_cb(NULL, pic.width, pic.height, pic.pitch);
   else
      video_cb(pic.data, pic.width, pic.height, pic.pitch);
   if (n)
      audio_batch_cb(g_audio, n);

   /* The frontend's callbacks count too: they are what it does with us. */
   last_end = av_gettime_relative();
   g_ctx->run.run_sum_us += last_end - start;
   if (last_end - start > g_ctx->run.run_max_us)
      g_ctx->run.run_max_us = last_end - start;
   g_ctx->run.runs++;
}

/* Live TV has nothing to restore (CLAUDE.md §5.12). */
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { return false; }
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
