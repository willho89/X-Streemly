/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Content → source, and opening a source with FFmpeg (CLAUDE.md §5.1, §5.2).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/dict.h>

#include "xs_source.h"

#define XS_USER_AGENT "X-Streemly/" XS_VERSION

/* Protocols a network source may use. Remote playlists must never reach
 * file: (CLAUDE.md §5.13). */
#define XS_NET_PROTOCOLS   "http,https,tls,tcp,crypto"
#define XS_LOCAL_PROTOCOLS "file,crypto"

static bool has_ext(const char *path, const char *ext)
{
   size_t n = strlen(path), e = strlen(ext);
   size_t i;
   if (n < e + 1 || path[n - e - 1] != '.')
      return false;
   for (i = 0; i < e; i++)
      if (tolower((unsigned char)path[n - e + i]) != ext[i])
         return false;
   return true;
}

static bool is_url(const char *s)
{
   return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static void trim(char *s)
{
   size_t n = strlen(s);
   char *p = s;
   while (n && isspace((unsigned char)s[n - 1]))
      s[--n] = '\0';
   while (isspace((unsigned char)*p))
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
}

/* .strm: the first line that isn't blank or a '#' comment is the URL. */
static bool read_strm(const char *path, char *url, size_t len)
{
   char line[4096];
   FILE *f = fopen(path, "rb");
   bool ok = false;
   if (!f)
      return false;
   while (fgets(line, sizeof(line), f))
   {
      char *s = line;
      if (!strncmp(s, "\xEF\xBB\xBF", 3))   /* UTF-8 BOM */
         s += 3;
      trim(s);
      if (!*s || *s == '#')
         continue;
      snprintf(url, len, "%s", s);
      ok = true;
      break;
   }
   fclose(f);
   return ok;
}

/* Display name: the content file's name without its extension, else the
 * URL's host. */
static void name_from(xs_source_t *src, const char *content)
{
   const char *p;
   size_t n;
   if (content)
   {
      const char *base = strrchr(content, '/');
      const char *dot;
      base = base ? base + 1 : content;
      dot  = strrchr(base, '.');
      n = dot && dot > base ? (size_t)(dot - base) : strlen(base);
      snprintf(src->name, sizeof(src->name), "%.*s", (int)n, base);
      return;
   }
   p = strstr(src->url, "://");
   p = p ? p + 3 : src->url;
   n = strcspn(p, "/:?#");
   snprintf(src->name, sizeof(src->name), "%.*s", (int)n, p);
}

bool xs_source_from_content(xs_ctx_t *ctx, const char *path, xs_source_t *src,
                            char *err, size_t errlen)
{
   memset(src, 0, sizeof(*src));
   if (is_url(path))
      snprintf(src->url, sizeof(src->url), "%s", path);
   else if (has_ext(path, "strm"))
   {
      if (!read_strm(path, src->url, sizeof(src->url)))
      {
         snprintf(err, errlen, "No URL in %s", path);
         return false;
      }
   }
   else
   {
      snprintf(err, errlen, "Unsupported content: %s", path);
      return false;
   }
   if (!is_url(src->url))
      src->local = true;   /* a .strm may name a local file (tests) */
   name_from(src, is_url(path) ? NULL : path);
   XS_DEBUG(ctx, "source: %s\n", src->url);
   return true;
}

int xs_source_open(xs_ctx_t *ctx, const xs_source_t *src, const AVIOInterruptCB *icb,
                   AVFormatContext **out)
{
   AVFormatContext *fmt = avformat_alloc_context();
   AVDictionary *opts = NULL;
   AVDictionaryEntry *e = NULL;
   int ret;

   if (!fmt)
      return AVERROR(ENOMEM);
   fmt->interrupt_callback = *icb;

   av_dict_set(&opts, "protocol_whitelist", src->local ? XS_LOCAL_PROTOCOLS : XS_NET_PROTOCOLS, 0);
   av_dict_set(&opts, "user_agent", src->user_agent[0] ? src->user_agent : XS_USER_AGENT, 0);
   if (src->referrer[0])
      av_dict_set(&opts, "referer", src->referrer, 0);
   /* http: give up on a silent connection, reconnect dropped ones. */
   av_dict_set(&opts, "rw_timeout", "10000000", 0);
   av_dict_set(&opts, "reconnect", "1", 0);
   av_dict_set(&opts, "reconnect_streamed", "1", 0);
   av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
   av_dict_set(&opts, "reconnect_delay_max", "5", 0);
   /* hls: keep-alive, start 3 segments from the live edge. */
   av_dict_set(&opts, "http_persistent", "1", 0);
   av_dict_set(&opts, "live_start_index", "-3", 0);
   /* Probing live HLS is slow; cap it (§5.2). */
   av_dict_set(&opts, "probesize", "2000000", 0);
   av_dict_set(&opts, "analyzeduration", "3000000", 0);

   ret = avformat_open_input(&fmt, src->url, NULL, &opts);
   while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)))
      XS_DEBUG(ctx, "open: option '%s' not used by %s\n", e->key,
               fmt && fmt->iformat ? fmt->iformat->name : "the input");
   av_dict_free(&opts);
   if (ret < 0)
      return ret;   /* avformat_open_input freed fmt */

   ret = avformat_find_stream_info(fmt, NULL);
   if (ret < 0)
   {
      avformat_close_input(&fmt);
      return ret;
   }
   *out = fmt;
   return 0;
}
