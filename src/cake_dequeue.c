/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - Dequeue node
 * VLIB_NODE_TYPE_INPUT polling node.
 *
 * Phase 3: DRR scheduling + COBALT AQM (CoDel + BLUE).
 *
 * MULTIARCH: compiled with SIMD variants (AVX2, AVX-512, NEON).
 */

#include <vnet/vnet.h>
#include <vnet/feature/feature.h>

#include <osvbng_qos_sched/osvbng_qos_sched.h>

typedef enum
{
  CAKE_DEQUEUE_NEXT_REINJECT_IP4,
  CAKE_DEQUEUE_NEXT_REINJECT_IP6,
  CAKE_DEQUEUE_N_NEXT,
} cake_dequeue_next_t;

typedef struct
{
  u32 sw_if_index;
  u32 n_dequeued;
  u32 flow_idx;
} cake_dequeue_trace_t;

/* GATE_CLOSED and DRR_BLOCKED are distinct from EMPTY because they make no
 * progress: retrying the same flow before now_ns is re-sampled would spin.
 * They are distinct from each other because they mean different things to an
 * operator - the parent is at its configured rate, versus this child has
 * spent its share of it. */
typedef enum
{
  CAKE_DEQ_EMPTY = 0,
  CAKE_DEQ_SENT,
  CAKE_DEQ_DROPPED,
  CAKE_DEQ_GATE_CLOSED,
  CAKE_DEQ_DRR_BLOCKED,
} cake_deq_result_t;

static u8 *
format_cake_dequeue_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  cake_dequeue_trace_t *t = va_arg (*args, cake_dequeue_trace_t *);

  s = format (s, "CAKE-DEQUEUE: sw_if_index %u flow %u dequeued %u",
	      t->sw_if_index, t->flow_idx, t->n_dequeued);
  return s;
}

static_always_inline void
cake_flow_reclaim (vlib_main_t *vm, cake_tin_t *tin, cake_sched_t *cs,
		   u32 flow_idx, u32 *list_head, u32 *list_tail, u32 now_us)
{
  cake_flow_t *f = &tin->flows[flow_idx];

  cobalt_queue_empty (f, cs->target_us, cs->p_dec, cs->interval_us, now_us);

  cake_flow_list_remove (list_head, list_tail, tin->flows, flow_idx);
  /* Callers check the queue is empty, so there is no credit to release. */
  cake_flow_ring_free (vm, f);

  if (f->flow_state == CAKE_FLOW_SPARSE)
    tin->sparse_flow_count--;
  else if (f->flow_state == CAKE_FLOW_BULK)
    {
      tin->bulk_flow_count--;
      if (f->dst_host_idx < CAKE_HOSTS)
	{
	  if (tin->hosts[f->dst_host_idx].bulk_flow_count > 0)
	    tin->hosts[f->dst_host_idx].bulk_flow_count--;
	}
    }

  tin->flow_tags[flow_idx] = 0;
  clib_memset (f, 0, sizeof (*f));
  f->next = ~0;
  f->prev = ~0;
  tin->flow_count--;
}

static_always_inline u32
cake_select_flow (cake_tin_t *tin)
{
  if (tin->new_flow_head != ~0)
    return tin->new_flow_head;
  if (tin->old_flow_head != ~0)
    return tin->old_flow_head;
  if (tin->decaying_flow_head != ~0)
    return tin->decaying_flow_head;
  return ~0;
}

