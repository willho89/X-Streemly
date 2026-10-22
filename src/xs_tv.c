/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The TV (see xs_tv.h).
 *
 * Zapping: Up/Down (and L/R for groups) move a *pending* channel, shown in
 * the banner at once; it is tuned when no zap button is held and the
 * input has been idle for XS_ZAP_SETTLE_MS, so flicking through channels
 * doesn't open every stream on the way (§5.9).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xs_input.h"
#include "xs_tv.h"

#define XS_ZAP_SETTLE_MS   500
#define XS_BANNER_MS       3000
#define XS_INFO_MS         6000
#define XS_NOTE_MS         2500
#define XS_DIGITS_MS       1500
#define XS_MAX_DIGITS      4
#define XS_SAVE_AFTER_MS   5000   /* write the last channel once it has settled */

struct xs_tv
{
   xs_ctx_t     *ctx;
   xs_tuner_t   *tuner;
   xs_present_t *present;
   xs_chan_t     chan;
   xs_input_t    in;

   uint64_t ticks;
   double   fps;
   bool     have_pending;
   unsigned pending;
   unsigned banner_idx;     /* the channel the banner is about */
   uint64_t pending_tick;
   uint64_t banner_until, info_until, note_until;
   char     note[128];
   char     digits[XS_MAX_DIGITS + 1];
   uint64_t digits_tick;
   bool     list;
   unsigned cursor, top;
   uint64_t tuned_tick;

   xs_overlay_t ov;
};

static uint64_t after_ms(const xs_tv_t *tv, unsigned ms)
{
   return tv->ticks + (uint64_t)(ms * tv->fps / 1000.0 + 0.5);
}

xs_tv_t *xs_tv_new(xs_ctx_t *ctx, xs_tuner_t *tuner, xs_present_t *present, xs_chan_t *chan,
                   bool bitmasks)
{
   xs_tv_t *tv = calloc(1, sizeof(*tv));
   if (!tv)
      return NULL;
   tv->ctx     = ctx;
   tv->tuner   = tuner;
   tv->present = present;
   tv->chan    = *chan;
   tv->fps     = 60.0;
   memset(chan, 0, sizeof(*chan));
   xs_input_init(&tv->in, bitmasks);
   return tv;
}

void xs_tv_free(xs_tv_t *tv)
{
   if (!tv)
      return;
   xs_chan_save(tv->ctx, &tv->chan);
   xs_chan_free(&tv->chan);
   free(tv);
}

static void note(xs_tv_t *tv, const char *msg)
{
   snprintf(tv->note, sizeof(tv->note), "%s", msg);
   tv->note_until = after_ms(tv, XS_NOTE_MS);
}

static void tune(xs_tv_t *tv, unsigned idx)
{
   const xs_entry_t *e = xs_chan_entry(&tv->chan, idx);
   xs_source_t src;

   tv->have_pending = false;
   tv->banner_until = after_ms(tv, XS_BANNER_MS);
   tv->banner_idx   = idx;
   if (e->flags & XS_CH_DRM)
   {
      note(tv, "ENCRYPTED: CAN'T PLAY");
      return;
   }
   xs_chan_set(tv->ctx, &tv->chan, idx);
   tv->tuned_tick = tv->ticks;
   xs_chan_source(&tv->chan, idx, &src);
   xs_present_set_name(tv->present, e->name);
   XS_INFO(tv->ctx, "channel %u: %s\n", xs_chan_number(&tv->chan, idx), e->name);
   xs_tuner_tune(tv->tuner, &src);
}

void xs_tv_start(xs_tv_t *tv)
{
   tv->banner_idx = tv->chan.cur;
   tune(tv, tv->chan.cur);
}

static void move_pending(xs_tv_t *tv, unsigned idx)
{
   tv->pending      = idx;
   tv->banner_idx   = idx;
   tv->have_pending = true;
   tv->pending_tick = tv->ticks;
   tv->banner_until = after_ms(tv, XS_BANNER_MS);
   tv->digits[0]    = '\0';
}

static void commit_digits(xs_tv_t *tv)
{
   unsigned idx, n = (unsigned)atoi(tv->digits);
   char msg[64];
   tv->digits[0] = '\0';
   if (xs_chan_find(&tv->chan, n, &idx))
      tune(tv, idx);
   else
   {
      snprintf(msg, sizeof(msg), "NO CHANNEL %u", n);
      note(tv, msg);
      tv->banner_until = after_ms(tv, XS_NOTE_MS);
   }
}

