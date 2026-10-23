/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Downloaded lineups (see xs_iptv.h).
 *
 * iptv-org (D-010): the lineup is built from its generated playlists, not
 * its 8 MB JSON API: countries/<cc>.m3u, or languages/<code>.m3u, or
 * index.m3u for everything; a country + language query keeps the country's
 * channels that are also in the language's list. Categories are matched in
 * group-title ("Animation;Kids"). Lists are cached in <cache_dir>/iptv-org/
 * and re-downloaded when older than refresh_hours; without a network the
 * cached copy is used however old (§5.8).
 *
 * For tests, XSTREEMLY_IPTV_BASE replaces the iptv-org URL and
 * XSTREEMLY_CA_FILE the CA bundle (tests/check.sh serves a fake iptv-org).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <libretro.h>
#include <rthreads/rthreads.h>

#include "xs_fetch.h"
#include "xs_iptv.h"
#include "xs_iptv_tables.h"

#define XS_LIST_MAX_BYTES (32u << 20)
#define XS_FETCH_TIMEOUT_US (60 * 1000000LL)

static const struct retro_core_option_value g_countries[] = { XS_IPTV_COUNTRY_VALUES };
static const struct retro_core_option_value g_languages[] = { XS_IPTV_LANGUAGE_VALUES };

typedef enum { JOB_NONE = 0, JOB_IPTV, JOB_URL } job_kind_t;
typedef enum { ST_IDLE = 0, ST_WORKING, ST_DONE, ST_FAILED } job_state_t;

struct xs_iptv
{
   xs_ctx_t  *ctx;
   char       cache_dir[4096];
   char       base[1024];
   sthread_t *thread;
   slock_t   *lock;
   scond_t   *cond;

   /* lock: request */
   bool            quit;
   unsigned        gen, run_gen;
   job_kind_t      kind;
   xs_iptv_query_t q;
   char            url[4096], ua[256], ref[1024];

   /* lock: result */
   job_state_t   state;
   unsigned      state_gen;
   char          msg[256];
   xs_playlist_t result;
};

/* ---- helpers ------------------------------------------------------------ */

static const char *label(const struct retro_core_option_value *v, const char *value)
{
   for (; v->value; v++)
      if (!strcmp(v->value, value))
         return v->label;
   return value;
}

void xs_iptv_describe(const xs_iptv_query_t *q, char *out, size_t len)
{
   size_t n = (size_t)snprintf(out, len, "IPTV-ORG %s",
                               strcmp(q->country, "all") ? label(g_countries, q->country) : "ALL COUNTRIES");
   if (strcmp(q->language, "any") && n < len)
      n += (size_t)snprintf(out + n, len - n, ", %s", label(g_languages, q->language));
   if (strcmp(q->category, "all") && n < len)
      snprintf(out + n, len - n, ", %s", q->category);
}

void xs_iptv_key(const xs_iptv_query_t *q, char *out, size_t len)
{
   snprintf(out, len, "iptv-org:%s:%s:%s:%d%d", q->country, q->language, q->category,
            q->hide_geoblocked, q->hide_not_247);
}

static int stop_cb(void *arg)
{
   xs_iptv_t *it = arg;
   bool stop;
   slock_lock(it->lock);
   stop = it->quit || it->gen != it->run_gen;
   slock_unlock(it->lock);
   return stop;
}

static void set_state(xs_iptv_t *it, job_state_t st, const char *msg)
{
   slock_lock(it->lock);
   if (it->gen == it->run_gen)
   {
      it->state = st;
      it->state_gen = it->run_gen;
      snprintf(it->msg, sizeof(it->msg), "%s", msg);
   }
   slock_unlock(it->lock);
}

static char *read_file(const char *path, size_t *len)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long size;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = size >= 0 && (unsigned long)size <= XS_LIST_MAX_BYTES ? malloc((size_t)size + 1) : NULL;
   *len = buf ? fread(buf, 1, (size_t)size, f) : 0;
   fclose(f);
   if (buf)
      buf[*len] = '\0';
   return buf;
}

static void write_file(xs_ctx_t *ctx, const char *path, const char *data, size_t len)
{
   char tmp[4200];
   FILE *f;
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(f = fopen(tmp, "wb")))
   {
      XS_WARN(ctx, "iptv-org: can't write %s: %s\n", tmp, strerror(errno));
      return;
   }
   fwrite(data, 1, len, f);
   if (fclose(f) || rename(tmp, path))
      XS_WARN(ctx, "iptv-org: can't save %s\n", path);
}

/* A list from iptv-org: the cached copy if fresh, else download it (and
 * cache it), else the cached copy however old. */
