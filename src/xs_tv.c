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
#define XS_MAX_DIGITS      7      /* "1234.56" */
#define XS_SAVE_AFTER_MS   5000   /* write the last channel once it has settled */
#define XS_AUTO_TRIES      20     /* dead channels skipped on our own at start */

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
   /* The last tune, watched until it shows a picture or fails before one:
    * that decides the channel's dead mark (§5.8). */
   bool     watching;
   unsigned watch_gen, watch_idx;
   /* The channel on was chosen by us (start, new lineup), not the viewer:
    * if it's dead, move on to the next. Any button press ends this. */
   unsigned auto_tries;

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

void xs_tv_set_lineup(xs_tv_t *tv, xs_chan_t *chan)
{
   xs_chan_save(tv->ctx, &tv->chan);
   xs_chan_free(&tv->chan);
   tv->chan = *chan;
   memset(chan, 0, sizeof(*chan));
   tv->have_pending = false;
   tv->list         = false;
   tv->digits[0]    = '\0';
   tv->watching     = false;
   tv->top = tv->cursor = 0;
   xs_tv_start(tv);
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
   if (e->flags & XS_CH_UNPLAYABLE)
   {
      note(tv, e->flags & XS_CH_DRM ? "ENCRYPTED: CAN'T PLAY" : "CAN'T PLAY: AC-4 AUDIO");
      return;
   }
   xs_chan_set(tv->ctx, &tv->chan, idx);
   tv->tuned_tick = tv->ticks;
   xs_chan_source(&tv->chan, idx, &src);
   xs_present_set_name(tv->present, e->name);
   {
      char num[16];
      XS_INFO(tv->ctx, "channel %s: %s\n", xs_chan_number_str(&tv->chan, idx, num, sizeof(num)), e->name);
   }
   tv->watch_gen = xs_tuner_tune(tv->tuner, &src);
   tv->watch_idx = idx;
   tv->watching  = true;
}

void xs_tv_start(xs_tv_t *tv)
{
   unsigned idx;
   if (!xs_chan_count(&tv->chan))
      return;
   /* Start on a channel that isn't known to be dead. */
   idx = xs_chan_is_dead(&tv->chan, tv->chan.cur) || (xs_chan_entry(&tv->chan, tv->chan.cur)->flags & XS_CH_UNPLAYABLE)
       ? xs_chan_step(&tv->chan, tv->chan.cur, 1) : tv->chan.cur;
   tv->auto_tries = XS_AUTO_TRIES;
   tv->banner_idx = idx;
   tune(tv, idx);
}

/* A tune that shows a picture clears the channel's dead mark; one that
 * fails before any picture sets it, so channel up/down skips it for a day
 * (it stays in the list and reachable by number). */
