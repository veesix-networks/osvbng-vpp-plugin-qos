/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin
 * CAKE-equivalent per-subscriber traffic scheduling.
 *
 * Algorithms derived from the Linux CAKE qdisc (sch_cake.c).
 * Original authors: Dave Taht, Jonathan Morton, Toke Hoiland-Jorgensen,
 * Sebastian Moeller, Kevin Darbyshire-Bryant, Ryan Mounce.
 *
 * Phase 4: DiffServ tins.
 */

#ifndef __included_osvbng_qos_sched_h__
#define __included_osvbng_qos_sched_h__

#include <vnet/plugin/plugin.h>
#include <vnet/feature/feature.h>
#include <vppinfra/error.h>
#include <vppinfra/pool.h>
#include <vppinfra/vec.h>
#include <vppinfra/xxhash.h>
#include <vppinfra/atomics.h>
#include <vppinfra/random.h>
#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vnet/buffer.h>
#include <vlib/vlib.h>

#include <osvbng_qos_sched/cake_drr.h>
#include <osvbng_qos_sched/cake_shaper.h>

#define CAKE_BUFFER_F_SCHEDULED VNET_BUFFER_F_AVAIL1

#define CAKE_QUEUES	  1024
#define CAKE_SET_WAYS	  8
#define CAKE_SET_COUNT	  (CAKE_QUEUES / CAKE_SET_WAYS)
#define CAKE_MAX_TINS	  8

#define CAKE_FLOW_NONE	   0
#define CAKE_FLOW_SPARSE   1
#define CAKE_FLOW_BULK	   2
#define CAKE_FLOW_DECAYING 3

#define CAKE_QUANTUM_DEFAULT	1514
#define CAKE_FLOW_RING_SIZE	128
#define CAKE_FLOW_RING_MASK	(CAKE_FLOW_RING_SIZE - 1)

#define CAKE_OWNER_UNSET ((u32) ~0)

#define CAKE_TARGET_US_DEFAULT	  5000
#define CAKE_INTERVAL_US_DEFAULT  100000
#define CAKE_REC_INV_SQRT_CACHE	  16

/* Default idle credit ceiling for an aggregate shaper, at the floor of the
 * 10-125 ms range commodity shapers use. Per-aggregate since the _v2 API. Burst credit is DRR-unarbitrated: under
 * sustained sub-saturation virtual time pins at now - CAKE_AGG_BURST_NS, the
 * work-conserving escape stays continuously open, and the first
 * burst x rate bytes of every saturation onset are admitted in walk order.
 * A wider window buys back the very starvation this tier exists to remove,
 * and it is also the post-idle line-rate burst released into the access
 * network. */
#define CAKE_AGG_BURST_NS	  (10ULL * 1000000ULL)
#define CAKE_AGG_BURST_NS_MIN	  (10ULL * 1000000ULL)
#define CAKE_AGG_BURST_NS_MAX	  (150ULL * 1000000ULL)

/* An operator multiplier on the rate-derived weight. Bounded because the
 * quantum's 128-bit intermediate is sized against it, and because no
 * residential BNG case needs more. */
#define CAKE_WEIGHT_MIN 1
#define CAKE_WEIGHT_MAX 256

static_always_inline u64
cake_effective_weight (u64 rate_bytes_per_sec, u32 weight)
{
  u64 w = weight ? weight : 1;
  u64 rate = rate_bytes_per_sec ? rate_bytes_per_sec : 1;
  return rate * w;
}

#define CAKE_HOSTS	  256
#define CAKE_HOSTS_MASK	  (CAKE_HOSTS - 1)

#define CAKE_TIN_MODE_BESTEFFORT 0
#define CAKE_TIN_MODE_DIFFSERV3	 1
#define CAKE_TIN_MODE_DIFFSERV4	 2
#define CAKE_TIN_MODE_DIFFSERV8	 3

#define cake_buffer_enqueue_time(b) (vnet_buffer2 (b)->unused[0])

typedef enum
{
#define cake_error(n, s) CAKE_ERROR_##n,
#include <osvbng_qos_sched/osvbng_qos_sched_error.def>
#undef cake_error
  CAKE_N_ERROR,
} cake_error_t;

extern char *cake_error_strings[];
extern u32 cobalt_rec_inv_sqrt_cache[CAKE_REC_INV_SQRT_CACHE];
extern u16 cake_quantum_div[CAKE_QUEUES + 1];

extern const u8 cake_dscp_besteffort[64];
extern const u8 cake_dscp_diffserv3[64];
extern const u8 cake_dscp_diffserv4[64];
extern const u8 cake_dscp_diffserv8[64];

typedef struct
{
  u32 *ring;
  u32 head;
  u32 tail;

  i32 deficit;
  u32 next;
  u32 prev;

  u32 backlog_bytes;
  u8 flow_state;
  u8 set_index;
  u8 dropping;
  u8 _pad;

  u32 codel_count;
  u32 rec_inv_sqrt;
  u32 drop_next_us;

  u32 p_drop;
  u32 blue_timer_us;

  u16 dst_host_idx;
  u16 _pad2;
} cake_flow_t;

