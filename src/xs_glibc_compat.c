/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Keep the glibc floor at 2.35 (as DeeVee). libretro-common's rthreads.c
 * defines _GNU_SOURCE, and with glibc >= 2.38 headers that redirects
 * strtoul/fscanf to the C23 variants (__isoc23_*, GLIBC_2.38). Provide them
 * here, inside the core, forwarding to the classic functions: this file is
 * built without _GNU_SOURCE, so the calls below bind to the old versions.
 * The only difference is C23's "0b" prefix, which rthreads never parses.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* __GLIBC__ comes from the headers above. */
#if defined(__linux__) && defined(__GLIBC__)

unsigned long __isoc23_strtoul(const char *s, char **end, int base);
int __isoc23_fscanf(FILE *f, const char *fmt, ...);

unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{
   return strtoul(s, end, base);
}

int __isoc23_fscanf(FILE *f, const char *fmt, ...)
{
   va_list va;
   int ret;
   va_start(va, fmt);
   ret = vfscanf(f, fmt, va);
   va_end(va);
   return ret;
}

#endif

/* Linux ARM (platform=linux-arm64 / linux-armhf, from DeeVee): glibc 2.38
 * gave fmod a new symbol version on ARM, which alone would stop the core
 * loading on glibc 2.36 (Raspberry Pi OS / Debian 12). Every fmod reference
 * (-Wl,--wrap=fmod, FFmpeg included) goes to the original version. */
#if defined(__linux__) && defined(__GLIBC__) && (defined(__aarch64__) || defined(__arm__))

#if defined(__aarch64__)
__asm__(".symver xs_fmod_base,fmod@GLIBC_2.17");
#else
__asm__(".symver xs_fmod_base,fmod@GLIBC_2.4");
#endif

double xs_fmod_base(double x, double y);
double __wrap_fmod(double x, double y);

double __wrap_fmod(double x, double y)
{
   return xs_fmod_base(x, y);
}

#endif