static void watch_tune(xs_tv_t *tv)
{
   xs_tuner_status_t st;
   if (!tv->watching)
      return;
   xs_tuner_get_status(tv->tuner, &st);
   if (st.gen != tv->watch_gen)
      return;
   if (xs_present_has_picture(tv->present) || (st.state == XS_TUNER_PLAYING && !st.has_video && st.packets))
   {
      char num[16];
      if (xs_chan_is_dead(&tv->chan, tv->watch_idx))
         XS_INFO(tv->ctx, "channel %s works again\n", xs_chan_number_str(&tv->chan, tv->watch_idx, num, sizeof(num)));
      xs_chan_mark_dead(&tv->chan, tv->watch_idx, false);
      tv->watching = false;
   }
   else if (st.state == XS_TUNER_FAILED && !st.packets &&
            (xs_chan_entry(&tv->chan, tv->watch_idx)->flags & XS_CH_TUNER))
   {
      /* A local tuner: a failure is a busy tuner or a weak signal, not a
       * dead channel. HDHomeRun answers 503 when all its tuners are in use. */
      if (strstr(st.msg, "5XX"))
         note(tv, "NO FREE TUNER (ALL IN USE?)");
      tv->watching = false;
   }
   else if (st.state == XS_TUNER_FAILED && !st.packets)
   {
      char num[16];
      XS_INFO(tv->ctx, "channel %s is dead (%s): skipped for a day\n",
              xs_chan_number_str(&tv->chan, tv->watch_idx, num, sizeof(num)), st.msg);
      xs_chan_mark_dead(&tv->chan, tv->watch_idx, true);
      tv->watching = false;
      if (tv->auto_tries && xs_chan_count(&tv->chan) > 1)
      {
         unsigned next = xs_chan_step(&tv->chan, tv->watch_idx, 1);
         tv->auto_tries--;
         if (next != tv->watch_idx)
            tune(tv, next);
      }
   }
   else if (xs_present_has_picture(tv->present))
      tv->auto_tries = 0;
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
   unsigned idx, n = xs_chno_parse(tv->digits);
   char msg[64];
   if (n && xs_chan_find(&tv->chan, n, &idx))
   {
      tv->digits[0] = '\0';
      tune(tv, idx);
   }
   else
   {
      snprintf(msg, sizeof(msg), "NO CHANNEL %s", tv->digits);
      tv->digits[0] = '\0';
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

   if (!xs_chan_count(&tv->chan))
      return;   /* still loading */

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

   if ((act >= XS_ACT_DIGIT0 && act < XS_ACT_DIGIT0 + 10) || act == XS_ACT_DOT)
   {
      size_t n = strlen(tv->digits);
      bool dot = act == XS_ACT_DOT, exact;
      unsigned matches;
      if (n >= XS_MAX_DIGITS || (n == 0 && (dot || act == XS_ACT_DIGIT0)) ||
          (dot && strchr(tv->digits, '.')))
         return;
      tv->digits[n]     = dot ? '.' : (char)('0' + (act - XS_ACT_DIGIT0));
      tv->digits[n + 1] = '\0';
      tv->digits_tick   = tv->ticks;
      tv->have_pending  = false;
      /* Tune as soon as no longer channel number starts like this ("3.1"
       * with no 3.10..3.19; "24" with no 240..). */
      matches = xs_chan_match(&tv->chan, tv->digits, &exact);
      if (!matches || (exact && matches == 1))
         commit_digits(tv);
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
      case XS_ACT_FAV:
         xs_chan_toggle_fav(&tv->chan, tv->chan.cur);
         note(tv, xs_chan_is_fav(&tv->chan, tv->chan.cur) ? "* ADDED TO FAVOURITES" : "REMOVED FROM FAVOURITES");
         tv->banner_idx   = tv->chan.cur;
         tv->banner_until = after_ms(tv, XS_NOTE_MS);
         break;
      case XS_ACT_NEXT_FAV:
      {
         unsigned f = xs_chan_next_fav(&tv->chan, base);
         if (f == base && !xs_chan_is_fav(&tv->chan, base))
            note(tv, "NO FAVOURITES YET: X ADDS ONE");
         else
            move_pending(tv, f);
         break;
      }
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
   const xs_entry_t *e;
   /* The tuning / no-signal card already names the channel. */
   bool card = xs_present_card_showing(tv->present);

   memset(&ov, 0, sizeof(ov));
   ov.list_sel = ov.list_on = -1;
   if (!xs_chan_count(&tv->chan))
   {
      if (memcmp(&ov, &tv->ov, sizeof(ov)))
      {
         ov.version = tv->ov.version + 1;
         tv->ov = ov;
      }
      return;
   }
   e = xs_chan_entry(&tv->chan, shown);

   if (tv->digits[0])
   {
      ov.banner = ov.typing = true;
      snprintf(ov.number, sizeof(ov.number), "%s-", tv->digits);
   }
   else if (tv->ticks < tv->banner_until || tv->have_pending)
   {
      ov.banner = true;
      char num[16];
      snprintf(ov.number, sizeof(ov.number), "%s%s", xs_chan_is_fav(&tv->chan, shown) ? "*" : "",
               xs_chan_number_str(&tv->chan, shown, num, sizeof(num)));
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
      char num[16];
      snprintf(ov.info_line[0], sizeof(ov.info_line[0]), "%s  %.230s",
               xs_chan_number_str(&tv->chan, tv->chan.cur, num, sizeof(num)), c->name);
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
         char num[16];
         snprintf(ov.list_row[i], sizeof(ov.list_row[i]), "%6s %c %.130s%s", xs_chan_number_str(&tv->chan, idx, num, sizeof(num)),
                  xs_chan_is_fav(&tv->chan, idx) ? '*' : ' ', r->name,
                  xs_chan_is_dead(&tv->chan, idx) ? "  (NO SIGNAL)" : "");
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
   watch_tune(tv);
   n = xs_input_poll(&tv->in, input_cb, fps, acts, &zapping);
   if (n)
      tv->auto_tries = 0;   /* the viewer is choosing now */
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
   if ((tv->chan.dirty || tv->chan.dead.dirty || tv->chan.favs.dirty) &&
       tv->ticks >= tv->tuned_tick + (uint64_t)(XS_SAVE_AFTER_MS * fps / 1000.0))
      xs_chan_save(tv->ctx, &tv->chan);

   build_overlay(tv, pic_h);
}

const xs_overlay_t *xs_tv_overlay(const xs_tv_t *tv)
{
   return &tv->ov;
}
