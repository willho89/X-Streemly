/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Core context shared by all modules (CLAUDE.md §3, §10).
 */
#ifndef XS_CTX_H
#define XS_CTX_H

#include <stdbool.h>
#include <stdint.h>

#include <libretro.h>

#define XS_VERSION     "0.0.1"
#define XS_SAMPLE_RATE 48000

typedef struct xs_tuner xs_tuner_t;

typedef struct
{
   bool verbose;   /* DEBUG-level core logs + FFmpeg verbose */
} xs_options_t;

typedef struct xs_ctx
{
   retro_log_printf_t log_cb;
   xs_options_t       opt;
   xs_tuner_t        *tuner;
} xs_ctx_t;

/* Thread-safe: may be called from the tuner thread. */
void xs_log(xs_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...);

#define XS_DEBUG(ctx, ...) xs_log(ctx, RETRO_LOG_DEBUG, __VA_ARGS__)
#define XS_INFO(ctx, ...)  xs_log(ctx, RETRO_LOG_INFO, __VA_ARGS__)
#define XS_WARN(ctx, ...)  xs_log(ctx, RETRO_LOG_WARN, __VA_ARGS__)
#define XS_ERROR(ctx, ...) xs_log(ctx, RETRO_LOG_ERROR, __VA_ARGS__)

#endif
