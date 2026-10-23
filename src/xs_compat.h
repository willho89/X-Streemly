/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The few file-system calls that differ between POSIX and Windows (M5).
 */
#ifndef XS_COMPAT_H
#define XS_COMPAT_H

/* Create a directory (no error if it exists). */
void xs_mkdir(const char *path);
/* rename(), replacing an existing destination on Windows too. */
int xs_replace_file(const char *from, const char *to);

#endif
