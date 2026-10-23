/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Small HTTP(S) downloads (see xs_fetch.h).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/time.h>

#include "xs_fetch.h"

#define XS_USER_AGENT "X-Streemly/" XS_VERSION

typedef struct
{
   const xs_fetch_opts_t *o;
   int64_t deadline;
   bool    timed_out;
} fetch_state_t;

static int interrupt_cb(void *opaque)
{
   fetch_state_t *fs = opaque;
   if (fs->o->stop && fs->o->stop(fs->o->arg))
      return 1;
   if (av_gettime_relative() > fs->deadline)
   {
      fs->timed_out = true;
      return 1;
   }
   return 0;
}

const char *xs_fetch_ca_file(void)
{
   /* Debian/Ubuntu/Arch/SteamOS, Fedora/RHEL, openSUSE, Alpine/others */
   static const char *paths[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/ca-bundle.pem",
      "/etc/ssl/cert.pem",
      NULL
   };
   const char *env = getenv("XSTREEMLY_CA_FILE");   /* tests: the server's own certificate */
   unsigned i;
   if (env && env[0])
      return env;
   for (i = 0; paths[i]; i++)
      if (!access(paths[i], R_OK))
         return paths[i];
   return NULL;
}

int xs_fetch(xs_ctx_t *ctx, const char *url, const xs_fetch_opts_t *o, char **out, size_t *len,
             char *err, size_t errlen)
{
   fetch_state_t fs = { o, av_gettime_relative() + o->timeout_us, false };
   AVIOInterruptCB icb = { interrupt_cb, &fs };
   AVIOContext *pb = NULL;
   AVDictionary *opts = NULL;
   const char *ca = o->verify ? xs_fetch_ca_file() : NULL;
   char *buf = NULL;
   size_t n = 0, cap = 0;
   int ret;

   *out = NULL;
   *len = 0;
   av_dict_set(&opts, "protocol_whitelist", "http,https,tls,tcp", 0);
   av_dict_set(&opts, "user_agent", o->user_agent && o->user_agent[0] ? o->user_agent : XS_USER_AGENT, 0);
   if (o->referrer && o->referrer[0])
      av_dict_set(&opts, "referer", o->referrer, 0);
   av_dict_set(&opts, "rw_timeout", "10000000", 0);
   if (o->verify && ca)
   {
      av_dict_set(&opts, "tls_verify", "1", 0);
      av_dict_set(&opts, "ca_file", ca, 0);
   }
   else if (o->verify)
      XS_WARN(ctx, "fetch: no CA bundle found, not checking certificates\n");

   ret = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opts);
   av_dict_free(&opts);
   if (ret < 0)
   {
      snprintf(err, errlen, "%s", fs.timed_out ? "timed out" : av_err2str(ret));
      return ret;
   }
   for (;;)
   {
      int got;
      if (n + 65536 + 1 > cap)
      {
         size_t ncap = cap ? cap * 2 : 262144;
         char *nb;
         if (ncap > o->max_bytes + 65536 + 1)
            ncap = o->max_bytes + 65536 + 1;
         if (ncap <= n + 1)
         {
            ret = AVERROR(EFBIG);
            snprintf(err, errlen, "bigger than %zu bytes", o->max_bytes);
            break;
         }
         nb = realloc(buf, ncap);
         if (!nb)
         {
            ret = AVERROR(ENOMEM);
            snprintf(err, errlen, "out of memory");
            break;
         }
         buf = nb;
         cap = ncap;
      }
      got = avio_read(pb, (unsigned char *)buf + n, (int)(cap - n - 1 < 65536 ? cap - n - 1 : 65536));
      if (got == AVERROR_EOF || got == 0)
      {
         ret = 0;
         break;
      }
      if (got == AVERROR(EIO) && n && avio_size(pb) < 0)
      {
         /* No Content-Length and no "Connection: close" (HDHomeRun's web
          * server): the server closing the connection is the end, as curl
          * takes it; FFmpeg calls it "Stream ends prematurely". */
         ret = 0;
         break;
      }
      if (got < 0)
      {
         ret = got;
         snprintf(err, errlen, "%s", fs.timed_out ? "timed out" : av_err2str(got));
         break;
      }
      n += (size_t)got;
      if (n > o->max_bytes)
      {
         ret = AVERROR(EFBIG);
         snprintf(err, errlen, "bigger than %zu bytes", o->max_bytes);
         break;
      }
   }
   avio_closep(&pb);
   if (ret < 0)
   {
      free(buf);
      return ret;
   }
   if (!buf && !(buf = malloc(1)))
      return AVERROR(ENOMEM);
   buf[n] = '\0';
   *out = buf;
   *len = n;
   return 0;
}
