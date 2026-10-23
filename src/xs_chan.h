/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The channel lineup (CLAUDE.md §5.1): channels in order, their numbers,
 * the channel on and the one before it, remembered between sessions.
 */
#ifndef XS_CHAN_H
#define XS_CHAN_H

#include <stdbool.h>

#include "xs_ctx.h"
#include "xs_playlist.h"
#include "xs_source.h"

/* Channels remembered across lineups (dead ones, favourites): a key per
 * channel (tvg-id, else URL) and a time. */
typedef struct
{
   unsigned long long *key;
   long long          *when;
   unsigned            n, cap;
   bool                dirty;
   char                path[4096];   /* "" = not kept */
} xs_marks_t;

typedef struct
{
   xs_playlist_t pl;
   unsigned     *number;     /* channel number of each entry */
   unsigned      cur, prev;  /* entry indices */
   bool          have_prev;
   bool          dirty;              /* state not yet written */
   char          state_path[4096];   /* "" = not remembered */
   xs_marks_t    dead;               /* failed to tune: skipped for a day */
   xs_marks_t    favs;
} xs_chan_t;

#define XS_DEAD_SECONDS (24 * 3600)

/* Build the lineup from content: a .strm (one channel), an .m3u/.m3u8
 * channel list, or an HLS playlist (one channel). save_dir may be NULL.
 * Starts on the channel remembered for this lineup, else the first. */
bool xs_chan_load(xs_ctx_t *ctx, xs_chan_t *c, const char *content, const char *save_dir,
                  char *err, size_t errlen);
/* Build the lineup from a parsed playlist (taken over). key names the
 * lineup for remembering its channel (e.g. its URL or iptv-org query). */
bool xs_chan_from_playlist(xs_ctx_t *ctx, xs_chan_t *c, xs_playlist_t *pl, const char *key,
                           const char *save_dir);
/* An empty lineup (while one is being downloaded). */
void xs_chan_empty(xs_chan_t *c);
void xs_chan_free(xs_chan_t *c);

unsigned xs_chan_count(const xs_chan_t *c);
const xs_entry_t *xs_chan_entry(const xs_chan_t *c, unsigned idx);
unsigned xs_chan_number(const xs_chan_t *c, unsigned idx);

/* Entry dir steps away from idx (wrapping), skipping unplayable ones and
 * ones that failed in the last day (unless all of them did). */
unsigned xs_chan_step(const xs_chan_t *c, unsigned idx, int dir);
/* First entry of the next/previous group from idx (wrapping). */
unsigned xs_chan_group_step(const xs_chan_t *c, unsigned idx, int dir);
/* Entry with this channel number; false if none. */
bool xs_chan_find(const xs_chan_t *c, unsigned number, unsigned *idx);
/* Highest channel number (for number entry). */
unsigned xs_chan_max_number(const xs_chan_t *c);

/* Make idx the channel on (the old one becomes "previous"). */
void xs_chan_set(xs_ctx_t *ctx, xs_chan_t *c, unsigned idx);
/* Write the remembered state if it changed (file I/O: called once the
 * channel has settled and at unload, not on every zap). */
void xs_chan_save(xs_ctx_t *ctx, xs_chan_t *c);

/* Dead channels (§5.8) and favourites. */
void xs_chan_mark_dead(xs_chan_t *c, unsigned idx, bool dead);
bool xs_chan_is_dead(const xs_chan_t *c, unsigned idx);
void xs_chan_toggle_fav(xs_chan_t *c, unsigned idx);
bool xs_chan_is_fav(const xs_chan_t *c, unsigned idx);
/* Next favourite after idx (wrapping); idx if there are none. */
unsigned xs_chan_next_fav(const xs_chan_t *c, unsigned idx);

/* The source to tune for an entry. */
void xs_chan_source(const xs_chan_t *c, unsigned idx, xs_source_t *src);

#endif
