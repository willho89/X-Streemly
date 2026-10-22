/*
 * X-Streemly — libretro network-video core
 * Copyright (C) 2026 X-Streemly contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bounded packet queue (see xs_pktq.h). Adapted from ffplay's PacketQueue
 * (fftools/ffplay.c @ n8.1.3): a linked list under one lock, consumers wait
 * on a condition variable.
 */
#include <stdlib.h>

#include <rthreads/rthreads.h>

#include "xs_pktq.h"

#define XS_PKTQ_POLL_US 50000

typedef struct node
{
   AVPacket    *pkt;
   unsigned     serial;
   int64_t      dur_us;
   struct node *next;
} node_t;

struct xs_pktq
{
   slock_t *lock;
   scond_t *cond;      /* signalled on put (getters) and on get/flush (putters) */
   node_t  *head, *tail;
   unsigned count;
   int64_t  dur_us;
   size_t   bytes;
   int64_t  max_dur_us;
   size_t   max_bytes;
   bool     aborted;
};

xs_pktq_t *xs_pktq_new(int64_t max_dur_us, size_t max_bytes)
{
   xs_pktq_t *q = calloc(1, sizeof(*q));
   if (!q)
      return NULL;
   q->lock = slock_new();
   q->cond = scond_new();
   q->max_dur_us = max_dur_us;
   q->max_bytes  = max_bytes;
   if (!q->lock || !q->cond)
   {
      xs_pktq_free(q);
      return NULL;
   }
   return q;
}

static void clear(xs_pktq_t *q)
{
   node_t *n = q->head;
   while (n)
   {
      node_t *next = n->next;
      av_packet_free(&n->pkt);
      free(n);
      n = next;
   }
   q->head = q->tail = NULL;
   q->count  = 0;
   q->dur_us = 0;
   q->bytes  = 0;
}

void xs_pktq_free(xs_pktq_t *q)
{
   if (!q)
      return;
   clear(q);
   if (q->cond)
      scond_free(q->cond);
   if (q->lock)
      slock_free(q->lock);
   free(q);
}

void xs_pktq_flush(xs_pktq_t *q)
{
   slock_lock(q->lock);
   clear(q);
   scond_broadcast(q->cond);
   slock_unlock(q->lock);
}

void xs_pktq_abort(xs_pktq_t *q)
{
   slock_lock(q->lock);
   q->aborted = true;
   scond_broadcast(q->cond);
   slock_unlock(q->lock);
}

bool xs_pktq_put(xs_pktq_t *q, AVPacket *pkt, unsigned serial, int64_t dur_us,
                 xs_abort_fn stop, void *arg)
{
   node_t *n = malloc(sizeof(*n));
   if (!n || !(n->pkt = av_packet_alloc()))
   {
      free(n);
      av_packet_unref(pkt);
      return false;
   }
   av_packet_move_ref(n->pkt, pkt);
   n->serial = serial;
   n->dur_us = dur_us > 0 ? dur_us : 0;
   n->next   = NULL;

   slock_lock(q->lock);
   /* An empty queue always takes a packet, however big. */
   while (q->count && (q->dur_us >= q->max_dur_us || q->bytes >= q->max_bytes))
   {
      bool give_up;
      scond_wait_timeout(q->cond, q->lock, XS_PKTQ_POLL_US);
      slock_unlock(q->lock);
      give_up = stop && stop(arg);   /* never call out under our lock */
      slock_lock(q->lock);
      if (give_up || q->aborted)
      {
         slock_unlock(q->lock);
         av_packet_free(&n->pkt);
         free(n);
         return false;
      }
   }
   if (q->tail)
      q->tail->next = n;
   else
      q->head = n;
   q->tail = n;
   q->count++;
   q->dur_us += n->dur_us;
   q->bytes  += (size_t)n->pkt->size;
   scond_broadcast(q->cond);
   slock_unlock(q->lock);
   return true;
}

int xs_pktq_get(xs_pktq_t *q, AVPacket *pkt, unsigned *serial, int64_t timeout_us)
{
   node_t *n;
   slock_lock(q->lock);
   if (!q->head && !q->aborted && timeout_us > 0)
      scond_wait_timeout(q->cond, q->lock, timeout_us);
   if (q->aborted)
   {
      slock_unlock(q->lock);
      return -1;
   }
   n = q->head;
   if (!n)
   {
      slock_unlock(q->lock);
      return 0;
   }
   q->head = n->next;
   if (!q->head)
      q->tail = NULL;
   q->count--;
   q->dur_us -= n->dur_us;
   q->bytes  -= (size_t)n->pkt->size;
   scond_broadcast(q->cond);
   slock_unlock(q->lock);

   av_packet_move_ref(pkt, n->pkt);
   *serial = n->serial;
   av_packet_free(&n->pkt);
   free(n);
   return 1;
}

void xs_pktq_stats(xs_pktq_t *q, unsigned *count, int64_t *dur_us, size_t *bytes)
{
   slock_lock(q->lock);
   if (count)
      *count = q->count;
   if (dur_us)
      *dur_us = q->dur_us;
   if (bytes)
      *bytes = q->bytes;
   slock_unlock(q->lock);
}
