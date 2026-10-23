/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Small HTTP(S) downloads through FFmpeg's avio (CLAUDE.md D-003): channel
 * lists and iptv-org data. Blocking: worker threads only.
 */
#ifndef XS_FETCH_H
#define XS_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xs_ctx.h"

typedef struct
{
   const char *user_agent;   /* NULL = X-Streemly's */
   const char *referrer;     /* NULL = none */
   bool        verify;       /* check the server's certificate (D-012) */
   size_t      max_bytes;
   int64_t     timeout_us;   /* whole download */
   xs_abort_fn stop;         /* polled while blocked; nonzero = give up */
   void       *arg;
} xs_fetch_opts_t;

/* Download url into *out (malloc'd, NUL-terminated; free() it). Returns
 * 0, or a negative AVERROR with a reason in err. */
int xs_fetch(xs_ctx_t *ctx, const char *url, const xs_fetch_opts_t *o, char **out, size_t *len,
             char *err, size_t errlen);

/* The CA bundle to check certificates against (D-027), or NULL if there is
 * none (verification is then skipped with a warning). Linux and macOS: the
 * system's bundle. Android: the system's certificates (one file each)
 * gathered into <save_dir>/X-Streemly/cacerts.pem. Windows needs none:
 * schannel checks against the Windows certificate store. */
const char *xs_fetch_ca_file(xs_ctx_t *ctx);

#endif
