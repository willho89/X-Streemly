/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * In-picture OSD (CLAUDE.md §5.10, D-009): a blocky 5x7 character-generator
 * font, like a 90s TV's on-screen display, drawn straight into XRGB8888
 * pictures. Text is shown in capitals.
 */
#ifndef XS_OSD_H
#define XS_OSD_H

#include <stddef.h>
#include <stdint.h>

#define XS_OSD_GLYPH_W 5
#define XS_OSD_GLYPH_H 7

/* Width in pixels of text drawn at this scale (each font pixel = scale²). */
unsigned xs_osd_text_width(const char *text, unsigned scale);

/* Draw text with its top-left at (x, y), clipped to the picture, with a
 * one-font-pixel drop shadow. */
void xs_osd_text(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 int x, int y, unsigned scale, uint32_t color, const char *text);

/* Fill a rectangle (clipped). */
void xs_osd_rect(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 int x, int y, unsigned rw, unsigned rh, uint32_t color);

/* The card shown while there's no picture: title (e.g. "TUNING"), the
 * channel name, and a smaller detail line. Any of them may be NULL. */
void xs_osd_card(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 const char *title, const char *name, const char *detail);

#endif