typedef struct
{
  u32 ip_hash;
  u16 bulk_flow_count;
} cake_host_t;

typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  cake_flow_t *flows;
  u32 *flow_tags;
  u32 flow_count;

  u32 new_flow_head;
  u32 new_flow_tail;
  u32 old_flow_head;
  u32 old_flow_tail;
  u32 decaying_flow_head;
  u32 decaying_flow_tail;

  u32 quantum;
  i32 tin_deficit;
  u32 tin_quantum;

  u64 packets;
  u64 bytes;
  u64 drops;
  u64 ecn_marks;
  u32 sparse_flow_count;
  u32 bulk_flow_count;

  cake_host_t hosts[CAKE_HOSTS];
} cake_tin_t;

typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  u64 shaped_pkts;
  u64 shaped_bytes;
  u64 backpressure_events;
  u64 drr_blocked;
  u64 parent_blocked;
} cake_agg_stats_t;

#define CAKE_AGG_LEVEL_PORT  0
#define CAKE_AGG_LEVEL_SVLAN 1

/* Tags an 802.1Q S-VLAN map can hold. level is an enum, not a depth counter:
 * a third tier would need the parent chain generalised, not a bigger number. */
#define CAKE_SVLAN_MAX 4096

/* Every worker shaping into an aggregate touches this struct per packet, so
 * the two genuinely-shared atomics each get their own line and the counters
 * move to per-thread slots. Packing them together cost a cache line bounce
 * per packet per core. */
typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  u64 rate_bytes_per_sec;
  u64 rate_ns_per_byte_scaled;
  u64 drr_round_bytes;

  u32 sw_if_index;
  u32 agg_index;
  u32 buffer_limit;
  u32 parent_index;
  u32 burst_ns;

  u16 svlan_id;
  u16 svlan_id_end;
  u16 weight;
  u8 level;

  cake_agg_stats_t *stats;

  CLIB_CACHE_LINE_ALIGN_MARK (cacheline1);
  u64 global_shaper_time_ns;

  CLIB_CACHE_LINE_ALIGN_MARK (cacheline2);
  u32 buffer_usage;

  /* This aggregate's own state as a child of parent_index, CAS-written per
   * packet by whichever worker dequeues any of its members. The one genuinely
   * new shared per-packet write the S-VLAN tier adds, so it takes its own line
   * rather than reintroducing the bounce 7c04b13 removed.
   *
   * Kept whole rather than splitting effective_weight into the read-mostly
   * group: the weight is read exclusively during a refill, by the same worker
   * that is about to CAS the word beside it, so splitting them would touch
   * two lines where one does. */
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline3);
  cake_drr_shared_child_t drr;

  /* Activation transitions track traffic, not configuration: a scheduler
   * deactivates the moment its queues drain and re-activates on the next
   * packet, so a 50 pps VoIP-only subscriber toggles once per packet, and a
   * single-member S-VLAN propagates each toggle to the port. Sharing
   * cacheline0 these writes would invalidate - at sparse-traffic rate - the
   * line every worker reads per packet for rate_ns_per_byte_scaled and
   * buffer_limit. The two share a line with each other because they are
   * always written together. */
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline4);
  u64 active_weight;
  u32 n_active_children;

  /* Level 0 only: CAKE_SVLAN_MAX entries of aggregate index, ~0 unmapped.
   * O(1) lookup at attachment for ~16 KB per port. Read at configuration
   * time only, so it sits here rather than displacing a per-packet field
   * from cacheline0. */
  u32 *svlan_map;
} cake_aggregate_t;

typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);

  u64 rate_bytes_per_sec;
  u64 rate_ns_per_byte_scaled;
  u64 global_shaper_time_ns;

  u32 sw_if_index;
  u32 sched_index;
  u32 owner_thread;

  i16 overhead_bytes;
  u8 atm_mode;
  u8 mpu;
  u8 n_tins;
  u8 tin_mode;

  u32 buffer_limit;
  u32 buffer_usage;
  u32 queued_buffers;

  u32 target_us;
  u32 interval_us;
  u32 mtu_time_us;
  u32 p_inc;
  u32 p_dec;

  const u8 *dscp_to_tin;

  cake_tin_t *tins;

  u64 enqueued_pkts;
  u64 enqueued_bytes;
  u64 dequeued_pkts;
  u64 dequeued_bytes;
  u64 dropped_pkts;

  /* Read together on the dequeue path: resolve the parent, then test this
   * scheduler's share of it. Owner-thread-local end to end - a scheduler has
   * exactly one owner and only that thread dequeues it - so the deficit costs
   * no shared cache line. */
  u32 aggregate_index;
  u32 weight;
  cake_drr_child_t drr;

  /* Dispatches deferred, not packets: the walk leaves the scheduler on either
   * outcome and retries next dispatch. Counted apart because they mean
   * different things - drr_blocked is "your siblings are using the capacity",
   * parent_blocked is "the parent is full". */
  u64 drr_blocked;
  u64 parent_blocked;
} cake_sched_t;

