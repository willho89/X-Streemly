/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lineups that have to be downloaded (CLAUDE.md §5.7, §5.8): the iptv-org
 * lineup for the core options, and channel lists named by a URL. Built on
 * a worker thread (download, cache, parse, filter); the emulation thread
 * only polls for the result.
 */
#ifndef XS_IPTV_H
#define XS_IPTV_H

#include <stdbool.h>

#include "xs_ctx.h"
#include "xs_playlist.h"

#define XS_IPTV_BASE "https://iptv-org.github.io/iptv/"

typedef struct
{
   char     source[12];      /* "auto" (HDHomeRun if found, else iptv-org), "iptv",
                                "hdhomerun", "both" */
   char     country[8];      /* "uk", or "all" */
   char     language[8];     /* "eng", or "any" */
   char     category[64];    /* group-title name ("News"), or "all" */
   bool     hide_geoblocked;
   bool     hide_not_247;
   unsigned refresh_hours;   /* re-download cached lists older than this */
} xs_iptv_query_t;

typedef struct xs_iptv xs_iptv_t;

/* cache_dir: where downloaded lists are kept ("" = no cache). */
xs_iptv_t *xs_iptv_new(xs_ctx_t *ctx, const char *cache_dir);
void xs_iptv_free(xs_iptv_t *it);

/* Start building a lineup (replaces any request still running). */
void xs_iptv_request(xs_iptv_t *it, const xs_iptv_query_t *q);
/* A channel list (or HLS playlist) at a URL, from a .strm. */
void xs_iptv_request_url(xs_iptv_t *it, const char *url, const char *user_agent, const char *referrer);

/* 1: *out holds the lineup (yours to free); 0: still working (msg says
 * what); -1: failed (msg says why). */
int xs_iptv_poll(xs_iptv_t *it, xs_playlist_t *out, char *msg, size_t msglen);

/* Short description of a query, e.g. "IPTV-ORG UNITED KINGDOM, NEWS". */
void xs_iptv_describe(const xs_iptv_query_t *q, char *out, size_t len);
/* Key for remembering a lineup's last channel. */
void xs_iptv_key(const xs_iptv_query_t *q, char *out, size_t len);

#endif