static void list_move(xs_tv_t *tv, int by)
{
   int n = (int)xs_chan_count(&tv->chan);
   tv->cursor = (unsigned)((((int)tv->cursor + by) % n + n) % n);
}

static void handle(xs_tv_t *tv, unsigned act, unsigned rows)
{
   unsigned base = tv->have_pending ? tv->pending : tv->chan.cur;

   if (tv->list)
   {
      switch (act)
      {
         case XS_ACT_UP:         list_move(tv, -1); break;
         case XS_ACT_DOWN:       list_move(tv, 1); break;
         case XS_ACT_PREV_GROUP: list_move(tv, -(int)rows); break;
         case XS_ACT_NEXT_GROUP: list_move(tv, (int)rows); break;
         case XS_ACT_OK:         tv->list = false; tune(tv, tv->cursor); break;
         case XS_ACT_BACK:
         case XS_ACT_LIST:       tv->list = false; break;
         default: break;
      }
      return;
   }

   if (act >= XS_ACT_DIGIT0 && act < XS_ACT_DIGIT0 + 10)
   {
      size_t n = strlen(tv->digits);
      if (n < XS_MAX_DIGITS && !(n == 0 && act == XS_ACT_DIGIT0))
      {
         tv->digits[n]     = (char)('0' + (act - XS_ACT_DIGIT0));
         tv->digits[n + 1] = '\0';
         tv->digits_tick   = tv->ticks;
         tv->have_pending  = false;
         /* No longer number can exist: tune now. */
         if ((unsigned)atoi(tv->digits) * 10 > xs_chan_max_number(&tv->chan))
            commit_digits(tv);
      }
      return;
   }

   switch (act)
   {
      case XS_ACT_UP:         move_pending(tv, xs_chan_step(&tv->chan, base, 1)); break;
      case XS_ACT_DOWN:       move_pending(tv, xs_chan_step(&tv->chan, base, -1)); break;
      case XS_ACT_NEXT_GROUP: move_pending(tv, xs_chan_group_step(&tv->chan, base, 1)); break;
      case XS_ACT_PREV_GROUP: move_pending(tv, xs_chan_group_step(&tv->chan, base, -1)); break;
      case XS_ACT_OK:
         tv->info_until = tv->ticks < tv->info_until ? 0 : after_ms(tv, XS_INFO_MS);
         break;
      case XS_ACT_BACK:
         if (tv->chan.have_prev)
            tune(tv, tv->chan.prev);
         else
            tv->banner_until = after_ms(tv, XS_BANNER_MS);
         break;
      case XS_ACT_LIST:
         tv->list   = true;
         tv->cursor = tv->chan.cur;
         tv->info_until = 0;
         break;
      case XS_ACT_ENTER:
         if (tv->digits[0])
            commit_digits(tv);
         break;
      default: break;
   }
}

static const char *state_text(const xs_tuner_status_t *st)
{
   switch (st->state)
   {
      case XS_TUNER_CONNECTING: return "TUNING";
      case XS_TUNER_PLAYING:    return "PLAYING";
      case XS_TUNER_FAILED:     return st->msg;
      case XS_TUNER_ENDED:      return "END OF STREAM";
      default:                  return "";
   }
}

