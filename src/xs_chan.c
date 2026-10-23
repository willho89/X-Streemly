/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The channel lineup (see xs_chan.h).
 *
 * Numbering: a playlist that gives every channel its own tvg-chno keeps
 * those numbers; otherwise channels are numbered 1.. in playlist order.
 * Remembered per lineup in <save_dir>/X-Streemly/<hash>.state as plain
 * "channel=<n>" / "previous=<n>" lines (channel numbers, so a reordered
 * playlist still lands on the same channel where it can).
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xs_chan.h"
#include "xs_compat.h"

static bool has_ext(const char *path, const char *ext)
{
   size_t n = strlen(path), e = strlen(ext), i;
   if (n < e + 1 || path[n - e - 1] != '.')
      return false;
   for (i = 0; i < e; i++)
      if (tolower((unsigned char)path[n - e + i]) != ext[i])
         return false;
   return true;
}

static void number_channels(xs_chan_t *c)
{
   unsigned i, j;
   bool own = true;
   for (i = 0; i < c->pl.n && own; i++)
   {
      if (!c->pl.e[i].chno)
         own = false;
      for (j = 0; j < i && own; j++)
         if (c->pl.e[j].chno == c->pl.e[i].chno)
            own = false;
   }
   for (i = 0; i < c->pl.n; i++)
      c->number[i] = own ? c->pl.e[i].chno : XS_CHNO(i + 1, 0);
}

/* FNV-1a of the content path: one remembered state per lineup. */
static unsigned long long lineup_hash(const char *s)
{
   unsigned long long h = 1469598103934665603ULL;
   for (; *s; s++)
      h = (h ^ (unsigned char)*s) * 1099511628211ULL;
   return h;
}

static void load_state(xs_ctx_t *ctx, xs_chan_t *c)
{
   char line[64];
   unsigned idx;
   FILE *f = c->state_path[0] ? fopen(c->state_path, "r") : NULL;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      line[strcspn(line, "\r\n")] = '\0';
      if (!strncmp(line, "channel=", 8) && xs_chan_find(c, xs_chno_parse(line + 8), &idx))
         c->cur = idx;
      else if (!strncmp(line, "previous=", 9) && xs_chan_find(c, xs_chno_parse(line + 9), &idx))
      {
         c->prev = idx;
         c->have_prev = true;
      }
   }
   fclose(f);
   {
      char num[16];
      xs_chno_format(c->number[c->cur], num, sizeof(num));
      XS_INFO(ctx, "lineup: back on channel %s\n", num);
   }
}

static void save_state(xs_ctx_t *ctx, const xs_chan_t *c)
{
   FILE *f;
   if (!c->state_path[0])
      return;
   f = fopen(c->state_path, "w");
   if (!f)
   {
      XS_WARN(ctx, "can't write %s: %s\n", c->state_path, strerror(errno));
      return;
   }
   {
      char num[16];
      xs_chno_format(c->number[c->cur], num, sizeof(num));
      fprintf(f, "channel=%s\n", num);
      if (c->have_prev)
      {
         xs_chno_format(c->number[c->prev], num, sizeof(num));
         fprintf(f, "previous=%s\n", num);
      }
   }
   fclose(f);
}

/* ---- marks ------------------------------------------------------------ */

static unsigned long long entry_key(const xs_entry_t *e)
{
   return lineup_hash(e->tvg_id[0] ? e->tvg_id : e->url);
}

static void marks_load(xs_marks_t *m, const char *dir, const char *name)
{
   FILE *f;
   char line[64];
   memset(m, 0, sizeof(*m));
   if (!dir || !dir[0])
      return;
   snprintf(m->path, sizeof(m->path), "%.4000s/%s", dir, name);
   if (!(f = fopen(m->path, "r")))
      return;
   while (fgets(line, sizeof(line), f))
   {
      unsigned long long k;
      long long t;
      if (sscanf(line, "%llx %lld", &k, &t) != 2)
         continue;
      if (m->n == m->cap)
      {
         unsigned cap = m->cap ? m->cap * 2 : 64;
         unsigned long long *nk = realloc(m->key, cap * sizeof(*nk));
         long long *nt = nk ? realloc(m->when, cap * sizeof(*nt)) : NULL;
         if (nk)
            m->key = nk;
         if (!nk || !nt)
            break;
         m->when = nt;
         m->cap  = cap;
      }
      m->key[m->n]  = k;
      m->when[m->n] = t;
      m->n++;
   }
   fclose(f);
}

static void marks_save(xs_ctx_t *ctx, xs_marks_t *m)
{
   FILE *f;
   unsigned i;
   if (!m->dirty || !m->path[0])
      return;
   m->dirty = false;
   if (!(f = fopen(m->path, "w")))
   {
      XS_WARN(ctx, "can't write %s: %s\n", m->path, strerror(errno));
      return;
   }
   for (i = 0; i < m->n; i++)
      fprintf(f, "%016llx %lld\n", m->key[i], m->when[i]);
   fclose(f);
}