static char *get_list(xs_iptv_t *it, const char *rel, unsigned refresh_hours, size_t *len,
                      char *err, size_t errlen)
{
   char cache[4096] = "", url[2048], name[256], msg[256], ferr[256];
   struct stat sb;
   bool cached = false;
   char *text;
   size_t i;
   xs_fetch_opts_t o;

   snprintf(name, sizeof(name), "%s", rel);
   for (i = 0; name[i]; i++)
      if (name[i] == '/')
         name[i] = '_';
   if (it->cache_dir[0])
   {
      snprintf(cache, sizeof(cache), "%.3800s/%.200s", it->cache_dir, name);
      cached = !stat(cache, &sb);
      if (cached && time(NULL) - sb.st_mtime < (time_t)refresh_hours * 3600 &&
          (text = read_file(cache, len)))
      {
         XS_INFO(it->ctx, "iptv-org: %s from the cache (%.1f h old)\n", rel,
                 (time(NULL) - sb.st_mtime) / 3600.0);
         return text;
      }
   }

   snprintf(msg, sizeof(msg), "DOWNLOADING %s", rel);
   set_state(it, ST_WORKING, msg);
   snprintf(url, sizeof(url), "%s%s", it->base, rel);
   memset(&o, 0, sizeof(o));
   o.verify     = true;          /* metadata is checked (D-012) */
   o.max_bytes  = XS_LIST_MAX_BYTES;
   o.timeout_us = XS_FETCH_TIMEOUT_US;
   o.stop       = stop_cb;
   o.arg        = it;
   if (xs_fetch(it->ctx, url, &o, &text, len, ferr, sizeof(ferr)) == 0)
   {
      XS_INFO(it->ctx, "iptv-org: downloaded %s (%zu KB)\n", rel, *len / 1024);
      if (cache[0])
         write_file(it->ctx, cache, text, *len);
      return text;
   }
   if (stop_cb(it))
      return NULL;
   if (cached && (text = read_file(cache, len)))
   {
      XS_WARN(it->ctx, "iptv-org: %s: %s; using the cached list from %.1f h ago\n", url, ferr,
              (time(NULL) - sb.st_mtime) / 3600.0);
      return text;
   }
   snprintf(err, errlen, "%.100s: %.150s", rel, ferr);
   return NULL;
}

static bool group_has(const char *groups, const char *name)
{
   size_t n = strlen(name);
   const char *p = groups;
   while (*p)
   {
      size_t len = strcspn(p, ";");
      if (len == n && !strncasecmp(p, name, n))
         return true;
      p += len;
      if (*p == ';')
         p++;
   }
   return false;
}

static unsigned long long url_hash(const char *s)
{
   unsigned long long h = 1469598103934665603ULL;
   for (; *s; s++)
      h = (h ^ (unsigned char)*s) * 1099511628211ULL;
   return h;
}

static int cmp_u64(const void *a, const void *b)
{
   unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
   return x < y ? -1 : x > y;
}

/* ---- jobs (worker thread) ----------------------------------------------- */

static bool parse_list(xs_iptv_t *it, const char *rel, const char *text, size_t len, xs_playlist_t *pl,
                       char *err, size_t errlen)
{
   char base[2048];
   snprintf(base, sizeof(base), "%s%s", it->base, rel);
   return xs_playlist_parse(text, len, base, pl, err, errlen);
}

