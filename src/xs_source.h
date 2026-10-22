/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A source: what to open and how (CLAUDE.md §3 "source abstraction", §5.2).
 * Everything the tuner needs to open a stream lives here, so a later
 * low-latency source (companion app) only adds fields and an open path.
 */
#ifndef XS_SOURCE_H
#define XS_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

#include <libavformat/avformat.h>

#include "xs_ctx.h"

typedef struct
{
   char name[256];         /* display name ("" = derive from the URL) */
   char url[4096];
   char user_agent[256];   /* "" = default */
   char referrer[1024];    /* "" = none */
   bool local;             /* plain file path: allow the file protocol */
} xs_source_t;

/* Fill *src from a content path: a .strm file (first non-comment line is
 * the URL) or, for tests, a URL given directly. Runs on the emulation
 * thread, so it reads local files only. */
bool xs_source_from_content(xs_ctx_t *ctx, const char *path, xs_source_t *src,
                            char *err, size_t errlen);

/* Open src into *out with the interrupt callback installed. Blocks; call on
 * the tuner thread only. Returns 0 or a negative AVERROR. */
int xs_source_open(xs_ctx_t *ctx, const xs_source_t *src, const AVIOInterruptCB *icb,
                   AVFormatContext **out);

#endif
