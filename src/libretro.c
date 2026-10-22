/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libretro entry points (CLAUDE.md §3). The only file allowed globals.
 *
 * M0: tunes the content's URL on the tuner thread and logs what it finds;
 * presents a black picture and silence at the display rate meanwhile.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <rthreads/rthreads.h>

#include "xs_ctx.h"
#include "xs_source.h"
#include "xs_tuner.h"

/* Until a stream gives its size (M1): a small black 16:9 picture. */
#define XS_IDLE_WIDTH   640
#define XS_IDLE_HEIGHT  360
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
static bool       g_frame_sent;
static double     g_fps = XS_DEFAULT_FPS;
static double     g_audio_accum;
static xs_source_t g_source;
static bool       g_have_source;

/* Last tuner status shown on the OSD. */
static unsigned         g_osd_gen;
static xs_tuner_state_t g_osd_state;

static uint32_t g_black[XS_IDLE_WIDTH * XS_IDLE_HEIGHT];
#define XS_AUDIO_CHUNK 1024
static int16_t  g_silence[2 * XS_AUDIO_CHUNK];

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
   { "advanced", "Advanced", "Logging." },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition g_opt_defs[] = {
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
   info->geometry.base_width   = XS_IDLE_WIDTH;
   info->geometry.base_height  = XS_IDLE_HEIGHT;
   info->geometry.max_width    = XS_MAX_WIDTH;
   info->geometry.max_height   = XS_MAX_HEIGHT;
   info->geometry.aspect_ratio = 16.0f / 9.0f;
   info->timing.fps            = g_fps;
   info->timing.sample_rate    = XS_SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { }

RETRO_API void retro_reset(void)
{
   if (g_ctx && g_ctx->tuner && g_have_source)
      xs_tuner_tune(g_ctx->tuner, &g_source);   /* re-tune */
}

/* Display-rate output (D-008): frames are presented by PTS from M1 on, so
 * the core runs at whatever the frontend's display runs at. */
static void query_display_rate(void)
{
   float rate = 0;
   g_fps = XS_DEFAULT_FPS;
   if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &rate) && rate >= 23.0f && rate <= 300.0f)
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

   g_frame_sent  = false;
   g_audio_accum = 0;
   g_osd_gen     = 0;
   g_osd_state   = XS_TUNER_IDLE;
   g_have_source = false;

   g_ctx->tuner = xs_tuner_new(g_ctx);
   if (!g_ctx->tuner)
      return false;

   if (!game || !game->path)
   {
      show_message(6000, false, "No content: load a .strm file to tune a stream");
      return true;
   }
   if (!xs_source_from_content(g_ctx, game->path, &g_source, err, sizeof(err)))
   {
      XS_ERROR(g_ctx, "%s\n", err);
      xs_tuner_free(g_ctx->tuner);
      g_ctx->tuner = NULL;
      return false;
   }
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
   if (st.gen == g_osd_gen && st.state == g_osd_state)
      return;
   g_osd_gen   = st.gen;
   g_osd_state = st.state;
   switch (st.state)
   {
      case XS_TUNER_CONNECTING: show_message(15000, false, "Tuning..."); break;
      case XS_TUNER_PLAYING:    show_message(4000, false, "%s", st.msg); break;
      case XS_TUNER_FAILED:     show_message(6000, true, "%s", st.msg); break;
      case XS_TUNER_ENDED:      show_message(4000, false, "%s", st.msg); break;
      default: break;
   }
}

static void output_audio(void)
{
   unsigned n;
   g_audio_accum += XS_SAMPLE_RATE / g_fps;
   n = (unsigned)g_audio_accum;
   g_audio_accum -= n;
   while (n)
   {
      unsigned chunk = n > XS_AUDIO_CHUNK ? XS_AUDIO_CHUNK : n;
      audio_batch_cb(g_silence, chunk);
      n -= chunk;
   }
}

RETRO_API void retro_run(void)
{
   bool updated = false;

   input_poll_cb();
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      read_options(g_ctx);
   update_osd();

   if (!g_frame_sent || !g_can_dupe)
   {
      video_cb(g_black, XS_IDLE_WIDTH, XS_IDLE_HEIGHT, XS_IDLE_WIDTH * sizeof(uint32_t));
      g_frame_sent = true;
   }
   else
      video_cb(NULL, XS_IDLE_WIDTH, XS_IDLE_HEIGHT, XS_IDLE_WIDTH * sizeof(uint32_t));
   output_audio();
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