static bool build_iptv(xs_iptv_t *it, const xs_iptv_query_t *q, xs_playlist_t *out, char *msg, size_t msglen)
{
   char rel[128], lrel[128], err[256];
   xs_playlist_t pl, lang;
   unsigned long long *lang_urls = NULL, *seen;
   unsigned i, n = 0, geo = 0, not247 = 0, other = 0, dup = 0;
   bool need_lang = strcmp(q->language, "any") != 0;
   size_t len;
   char *text;

   memset(&lang, 0, sizeof(lang));
   if (strcmp(q->country, "all"))
      snprintf(rel, sizeof(rel), "countries/%s.m3u", q->country);
   else if (need_lang)
   {
      snprintf(rel, sizeof(rel), "languages/%s.m3u", q->language);
      need_lang = false;   /* the list is the language */
   }
   else
      snprintf(rel, sizeof(rel), "index.m3u");

   if (!(text = get_list(it, rel, q->refresh_hours, &len, err, sizeof(err))))
   {
      snprintf(msg, msglen, "%s", err);
      return false;
   }
   if (!parse_list(it, rel, text, len, &pl, err, sizeof(err)))
   {
      free(text);
      snprintf(msg, msglen, "%.100s: %.150s", rel, err);
      return false;
   }
   free(text);

   if (need_lang)
   {
      snprintf(lrel, sizeof(lrel), "languages/%s.m3u", q->language);
      text = get_list(it, lrel, q->refresh_hours, &len, err, sizeof(err));
      if (!text || !parse_list(it, lrel, text, len, &lang, err, sizeof(err)))
      {
         free(text);
         xs_playlist_free(&pl);
         snprintf(msg, msglen, "%s", err);
         return false;
      }
      free(text);
      lang_urls = malloc((lang.n ? lang.n : 1) * sizeof(*lang_urls));
      for (i = 0; lang_urls && i < lang.n; i++)
         lang_urls[i] = url_hash(lang.e[i].url);
      if (lang_urls)
         qsort(lang_urls, lang.n, sizeof(*lang_urls), cmp_u64);
   }

   /* Filter in place, dropping exact duplicate URLs. */
   seen = malloc((pl.n ? pl.n : 1) * sizeof(*seen));
   for (i = 0; i < pl.n; i++)
   {
      xs_entry_t *e = &pl.e[i];
      unsigned long long h = url_hash(e->url);
      bool keep = true;
      unsigned j;
      if (q->hide_geoblocked && (e->flags & XS_CH_GEOBLOCKED))
      {
         geo++;
         keep = false;
      }
      else if (q->hide_not_247 && (e->flags & XS_CH_NOT_247))
      {
         not247++;
         keep = false;
      }
      else if (group_has(e->group, "XXX") ||   /* none since 2024-01-30; belt and braces (D-011) */
               (strcmp(q->category, "all") && !group_has(e->group, q->category)) ||
               (lang_urls && !bsearch(&h, lang_urls, lang.n, sizeof(h), cmp_u64)))
      {
         other++;
         keep = false;
      }
      for (j = 0; keep && seen && j < n; j++)
         if (seen[j] == h)
         {
            dup++;
            keep = false;
         }
      if (!keep)
         continue;
      if (seen)
         seen[n] = h;
      pl.e[n++] = *e;
   }
   pl.n = n;
   free(seen);
   free(lang_urls);
   xs_playlist_free(&lang);

   XS_INFO(it->ctx, "iptv-org: %u channels (hidden: %u geo-blocked, %u not 24/7, %u other filters, %u duplicates)\n",
           n, geo, not247, other, dup);
   if (!n)
   {
      xs_playlist_free(&pl);
      snprintf(msg, msglen, "NO CHANNELS MATCH THE OPTIONS");
      return false;
   }
   *out = pl;
   snprintf(msg, msglen, "%u CHANNELS", n);
   return true;
}

static bool build_url(xs_iptv_t *it, const char *url, const char *ua, const char *ref,
                      xs_playlist_t *out, char *msg, size_t msglen)
{
   xs_fetch_opts_t o;
   char err[256], *text;
   size_t len;

   set_state(it, ST_WORKING, "DOWNLOADING THE CHANNEL LIST");
   memset(&o, 0, sizeof(o));
   o.user_agent = ua;
   o.referrer   = ref;
   o.verify     = false;   /* a user's own source: like streams (D-012) */
   o.max_bytes  = XS_LIST_MAX_BYTES;
   o.timeout_us = XS_FETCH_TIMEOUT_US;
   o.stop       = stop_cb;
   o.arg        = it;
   if (xs_fetch(it->ctx, url, &o, &text, &len, err, sizeof(err)) < 0)
   {
      snprintf(msg, msglen, "%s", err);
      return false;
   }
   if (!xs_playlist_parse(text, len, url, out, err, sizeof(err)))
   {
      free(text);
      snprintf(msg, msglen, "%s", err);
      return false;
   }
   free(text);
   if (out->hls)
   {
      /* An HLS playlist: one channel, the URL itself, with its headers. */
      snprintf(out->e[0].user_agent, sizeof(out->e[0].user_agent), "%s", ua);
      snprintf(out->e[0].referrer, sizeof(out->e[0].referrer), "%s", ref);
   }
   XS_INFO(it->ctx, "channel list %s: %u channel(s)%s\n", url, out->n, out->hls ? " (an HLS playlist)" : "");
   snprintf(msg, msglen, "%u CHANNELS", out->n);
   return true;
}