typedef struct
{
  uword *active_bitmap;
  u32 random_seed;
} cake_per_thread_t;

typedef struct
{
  cake_sched_t *schedulers;
  cake_per_thread_t *per_thread;
  u32 *sched_index_by_sw_if_index;
  u32 n_schedulers;

  cake_aggregate_t *aggregates;
  u32 *agg_index_by_sw_if_index;

  u16 msg_id_base;

  u32 ip4_enqueue_node_index;
  u32 ip6_enqueue_node_index;
  u32 dequeue_node_index;

  u32 fq_ip4_index;
  u32 fq_ip6_index;

  u8 ip4_output_arc_index;
  u8 ip6_output_arc_index;

  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
  vlib_log_class_t log_class;
} cake_main_t;

extern cake_main_t cake_main;

int cake_sched_enable_disable (vlib_main_t *vm, u32 sw_if_index, u8 is_enable,
			       u64 rate_bytes_per_sec, u8 tin_mode,
			       i16 overhead_bytes, u8 atm_mode, u8 mpu,
			       u32 buffer_limit, u32 target_us,
			       u32 interval_us, u32 flags, u32 weight);

void cake_sched_reset_stats (u32 sw_if_index);
void cake_cobalt_cache_init (void);

int cake_aggregate_create (vlib_main_t *vm, u32 sw_if_index,
			    u64 rate_bytes_per_sec, u32 weight, u32 burst_ns,
			    u32 buffer_limit);
int cake_aggregate_delete (vlib_main_t *vm, u32 sw_if_index);
int cake_svlan_aggregate_create (vlib_main_t *vm, u32 sw_if_index,
				 u16 svlan_id, u16 svlan_id_end,
				 u64 rate_bytes_per_sec, u32 weight,
				 u32 burst_ns, u32 buffer_limit);
int cake_aggregate_update (vlib_main_t *vm, u32 sw_if_index, u8 level,
			   u16 svlan_id, u64 rate_bytes_per_sec, u32 weight,
			   u32 burst_ns, u32 buffer_limit);
int cake_svlan_aggregate_delete (vlib_main_t *vm, u32 sw_if_index,
				 u16 svlan_id);
void cake_sched_resolve_attachment (cake_main_t *cm, cake_sched_t *cs);

static_always_inline u32
cake_overhead_adjust (cake_sched_t *cs, u32 pkt_len)
{
  i32 adjusted = (i32) pkt_len + cs->overhead_bytes;
  if (adjusted < cs->mpu)
    adjusted = cs->mpu;
  if (cs->atm_mode == 1)
    adjusted = ((adjusted + 47) / 48) * 53;
  return (u32) adjusted;
}

static_always_inline u32
cake_flow_queue_len (cake_flow_t *f)
{
  return f->tail - f->head;
}

static_always_inline void *
cake_l3_header (vlib_buffer_t *b)
{
  if (b->flags & VNET_BUFFER_F_L3_HDR_OFFSET_VALID)
    return b->data + vnet_buffer (b)->l3_hdr_offset;
  return (u8 *) vlib_buffer_get_current (b) +
	 vnet_buffer (b)->ip.save_rewrite_length;
}

static_always_inline u8
cake_dscp_from_buffer (vlib_buffer_t *b, u8 is_ip4)
{
  if (is_ip4)
    {
      ip4_header_t *ip4 = cake_l3_header (b);
      return (ip4->tos >> 2) & 0x3f;
    }
  else
    {
      ip6_header_t *ip6 = cake_l3_header (b);
      u32 vtcfl = clib_net_to_host_u32 (
	ip6->ip_version_traffic_class_and_flow_label);
      return (vtcfl >> 22) & 0x3f;
    }
}

static_always_inline u32
cake_dst_host_hash (vlib_buffer_t *b, u8 is_ip4)
{
  if (is_ip4)
    {
      ip4_header_t *ip4 = cake_l3_header (b);
      return ip4->dst_address.as_u32;
    }
  else
    {
      ip6_header_t *ip6 = cake_l3_header (b);
      return ip6->dst_address.as_u32[0] ^ ip6->dst_address.as_u32[3];
    }
}

static_always_inline u16
cake_host_lookup (cake_tin_t *tin, u32 ip_hash)
{
  u16 idx = ip_hash & CAKE_HOSTS_MASK;

  for (u16 i = 0; i < 4; i++)
    {
      u16 slot = (idx + i) & CAKE_HOSTS_MASK;
      if (tin->hosts[slot].ip_hash == ip_hash)
	return slot;
      if (tin->hosts[slot].ip_hash == 0)
	{
	  tin->hosts[slot].ip_hash = ip_hash;
	  return slot;
	}
    }

  tin->hosts[idx].ip_hash = ip_hash;
  tin->hosts[idx].bulk_flow_count = 0;
  return idx;
}

