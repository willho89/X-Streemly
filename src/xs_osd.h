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

#include <stdbool.h>
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

/* What the TV draws over the picture (CLAUDE.md §5.10). Built by xs_tv;
 * version changes whenever any of it does. */
#define XS_OV_ROWS  40
#define XS_OV_INFO  4
typedef struct
{
   unsigned version;
   bool     banner;               /* channel number (+ name, group) top right */
   char     number[16];           /* "12", or typed digits "1-" */
   bool     typing;               /* number[] is being typed */
   char     name[256];
   char     group[128];
   char     note[128];            /* short message under the banner */
   bool     info;                 /* info band at the bottom */
   char     info_line[XS_OV_INFO][256];
   bool     list;                 /* channel list panel on the left */
   char     list_title[128];
   unsigned list_n;               /* rows used */
   int      list_sel;             /* highlighted row, -1 none */
   int      list_on;              /* row of the channel on, -1 none */
   char     list_row[XS_OV_ROWS][160];
} xs_overlay_t;

typedef struct
{
   int      x, y;
   unsigned w, h;
   bool     panel;    /* opaque: its pixels don't depend on the picture */
} xs_osd_rect_t;

#define XS_OSD_MAX_RECTS 3

bool xs_osd_overlay_visible(const xs_overlay_t *ov);
/* The areas xs_osd_overlay() will draw on, clipped to the picture: save
 * them first to undo the overlay later. */
unsigned xs_osd_overlay_rects(const xs_overlay_t *ov, unsigned w, unsigned h,
                              xs_osd_rect_t out[XS_OSD_MAX_RECTS]);
/* Rows the list panel holds at this picture height. */
unsigned xs_osd_list_rows(unsigned h);
/* Draw the overlay: XS_OSD_PANELS (list, info: opaque, can be drawn once
 * and pasted), XS_OSD_TEXT (banner, note: drawn over the picture). */
#define XS_OSD_PANELS 1
#define XS_OSD_TEXT   2
void xs_osd_overlay(uint32_t *pix, unsigned w, unsigned h, size_t pitch, const xs_overlay_t *ov,
                    unsigned parts);

#endif
