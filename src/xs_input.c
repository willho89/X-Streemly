/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Input (see xs_input.h). fire() follows DeeVee src/dv_input.c.
 */
#include <string.h>

#include "xs_input.h"

#define REPEAT_DELAY_MS  400
#define REPEAT_PERIOD_MS 120

void xs_input_init(xs_input_t *in, bool bitmasks)
{
   memset(in, 0, sizeof(*in));
   in->bitmasks = bitmasks;
}

void xs_input_set_descriptors(retro_environment_t environ_cb)
{
   static const struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Channel Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Channel Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Group / Page Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Previous Group / Page Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Info / Tune" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Last Channel / Close" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Channel List" },
      { 0, 0, 0, 0, NULL },
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

static uint32_t read_buttons(xs_input_t *in, retro_input_state_t cb)
{
   uint32_t bits = 0;
   unsigned id;
   if (in->bitmasks)
      return (uint32_t)cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   for (id = 0; id < 16; id++)
      if (cb(0, RETRO_DEVICE_JOYPAD, 0, id))
         bits |= 1u << id;
   return bits;
}

/* Digits 0-9 (top row or keypad) as bits 0-9, Enter as bit 10. */
static uint32_t read_keys(retro_input_state_t cb)
{
   uint32_t bits = 0;
   unsigned d;
   for (d = 0; d < 10; d++)
      if (cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_0 + d) || cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP0 + d))
         bits |= 1u << d;
   if (cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RETURN) || cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP_ENTER))
      bits |= 1u << 10;
   return bits;
}

/* True on press, and periodically while held (auto-repeat). */
static bool fire(xs_input_t *in, uint32_t bits, unsigned id, bool repeat, double fps)
{
   unsigned delay  = (unsigned)(REPEAT_DELAY_MS * fps / 1000.0 + 0.5);
   unsigned period = (unsigned)(REPEAT_PERIOD_MS * fps / 1000.0 + 0.5);
   bool down = (bits >> id) & 1;
   bool pressed = down && !((in->prev >> id) & 1);

   if (!down)
   {
      in->held[id] = 0;
      return false;
   }
   in->held[id]++;
   if (pressed)
      return true;
   return repeat && period && in->held[id] > delay && (in->held[id] - delay) % period == 0;
}

unsigned xs_input_poll(xs_input_t *in, retro_input_state_t state_cb, double fps,
                       unsigned *acts, bool *zapping)
{
   static const struct { unsigned id; unsigned act; bool repeat; } map[] = {
      { RETRO_DEVICE_ID_JOYPAD_UP,     XS_ACT_UP,         true  },
      { RETRO_DEVICE_ID_JOYPAD_DOWN,   XS_ACT_DOWN,       true  },
      { RETRO_DEVICE_ID_JOYPAD_R,      XS_ACT_NEXT_GROUP, true  },
      { RETRO_DEVICE_ID_JOYPAD_L,      XS_ACT_PREV_GROUP, true  },
      { RETRO_DEVICE_ID_JOYPAD_A,      XS_ACT_OK,         false },
      { RETRO_DEVICE_ID_JOYPAD_B,      XS_ACT_BACK,       false },
      { RETRO_DEVICE_ID_JOYPAD_SELECT, XS_ACT_LIST,       false },
   };
   uint32_t bits, keys;
   unsigned i, n = 0;

   *zapping = false;
   if (!state_cb)
      return 0;
   bits = read_buttons(in, state_cb);
   keys = read_keys(state_cb);
   for (i = 0; i < sizeof(map) / sizeof(map[0]) && n < XS_MAX_ACTIONS; i++)
   {
      if (fire(in, bits, map[i].id, map[i].repeat, fps))
         acts[n++] = map[i].act;
      if (map[i].repeat && ((bits >> map[i].id) & 1))
         *zapping = true;
   }
   for (i = 0; i < 11 && n < XS_MAX_ACTIONS; i++)
      if (((keys >> i) & 1) && !((in->keys_prev >> i) & 1))
         acts[n++] = i < 10 ? XS_ACT_DIGIT0 + i : XS_ACT_ENTER;
   in->prev = bits;
   in->keys_prev = keys;
   return n;
}
