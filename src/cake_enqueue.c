/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - Enqueue node
 * ip4-output / ip6-output feature arc nodes.
 *
 * Phase 2: Per-flow queuing, owner-thread handoff, ring buffers.
 *
 * Owner-thread model: each subscriber scheduler has one owner worker.
 * Packets arriving on the wrong worker are handed off via frame queue.
 * All scheduler state mutation happens exclusively on the owner thread.
 *
 * MULTIARCH: compiled with SIMD variants (AVX2, AVX-512, NEON).
 */

#include <vnet/vnet.h>
#include <vnet/feature/feature.h>

#include <osvbng_qos_sched/osvbng_qos_sched.h>

typedef enum
{
  CAKE_ENQUEUE_NEXT_HANDOFF,
  CAKE_ENQUEUE_N_STATIC_NEXT,
} cake_enqueue_static_next_t;

typedef struct
{
  u32 sw_if_index;
  u32 flow_idx;
  u8 enqueued;
  u8 scheduled;
  u8 handoff;
} cake_enqueue_trace_t;

static u8 *
format_cake_enqueue_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  cake_enqueue_trace_t *t = va_arg (*args, cake_enqueue_trace_t *);

  s = format (s, "CAKE-ENQUEUE: sw_if_index %u flow %u, %s",
	      t->sw_if_index, t->flow_idx,
	      t->handoff	? "handoff" :
	      t->scheduled ? "scheduled-passthrough" :
	      t->enqueued  ? "enqueued" :
			     "passthrough");
  return s;
}

static_always_inline void
cake_flow_evict (vlib_main_t *vm, cake_main_t *cm, cake_sched_t *cs,
		 cake_tin_t *tin, u32 slot)
{
  cake_flow_t *ef = &tin->flows[slot];

  cake_flow_discard (vm, cm, cs, tin, ef);

  if (ef->flow_state == CAKE_FLOW_SPARSE)
    {
      cake_flow_list_remove (&tin->new_flow_head, &tin->new_flow_tail,
			     tin->flows, slot);
      tin->sparse_flow_count--;
    }
  else if (ef->flow_state == CAKE_FLOW_BULK)
    {
      cake_flow_list_remove (&tin->old_flow_head, &tin->old_flow_tail,
			     tin->flows, slot);
      tin->bulk_flow_count--;
      if (ef->dst_host_idx < CAKE_HOSTS &&
	  tin->hosts[ef->dst_host_idx].bulk_flow_count > 0)
	tin->hosts[ef->dst_host_idx].bulk_flow_count--;
    }
  else if (ef->flow_state == CAKE_FLOW_DECAYING)
    {
      cake_flow_list_remove (&tin->decaying_flow_head,
			     &tin->decaying_flow_tail, tin->flows, slot);
    }

  if (ef->flow_state != CAKE_FLOW_NONE)
    tin->flow_count--;

  clib_memset (ef, 0, sizeof (*ef));
  ef->next = ~0;
  ef->prev = ~0;
}

