/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HDHomeRun tuners (see xs_hdhr.h).
 *
 * Discovery follows SiliconDust's protocol as libhdhomerun implements it
 * (hdhomerun_pkt.h, hdhomerun_discover.c @ master 2026-10; written here
 * from the protocol, not copied): a frame is a big-endian type and payload
 * length, the payload as tag/length/value fields (lengths of 128 and more
 * take two bytes), then a little-endian CRC-32 of everything before it. A
 * discover request (type 2, tag 1 = device type: tuner) goes to UDP port
 * 65001 by broadcast; each device answers (type 3) with tags including its
 * base URL (0x2A).
 *
 * The lineup comes from <base>/lineup.json: GuideNumber ("3.1"),
 * GuideName, URL (MPEG-TS over HTTP on port 5004), DRM, VideoCodec,
 * AudioCodec. FFmpeg has no AC-4 decoder, so ATSC 3.0 channels with AC-4
 * audio are marked unplayable along with DRM ones.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <formats/rjson.h>
#include <libavutil/time.h>

#include "xs_fetch.h"
#include "xs_hdhr.h"

#define HDHR_PORT          65001
#define HDHR_TYPE_DISC_REQ 0x0002
#define HDHR_TYPE_DISC_RPY 0x0003
#define HDHR_TAG_DEV_TYPE  0x01
#define HDHR_TAG_BASE_URL  0x2A
#define HDHR_DEV_TUNER     0x00000001u
#define HDHR_WAIT_MS       800
#define HDHR_JSON_MAX      (2u << 20)
#define HDHR_TIMEOUT_US    (10 * 1000000LL)

/* ---- discovery ------------------------------------------------------------ */

static uint32_t crc32_le(const uint8_t *p, size_t n)
{
   uint32_t crc = 0xFFFFFFFFu;
   size_t i;
   int b;
   for (i = 0; i < n; i++)
   {
      crc ^= p[i];
      for (b = 0; b < 8; b++)
         crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
   }
   return crc ^ 0xFFFFFFFFu;
}

static size_t build_request(uint8_t *buf)
{
   size_t n = 4;
   uint32_t crc;
   buf[n++] = HDHR_TAG_DEV_TYPE;
   buf[n++] = 4;
   buf[n++] = (uint8_t)(HDHR_DEV_TUNER >> 24);
   buf[n++] = (uint8_t)(HDHR_DEV_TUNER >> 16);
   buf[n++] = (uint8_t)(HDHR_DEV_TUNER >> 8);
   buf[n++] = (uint8_t)HDHR_DEV_TUNER;
   buf[0] = (uint8_t)(HDHR_TYPE_DISC_REQ >> 8);
   buf[1] = (uint8_t)HDHR_TYPE_DISC_REQ;
   buf[2] = (uint8_t)((n - 4) >> 8);
   buf[3] = (uint8_t)(n - 4);
   crc = crc32_le(buf, n);
   buf[n++] = (uint8_t)crc;
   buf[n++] = (uint8_t)(crc >> 8);
   buf[n++] = (uint8_t)(crc >> 16);
   buf[n++] = (uint8_t)(crc >> 24);
   return n;
}

/* The base URL from a discover reply, or false if it isn't one. */
static bool parse_reply(const uint8_t *p, size_t n, char *base, size_t len)
{
   size_t plen, pos = 4, end;
   uint32_t crc;
   if (n < 8 || ((p[0] << 8) | p[1]) != HDHR_TYPE_DISC_RPY)
      return false;
   plen = (size_t)((p[2] << 8) | p[3]);
   if (4 + plen + 4 > n)
      return false;
   end = 4 + plen;
   crc = (uint32_t)p[end] | (uint32_t)p[end + 1] << 8 | (uint32_t)p[end + 2] << 16 | (uint32_t)p[end + 3] << 24;
   if (crc != crc32_le(p, end))
      return false;
   while (pos + 2 <= end)
   {
      uint8_t tag = p[pos++];
      size_t tl = p[pos++];
      if (tl & 0x80)
      {
         if (pos >= end)
            return false;
         tl = (tl & 0x7F) | ((size_t)p[pos++] << 7);
      }
      if (pos + tl > end)
         return false;
      if (tag == HDHR_TAG_BASE_URL && tl > 0 && tl < len)
      {
         memcpy(base, p + pos, tl);
         base[tl] = '\0';
         return true;
      }
      pos += tl;
   }
   return false;
}

