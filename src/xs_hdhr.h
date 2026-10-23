/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HDHomeRun network tuners on the LAN (CLAUDE.md M3b): find them and read
 * their channel lineup. Blocking: the lineup thread only.
 */
#ifndef XS_HDHR_H
#define XS_HDHR_H

#include <stdbool.h>
#include <stddef.h>

#include "xs_ctx.h"
#include "xs_playlist.h"

#define XS_HDHR_MAX 4

typedef struct
{
   char base[256];        /* "http://192.168.0.17" */
   char name[64];         /* "HDHomeRun FLEX 4K" */
   unsigned tuners;
} xs_hdhr_dev_t;

/* Find tuners: SiliconDust's UDP discovery broadcast, else the
 * hdhomerun.local name. XSTREEMLY_HDHR (base URLs, comma separated)
 * replaces both, for tests. Returns how many were found (<= max). */
unsigned xs_hdhr_discover(xs_ctx_t *ctx, xs_hdhr_dev_t *devs, unsigned max, xs_abort_fn stop, void *arg);

/* A tuner's channels from <base>/lineup.json, appended to *out. */
bool xs_hdhr_lineup(xs_ctx_t *ctx, const xs_hdhr_dev_t *dev, xs_playlist_t *out, char *err, size_t errlen,
                    xs_abort_fn stop, void *arg);

#endif