static int marks_find(const xs_marks_t *m, unsigned long long k)
{
   unsigned i;
   for (i = 0; i < m->n; i++)
      if (m->key[i] == k)
         return (int)i;
   return -1;
}

static void marks_set(xs_marks_t *m, unsigned long long k, bool on)
{
   int i = marks_find(m, k);
   if (on)
   {
      if (i < 0)
      {
         if (m->n == m->cap)
         {
            unsigned cap = m->cap ? m->cap * 2 : 64;
            unsigned long long *nk = realloc(m->key, cap * sizeof(*nk));
            long long *nt = nk ? realloc(m->when, cap * sizeof(*nt)) : NULL;
            if (nk)
               m->key = nk;
            if (!nk || !nt)
               return;
            m->when = nt;
            m->cap  = cap;
         }
         i = (int)m->n++;
         m->key[i] = k;
      }
      m->when[i] = (long long)time(NULL);
   }
   else if (i >= 0)
   {
      m->key[i]  = m->key[m->n - 1];
      m->when[i] = m->when[m->n - 1];
      m->n--;
   }
   m->dirty = true;
}

static void marks_free(xs_marks_t *m)
{
   free(m->key);
   free(m->when);
   memset(m, 0, sizeof(*m));
}

static void finish(xs_ctx_t *ctx, xs_chan_t *c, const char *key, const char *save_dir)
{
   unsigned i;
   number_channels(c);
   XS_INFO(ctx, "lineup: %u channel(s)%s\n", c->pl.n, c->pl.hls ? " (an HLS playlist)" : "");
   for (i = 0; i < c->pl.n; i++)
   {
      const xs_entry_t *e = &c->pl.e[i];
      char num[16];
      xs_chno_format(c->number[i], num, sizeof(num));
      XS_DEBUG(ctx, "channel %s: \"%s\" group \"%s\" tvg-id \"%s\" %up%s%s%s url %s%s%s%s%s\n",
               num, e->name, e->group, e->tvg_id, e->height,
               e->flags & XS_CH_GEOBLOCKED ? " geo-blocked" : "",
               e->flags & XS_CH_NOT_247 ? " not-24/7" : "",
               e->flags & XS_CH_DRM ? " DRM" : e->flags & XS_CH_NOCODEC ? " no-codec" : "", e->url,
               e->user_agent[0] ? " ua " : "", e->user_agent,
               e->referrer[0] ? " referrer " : "", e->referrer);
   }
   if (save_dir && save_dir[0])
   {
      char dir[4096];
      snprintf(dir, sizeof(dir), "%.4000s/X-Streemly", save_dir);
      xs_mkdir(dir);
      snprintf(c->state_path, sizeof(c->state_path), "%.4000s/%016llx.state", dir, lineup_hash(key));
      marks_load(&c->dead, dir, "dead.txt");
      marks_load(&c->favs, dir, "favourites.txt");
   }
   load_state(ctx, c);
}

bool xs_chan_from_playlist(xs_ctx_t *ctx, xs_chan_t *c, xs_playlist_t *pl, const char *key,
                           const char *save_dir)
{
   memset(c, 0, sizeof(*c));
   c->pl = *pl;
   memset(pl, 0, sizeof(*pl));
   c->number = calloc(c->pl.n ? c->pl.n : 1, sizeof(*c->number));
   if (!c->number)
   {
      xs_playlist_free(&c->pl);
      return false;
   }
   finish(ctx, c, key, save_dir);
   return true;
}

void xs_chan_empty(xs_chan_t *c)
{
   memset(c, 0, sizeof(*c));
}

bool xs_chan_load(xs_ctx_t *ctx, xs_chan_t *c, const char *content, const char *save_dir,
                  char *err, size_t errlen)
{
   xs_playlist_t pl;

   memset(c, 0, sizeof(*c));
   memset(&pl, 0, sizeof(pl));
   if (has_ext(content, "m3u") || has_ext(content, "m3u8"))
   {
      if (!xs_playlist_load(content, &pl, err, errlen))
         return false;
   }
   else
   {
      /* .strm or a URL: one channel */
      xs_source_t src;
      if (!xs_source_from_content(ctx, content, &src, err, errlen))
         return false;
      pl.e = calloc(1, sizeof(*pl.e));
      if (!pl.e)
         return false;
      pl.n = pl.cap = 1;
      snprintf(pl.e[0].name, sizeof(pl.e[0].name), "%s", src.name);
      snprintf(pl.e[0].url, sizeof(pl.e[0].url), "%s", src.url);
      pl.e[0].local = src.local;
   }
   return xs_chan_from_playlist(ctx, c, &pl, content, save_dir);
}

void xs_chan_free(xs_chan_t *c)
{
   xs_playlist_free(&c->pl);
   free(c->number);
   marks_free(&c->dead);
   marks_free(&c->favs);
   memset(c, 0, sizeof(*c));
}