static_always_inline cake_deq_result_t
cake_dequeue_one (vlib_main_t *vm, vlib_node_runtime_t *node,
		  cake_main_t *cm, cake_sched_t *cs, cake_tin_t *tin,
		  cake_flow_t *flow, u32 now_us, u64 now_ns,
		  u32 *out_bi, u16 *out_next, u32 *n_aqm_drops,
		  u32 *n_ecn_marks, u32 *random_seed)
{
  if (cake_flow_queue_len (flow) == 0)
    return CAKE_DEQ_EMPTY;

  u32 bi = flow->ring[flow->head & CAKE_FLOW_RING_MASK];

  /* Queued packets have long since fallen out of cache and we read this one's
   * header immediately below, so warm the next one in this flow first rather
   * than issuing the prefetch behind that stall. */
  if (cake_flow_queue_len (flow) > 1)
    {
      vlib_buffer_t *nb = vlib_get_buffer (
	vm, flow->ring[(flow->head + 1) & CAKE_FLOW_RING_MASK]);
      vlib_prefetch_buffer_header (nb, LOAD);
      CLIB_PREFETCH (nb->data, CLIB_CACHE_LINE_BYTES, LOAD);
    }

  vlib_buffer_t *b = vlib_get_buffer (vm, bi);
  u32 pkt_len = vlib_buffer_length_in_chain (vm, b);
  u32 adj_len = cake_overhead_adjust (cs, pkt_len);

  /* DRR eligibility against the immediate parent, before COBALT and before
   * the ECN mark. Its only inputs are the head packet's length and
   * owner-local state, so a blocked visit mutates no AQM state at all.
   *
   * This placement is load-bearing. A DRR-blocked child retries every
   * polling dispatch - three orders of magnitude more often than rounds turn
   * - and checked after the AQM like the rate gate below, each retry would
   * re-run cobalt_should_drop on the same head packet, escalate codel_count,
   * inflate ecn_marks and over-drop once the child unblocks. sch_cake
   * likewise does not run its AQM while the shaper holds the qdisc closed;
   * AQM action is deferred at most one round. */
  cake_drr_admit_t drr = cake_sched_drr_admit (cm, cs, now_ns);
  if (PREDICT_FALSE (drr == CAKE_DRR_BLOCKED))
    {
      cs->drr_blocked++;
      return CAKE_DEQ_DRR_BLOCKED;
    }

  flow->head++;

  u32 enqueue_us = cake_buffer_enqueue_time (b);
  u32 sojourn_us = now_us - enqueue_us;

  u8 ecn_capable = 0;
  u8 *ip_hdr = cake_l3_header (b);
  u8 is_v4 = (ip_hdr[0] >> 4) == 4;

  if (is_v4)
    ecn_capable =
      (((ip4_header_t *) ip_hdr)->tos & IP_PACKET_TC_FIELD_ECN_MASK) !=
      IP_ECN_NON_ECN;
  else
    {
      u32 vtcfl = clib_net_to_host_u32 (
	((ip6_header_t *) ip_hdr)->ip_version_traffic_class_and_flow_label);
      ecn_capable = ((vtcfl >> 20) & 0x03) != 0;
    }

  u8 ecn_marked = 0;
  u8 should_drop = cobalt_should_drop (
    flow, cs, sojourn_us, now_us,
    tin->bulk_flow_count > 0 ? tin->bulk_flow_count : 1, ecn_capable,
    &ecn_marked, random_seed);

  if (PREDICT_FALSE (should_drop))
    {
      flow->backlog_bytes -= pkt_len;
      cs->buffer_usage -= pkt_len;
      cs->queued_buffers--;
      cs->dropped_pkts++;
      tin->drops++;
      (*n_aqm_drops)++;
      cake_agg_discharge (cm, cs, pkt_len);
      vlib_buffer_free_one (vm, bi);
      return CAKE_DEQ_DROPPED;
    }

  if (PREDICT_FALSE (ecn_marked))
    {
      cake_ecn_mark (b);
      tin->ecn_marks++;
      (*n_ecn_marks)++;
    }

  /* The parent chain's rate gates and, where there is a tier above the
   * immediate parent, that tier's own DRR reserve. */
  cake_parent_result_t pr =
    cake_agg_dequeue_gate (cm, cs, adj_len, now_ns, vm->thread_index);

  if (PREDICT_FALSE (pr != CAKE_PARENT_OK))
    {
      flow->head--;
      if (pr == CAKE_PARENT_DRR_BLOCKED)
	{
	  cs->drr_blocked++;
	  return CAKE_DEQ_DRR_BLOCKED;
	}
      cs->parent_blocked++;
      return CAKE_DEQ_GATE_CLOSED;
    }

  cs->global_shaper_time_ns +=
    cake_cost_ns (adj_len, cs->rate_ns_per_byte_scaled);
  u64 max_shaper = now_ns + (u64) 150000000;
  if (cs->global_shaper_time_ns > max_shaper)
    cs->global_shaper_time_ns = max_shaper;

  flow->backlog_bytes -= pkt_len;
  flow->deficit -= (i32) adj_len;
  cs->buffer_usage -= pkt_len;
  cs->queued_buffers--;
  cs->dequeued_pkts++;
  cs->dequeued_bytes += pkt_len;
  cake_agg_discharge (cm, cs, pkt_len);

  /* The send has cleared every gate, so charge the share it consumed. Escape
   * admissions are charged like any other; only CAKE_DRR_UNARBITRATED goes
   * uncharged. */
  if (PREDICT_TRUE (drr == CAKE_DRR_ADMIT))
    cake_drr_local_charge (&cs->drr, adj_len);

  b->flags |= CAKE_BUFFER_F_SCHEDULED;
  u32 sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_TX];
  u32 dummy;

  if (is_v4)
    {
      vnet_feature_arc_start (cm->ip4_output_arc_index, sw_if_index, &dummy,
			      b);
      *out_next = CAKE_DEQUEUE_NEXT_REINJECT_IP4;
    }
  else
    {
      vnet_feature_arc_start (cm->ip6_output_arc_index, sw_if_index, &dummy,
			      b);
      *out_next = CAKE_DEQUEUE_NEXT_REINJECT_IP6;
    }

  *out_bi = bi;
  return CAKE_DEQ_SENT;
}