static_always_inline u32
cake_quantum_for_flow (cake_tin_t *tin, cake_flow_t *f)
{
  u16 host_load = 1;
  if (f->dst_host_idx < CAKE_HOSTS)
    {
      u16 hl = tin->hosts[f->dst_host_idx].bulk_flow_count;
      if (hl > CAKE_QUEUES)
	hl = CAKE_QUEUES;
      if (hl > host_load)
	host_load = hl;
    }
  u32 quantum = (tin->quantum * cake_quantum_div[host_load]) >> 16;
  return quantum ? quantum : 1;
}

static_always_inline void
cobalt_newton_step (cake_flow_t *f)
{
  u32 invsqrt = f->rec_inv_sqrt;
  u32 invsqrt2 = ((u64) invsqrt * invsqrt) >> 32;
  u64 val = (3LL << 32) - ((u64) f->codel_count * invsqrt2);
  val >>= 2;
  val = (val * invsqrt) >> (32 - 2 + 1);
  f->rec_inv_sqrt = (u32) val;
}

static_always_inline void
cobalt_invsqrt (cake_flow_t *f)
{
  if (f->codel_count < CAKE_REC_INV_SQRT_CACHE)
    f->rec_inv_sqrt = cobalt_rec_inv_sqrt_cache[f->codel_count];
  else
    cobalt_newton_step (f);
}

static_always_inline u32
cobalt_control (u32 t_us, u32 interval_us, u32 rec_inv_sqrt)
{
  return t_us + (u32) (((u64) interval_us * rec_inv_sqrt) >> 32);
}

static_always_inline void
cobalt_queue_full (cake_flow_t *f, u32 target_us, u32 p_inc, u32 now_us)
{
  if ((i32) (now_us - f->blue_timer_us) > (i32) target_us)
    {
      f->p_drop += p_inc;
      if (f->p_drop < p_inc)
	f->p_drop = ~0U;
      f->blue_timer_us = now_us;
    }
  f->dropping = 1;
  f->drop_next_us = now_us;
  if (!f->codel_count)
    f->codel_count = 1;
}

static_always_inline void
cobalt_queue_empty (cake_flow_t *f, u32 target_us, u32 p_dec,
		    u32 interval_us, u32 now_us)
{
  if (f->p_drop && (i32) (now_us - f->blue_timer_us) > (i32) target_us)
    {
      if (f->p_drop < p_dec)
	f->p_drop = 0;
      else
	f->p_drop -= p_dec;
      f->blue_timer_us = now_us;
    }
  f->dropping = 0;

  if (f->codel_count && (i32) (now_us - f->drop_next_us) >= 0)
    {
      f->codel_count--;
      cobalt_invsqrt (f);
      f->drop_next_us =
	cobalt_control (f->drop_next_us, interval_us, f->rec_inv_sqrt);
    }
}

static_always_inline u8
cobalt_should_drop (cake_flow_t *f, cake_sched_t *cs, u32 sojourn_us,
		    u32 now_us, u32 bulk_flows, u8 ecn_capable,
		    u8 *ecn_marked, u32 *random_seed)
{
  u8 drop = 0;
  u8 over_target;
  u8 next_due;
  i32 schedule;

  *ecn_marked = 0;

  over_target = sojourn_us > cs->target_us &&
		sojourn_us > cs->mtu_time_us * bulk_flows * 2 &&
		sojourn_us > cs->mtu_time_us * 4;

  schedule = (i32) (now_us - f->drop_next_us);
  next_due = f->codel_count && schedule >= 0;

  if (over_target)
    {
      if (!f->dropping)
	{
	  f->dropping = 1;
	  f->drop_next_us =
	    cobalt_control (now_us, cs->interval_us, f->rec_inv_sqrt);
	}
      if (!f->codel_count)
	f->codel_count = 1;
    }
  else if (f->dropping)
    f->dropping = 0;

  if (next_due && f->dropping)
    {
      drop = 1;
      if (ecn_capable)
	{
	  *ecn_marked = 1;
	  drop = 0;
	}

      f->codel_count++;
      if (!f->codel_count)
	f->codel_count--;
      cobalt_invsqrt (f);
      f->drop_next_us =
	cobalt_control (f->drop_next_us, cs->interval_us, f->rec_inv_sqrt);
    }
  else
    {
      while (next_due)
	{
	  f->codel_count--;
	  cobalt_invsqrt (f);
	  f->drop_next_us = cobalt_control (f->drop_next_us, cs->interval_us,
					    f->rec_inv_sqrt);
	  schedule = (i32) (now_us - f->drop_next_us);
	  next_due = f->codel_count && schedule >= 0;
	}
    }

  if (f->p_drop)
    drop |= (random_u32 (random_seed) < f->p_drop);

  if (!f->codel_count)
    f->drop_next_us = now_us + cs->interval_us;
  else if (schedule > 0 && !drop)
    f->drop_next_us = now_us;

  return drop;
}