static void build_overlay(xs_tv_t *tv, unsigned pic_h)
{
   xs_overlay_t ov;
   unsigned shown = tv->banner_idx;
   const xs_entry_t *e = xs_chan_entry(&tv->chan, shown);
   /* The tuning / no-signal card already names the channel. */
   bool card = xs_present_card_showing(tv->present);

   memset(&ov, 0, sizeof(ov));
   ov.list_sel = ov.list_on = -1;

   if (tv->digits[0])
   {
      ov.banner = ov.typing = true;
      snprintf(ov.number, sizeof(ov.number), "%s-", tv->digits);
   }
   else if (tv->ticks < tv->banner_until || tv->have_pending)
   {
      ov.banner = true;
      snprintf(ov.number, sizeof(ov.number), "%u", xs_chan_number(&tv->chan, shown));
      if (!card || shown != tv->chan.cur)
      {
         snprintf(ov.name, sizeof(ov.name), "%s", e->name);
         snprintf(ov.group, sizeof(ov.group), "%s", e->group);
      }
   }
   if (tv->ticks < tv->note_until)
      snprintf(ov.note, sizeof(ov.note), "%s", tv->note);

   if (tv->ticks < tv->info_until)
   {
      const xs_entry_t *c = xs_chan_entry(&tv->chan, tv->chan.cur);
      xs_tuner_status_t st;
      xs_tuner_get_status(tv->tuner, &st);
      ov.info = true;
      snprintf(ov.info_line[0], sizeof(ov.info_line[0]), "%u  %.240s", xs_chan_number(&tv->chan, tv->chan.cur), c->name);
      snprintf(ov.info_line[1], sizeof(ov.info_line[1]), "%s%s%s", c->group,
               c->flags & XS_CH_GEOBLOCKED ? "  GEO-BLOCKED" : "",
               c->flags & XS_CH_NOT_247 ? "  NOT 24/7" : "");
      snprintf(ov.info_line[2], sizeof(ov.info_line[2]), "%s",
               st.state == XS_TUNER_PLAYING ? st.msg : "");
      snprintf(ov.info_line[3], sizeof(ov.info_line[3]), "%s", state_text(&st));
      if (!ov.info_line[1][0])
      {
         memmove(ov.info_line[1], ov.info_line[2], sizeof(ov.info_line[1]));
         memmove(ov.info_line[2], ov.info_line[3], sizeof(ov.info_line[2]));
         ov.info_line[3][0] = '\0';
      }
   }

   if (tv->list)
   {
      unsigned n = xs_chan_count(&tv->chan), rows = xs_osd_list_rows(pic_h ? pic_h : 360), i;
      if (rows > n)
         rows = n;
      if (tv->cursor < tv->top)
         tv->top = tv->cursor;
      if (tv->cursor >= tv->top + rows)
         tv->top = tv->cursor - rows + 1;
      if (tv->top + rows > n)
         tv->top = n - rows;
      ov.list = true;
      snprintf(ov.list_title, sizeof(ov.list_title), "CHANNELS  %u/%u", tv->cursor + 1, n);
      for (i = 0; i < rows; i++)
      {
         unsigned idx = tv->top + i;
         const xs_entry_t *r = xs_chan_entry(&tv->chan, idx);
         snprintf(ov.list_row[i], sizeof(ov.list_row[i]), "%4u  %.140s", xs_chan_number(&tv->chan, idx), r->name);
         if (idx == tv->cursor)
            ov.list_sel = (int)i;
         if (idx == tv->chan.cur)
            ov.list_on = (int)i;
      }
      ov.list_n = rows;
   }

   ov.version = tv->ov.version;
   if (memcmp(&ov, &tv->ov, sizeof(ov)))
   {
      ov.version++;
      tv->ov = ov;
   }
}

void xs_tv_tick(xs_tv_t *tv, retro_input_state_t input_cb, double fps, unsigned pic_h)
{
   unsigned acts[XS_MAX_ACTIONS], n, i;
   bool zapping;
   unsigned rows = xs_osd_list_rows(pic_h ? pic_h : 360);

   tv->ticks++;
   tv->fps = fps;
   n = xs_input_poll(&tv->in, input_cb, fps, acts, &zapping);
   for (i = 0; i < n; i++)
      handle(tv, acts[i], rows);

   if (tv->have_pending && !zapping && tv->ticks >= tv->pending_tick + (uint64_t)(XS_ZAP_SETTLE_MS * fps / 1000.0))
   {
      if (tv->pending != tv->chan.cur)
         tune(tv, tv->pending);
      else
         tv->have_pending = false;
   }
   if (tv->digits[0] && tv->ticks >= tv->digits_tick + (uint64_t)(XS_DIGITS_MS * fps / 1000.0))
      commit_digits(tv);
   if (tv->chan.dirty && tv->ticks >= tv->tuned_tick + (uint64_t)(XS_SAVE_AFTER_MS * fps / 1000.0))
      xs_chan_save(tv->ctx, &tv->chan);

   build_overlay(tv, pic_h);
}

const xs_overlay_t *xs_tv_overlay(const xs_tv_t *tv)
{
   return &tv->ov;
}