VLIB_NODE_FN (cake_dequeue_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  cake_main_t *cm = &cake_main;
  u32 thread_index = vm->thread_index;

  if (PREDICT_FALSE (thread_index >= vec_len (cm->per_thread)))
    return 0;

  cake_per_thread_t *pt = vec_elt_at_index (cm->per_thread, thread_index);
  if (PREDICT_FALSE (clib_bitmap_is_zero (pt->active_bitmap)))
    return 0;

  f64 now = vlib_time_now (vm);
  u64 now_ns = (u64) (now * 1e9);
  u32 now_us = (u32) (now * 1e6);
  u32 budget = VLIB_FRAME_SIZE;

  u32 out_bi[VLIB_FRAME_SIZE];
  u16 out_nexts[VLIB_FRAME_SIZE];
  u32 n_out = 0;

  u32 deactivate[VLIB_FRAME_SIZE];
  u32 n_deactivate = 0;

  u32 n_aqm_drops = 0;
  u32 n_ecn_marks = 0;
  u32 n_drr_blocked = 0;
  u32 n_parent_blocked = 0;
  u32 n_agg_shaped = 0;

  /* Bitmap order is ascending pool index and stable, which before DRR made a
   * shared aggregate gate a strict priority: the first scheduler polled took
   * the credit and the rest found it shut, on every dispatch (issue #8).
   *
   * The order is deliberately left alone. A child can now only take its
   * quantum whichever order it is reached in, so starting at the bottom costs
   * nothing measurable: 0.63% spread over 30 s for four equal children under
   * an 8 Mbit/s aggregate, and +0.57 points worst error at weights 1:2:4:8.
   * Rotating the start (PR #9) improves those to 0.28% and +0.41 points by
   * spreading the work-conserving escape, which is unarbitrated and does
   * favour whoever is reached first. That is a refinement, not a fix, and it
   * is not this spec's to make. */
  uword si;
  clib_bitmap_foreach (si, pt->active_bitmap)
    {
      if (budget == 0 || n_deactivate >= VLIB_FRAME_SIZE - 1)
	break;

      if (PREDICT_FALSE (pool_is_free_index (cm->schedulers, si)))
	{
	  deactivate[n_deactivate++] = si;
	  continue;
	}

      cake_sched_t *cs = pool_elt_at_index (cm->schedulers, si);

      if (PREDICT_FALSE (cs->owner_thread != thread_index))
	{
	  deactivate[n_deactivate++] = si;
	  continue;
	}

      if (now_ns < cs->global_shaper_time_ns)
	continue;

      u32 sched_dequeued = 0;
      u32 last_flow_idx = ~0;

      while (budget > 0)
	{
	  cake_tin_t *tin = NULL;
	  for (i32 t = cs->n_tins - 1; t >= 0; t--)
	    {
	      cake_tin_t *candidate = &cs->tins[t];
	      if (candidate->sparse_flow_count + candidate->bulk_flow_count >
		      0 ||
		  candidate->decaying_flow_head != ~0)
		{
		  tin = candidate;
		  break;
		}
	    }

	  if (!tin)
	    {
	      /* Empty-detect on a live, owned scheduler: this is one of the
	       * two weight-bearing deactivations. */
	      cake_sched_drr_deactivate (cm, cs);
	      if (n_deactivate < VLIB_FRAME_SIZE)
		deactivate[n_deactivate++] = si;
	      break;
	    }

	  u32 flow_idx = cake_select_flow (tin);
	  if (flow_idx == ~0)
	    {
	      cake_sched_drr_deactivate (cm, cs);
	      if (n_deactivate < VLIB_FRAME_SIZE)
		deactivate[n_deactivate++] = si;
	      break;
	    }

	  cake_flow_t *flow = &tin->flows[flow_idx];
	  last_flow_idx = flow_idx;

	  if (flow->flow_state == CAKE_FLOW_SPARSE)
	    {
	      if (cake_flow_queue_len (flow) == 0)
		{
		  cake_flow_reclaim (vm, tin, cs, flow_idx,
				     &tin->new_flow_head,
				     &tin->new_flow_tail, now_us);
		  continue;
		}

	      cake_deq_result_t r = cake_dequeue_one (
		vm, node, cm, cs, tin, flow, now_us, now_ns,
		&out_bi[n_out], &out_nexts[n_out], &n_aqm_drops,
		&n_ecn_marks, &pt->random_seed);

	      if (r == CAKE_DEQ_SENT)
		{
		  n_out++;
		  budget--;
		  sched_dequeued++;
		}

	      if (cake_flow_queue_len (flow) == 0)
		cake_flow_reclaim (vm, tin, cs, flow_idx,
				   &tin->new_flow_head,
				   &tin->new_flow_tail, now_us);

	      /* Nothing advanced, so retrying before the next dispatch
	       * re-samples now_ns would spin. */
	      if (r == CAKE_DEQ_GATE_CLOSED)
		{
		  n_parent_blocked++;
		  break;
		}
	      if (r == CAKE_DEQ_DRR_BLOCKED)
		{
		  n_drr_blocked++;
		  break;
		}

	      if (now_ns < cs->global_shaper_time_ns)
		break;
	      continue;
	    }

	  if (flow->deficit <= 0)
	    flow->deficit += cake_quantum_for_flow (tin, flow);

	  while (flow->deficit > 0 && budget > 0)
	    {
	      if (cake_flow_queue_len (flow) == 0)
		{
		  if (flow->flow_state == CAKE_FLOW_BULK)
		    {
		      if (flow->dst_host_idx < CAKE_HOSTS &&
			  tin->hosts[flow->dst_host_idx].bulk_flow_count > 0)
			tin->hosts[flow->dst_host_idx].bulk_flow_count--;

		      flow->flow_state = CAKE_FLOW_DECAYING;
		      cake_flow_list_remove (&tin->old_flow_head,
					     &tin->old_flow_tail, tin->flows,
					     flow_idx);
		      cake_flow_list_append_tail (&tin->decaying_flow_head,
						  &tin->decaying_flow_tail,
						  tin->flows, flow_idx);
		      tin->bulk_flow_count--;
		    }
		  else if (flow->flow_state == CAKE_FLOW_DECAYING)
		    {
		      cake_flow_reclaim (vm, tin, cs, flow_idx,
					 &tin->decaying_flow_head,
					 &tin->decaying_flow_tail, now_us);
		    }
		  break;
		}

	      cake_deq_result_t r = cake_dequeue_one (
		vm, node, cm, cs, tin, flow, now_us, now_ns,
		&out_bi[n_out], &out_nexts[n_out], &n_aqm_drops,
		&n_ecn_marks, &pt->random_seed);

	      if (r == CAKE_DEQ_SENT)
		{
		  n_out++;
		  budget--;
		  sched_dequeued++;
		}

	      /* deficit and budget are unchanged, so this loop would spin on
	       * the same closed gate or spent quantum. */
	      if (r == CAKE_DEQ_GATE_CLOSED)
		{
		  n_parent_blocked++;
		  goto shaper_exhausted;
		}
	      if (r == CAKE_DEQ_DRR_BLOCKED)
		{
		  n_drr_blocked++;
		  goto shaper_exhausted;
		}

	      if (now_ns < cs->global_shaper_time_ns)
		goto shaper_exhausted;
	    }

	  if (flow->deficit <= 0 && flow->flow_state == CAKE_FLOW_BULK &&
	      cake_flow_queue_len (flow) > 0)
	    {
	      cake_flow_list_remove (&tin->old_flow_head, &tin->old_flow_tail,
				     tin->flows, flow_idx);
	      cake_flow_list_append_tail (&tin->old_flow_head,
					  &tin->old_flow_tail, tin->flows,
					  flow_idx);
	      flow->deficit = 0;
	    }

	  continue;

	shaper_exhausted:
	  break;
	}

      if (cs->aggregate_index != ~0)
	n_agg_shaped += sched_dequeued;

      if (sched_dequeued > 0
	  && PREDICT_FALSE (node->flags & VLIB_NODE_FLAG_TRACE))
	{
	  vlib_buffer_t *b0 = vlib_get_buffer (vm, out_bi[n_out - 1]);
	  cake_dequeue_trace_t *t =
	    vlib_add_trace (vm, node, b0, sizeof (*t));
	  t->sw_if_index = cs->sw_if_index;
	  t->n_dequeued = sched_dequeued;
	  t->flow_idx = last_flow_idx;
	}
    }

  for (u32 i = 0; i < n_deactivate; i++)
    pt->active_bitmap =
      clib_bitmap_set (pt->active_bitmap, deactivate[i], 0);

  if (n_out > 0)
    vlib_buffer_enqueue_to_next (vm, node, out_bi, out_nexts, n_out);

  vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_DEQUEUED,
			       n_out);

  if (n_aqm_drops)
    vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_DROPPED_AQM,
				 n_aqm_drops);
  if (n_ecn_marks)
    vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_ECN_MARKED,
				 n_ecn_marks);
  if (n_drr_blocked)
    vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_DRR_BLOCKED,
				 n_drr_blocked);
  if (n_parent_blocked)
    vlib_node_increment_counter (vm, node->node_index,
				 CAKE_ERROR_PARENT_BLOCKED, n_parent_blocked);
  if (n_agg_shaped)
    vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_AGG_SHAPED,
				 n_agg_shaped);

  return n_out;
}

VLIB_REGISTER_NODE (cake_dequeue_node) = {
  .name = "cake-dequeue",
  .vector_size = sizeof (u32),
  .format_trace = format_cake_dequeue_trace,
  .type = VLIB_NODE_TYPE_INPUT,
  .state = VLIB_NODE_STATE_DISABLED,
  .n_errors = CAKE_N_ERROR,
  .error_strings = cake_error_strings,
  .n_next_nodes = CAKE_DEQUEUE_N_NEXT,
  .next_nodes = {
    [CAKE_DEQUEUE_NEXT_REINJECT_IP4] = "ip4-cake-enqueue",
    [CAKE_DEQUEUE_NEXT_REINJECT_IP6] = "ip6-cake-enqueue",
  },
};

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