static_always_inline u32
cake_hash_flow (vlib_buffer_t *b, u8 is_ip4)
{
  u64 k0, k1;

  if (is_ip4)
    {
      ip4_header_t *ip4 = cake_l3_header (b);
      u16 sport = 0, dport = 0;

      if (PREDICT_TRUE (ip4->protocol == IP_PROTOCOL_TCP ||
			ip4->protocol == IP_PROTOCOL_UDP))
	{
	  u8 *l4 = (u8 *) ip4 + ip4_header_bytes (ip4);
	  sport = *(u16 *) l4;
	  dport = *(u16 *) (l4 + 2);
	}

      k0 = ((u64) ip4->src_address.as_u32 << 32) | ip4->dst_address.as_u32;
      k1 = ((u64) sport << 16) | dport | ((u64) ip4->protocol << 32);
    }
  else
    {
      ip6_header_t *ip6 = cake_l3_header (b);
      u16 sport = 0, dport = 0;
      u32 flow_label = ip6_flow_label_network_order (ip6);

      if (PREDICT_TRUE (ip6->protocol == IP_PROTOCOL_TCP ||
			ip6->protocol == IP_PROTOCOL_UDP))
	{
	  u8 *l4 = (u8 *) (ip6 + 1);
	  sport = *(u16 *) l4;
	  dport = *(u16 *) (l4 + 2);
	}

      k0 = ip6->src_address.as_u64[0] ^ ip6->src_address.as_u64[1];
      k1 = (ip6->dst_address.as_u64[0] ^ ip6->dst_address.as_u64[1]) ^
	   (((u64) sport << 16) | dport | ((u64) flow_label << 32));
    }

  u32 hash = (u32) clib_xxhash (k0 ^ k1);
  return hash | 1;
}

static_always_inline void
cake_flow_list_prepend (u32 *list_head, u32 *list_tail, cake_flow_t *flows,
			u32 idx)
{
  cake_flow_t *f = &flows[idx];
  f->next = *list_head;
  f->prev = ~0;

  if (*list_head != ~0)
    flows[*list_head].prev = idx;
  else
    *list_tail = idx;

  *list_head = idx;
}

static_always_inline void
cake_flow_list_append_tail (u32 *list_head, u32 *list_tail,
			    cake_flow_t *flows, u32 idx)
{
  cake_flow_t *f = &flows[idx];
  f->next = ~0;
  f->prev = *list_tail;

  if (*list_tail != ~0)
    flows[*list_tail].next = idx;
  else
    *list_head = idx;

  *list_tail = idx;
}

static_always_inline void
cake_flow_list_remove (u32 *list_head, u32 *list_tail, cake_flow_t *flows,
		       u32 idx)
{
  cake_flow_t *f = &flows[idx];

  if (f->prev != ~0)
    flows[f->prev].next = f->next;
  else
    *list_head = f->next;

  if (f->next != ~0)
    flows[f->next].prev = f->prev;
  else
    *list_tail = f->prev;

  f->next = ~0;
  f->prev = ~0;
}

static_always_inline void
cake_flow_ring_alloc (cake_flow_t *f)
{
  f->ring =
    clib_mem_alloc_aligned (CAKE_FLOW_RING_SIZE * sizeof (u32),
			    CLIB_CACHE_LINE_BYTES);
  f->head = 0;
  f->tail = 0;
}

static_always_inline void
cake_flow_ring_free (vlib_main_t *vm, cake_flow_t *f)
{
  if (PREDICT_FALSE (f->ring == NULL))
    return;

  while (f->head != f->tail)
    {
      vlib_buffer_free_one (vm,
			    f->ring[f->head & CAKE_FLOW_RING_MASK]);
      f->head++;
    }

  clib_mem_free (f->ring);
  f->ring = NULL;
  f->head = 0;
  f->tail = 0;
}

static_always_inline u32
cake_flow_lookup (cake_tin_t *tin, u32 tag, u32 set_base, u32 *evict_slot)
{
  u32 empty_slot = ~0;
  *evict_slot = ~0;
  u32 evict_backlog = ~0U;

  for (u32 i = 0; i < CAKE_SET_WAYS; i++)
    {
      u32 slot = set_base + i;
      u32 slot_tag = tin->flow_tags[slot];

      if (slot_tag == tag)
	return slot;

      if (slot_tag == 0 && empty_slot == ~0)
	empty_slot = slot;

      if (slot_tag != 0 && empty_slot == ~0)
	{
	  u32 bl = tin->flows[slot].backlog_bytes;
	  if (bl < evict_backlog)
	    {
	      evict_backlog = bl;
	      *evict_slot = slot;
	    }
	}
    }

  if (empty_slot != ~0)
    {
      *evict_slot = ~0;
      tin->flow_tags[empty_slot] = tag;
      return empty_slot;
    }

  return ~0;
}

