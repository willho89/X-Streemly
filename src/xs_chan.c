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
#include <sys/stat.h>

#include "xs_chan.h"

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
      if (c->pl.e[i].chno <= 0)
         own = false;
      for (j = 0; j < i && own; j++)
         if (c->pl.e[j].chno == c->pl.e[i].chno)
            own = false;
   }
   for (i = 0; i < c->pl.n; i++)
      c->number[i] = own ? (unsigned)c->pl.e[i].chno : i + 1;
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
      if (!strncmp(line, "channel=", 8) && xs_chan_find(c, (unsigned)atoi(line + 8), &idx))
         c->cur = idx;
      else if (!strncmp(line, "previous=", 9) && xs_chan_find(c, (unsigned)atoi(line + 9), &idx))
      {
         c->prev = idx;
         c->have_prev = true;
      }
   }
   fclose(f);
   XS_INFO(ctx, "lineup: back on channel %u\n", c->number[c->cur]);
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
   fprintf(f, "channel=%u\n", c->number[c->cur]);
   if (c->have_prev)
      fprintf(f, "previous=%u\n", c->number[c->prev]);
   fclose(f);
}

bool xs_chan_load(xs_ctx_t *ctx, xs_chan_t *c, const char *content, const char *save_dir,
                  char *err, size_t errlen)
{
   unsigned i;

   memset(c, 0, sizeof(*c));
   if (has_ext(content, "m3u") || has_ext(content, "m3u8"))
   {
      if (!xs_playlist_load(content, &c->pl, err, errlen))
         return false;
   }
   else
   {
      /* .strm or a URL: one channel */
      xs_source_t src;
      if (!xs_source_from_content(ctx, content, &src, err, errlen))
         return false;
      c->pl.e = calloc(1, sizeof(*c->pl.e));
      if (!c->pl.e)
         return false;
      c->pl.n = c->pl.cap = 1;
      c->pl.e[0].chno = -1;
      snprintf(c->pl.e[0].name, sizeof(c->pl.e[0].name), "%s", src.name);
      snprintf(c->pl.e[0].url, sizeof(c->pl.e[0].url), "%s", src.url);
      c->pl.e[0].local = src.local;
   }
   c->number = calloc(c->pl.n, sizeof(*c->number));
   if (!c->number)
   {
      xs_playlist_free(&c->pl);
      return false;
   }
   number_channels(c);

   XS_INFO(ctx, "lineup: %u channel(s)%s\n", c->pl.n, c->pl.hls ? " (an HLS playlist)" : "");
   for (i = 0; i < c->pl.n; i++)
   {
      const xs_entry_t *e = &c->pl.e[i];
      XS_DEBUG(ctx, "channel %u: \"%s\" group \"%s\" tvg-id \"%s\" %up%s%s%s url %s%s%s%s%s\n",
               c->number[i], e->name, e->group, e->tvg_id, e->height,
               e->flags & XS_CH_GEOBLOCKED ? " geo-blocked" : "",
               e->flags & XS_CH_NOT_247 ? " not-24/7" : "",
               e->flags & XS_CH_DRM ? " DRM" : "", e->url,
               e->user_agent[0] ? " ua " : "", e->user_agent,
               e->referrer[0] ? " referrer " : "", e->referrer);
   }

   if (save_dir && save_dir[0])
   {
      char dir[4096];
      snprintf(dir, sizeof(dir), "%.4000s/X-Streemly", save_dir);
      mkdir(dir, 0755);
      snprintf(c->state_path, sizeof(c->state_path), "%.4000s/%016llx.state", dir, lineup_hash(content));
   }
   load_state(ctx, c);
   return true;
}

void xs_chan_free(xs_chan_t *c)
{
   xs_playlist_free(&c->pl);
   free(c->number);
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
   unsigned i, n = c->pl.n;
   for (i = 1; i <= n; i++)
   {
      unsigned j = (unsigned)(((int)idx + dir * (int)i) % (int)n + (int)n) % n;
      if (!(c->pl.e[j].flags & XS_CH_DRM))
         return j;
   }
   return idx;
}

unsigned xs_chan_group_step(const xs_chan_t *c, unsigned idx, int dir)
{
   unsigned i, n = c->pl.n;
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

unsigned xs_chan_max_number(const xs_chan_t *c)
{
   unsigned i, m = 0;
   for (i = 0; i < c->pl.n; i++)
      if (c->number[i] > m)
         m = c->number[i];
   return m;
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
