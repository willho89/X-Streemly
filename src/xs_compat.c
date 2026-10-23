/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "xs_compat.h"

void xs_mkdir(const char *path)
{
#ifdef _WIN32
   _mkdir(path);
#else
   mkdir(path, 0755);
#endif
}

int xs_replace_file(const char *from, const char *to)
{
#ifdef _WIN32
   /* rename() fails on Windows when the destination exists. */
   return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
   return rename(from, to);
#endif
}