static_always_inline u8
cake_ecn_mark (vlib_buffer_t *b)
{
  u8 *ip_hdr = cake_l3_header (b);

  if ((ip_hdr[0] >> 4) == 4)
    {
      ip4_header_t *ip4 = (ip4_header_t *) ip_hdr;
      if ((ip4->tos & IP_PACKET_TC_FIELD_ECN_MASK) == IP_ECN_NON_ECN)
	return 0;
      ip4_header_set_ecn_w_chksum (ip4, IP_ECN_CE);
      return 1;
    }
  else
    {
      ip6_header_t *ip6 = (ip6_header_t *) ip_hdr;
      u32 vtcfl =
	clib_net_to_host_u32 (ip6->ip_version_traffic_class_and_flow_label);
      u8 tc = (vtcfl >> 20) & 0xff;
      if ((tc & 0x03) == 0)
	return 0;
      tc = (tc & ~0x03) | 0x03;
      vtcfl = (vtcfl & ~(0xffU << 20)) | ((u32) tc << 20);
      ip6->ip_version_traffic_class_and_flow_label =
	clib_host_to_net_u32 (vtcfl);
      return 1;
    }
}

/* Release at every tier the packet was charged to. Resolves through the
 * scheduler's *current* attachment, which is why reparenting a scheduler that
 * still holds charged packets has to transfer the charge with it. */
static_always_inline void
cake_agg_discharge (cake_main_t *cm, cake_sched_t *cs, u32 pkt_len)
{
  u32 idx = cs->aggregate_index;

  while (idx != ~0)
    {
      cake_aggregate_t *agg = pool_elt_at_index (cm->aggregates, idx);
      cake_agg_release (&agg->buffer_usage, pkt_len);
      idx = agg->parent_index;
    }
}

/* Charge every tier, unwinding the ones already taken if a later one refuses.
 *
 * A read-only overload filter fronts each level: at an already-full queue a
 * marginally early or late drop is the outcome regardless, and without it
 * sustained incast pays four RMWs per rejected packet on the two hottest
 * lines, from every worker, at *offered* rate. Every admission still goes
 * through the exact fetch-add-then-verify, so the pre-#5 over-admission race
 * does not come back. */
static_always_inline u8
cake_agg_admit_chain (cake_main_t *cm, cake_sched_t *cs, u32 pkt_len,
		      u32 thread_index)
{
  cake_aggregate_t *agg, *parent;

  if (cs->aggregate_index == ~0)
    return 1;

  agg = pool_elt_at_index (cm->aggregates, cs->aggregate_index);

  if (PREDICT_FALSE (__atomic_load_n (&agg->buffer_usage, __ATOMIC_RELAXED) >
		     agg->buffer_limit) ||
      PREDICT_FALSE (
	!cake_agg_admit (&agg->buffer_usage, agg->buffer_limit, pkt_len)))
    {
      vec_elt_at_index (agg->stats, thread_index)->backpressure_events++;
      return 0;
    }

  if (agg->parent_index == ~0)
    return 1;

  parent = pool_elt_at_index (cm->aggregates, agg->parent_index);

  if (PREDICT_FALSE (__atomic_load_n (&parent->buffer_usage,
				      __ATOMIC_RELAXED) >
		     parent->buffer_limit) ||
      PREDICT_FALSE (!cake_agg_admit (&parent->buffer_usage,
				      parent->buffer_limit, pkt_len)))
    {
      cake_agg_release (&agg->buffer_usage, pkt_len);
      vec_elt_at_index (parent->stats, thread_index)->backpressure_events++;
      return 0;
    }

  return 1;
}

/* Release what a flow's queued buffers hold at both levels, then free the ring.
 * Bypassing this leaks agg->buffer_usage, which only ratchets up and
 * eventually wedges the aggregate shut. */
static_always_inline void
cake_flow_discard (vlib_main_t *vm, cake_main_t *cm, cake_sched_t *cs,
		   cake_tin_t *tin, cake_flow_t *f)
{
  u32 queued = cake_flow_queue_len (f);

  if (queued)
    {
      cake_agg_discharge (cm, cs, f->backlog_bytes);
      cs->buffer_usage -= f->backlog_bytes;
      cs->queued_buffers -= queued;
      cs->dropped_pkts += queued;
      tin->drops += queued;
      f->backlog_bytes = 0;
    }

  cake_flow_ring_free (vm, f);
}

/* Bind the dependency-free DRR core to a scheduler and the aggregate it
 * competes in. A scheduler with no aggregate has nothing to arbitrate
 * against, so it admits without charging. */
/* Bytes a tier can actually pass in a round, which is its configured rate only
 * while nothing above it is throttling it.
 *
 * Deriving a child's quantum from its parent's *configured* rate is right
 * for the port, and wrong for any tier under an oversubscribed parent - the
 * ordinary HQoS case. An S-VLAN provisioned at port rate but
 * winning half the port hands each of its children a quantum sized for the
 * whole S-VLAN rate, so no child's deficit ever runs out, every child stays
 * permanently eligible, and the tier stops arbitrating: measured, one child of
 * two took 49.9% of the port and the other 0.12%.
 *
 * Folding the parent's own share in makes the quanta beneath it sum to what
 * the tier really gets. Evaluated once per child per round, on the refill
 * path, never per packet. */
