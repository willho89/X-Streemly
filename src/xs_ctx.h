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

#define XS_VERSION     "0.1.0"
#define XS_SAMPLE_RATE 48000

typedef struct xs_tuner xs_tuner_t;

typedef enum
{
   XS_DEINT_BWDIF = 0,   /* field rate (DeeVee D-021) */
   XS_DEINT_YADIF,       /* field rate */
   XS_DEINT_OFF
} xs_deint_t;

typedef struct
{
   bool       verbose;        /* FFmpeg verbose logging */
   unsigned   max_height;     /* 0 = native; else cap variant choice and output */
   xs_deint_t deint;
   unsigned   buffer_ms;      /* start / steady-state buffer target */
   bool       osd;            /* in-picture cards and messages */
} xs_options_t;

typedef struct
{
   int64_t  run_sum_us, run_max_us;   /* time inside retro_run */
   int64_t  gap_max_us;               /* longest time between two calls */
   uint64_t runs;
} xs_run_stats_t;

typedef struct xs_ctx
{
   retro_log_printf_t log_cb;
   xs_options_t       opt;
   xs_tuner_t        *tuner;
   xs_run_stats_t     run;      /* emulation thread; reset by each stats line */
} xs_ctx_t;

/* Thread-safe: may be called from any thread. */
void xs_log(xs_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...);

#define XS_DEBUG(ctx, ...) xs_log(ctx, RETRO_LOG_DEBUG, __VA_ARGS__)
#define XS_INFO(ctx, ...)  xs_log(ctx, RETRO_LOG_INFO, __VA_ARGS__)
#define XS_WARN(ctx, ...)  xs_log(ctx, RETRO_LOG_WARN, __VA_ARGS__)
#define XS_ERROR(ctx, ...) xs_log(ctx, RETRO_LOG_ERROR, __VA_ARGS__)

/* Callback a producer polls while it waits for queue space: nonzero means
 * stop waiting (newer tune, unload). */
typedef int (*xs_abort_fn)(void *arg);

#endif