unsigned xs_chan_count(const xs_chan_t *c)
{
   return c->pl.n;
}

const xs_entry_t *xs_chan_entry(const xs_chan_t *c, unsigned idx)
{
   return &c->pl.e[idx];
}

unsigned xs_chan_number(const xs_chan_t *c, unsigned idx)
{
   return c->number[idx];
}

unsigned xs_chan_step(const xs_chan_t *c, unsigned idx, int dir)
{
   unsigned i, n = c->pl.n, pass;
   if (!n)
      return idx;
   /* First pass skips dead channels too; if every channel is dead, the
    * second doesn't. */
   for (pass = 0; pass < 2; pass++)
      for (i = 1; i <= n; i++)
      {
         unsigned j = (unsigned)(((int)idx + dir * (int)i) % (int)n + (int)n) % n;
         if (!(c->pl.e[j].flags & XS_CH_UNPLAYABLE) && (pass || !xs_chan_is_dead(c, j)))
            return j;
      }
   return idx;
}

unsigned xs_chan_group_step(const xs_chan_t *c, unsigned idx, int dir)
{
   unsigned i, n = c->pl.n;
   if (!n)
      return idx;
   const char *g = c->pl.e[idx].group;
   for (i = 1; i < n; i++)
   {
      unsigned j = (unsigned)(((int)idx + dir * (int)i) % (int)n + (int)n) % n;
      if (strcmp(c->pl.e[j].group, g))
      {
         /* backwards: go on to the first entry of that group */
         if (dir < 0)
            while (j > 0 && !strcmp(c->pl.e[j - 1].group, c->pl.e[j].group))
               j--;
         return j;
      }
   }
   return idx;
}

bool xs_chan_find(const xs_chan_t *c, unsigned number, unsigned *idx)
{
   unsigned i;
   for (i = 0; i < c->pl.n; i++)
      if (c->number[i] == number)
      {
         *idx = i;
         return true;
      }
   return false;
}

const char *xs_chan_number_str(const xs_chan_t *c, unsigned idx, char *buf, size_t len)
{
   xs_chno_format(c->number[idx], buf, len);
   return buf;
}

unsigned xs_chan_match(const xs_chan_t *c, const char *typed, bool *exact)
{
   unsigned i, n = 0;
   size_t tl = strlen(typed);
   *exact = false;
   for (i = 0; i < c->pl.n; i++)
   {
      char num[16];
      xs_chno_format(c->number[i], num, sizeof(num));
      if (!strncmp(num, typed, tl))
      {
         n++;
         if (!num[tl])
            *exact = true;
      }
   }
   return n;
}

void xs_chan_mark_dead(xs_chan_t *c, unsigned idx, bool dead)
{
   unsigned long long k = entry_key(&c->pl.e[idx]);
   if (dead || marks_find(&c->dead, k) >= 0)
      marks_set(&c->dead, k, dead);
}

bool xs_chan_is_dead(const xs_chan_t *c, unsigned idx)
{
   int i = marks_find(&c->dead, entry_key(&c->pl.e[idx]));
   return i >= 0 && (long long)time(NULL) - c->dead.when[i] < XS_DEAD_SECONDS;
}

void xs_chan_toggle_fav(xs_chan_t *c, unsigned idx)
{
   unsigned long long k = entry_key(&c->pl.e[idx]);
   marks_set(&c->favs, k, marks_find(&c->favs, k) < 0);
}

bool xs_chan_is_fav(const xs_chan_t *c, unsigned idx)
{
   return marks_find(&c->favs, entry_key(&c->pl.e[idx])) >= 0;
}

unsigned xs_chan_next_fav(const xs_chan_t *c, unsigned idx)
{
   unsigned i, n = c->pl.n;
   for (i = 1; i <= n; i++)
   {
      unsigned j = (idx + i) % n;
      if (xs_chan_is_fav(c, j))
         return j;
   }
   return idx;
}

void xs_chan_set(xs_ctx_t *ctx, xs_chan_t *c, unsigned idx)
{
   if (idx != c->cur)
   {
      c->prev = c->cur;
      c->have_prev = true;
      c->dirty = true;
   }
   c->cur = idx;
}

void xs_chan_save(xs_ctx_t *ctx, xs_chan_t *c)
{
   marks_save(ctx, &c->dead);
   marks_save(ctx, &c->favs);
   if (!c->dirty)
      return;
   c->dirty = false;
   save_state(ctx, c);
}

void xs_chan_source(const xs_chan_t *c, unsigned idx, xs_source_t *src)
{
   const xs_entry_t *e = &c->pl.e[idx];
   memset(src, 0, sizeof(*src));
   snprintf(src->name, sizeof(src->name), "%s", e->name);
   snprintf(src->url, sizeof(src->url), "%s", e->url);
   snprintf(src->user_agent, sizeof(src->user_agent), "%s", e->user_agent);
   snprintf(src->referrer, sizeof(src->referrer), "%s", e->referrer);
   src->local = e->local;
}