static_always_inline uword
cake_enqueue_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
		     vlib_frame_t *frame, u8 is_ip4)
{
  cake_main_t *cm = &cake_main;
  u32 thread_index = vm->thread_index;
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;

  u32 pass_bi[VLIB_FRAME_SIZE];
  u16 pass_nexts[VLIB_FRAME_SIZE];
  u32 n_pass = 0;

  u32 handoff_bi[VLIB_FRAME_SIZE];
  u32 n_handoff = 0;

  u32 n_enqueued = 0;
  u32 n_dropped = 0;
  u32 n_agg_backpressure = 0;

  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b = bufs;
  vlib_get_buffers (vm, from, bufs, n_left);

  while (n_left > 0)
    {
      u32 bi0 = from[0];
      vlib_buffer_t *b0 = b[0];

      if (PREDICT_TRUE (n_left >= 5))
	{
	  vlib_prefetch_buffer_header (b[4], LOAD);
	  CLIB_PREFETCH (b[4]->data, CLIB_CACHE_LINE_BYTES, LOAD);
	}

      from++;
      b++;
      n_left--;

      if (PREDICT_FALSE (b0->flags & CAKE_BUFFER_F_SCHEDULED))
	{
	  b0->flags &= ~CAKE_BUFFER_F_SCHEDULED;
	  u32 next0;
	  vnet_feature_next (&next0, b0);
	  pass_bi[n_pass] = bi0;
	  pass_nexts[n_pass] = (u16) next0;
	  n_pass++;

	  if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	    {
	      cake_enqueue_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->sw_if_index = vnet_buffer (b0)->sw_if_index[VLIB_TX];
	      t->flow_idx = ~0;
	      t->enqueued = 0;
	      t->scheduled = 1;
	      t->handoff = 0;
	    }
	  continue;
	}

      u32 sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_TX];

      u32 si = ~0;
      if (sw_if_index0 < vec_len (cm->sched_index_by_sw_if_index))
	si = cm->sched_index_by_sw_if_index[sw_if_index0];

      /* No hint: which way this goes is a deployment property, not a
       * property of the code. Shaped interfaces take the other branch. */
      if (si == ~0)
	{
	  u32 next0;
	  vnet_feature_next (&next0, b0);
	  pass_bi[n_pass] = bi0;
	  pass_nexts[n_pass] = (u16) next0;
	  n_pass++;

	  if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	    {
	      cake_enqueue_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->sw_if_index = sw_if_index0;
	      t->flow_idx = ~0;
	      t->enqueued = 0;
	      t->scheduled = 0;
	      t->handoff = 0;
	    }
	  continue;
	}

      cake_sched_t *cs = pool_elt_at_index (cm->schedulers, si);

      /* Owner-thread check: CAS claim or handoff */
      if (PREDICT_TRUE (cs->owner_thread == thread_index))
	goto enqueue_local;

      if (cs->owner_thread == CAKE_OWNER_UNSET)
	{
	  u32 expected = CAKE_OWNER_UNSET;
	  if (__atomic_compare_exchange_n (&cs->owner_thread, &expected,
					   thread_index, 0, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE))
	    {
	      vlib_node_set_state (vm, cm->dequeue_node_index,
				   VLIB_NODE_STATE_POLLING);
	      goto enqueue_local;
	    }
	}

      /* Wrong worker — collect for handoff */
      handoff_bi[n_handoff++] = bi0;

      if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	{
	  cake_enqueue_trace_t *t =
	    vlib_add_trace (vm, node, b0, sizeof (*t));
	  t->sw_if_index = sw_if_index0;
	  t->flow_idx = ~0;
	  t->enqueued = 0;
	  t->scheduled = 0;
	  t->handoff = 1;
	}
      continue;

    enqueue_local:;
      u8 dscp = cake_dscp_from_buffer (b0, is_ip4);
      u8 tin_idx = cs->dscp_to_tin[dscp];
      if (PREDICT_FALSE (tin_idx >= cs->n_tins))
	tin_idx = 0;
      cake_tin_t *tin = &cs->tins[tin_idx];
      u32 pkt_len = vlib_buffer_length_in_chain (vm, b0);

      u32 tag = cake_hash_flow (b0, is_ip4);
      u32 set_base = (tag % CAKE_SET_COUNT) * CAKE_SET_WAYS;
      u32 evict_slot;
      u32 flow_idx = cake_flow_lookup (tin, tag, set_base, &evict_slot);

      if (PREDICT_FALSE (flow_idx == ~0))
	{
	  cake_flow_evict (vm, cm, cs, tin, evict_slot);
	  tin->flow_tags[evict_slot] = tag;
	  flow_idx = evict_slot;
	  vlib_node_increment_counter (vm, node->node_index,
				       CAKE_ERROR_FLOW_EVICTED, 1);
	}

      cake_flow_t *flow = &tin->flows[flow_idx];

      if (PREDICT_FALSE (cs->buffer_usage + pkt_len > cs->buffer_limit))
	{
	  cobalt_queue_full (flow, cs->target_us, cs->p_inc,
			     (u32) (vlib_time_now (vm) * 1e6));
	  vlib_buffer_free_one (vm, bi0);
	  cs->dropped_pkts++;
	  tin->drops++;
	  n_dropped++;

	  if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	    {
	      cake_enqueue_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->sw_if_index = sw_if_index0;
	      t->flow_idx = flow_idx;
	      t->enqueued = 0;
	      t->scheduled = 0;
	      t->handoff = 0;
	    }
	  continue;
	}

      if (PREDICT_FALSE (!cake_agg_admit_chain (cm, cs, pkt_len, thread_index)))
	{
	  cobalt_queue_full (flow, cs->target_us, cs->p_inc,
			     (u32) (vlib_time_now (vm) * 1e6));
	  vlib_buffer_free_one (vm, bi0);
	  cs->dropped_pkts++;
	  tin->drops++;
	  n_dropped++;
	  n_agg_backpressure++;
	  continue;
	}

      if (PREDICT_FALSE (flow->flow_state == CAKE_FLOW_NONE))
	{
	  cake_flow_ring_alloc (flow);
	  flow->flow_state = CAKE_FLOW_SPARSE;
	  flow->set_index = (u8) (set_base / CAKE_SET_WAYS);

	  u32 dst_hash = cake_dst_host_hash (b0, is_ip4);
	  flow->dst_host_idx = cake_host_lookup (tin, dst_hash);

	  flow->deficit = cake_quantum_for_flow (tin, flow);
	  cake_flow_list_prepend (&tin->new_flow_head, &tin->new_flow_tail,
				  tin->flows, flow_idx);
	  tin->flow_count++;
	  tin->sparse_flow_count++;
	}

      if (PREDICT_FALSE (cake_flow_queue_len (flow) >= CAKE_FLOW_RING_SIZE))
	{
	  /* Charged by aggregate admission above, and this buffer never
	   * reaches a queue. */
	  cake_agg_discharge (cm, cs, pkt_len);
	  cobalt_queue_full (flow, cs->target_us, cs->p_inc,
			     (u32) (vlib_time_now (vm) * 1e6));
	  vlib_buffer_free_one (vm, bi0);
	  cs->dropped_pkts++;
	  tin->drops++;
	  n_dropped++;
	  continue;
	}

      cake_buffer_enqueue_time (b0) = (u32) (vlib_time_now (vm) * 1e6);
      flow->ring[flow->tail & CAKE_FLOW_RING_MASK] = bi0;
      flow->tail++;

      flow->backlog_bytes += pkt_len;
      cs->buffer_usage += pkt_len;
      cs->queued_buffers++;
      cs->enqueued_pkts++;
      cs->enqueued_bytes += pkt_len;
      tin->packets++;
      tin->bytes += pkt_len;
      n_enqueued++;

      if (flow->flow_state == CAKE_FLOW_SPARSE &&
	  cake_flow_queue_len (flow) > 1)
	{
	  flow->flow_state = CAKE_FLOW_BULK;
	  cake_flow_list_remove (&tin->new_flow_head, &tin->new_flow_tail,
				 tin->flows, flow_idx);
	  cake_flow_list_append_tail (&tin->old_flow_head, &tin->old_flow_tail,
				      tin->flows, flow_idx);
	  tin->sparse_flow_count--;
	  tin->bulk_flow_count++;
	  if (flow->dst_host_idx < CAKE_HOSTS)
	    tin->hosts[flow->dst_host_idx].bulk_flow_count++;
	}
      else if (flow->flow_state == CAKE_FLOW_DECAYING)
	{
	  flow->flow_state = CAKE_FLOW_BULK;
	  cake_flow_list_remove (&tin->decaying_flow_head,
				 &tin->decaying_flow_tail, tin->flows,
				 flow_idx);
	  cake_flow_list_append_tail (&tin->old_flow_head, &tin->old_flow_tail,
				      tin->flows, flow_idx);
	  tin->bulk_flow_count++;
	  if (flow->dst_host_idx < CAKE_HOSTS)
	    tin->hosts[flow->dst_host_idx].bulk_flow_count++;
	}

      /* 0->1 active: this scheduler now competes for its parent's rate, so
       * its weight joins the parent's active sum. Idempotent - the common
       * case is an already-active scheduler and returns without touching the
       * parent. */
      cake_sched_drr_activate (cm, cs);

      if (!clib_bitmap_get (cm->per_thread[thread_index].active_bitmap,
			    cs->sched_index))
	{
	  cm->per_thread[thread_index].active_bitmap = clib_bitmap_set (
	    cm->per_thread[thread_index].active_bitmap, cs->sched_index, 1);
	}

      if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	{
	  cake_enqueue_trace_t *t =
	    vlib_add_trace (vm, node, b0, sizeof (*t));
	  t->sw_if_index = sw_if_index0;
	  t->flow_idx = flow_idx;
	  t->enqueued = 1;
	  t->scheduled = 0;
	  t->handoff = 0;
	}
    }

  if (n_pass > 0)
    vlib_buffer_enqueue_to_next (vm, node, pass_bi, pass_nexts, n_pass);

  if (n_handoff > 0)
    {
      u32 handoff_next = is_ip4
	? vlib_node_add_next (vm, node->node_index,
			      ip4_cake_handoff_node.index)
	: vlib_node_add_next (vm, node->node_index,
			      ip6_cake_handoff_node.index);

      vlib_buffer_enqueue_to_single_next (vm, node, handoff_bi, handoff_next,
					  n_handoff);

      vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_HANDOFF,
				   n_handoff);
    }

  vlib_node_increment_counter (vm, node->node_index, CAKE_ERROR_ENQUEUED,
			       n_enqueued);
  vlib_node_increment_counter (vm, node->node_index,
			       CAKE_ERROR_DROPPED_OVERFLOW, n_dropped);
  if (n_agg_backpressure)
    vlib_node_increment_counter (vm, node->node_index,
				 CAKE_ERROR_AGG_BACKPRESSURE,
				 n_agg_backpressure);

  return frame->n_vectors;
}

VLIB_NODE_FN (ip4_cake_enqueue_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return cake_enqueue_inline (vm, node, frame, 1);
}

VLIB_NODE_FN (ip6_cake_enqueue_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return cake_enqueue_inline (vm, node, frame, 0);
}

VLIB_REGISTER_NODE (ip4_cake_enqueue_node) = {
  .name = "ip4-cake-enqueue",
  .vector_size = sizeof (u32),
  .format_trace = format_cake_enqueue_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = CAKE_N_ERROR,
  .error_strings = cake_error_strings,
  .n_next_nodes = 0,
};

VLIB_REGISTER_NODE (ip6_cake_enqueue_node) = {
  .name = "ip6-cake-enqueue",
  .vector_size = sizeof (u32),
  .format_trace = format_cake_enqueue_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = CAKE_N_ERROR,
  .error_strings = cake_error_strings,
  .n_next_nodes = 0,
};

VNET_FEATURE_INIT (ip4_cake_enqueue_feature, static) = {
  .arc_name = "ip4-output",
  .node_name = "ip4-cake-enqueue",
};

VNET_FEATURE_INIT (ip6_cake_enqueue_feature, static) = {
  .arc_name = "ip6-output",
  .node_name = "ip6-cake-enqueue",
};

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