static unsigned discover_udp(xs_ctx_t *ctx, xs_hdhr_dev_t *devs, unsigned max, xs_abort_fn stop, void *arg)
{
   struct sockaddr_in to;
   uint8_t req[32], rx[2048];
   int fd, on = 1;
   unsigned n = 0, i;
   size_t len = build_request(req);
   int64_t until;

   if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
      return 0;
   setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
   memset(&to, 0, sizeof(to));
   to.sin_family      = AF_INET;
   to.sin_port        = htons(HDHR_PORT);
   to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
   if (sendto(fd, req, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
   {
      XS_DEBUG(ctx, "hdhomerun: discovery broadcast failed: %s\n", strerror(errno));
      close(fd);
      return 0;
   }
   until = av_gettime_relative() + HDHR_WAIT_MS * 1000LL;
   while (n < max)
   {
      struct pollfd pfd = { fd, POLLIN, 0 };
      int64_t left = (until - av_gettime_relative()) / 1000;
      ssize_t got;
      char base[256];
      bool dup = false;
      if (left <= 0 || (stop && stop(arg)))
         break;
      if (poll(&pfd, 1, (int)(left < 100 ? left : 100)) <= 0)
         continue;
      got = recv(fd, rx, sizeof(rx), 0);
      if (got <= 0 || !parse_reply(rx, (size_t)got, base, sizeof(base)))
         continue;
      for (i = 0; i < n; i++)
         dup |= !strcmp(devs[i].base, base);
      if (!dup)
      {
         memset(&devs[n], 0, sizeof(devs[n]));
         snprintf(devs[n].base, sizeof(devs[n].base), "%s", base);
         n++;
      }
   }
   close(fd);
   return n;
}

/* ---- discover.json / lineup.json ----------------------------------------- */

static char *get_json(xs_ctx_t *ctx, const char *url, size_t *len, char *err, size_t errlen,
                      xs_abort_fn stop, void *arg)
{
   xs_fetch_opts_t o;
   char *text;
   memset(&o, 0, sizeof(o));
   o.max_bytes  = HDHR_JSON_MAX;
   o.timeout_us = HDHR_TIMEOUT_US;
   o.stop       = stop;
   o.arg        = arg;
   return xs_fetch(ctx, url, &o, &text, len, err, errlen) == 0 ? text : NULL;
}

/* Read the member values of one flat JSON object at a time from an array
 * (lineup.json) or the top level (discover.json); cb gets key/value pairs,
 * and NULL key at each object's end. */
typedef void (*member_cb)(void *user, const char *key, const char *str, double num, bool is_num);

static bool walk_objects(const char *text, size_t len, member_cb cb, void *user)
{
   rjson_t *j = rjson_open_buffer(text, len);
   char key[64] = "";
   bool ok = true;
   enum rjson_type t;
   if (!j)
      return false;
   while ((t = rjson_next(j)) != RJSON_DONE && t != RJSON_ERROR)
   {
      bool member = rjson_get_context_type(j) == RJSON_OBJECT && (rjson_get_context_count(j) & 1);
      size_t sl;
      const char *s;
      switch (t)
      {
         case RJSON_STRING:
            s = rjson_get_string(j, &sl);
            if (member)
               snprintf(key, sizeof(key), "%.*s", (int)sl, s);
            else if (key[0])
               cb(user, key, s, 0, false);
            break;
         case RJSON_NUMBER:
            cb(user, key, NULL, rjson_get_double(j), true);
            break;
         case RJSON_TRUE:
            cb(user, key, NULL, 1, true);
            break;
         case RJSON_OBJECT_END:
            cb(user, NULL, NULL, 0, false);
            key[0] = '\0';
            break;
         default:
            break;
      }
   }
   ok = t == RJSON_DONE;
   rjson_free(j);
   return ok;
}

static void discover_member(void *user, const char *key, const char *str, double num, bool is_num)
{
   xs_hdhr_dev_t *d = user;
   if (!key)
      return;
   if (!strcmp(key, "FriendlyName") && str)
      snprintf(d->name, sizeof(d->name), "%s", str);
   else if (!strcmp(key, "TunerCount") && is_num)
      d->tuners = (unsigned)num;
}

typedef struct
{
   xs_playlist_t *out;
   xs_entry_t     e;
   char           video[16], audio[16];
   const char    *group;
   unsigned       drm, added, unplayable;
} lineup_state_t;

static void lineup_member(void *user, const char *key, const char *str, double num, bool is_num)
{
   lineup_state_t *ls = user;
   if (!key)
   {
      /* end of a channel */
      if (ls->e.url[0])
      {
         if (!strcasecmp(ls->audio, "AC4"))
            ls->e.flags |= XS_CH_NOCODEC;
         if (ls->e.flags & XS_CH_UNPLAYABLE)
            ls->unplayable++;
         ls->e.flags |= XS_CH_TUNER;
         snprintf(ls->e.group, sizeof(ls->e.group), "%s", ls->group);
         if (xs_playlist_push(ls->out, &ls->e))
            ls->added++;
      }
      memset(&ls->e, 0, sizeof(ls->e));
      ls->video[0] = ls->audio[0] = '\0';
      return;
   }
   if (!strcmp(key, "GuideNumber") && str)
   {
      ls->e.chno = xs_chno_parse(str);
      if (!ls->e.name[0])
         snprintf(ls->e.name, sizeof(ls->e.name), "%s", str);
   }
   else if (!strcmp(key, "GuideName") && str)
      snprintf(ls->e.name, sizeof(ls->e.name), "%s", str);
   else if (!strcmp(key, "URL") && str)
      snprintf(ls->e.url, sizeof(ls->e.url), "%s", str);
   else if (!strcmp(key, "DRM") && is_num && num)
      ls->e.flags |= XS_CH_DRM;
   else if (!strcmp(key, "VideoCodec") && str)
      snprintf(ls->video, sizeof(ls->video), "%s", str);
   else if (!strcmp(key, "AudioCodec") && str)
      snprintf(ls->audio, sizeof(ls->audio), "%s", str);
   else if (!strcmp(key, "HD") && is_num && num)
      ls->e.height = 1080;
}

unsigned xs_hdhr_discover(xs_ctx_t *ctx, xs_hdhr_dev_t *devs, unsigned max, xs_abort_fn stop, void *arg)
{
   const char *env = getenv("XSTREEMLY_HDHR");
   unsigned n = 0, i;

   if (env && env[0])
   {
      const char *p = env;
      while (*p && n < max)
      {
         size_t l = strcspn(p, ",");
         memset(&devs[n], 0, sizeof(devs[n]));
         snprintf(devs[n].base, sizeof(devs[n].base), "%.*s", (int)l, p);
         n++;
         p += l;
         if (*p == ',')
            p++;
      }
   }
   else
   {
      n = discover_udp(ctx, devs, max, stop, arg);
      if (!n)
      {
         /* No answer to the broadcast (another subnet, a firewall): the
          * name their firmware announces over mDNS. */
         struct addrinfo hints, *res = NULL;
         memset(&hints, 0, sizeof(hints));
         hints.ai_family = AF_INET;
         if (!getaddrinfo("hdhomerun.local", NULL, &hints, &res) && res)
         {
            char ip[64];
            inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
            memset(&devs[0], 0, sizeof(devs[0]));
            snprintf(devs[0].base, sizeof(devs[0].base), "http://%s", ip);
            n = 1;
            freeaddrinfo(res);
         }
      }
   }
   for (i = 0; i < n; i++)
   {
      char url[320], err[128], *text;
      size_t len;
      snprintf(url, sizeof(url), "%s/discover.json", devs[i].base);
      if ((text = get_json(ctx, url, &len, err, sizeof(err), stop, arg)))
      {
         walk_objects(text, len, discover_member, &devs[i]);
         free(text);
      }
      if (!devs[i].name[0])
         snprintf(devs[i].name, sizeof(devs[i].name), "HDHomeRun");
      XS_INFO(ctx, "hdhomerun: %s at %s, %u tuners\n", devs[i].name, devs[i].base, devs[i].tuners);
   }
   if (!n)
      XS_INFO(ctx, "hdhomerun: none found\n");
   return n;
}

bool xs_hdhr_lineup(xs_ctx_t *ctx, const xs_hdhr_dev_t *dev, xs_playlist_t *out, char *err, size_t errlen,
                    xs_abort_fn stop, void *arg)
{
   lineup_state_t ls;
   char url[320], ferr[128], *text;
   size_t len;
   bool ok;

   snprintf(url, sizeof(url), "%s/lineup.json", dev->base);
   if (!(text = get_json(ctx, url, &len, ferr, sizeof(ferr), stop, arg)))
   {
      snprintf(err, errlen, "%s: %s", dev->name, ferr);
      return false;
   }
   memset(&ls, 0, sizeof(ls));
   ls.out   = out;
   ls.group = "Local TV";
   ok = walk_objects(text, len, lineup_member, &ls);
   free(text);
   if (!ok || !ls.added)
   {
      snprintf(err, errlen, "%s: no channels in its lineup", dev->name);
      return false;
   }
   XS_INFO(ctx, "hdhomerun: %u channels from %s (%u can't be played: DRM or AC-4 audio)\n",
           ls.added, dev->name, ls.unplayable);
   return true;
}