static_always_inline u64
cake_agg_effective_round_bytes (cake_main_t *cm, cake_aggregate_t *agg)
{
  u64 round_bytes = agg->drr_round_bytes;
  cake_aggregate_t *parent;
  u64 share;

  if (agg->parent_index == ~0)
    return round_bytes;

  parent = pool_elt_at_index (cm->aggregates, agg->parent_index);
  share = cake_drr_quantum (
    parent->drr_round_bytes, agg->drr.effective_weight,
    __atomic_load_n (&parent->active_weight, __ATOMIC_RELAXED));

  return share < round_bytes ? share : round_bytes;
}

static_always_inline cake_drr_admit_t
cake_sched_drr_admit (cake_main_t *cm, cake_sched_t *cs, u64 now_ns)
{
  cake_aggregate_t *agg;
  u64 idle_vt, round_bytes = 0;

  if (cs->aggregate_index == ~0)
    return CAKE_DRR_UNARBITRATED;

  agg = pool_elt_at_index (cm->aggregates, cs->aggregate_index);

  /* Only the refill consumes round_bytes, so only a round boundary pays for
   * resolving what the parent can really pass. */
  if (PREDICT_FALSE (cs->drr.round != cake_drr_round (now_ns)))
    round_bytes = cake_agg_effective_round_bytes (cm, agg);

  idle_vt = __atomic_load_n (&agg->global_shaper_time_ns, __ATOMIC_RELAXED);

  /* The work-conserving escape admits without charging, on the premise that a
   * parent whose virtual time is a round behind the wall clock has real spare
   * capacity. That premise only holds for the root.
   *
   * A non-root tier's virtual time also lags while the tier itself is blocked
   * against the tier above - it is not sending, so its clock is not advancing
   * - and that is congestion, not spare capacity. Measured: with an S-VLAN
   * throttled by its port, every one of its children read the lag as an
   * escape, admitted uncharged, and the intra-S-VLAN deficits stopped moving
   * altogether, so walk order decided everything and two of four children got
   * nothing at all in 30 s.
   *
   * Taking the latest virtual time in the chain means the escape fires only
   * when every tier is genuinely idle. */
  if (agg->parent_index != ~0)
    {
      cake_aggregate_t *parent =
	pool_elt_at_index (cm->aggregates, agg->parent_index);
      u64 parent_vt =
	__atomic_load_n (&parent->global_shaper_time_ns, __ATOMIC_RELAXED);

      if (parent_vt > idle_vt)
	idle_vt = parent_vt;
    }

  return cake_drr_local_admit (&cs->drr, round_bytes, &agg->active_weight,
			       &idle_vt, now_ns);
}

/* Move a child's weight into or out of an aggregate, propagating the
 * transition upward as a refcount: an aggregate counts toward its own parent's
 * W exactly while it has at least one active child, so only the caller that
 * observes the 0->1 (join) or 1->0 (leave) transition carries it up. Taking
 * the aggregate index as an argument is what lets a reparent detach from the
 * old parent and attach to the new one while the child stays active. */
static_always_inline void
cake_agg_weight_add (cake_main_t *cm, u32 agg_index, u64 weight)
{
  cake_aggregate_t *agg;

  if (agg_index == ~0)
    return;

  agg = pool_elt_at_index (cm->aggregates, agg_index);

  if (cake_drr_parent_join (&agg->active_weight, &agg->n_active_children,
			    weight) == 0 &&
      agg->parent_index != ~0)
    {
      cake_aggregate_t *parent =
	pool_elt_at_index (cm->aggregates, agg->parent_index);
      cake_drr_parent_join (&parent->active_weight,
			    &parent->n_active_children,
			    agg->drr.effective_weight);
    }
}

static_always_inline void
cake_agg_weight_sub (cake_main_t *cm, u32 agg_index, u64 weight)
{
  cake_aggregate_t *agg;

  if (agg_index == ~0)
    return;

  agg = pool_elt_at_index (cm->aggregates, agg_index);

  if (cake_drr_parent_leave (&agg->active_weight, &agg->n_active_children,
			     weight) == 1 &&
      agg->parent_index != ~0)
    {
      cake_aggregate_t *parent =
	pool_elt_at_index (cm->aggregates, agg->parent_index);
      cake_drr_parent_leave (&parent->active_weight,
			     &parent->n_active_children,
			     agg->drr.effective_weight);
    }
}

/* Weight accounting, at exactly three sites: activation on enqueue,
 * empty-detect deactivation on dequeue, and teardown. Both directions are
 * guarded by the child's own flag, which is owner-thread-local; only the
 * parent's counters are atomic, and they move on activation transitions
 * rather than per packet.
 *
 * The defensive deactivations in the dequeue walk - pool-free and
 * owner-mismatch - must NOT call this. They clear the activity bit for a
 * scheduler whose teardown already ran, and subtracting there double-counts:
 * active_weight underflows and every sibling's quantum collapses. */
static_always_inline void
cake_sched_drr_activate (cake_main_t *cm, cake_sched_t *cs)
{
  if (PREDICT_TRUE (cs->drr.active))
    return;

  if (!cake_drr_child_activate (&cs->drr))
    return;

  cake_agg_weight_add (cm, cs->aggregate_index, cs->drr.effective_weight);
}