static void worker(void *arg)
{
   xs_iptv_t *it = arg;
   sthread_setname("xs-lineup");
   slock_lock(it->lock);
   for (;;)
   {
      job_kind_t kind;
      xs_iptv_query_t q;
      char url[4096], ua[256], ref[1024], msg[256];
      xs_playlist_t pl;
      bool ok;

      while (!it->quit && it->gen == it->run_gen)
         scond_wait(it->cond, it->lock);
      if (it->quit)
         break;
      it->run_gen = it->gen;
      kind = it->kind;
      q = it->q;
      memcpy(url, it->url, sizeof(url));
      memcpy(ua, it->ua, sizeof(ua));
      memcpy(ref, it->ref, sizeof(ref));
      slock_unlock(it->lock);

      memset(&pl, 0, sizeof(pl));
      ok = kind == JOB_IPTV ? build_iptv(it, &q, &pl, msg, sizeof(msg))
                            : build_url(it, url, ua, ref, &pl, msg, sizeof(msg));

      slock_lock(it->lock);
      if (it->gen == it->run_gen)
      {
         xs_playlist_free(&it->result);
         it->result    = pl;
         it->state     = ok ? ST_DONE : ST_FAILED;
         it->state_gen = it->run_gen;
         snprintf(it->msg, sizeof(it->msg), "%s", msg);
         if (!ok)
            XS_WARN(it->ctx, "lineup: %s\n", msg);
      }
      else
         xs_playlist_free(&pl);   /* superseded */
   }
   slock_unlock(it->lock);
}

/* ---- API (emulation thread) --------------------------------------------- */

xs_iptv_t *xs_iptv_new(xs_ctx_t *ctx, const char *cache_dir)
{
   xs_iptv_t *it = calloc(1, sizeof(*it));
   const char *base = getenv("XSTREEMLY_IPTV_BASE");
   if (!it)
      return NULL;
   it->ctx = ctx;
   snprintf(it->base, sizeof(it->base), "%s", base && base[0] ? base : XS_IPTV_BASE);
   if (cache_dir && cache_dir[0])
   {
      char dir[4096];
      snprintf(dir, sizeof(dir), "%.4000s/X-Streemly", cache_dir);
      mkdir(dir, 0755);
      snprintf(it->cache_dir, sizeof(it->cache_dir), "%.4000s/iptv-org", dir);
      mkdir(it->cache_dir, 0755);
   }
   it->lock = slock_new();
   it->cond = scond_new();
   if (it->lock && it->cond)
      it->thread = sthread_create(worker, it);
   if (!it->thread)
   {
      if (it->cond)
         scond_free(it->cond);
      if (it->lock)
         slock_free(it->lock);
      free(it);
      return NULL;
   }
   return it;
}

void xs_iptv_free(xs_iptv_t *it)
{
   if (!it)
      return;
   slock_lock(it->lock);
   it->quit = true;
   scond_signal(it->cond);
   slock_unlock(it->lock);
   sthread_join(it->thread);
   xs_playlist_free(&it->result);
   scond_free(it->cond);
   slock_free(it->lock);
   free(it);
}

void xs_iptv_request(xs_iptv_t *it, const xs_iptv_query_t *q)
{
   slock_lock(it->lock);
   it->kind  = JOB_IPTV;
   it->q     = *q;
   it->gen++;
   it->state = ST_WORKING;
   it->state_gen = it->gen;
   snprintf(it->msg, sizeof(it->msg), "LOADING CHANNELS");
   scond_signal(it->cond);
   slock_unlock(it->lock);
}

void xs_iptv_request_url(xs_iptv_t *it, const char *url, const char *user_agent, const char *referrer)
{
   slock_lock(it->lock);
   it->kind = JOB_URL;
   snprintf(it->url, sizeof(it->url), "%s", url);
   snprintf(it->ua, sizeof(it->ua), "%s", user_agent ? user_agent : "");
   snprintf(it->ref, sizeof(it->ref), "%s", referrer ? referrer : "");
   it->gen++;
   it->state = ST_WORKING;
   it->state_gen = it->gen;
   snprintf(it->msg, sizeof(it->msg), "LOADING CHANNELS");
   scond_signal(it->cond);
   slock_unlock(it->lock);
}

int xs_iptv_poll(xs_iptv_t *it, xs_playlist_t *out, char *msg, size_t msglen)
{
   int ret = 0;
   slock_lock(it->lock);
   if (it->state_gen == it->gen)
   {
      snprintf(msg, msglen, "%s", it->msg);
      if (it->state == ST_DONE)
      {
         *out = it->result;
         memset(&it->result, 0, sizeof(it->result));
         it->state = ST_IDLE;
         ret = 1;
      }
      else if (it->state == ST_FAILED)
      {
         it->state = ST_IDLE;
         ret = -1;
      }
   }
   slock_unlock(it->lock);
   return ret;
}
