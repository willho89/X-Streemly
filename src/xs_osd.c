/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * In-picture OSD (see xs_osd.h). The glyphs are original, drawn for
 * X-Streemly: 7 rows of 5 pixels, '#' = lit.
 */
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "xs_osd.h"

typedef struct
{
   char        c;
   const char *rows[XS_OSD_GLYPH_H];
} glyph_t;

static const glyph_t g_glyphs[] = {
   { ' ', { ".....", ".....", ".....", ".....", ".....", ".....", "....." } },
   { '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
   { '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
   { '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
   { '3', { "####.", "....#", "....#", ".###.", "....#", "....#", "####." } },
   { '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
   { '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
   { '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
   { '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
   { '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
   { '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
   { 'A', { "..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#" } },
   { 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
   { 'C', { ".####", "#....", "#....", "#....", "#....", "#....", ".####" } },
   { 'D', { "###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.." } },
   { 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
   { 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
   { 'G', { ".####", "#....", "#....", "#..##", "#...#", "#...#", ".####" } },
   { 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
   { 'I', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####" } },
   { 'J', { "....#", "....#", "....#", "....#", "....#", "#...#", ".###." } },
   { 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
   { 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
   { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
   { 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
   { 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
   { 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
   { 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
   { 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
   { 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
   { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
   { 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
   { 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
   { 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#" } },
   { 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
   { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
   { 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
   { '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
   { ',', { ".....", ".....", ".....", ".....", ".##..", "..#..", ".#..." } },
   { ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
   { ';', { ".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..." } },
   { '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
   { '+', { ".....", "..#..", "..#..", "#####", "..#..", "..#..", "....." } },
   { '=', { ".....", ".....", "#####", ".....", "#####", ".....", "....." } },
   { '_', { ".....", ".....", ".....", ".....", ".....", ".....", "#####" } },
   { '/', { "....#", "....#", "...#.", "..#..", ".#...", "#....", "#...." } },
   { '\\',{ "#....", "#....", ".#...", "..#..", "...#.", "....#", "....#" } },
   { '(', { "...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#." } },
   { ')', { ".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..." } },
   { '[', { ".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###." } },
   { ']', { ".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###." } },
   { '<', { "...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#." } },
   { '>', { ".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..." } },
   { '!', { "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.." } },
   { '?', { ".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.." } },
   { '\'',{ "..#..", "..#..", ".#...", ".....", ".....", ".....", "....." } },
   { '"', { ".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....." } },
   { '&', { ".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#" } },
   { '%', { "##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##" } },
   { '#', { ".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#." } },
   { '*', { ".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....." } },
   { '@', { ".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####" } },
   { '|', { "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
   { '~', { ".....", ".....", ".#...", "#.#.#", "...#.", ".....", "....." } },
};

static const glyph_t *find_glyph(char c)
{
   size_t i;
   c = (char)toupper((unsigned char)c);
   for (i = 0; i < sizeof(g_glyphs) / sizeof(g_glyphs[0]); i++)
      if (g_glyphs[i].c == c)
         return &g_glyphs[i];
   return find_glyph('?');
}

/* UTF-8 continuation bytes are skipped; each multi-byte character draws as
 * one '?' (channel names are ASCII far more often than not). */
static bool is_cont(unsigned char c)
{
   return (c & 0xC0) == 0x80;
}

unsigned xs_osd_text_width(const char *text, unsigned scale)
{
   unsigned n = 0;
   for (; *text; text++)
      if (!is_cont((unsigned char)*text))
         n++;
   return n ? (n * (XS_OSD_GLYPH_W + 1) - 1) * scale : 0;
}

void xs_osd_rect(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 int x, int y, unsigned rw, unsigned rh, uint32_t color)
{
   int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
   int x1 = x + (int)rw > (int)w ? (int)w : x + (int)rw;
   int y1 = y + (int)rh > (int)h ? (int)h : y + (int)rh;
   int yy, xx;
   for (yy = y0; yy < y1; yy++)
   {
      uint32_t *row = (uint32_t *)((uint8_t *)pix + (size_t)yy * pitch);
      for (xx = x0; xx < x1; xx++)
         row[xx] = color;
   }
}

static void draw_glyph(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                       int x, int y, unsigned scale, uint32_t color, const glyph_t *g)
{
   int r, c;
   for (r = 0; r < XS_OSD_GLYPH_H; r++)
      for (c = 0; c < XS_OSD_GLYPH_W; c++)
         if (g->rows[r][c] == '#')
            xs_osd_rect(pix, w, h, pitch, x + c * (int)scale, y + r * (int)scale, scale, scale, color);
}

void xs_osd_text(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 int x, int y, unsigned scale, uint32_t color, const char *text)
{
   int pass;
   for (pass = 0; pass < 2; pass++)
   {
      const char *p;
      int cx = x + (pass ? 0 : (int)scale), cy = y + (pass ? 0 : (int)scale);
      for (p = text; *p; p++)
      {
         unsigned char ch = (unsigned char)*p;
         if (is_cont(ch))
            continue;
         draw_glyph(pix, w, h, pitch, cx, cy, scale, pass ? color : 0x000000,
                    find_glyph(ch < 0x80 ? (char)ch : '?'));
         cx += (XS_OSD_GLYPH_W + 1) * (int)scale;
      }
   }
}

/* Draw text centred on a row, shrinking the scale until it fits. */
static void centred(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                    int y, unsigned scale, uint32_t color, const char *text)
{
   unsigned tw;
   while (scale > 1 && xs_osd_text_width(text, scale) > w - w / 10)
      scale--;
   tw = xs_osd_text_width(text, scale);
   xs_osd_text(pix, w, h, pitch, ((int)w - (int)tw) / 2, y, scale, color, text);
}

void xs_osd_card(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                 const char *title, const char *name, const char *detail)
{
   unsigned big = h / 60 ? h / 60 : 1, small = big / 2 ? big / 2 : 1;
   /* The three lines as a block centred a little below the middle, clear
    * of the channel banner in the top right. */
   int block = (int)(XS_OSD_GLYPH_H * (2 * big + 2 * (small + 1) + small));
   int y = (int)h * 55 / 100 - block / 2;

   xs_osd_rect(pix, w, h, pitch, 0, 0, w, h, 0x101418);
   if (title)
      centred(pix, w, h, pitch, y, big, 0xF0F0F0, title);
   y += (int)(XS_OSD_GLYPH_H * big) * 2;
   if (name && *name)
      centred(pix, w, h, pitch, y, small + 1, 0xF8D030, name);
   y += (int)(XS_OSD_GLYPH_H * (small + 1)) * 2;
   if (detail && *detail)
      centred(pix, w, h, pitch, y, small, 0xA0A8B0, detail);
}

/* ---- overlay ------------------------------------------------------------ */

#define XS_INK      0xF0F0F0
#define XS_GREEN    0x40E060   /* channel number, like a 90s set */
#define XS_YELLOW   0xF8D030
#define XS_GREY     0xA0A8B0
#define XS_HILITE   0x2850A0
#define XS_PANEL    0x0C1230   /* list and info background, a 90s menu blue */

/* Font pixel size for overlay text at this picture height. */
static unsigned unit(unsigned h)
{
   return h / 180 ? h / 180 : 1;
}

/* Text cut to at most max_w pixels (whole characters). */
static void text_fit(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                     int x, int y, unsigned scale, uint32_t color, const char *text, unsigned max_w)
{
   char buf[256];
   size_t n = 0, chars = 0, max_chars = (max_w / scale + 1) / (XS_OSD_GLYPH_W + 1);
   const char *p;
   for (p = text; *p && n < sizeof(buf) - 1; p++)
   {
      if (!is_cont((unsigned char)*p))
      {
         if (chars == max_chars)
            break;
         chars++;
      }
      buf[n++] = *p;
   }
   buf[n] = '\0';
   xs_osd_text(pix, w, h, pitch, x, y, scale, color, buf);
}

static void text_right(uint32_t *pix, unsigned w, unsigned h, size_t pitch,
                       int right, int y, unsigned scale, uint32_t color, const char *text)
{
   unsigned tw = xs_osd_text_width(text, scale);
   if (tw > w / 2)
   {
      text_fit(pix, w, h, pitch, right - (int)(w / 2), y, scale, color, text, w / 2);
      return;
   }
   xs_osd_text(pix, w, h, pitch, right - (int)tw, y, scale, color, text);
}

bool xs_osd_overlay_visible(const xs_overlay_t *ov)
{
   return ov->banner || ov->info || ov->list || ov->note[0];
}

unsigned xs_osd_list_rows(unsigned h)
{
   unsigned u = unit(h), row = (XS_OSD_GLYPH_H + 4) * u;
   unsigned rows = (h - 2 * (h / 12) - row * 2) / row;
   return rows > XS_OV_ROWS ? XS_OV_ROWS : rows;
}

/* Where each part of the overlay goes. Drawing and xs_osd_overlay_rects()
 * both use this, so the saved areas always cover what is drawn. */
typedef struct
{
   unsigned u, row;
   int      margin_x, margin_y, right;
   bool     list, banner, info;
   xs_osd_rect_t list_r, banner_r, info_r;
   unsigned info_lines;
   unsigned num_scale, name_scale;
} layout_t;

static unsigned clipped_width(const char *text, unsigned scale, unsigned w)
{
   unsigned tw = xs_osd_text_width(text, scale);
   return tw > w / 2 ? w / 2 : tw;
}

static void layout(const xs_overlay_t *ov, unsigned w, unsigned h, layout_t *l)
{
   unsigned i;
   memset(l, 0, sizeof(*l));
   l->u          = unit(h);
   l->row        = (XS_OSD_GLYPH_H + 4) * l->u;
   l->margin_x   = (int)(w / 16);   /* inside the overscan */
   l->margin_y   = (int)(h / 12);
   l->right      = (int)w - l->margin_x;
   l->num_scale  = 4 * l->u;
   l->name_scale = l->u + l->u / 2;

   if (ov->list)
   {
      unsigned pw = w * 9 / 20;
      l->list     = true;
      l->list_r.x = l->margin_x - (int)(2 * l->u);
      l->list_r.y = l->margin_y - (int)(2 * l->u);
      l->list_r.w = pw + 4 * l->u;
      l->list_r.h = l->row * (ov->list_n + 2) + 4 * l->u;
   }
   if (ov->banner || ov->note[0])
   {
      unsigned bw = 0, bh = 0, tw;
      if (ov->banner)
      {
         bw = clipped_width(ov->number, l->num_scale, w);
         bh = XS_OSD_GLYPH_H * l->num_scale + 3 * l->u;
         if (!ov->typing && ov->name[0])
         {
            tw = clipped_width(ov->name, l->name_scale, w);
            bw = tw > bw ? tw : bw;
            bh += XS_OSD_GLYPH_H * l->name_scale + 3 * l->u;
         }
         if (!ov->typing && ov->group[0])
         {
            tw = clipped_width(ov->group, l->u, w);
            bw = tw > bw ? tw : bw;
            bh += XS_OSD_GLYPH_H * l->u + 3 * l->u;
         }
      }
      if (ov->note[0])
      {
         tw = clipped_width(ov->note, l->u, w);
         bw = tw > bw ? tw : bw;
         bh += XS_OSD_GLYPH_H * l->u;
      }
      l->banner     = true;
      l->banner_r.x = l->right - (int)bw;
      l->banner_r.y = l->margin_y;
      l->banner_r.w = bw + l->num_scale;       /* + drop shadow */
      l->banner_r.h = bh + l->num_scale;
   }
   if (ov->info)
   {
      for (i = 0; i < XS_OV_INFO; i++)
         if (ov->info_line[i][0])
            l->info_lines = i + 1;
      l->info     = l->info_lines > 0;
      l->info_r.x = l->margin_x - (int)(3 * l->u);
      l->info_r.y = (int)h - l->margin_y - (int)(l->row * l->info_lines) - (int)(3 * l->u);
      l->info_r.w = w - 2 * (unsigned)l->margin_x + 6 * l->u;
      l->info_r.h = l->row * l->info_lines + 4 * l->u;
   }
}

unsigned xs_osd_overlay_rects(const xs_overlay_t *ov, unsigned w, unsigned h,
                              xs_osd_rect_t out[XS_OSD_MAX_RECTS])
{
   layout_t l;
   unsigned n = 0, i;
   layout(ov, w, h, &l);
   if (l.list)
   {
      out[n] = l.list_r;
      out[n++].panel = true;
   }
   if (l.banner)
   {
      out[n] = l.banner_r;
      out[n++].panel = false;
   }
   if (l.info)
   {
      out[n] = l.info_r;
      out[n++].panel = true;
   }
   /* clip to the picture */
   for (i = 0; i < n; i++)
   {
      int x1 = out[i].x + (int)out[i].w, y1 = out[i].y + (int)out[i].h;
      if (out[i].x < 0) out[i].x = 0;
      if (out[i].y < 0) out[i].y = 0;
      if (x1 > (int)w) x1 = (int)w;
      if (y1 > (int)h) y1 = (int)h;
      out[i].w = x1 > out[i].x ? (unsigned)(x1 - out[i].x) : 0;
      out[i].h = y1 > out[i].y ? (unsigned)(y1 - out[i].y) : 0;
   }
   return n;
}

void xs_osd_overlay(uint32_t *pix, unsigned w, unsigned h, size_t pitch, const xs_overlay_t *ov,
                    unsigned parts)
{
   layout_t l;
   unsigned u, i;
   layout(ov, w, h, &l);
   u = l.u;

   if (l.list && (parts & XS_OSD_PANELS))
   {
      unsigned pw = w * 9 / 20;
      int x = l.margin_x, y = l.margin_y;
      /* Opaque: a fill is far cheaper than darkening the picture under it,
       * and this is redrawn on every new frame while open. */
      xs_osd_rect(pix, w, h, pitch, l.list_r.x, l.list_r.y, l.list_r.w, l.list_r.h, XS_PANEL);
      text_fit(pix, w, h, pitch, x, y, u, XS_YELLOW, ov->list_title, pw);
      y += (int)(l.row + l.row / 2);
      for (i = 0; i < ov->list_n; i++, y += (int)l.row)
      {
         if ((int)i == ov->list_sel)
            xs_osd_rect(pix, w, h, pitch, x - (int)u, y - (int)(2 * u), pw + 2 * u, l.row, XS_HILITE);
         text_fit(pix, w, h, pitch, x, y, u, (int)i == ov->list_on ? XS_GREEN : XS_INK,
                  ov->list_row[i], pw);
      }
   }

   if (l.banner && (parts & XS_OSD_TEXT))
   {
      int y = l.margin_y;
      if (ov->banner)
      {
         text_right(pix, w, h, pitch, l.right, y, l.num_scale, ov->typing ? XS_YELLOW : XS_GREEN, ov->number);
         y += (int)(XS_OSD_GLYPH_H * l.num_scale + 3 * u);
         if (!ov->typing && ov->name[0])
         {
            text_right(pix, w, h, pitch, l.right, y, l.name_scale, XS_INK, ov->name);
            y += (int)(XS_OSD_GLYPH_H * l.name_scale + 3 * u);
         }
         if (!ov->typing && ov->group[0])
         {
            text_right(pix, w, h, pitch, l.right, y, u, XS_GREY, ov->group);
            y += (int)(XS_OSD_GLYPH_H * u + 3 * u);
         }
      }
      if (ov->note[0])
         text_right(pix, w, h, pitch, l.right, y, u, XS_YELLOW, ov->note);
   }

   if (l.info && (parts & XS_OSD_PANELS))
   {
      int y = l.info_r.y + (int)(3 * u);
      xs_osd_rect(pix, w, h, pitch, l.info_r.x, l.info_r.y, l.info_r.w, l.info_r.h, XS_PANEL);
      for (i = 0; i < l.info_lines; i++, y += (int)l.row)
         text_fit(pix, w, h, pitch, l.margin_x, y, u, i ? XS_GREY : XS_INK, ov->info_line[i],
                  w - 2 * (unsigned)l.margin_x);
   }
}