static_always_inline void
cake_sched_drr_deactivate (cake_main_t *cm, cake_sched_t *cs)
{
  if (!cake_drr_child_deactivate (&cs->drr))
    return;

  cake_agg_weight_sub (cm, cs->aggregate_index, cs->drr.effective_weight);
}

/* The parent chain's gates, in deliberate order: the commonest rejection
 * under saturation - the immediate parent sitting at its own configured rate
 * - fails first and unwinds nothing. A DRR block against the tier above
 * unwinds one gate charge. Only the rare port rejection unwinds two things,
 * and S-VLANs are normally provisioned under port rate so the port rarely
 * refuses what the S-VLAN accepted. */
typedef enum
{
  CAKE_PARENT_OK = 0,
  CAKE_PARENT_GATE_CLOSED,
  CAKE_PARENT_DRR_BLOCKED,
} cake_parent_result_t;

static_always_inline cake_parent_result_t
cake_agg_dequeue_gate (cake_main_t *cm, cake_sched_t *cs, u32 adj_len,
			u64 now_ns, u32 thread_index)
{
  cake_aggregate_t *agg, *parent;
  cake_agg_stats_t *st;
  u64 cost_ns;
  cake_drr_admit_t reserved;

  if (cs->aggregate_index == ~0)
    return CAKE_PARENT_OK;

  agg = pool_elt_at_index (cm->aggregates, cs->aggregate_index);
  cost_ns = cake_cost_ns (adj_len, agg->rate_ns_per_byte_scaled);

  /* The immediate parent's rate gate. */
  if (!cake_shaper_gate_take (&agg->global_shaper_time_ns, cost_ns, now_ns,
			      agg->burst_ns))
    {
      vec_elt_at_index (agg->stats, thread_index)->parent_blocked++;
      return CAKE_PARENT_GATE_CLOSED;
    }

  st = vec_elt_at_index (agg->stats, thread_index);
  st->shaped_pkts++;
  st->shaped_bytes += adj_len;

  if (agg->parent_index == ~0)
    return CAKE_PARENT_OK;

  parent = pool_elt_at_index (cm->aggregates, agg->parent_index);

  /* This aggregate's own share of the tier above, one CAS. */
  reserved =
    cake_drr_shared_reserve (&agg->drr, parent->drr_round_bytes,
			     &parent->active_weight,
			     &parent->global_shaper_time_ns, adj_len, now_ns);

  if (reserved == CAKE_DRR_BLOCKED)
    {
      __atomic_fetch_sub (&agg->global_shaper_time_ns, cost_ns,
			  __ATOMIC_RELAXED);
      st->shaped_pkts--;
      st->shaped_bytes -= adj_len;
      st->drr_blocked++;
      return CAKE_PARENT_DRR_BLOCKED;
    }

  /* The tier above's rate gate. */
  if (!cake_shaper_gate_take (&parent->global_shaper_time_ns,
			      cake_cost_ns (adj_len,
					    parent->rate_ns_per_byte_scaled),
			      now_ns, parent->burst_ns))
    {
      /* Escapes charge inside the CAS and refund like any other admission;
       * the test only guards a future CAKE_DRR_UNARBITRATED return, which
       * must never be given credit back. */
      if (reserved == CAKE_DRR_ADMIT)
	cake_drr_shared_refund (&agg->drr, adj_len);

      __atomic_fetch_sub (&agg->global_shaper_time_ns, cost_ns,
			  __ATOMIC_RELAXED);
      st->shaped_pkts--;
      st->shaped_bytes -= adj_len;
      vec_elt_at_index (parent->stats, thread_index)->parent_blocked++;
      return CAKE_PARENT_GATE_CLOSED;
    }

  st = vec_elt_at_index (parent->stats, thread_index);
  st->shaped_pkts++;
  st->shaped_bytes += adj_len;

  return CAKE_PARENT_OK;
}

static_always_inline void
cake_agg_stats_sum (cake_aggregate_t *agg, u64 *shaped_pkts, u64 *shaped_bytes,
		    u64 *backpressure_events, u64 *drr_blocked,
		    u64 *parent_blocked)
{
  *shaped_pkts = *shaped_bytes = *backpressure_events = 0;
  *drr_blocked = *parent_blocked = 0;

  cake_agg_stats_t *st;
  vec_foreach (st, agg->stats)
    {
      *shaped_pkts += st->shaped_pkts;
      *shaped_bytes += st->shaped_bytes;
      *backpressure_events += st->backpressure_events;
      *drr_blocked += st->drr_blocked;
      *parent_blocked += st->parent_blocked;
    }
}

extern vlib_node_registration_t ip4_cake_enqueue_node;
extern vlib_node_registration_t ip6_cake_enqueue_node;
extern vlib_node_registration_t ip4_cake_handoff_node;
extern vlib_node_registration_t ip6_cake_handoff_node;
extern vlib_node_registration_t cake_dequeue_node;

#endif /* __included_osvbng_qos_sched_h__ */

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
