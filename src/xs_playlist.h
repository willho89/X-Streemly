/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * M3U/M3U8 channel lists (CLAUDE.md §5.7). Parsing follows VLC
 * modules/demux/playlist/m3u.c (ReadDir, parseEXTINF*) and the extensions
 * used by IPTV lists (tvg-* attributes, group-title, #EXTVLCOPT,
 * #KODIPROP, iptv-org's title suffixes).
 */
#ifndef XS_PLAYLIST_H
#define XS_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>

#define XS_CH_GEOBLOCKED 0x01   /* "[Geo-blocked]" */
#define XS_CH_NOT_247    0x02   /* "[Not 24/7]" */
#define XS_CH_DRM        0x04   /* #KODIPROP licence: can't be played */

typedef struct
{
   char     name[256];
   char     group[128];
   char     tvg_id[128];
   char     logo[512];
   char     language[64];
   char     url[4096];
   char     user_agent[256];
   char     referrer[1024];
   int      chno;        /* tvg-chno, -1 if none */
   unsigned height;      /* from an iptv-org "(720p)" suffix, 0 if unknown */
   unsigned flags;       /* XS_CH_* */
   bool     local;       /* url is a local path */
} xs_entry_t;

typedef struct
{
   xs_entry_t *e;
   unsigned    n, cap;
   bool        hls;      /* the file is an HLS playlist, not a channel list */
} xs_playlist_t;

/* Parse playlist text. base: the playlist's own path or URL, for relative
 * entries. Returns false (with a reason) if it holds no entries. */
bool xs_playlist_parse(const char *text, size_t len, const char *base, xs_playlist_t *out,
                       char *err, size_t errlen);
/* Read and parse a local file. */
bool xs_playlist_load(const char *path, xs_playlist_t *out, char *err, size_t errlen);
void xs_playlist_free(xs_playlist_t *pl);

#endif
