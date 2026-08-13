/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - Core implementation
 * Plugin init, per-subscriber enable/disable, CLI commands.
 *
 * Phase 4: DiffServ tins.
 *
 * Algorithms derived from the Linux CAKE qdisc (sch_cake.c).
 * Original authors: Dave Taht, Jonathan Morton, Toke Hoiland-Jorgensen,
 * Sebastian Moeller, Kevin Darbyshire-Bryant, Ryan Mounce.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vnet/feature/feature.h>

#include <osvbng_qos_sched/osvbng_qos_sched.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

cake_main_t cake_main;
u16 cake_quantum_div[CAKE_QUEUES + 1] = { 0 };

char *cake_error_strings[] = {
#define cake_error(n, s) s,
#include <osvbng_qos_sched/osvbng_qos_sched_error.def>
#undef cake_error
};

const u8 cake_dscp_besteffort[64] = { 0 };

/* clang-format off */
/* diffserv3: Bulk(0), Best Effort(1), Voice(2) */
const u8 cake_dscp_diffserv3[64] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 */
         1, 1, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1,
/*      16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 */
         1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*      32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 */
         1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 1,
/*      48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 */
         2, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1,
};

/* diffserv4: Bulk(0), Best Effort(1), Video(2), Voice(3) */
const u8 cake_dscp_diffserv4[64] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 */
         1, 2, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1,
/*      16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 */
         2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1,
/*      32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 */
         3, 1, 2, 1, 2, 1, 2, 1, 3, 1, 1, 1, 3, 1, 3, 1,
/*      48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 */
         3, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1,
};

/* diffserv8: Bulk(0), HiTput(1), BestEff(2), Video(3), LowLat(4),
 *            Interactive(5), Realtime(6), NetCtrl(7) */
const u8 cake_dscp_diffserv8[64] = {
/*       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 */
         2, 5, 1, 2, 4, 2, 2, 2, 0, 2, 1, 2, 1, 2, 1, 2,
/*      16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 */
         5, 2, 4, 2, 4, 2, 4, 2, 3, 2, 3, 2, 3, 2, 3, 2,
/*      32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 */
         6, 2, 3, 2, 3, 2, 3, 2, 6, 2, 2, 2, 6, 2, 6, 2,
/*      48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 */
         7, 2, 2, 2, 2, 2, 2, 2, 7, 2, 2, 2, 2, 2, 2, 2,
};
/* clang-format on */

static const u8 *cake_dscp_tables[] = {
  [CAKE_TIN_MODE_BESTEFFORT] = cake_dscp_besteffort,
  [CAKE_TIN_MODE_DIFFSERV3] = cake_dscp_diffserv3,
  [CAKE_TIN_MODE_DIFFSERV4] = cake_dscp_diffserv4,
  [CAKE_TIN_MODE_DIFFSERV8] = cake_dscp_diffserv8,
};

static const u8 cake_tin_counts[] = {
  [CAKE_TIN_MODE_BESTEFFORT] = 1,
  [CAKE_TIN_MODE_DIFFSERV3] = 3,
  [CAKE_TIN_MODE_DIFFSERV4] = 4,
  [CAKE_TIN_MODE_DIFFSERV8] = 8,
};

static void
cake_tin_init (cake_tin_t *tin, u32 quantum)
{
  clib_memset (tin, 0, sizeof (*tin));

  tin->flows =
    clib_mem_alloc_aligned (CAKE_QUEUES * sizeof (cake_flow_t),
			    CLIB_CACHE_LINE_BYTES);
  clib_memset (tin->flows, 0, CAKE_QUEUES * sizeof (cake_flow_t));

  tin->flow_tags =
    clib_mem_alloc_aligned (CAKE_QUEUES * sizeof (u32),
			    CLIB_CACHE_LINE_BYTES);
  clib_memset (tin->flow_tags, 0, CAKE_QUEUES * sizeof (u32));

  for (u32 i = 0; i < CAKE_QUEUES; i++)
    {
      tin->flows[i].next = ~0;
      tin->flows[i].prev = ~0;
    }

  tin->new_flow_head = ~0;
  tin->new_flow_tail = ~0;
  tin->old_flow_head = ~0;
  tin->old_flow_tail = ~0;
  tin->decaying_flow_head = ~0;
  tin->decaying_flow_tail = ~0;
  tin->quantum = quantum;
  tin->tin_deficit = 0;
  tin->tin_quantum = 0;
}

static void
cake_tin_drain (vlib_main_t *vm, cake_main_t *cm, cake_sched_t *cs,
		cake_tin_t *tin)
{
  if (tin->flows)
    {
      for (u32 i = 0; i < CAKE_QUEUES; i++)
	cake_flow_discard (vm, cm, cs, tin, &tin->flows[i]);
      clib_mem_free (tin->flows);
      tin->flows = NULL;
    }
  if (tin->flow_tags)
    {
      clib_mem_free (tin->flow_tags);
      tin->flow_tags = NULL;
    }
}

static u8
cake_worker_has_other_schedulers (cake_main_t *cm, cake_sched_t *exclude,
				  u32 owner_thread)
{
  cake_sched_t *cs;
  pool_foreach (cs, cm->schedulers)
    {
      if (cs != exclude && cs->owner_thread == owner_thread)
	return 1;
    }
  return 0;
}

/* Walk sup_sw_if_index toward the physical port, remembering the outermost
 * single 802.1Q tag on the way. The port's S-VLAN map then decides whether the
 * scheduler's immediate parent is an S-VLAN aggregate or the port itself.
 *
 * osvbng's S-VLAN sub-interfaces are single-tagged (parent.svlan via
 * CreateSubif; the C-VLAN is never a VPP sub-interface), so this reads one_tag
 * rather than a QinQ pair.
 *
 * aggregate_index is always the *immediate* parent. Everything on the hot path
 * - the gate, the discharge chain, the DRR check - follows parent_index from
 * there, so neither tier needs a second cached index that could go stale. */
void
cake_sched_resolve_attachment (cake_main_t *cm, cake_sched_t *cs)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 walk = cs->sw_if_index;
  u32 svlan_tag = ~0;
  u32 port_index = ~0;
  cake_aggregate_t *port;

  cs->aggregate_index = ~0;

  while (1)
    {
      vnet_sw_interface_t *wi = vnet_get_sw_interface (vnm, walk);
      u32 parent;

      if (wi->type == VNET_SW_INTERFACE_TYPE_SUB && wi->sub.eth.flags.one_tag)
	svlan_tag = wi->sub.eth.outer_vlan_id;

      parent = wi->sup_sw_if_index;
      if (parent == walk)
	break;

      if (parent < vec_len (cm->agg_index_by_sw_if_index) &&
	  cm->agg_index_by_sw_if_index[parent] != ~0)
	{
	  port_index = cm->agg_index_by_sw_if_index[parent];
	  break;
	}
      walk = parent;
    }

  if (port_index == ~0)
    return;

  cs->aggregate_index = port_index;

  port = pool_elt_at_index (cm->aggregates, port_index);
  if (port->svlan_map == NULL || svlan_tag >= CAKE_SVLAN_MAX)
    return;

  if (port->svlan_map[svlan_tag] != ~0)
    cs->aggregate_index = port->svlan_map[svlan_tag];
}

/* Three levels ship at most, but the walks below are written against the
 * chain rather than the number. */
#define CAKE_AGG_MAX_DEPTH 4

static u32
cake_agg_chain (cake_main_t *cm, u32 idx, u32 *chain)
{
  u32 n = 0;

  while (idx != ~0 && n < CAKE_AGG_MAX_DEPTH)
    {
      chain[n++] = idx;
      idx = pool_elt_at_index (cm->aggregates, idx)->parent_index;
    }
  return n;
}

static u8
cake_agg_chain_has (u32 *chain, u32 n, u32 idx)
{
  for (u32 i = 0; i < n; i++)
    if (chain[i] == idx)
      return 1;
  return 0;
}

/* Re-resolve every scheduler's attachment and move what the move implies.
 * Called under the worker barrier after any change to the aggregate topology,
 * which is the only time attachment can change.
 *
 * Two things travel with a member, and getting either wrong is silent:
 *
 * Weight, so a tier's active_weight keeps matching the sum of its active
 * children. It leaves the old chain and joins the new one, and because the
 * child stays active throughout, the refcount propagation handles the tiers
 * above without special-casing.
 *
 * Outstanding buffer charge, because cake_agg_discharge resolves through the
 * scheduler's *current* attachment at free time and nothing on the packet
 * records what it was charged to. A tier the member is joining will be
 * discharged for packets it never admitted, so it is pre-credited; a tier the
 * member is leaving was charged for packets it will never see freed, so it is
 * debited. Tiers common to both chains - the port, under a backfill - are
 * charged and discharged exactly once and must not be touched. Without the
 * pre-credit the first pre-backfill packet freed underflows a u32 and pins the
 * tier's admission check shut for good.
 *
 * Exact rather than approximate: cs->buffer_usage and the aggregate charge
 * share the raw pkt_len basis, the charge site runs only on the owner thread,
 * handed-off packets are uncharged until re-enqueued owner-side, and the
 * barrier means nothing is in flight. */
static void
cake_agg_reparent_all (cake_main_t *cm)
{
  cake_sched_t *cs;

  pool_foreach (cs, cm->schedulers)
    {
      u32 old_chain[CAKE_AGG_MAX_DEPTH], new_chain[CAKE_AGG_MAX_DEPTH];
      u32 n_old, n_new, old_parent;

      old_parent = cs->aggregate_index;
      n_old = cake_agg_chain (cm, old_parent, old_chain);

      cake_sched_resolve_attachment (cm, cs);

      if (cs->aggregate_index == old_parent)
	continue;

      n_new = cake_agg_chain (cm, cs->aggregate_index, new_chain);

      if (cs->drr.active)
	{
	  cake_agg_weight_sub (cm, old_parent, cs->drr.effective_weight);
	  cake_agg_weight_add (cm, cs->aggregate_index,
			       cs->drr.effective_weight);
	}

      if (cs->buffer_usage == 0)
	continue;

      for (u32 i = 0; i < n_new; i++)
	if (!cake_agg_chain_has (old_chain, n_old, new_chain[i]))
	  pool_elt_at_index (cm->aggregates, new_chain[i])->buffer_usage +=
	    cs->buffer_usage;

      for (u32 i = 0; i < n_old; i++)
	if (!cake_agg_chain_has (new_chain, n_new, old_chain[i]))
	  pool_elt_at_index (cm->aggregates, old_chain[i])->buffer_usage -=
	    cs->buffer_usage;
    }
}

int
cake_svlan_aggregate_create (vlib_main_t *vm, u32 sw_if_index, u16 svlan_id,
			     u16 svlan_id_end, u64 rate_bytes_per_sec,
			     u32 weight, u32 burst_ns, u32 buffer_limit)
{
  cake_main_t *cm = &cake_main;
  cake_aggregate_t *port, *agg;
  u32 port_idx, agg_idx;

  if (svlan_id_end < svlan_id || svlan_id_end >= CAKE_SVLAN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return VNET_API_ERROR_INVALID_VALUE;

  if (burst_ns == 0)
    burst_ns = CAKE_AGG_BURST_NS;
  if (burst_ns < CAKE_AGG_BURST_NS_MIN || burst_ns > CAKE_AGG_BURST_NS_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  vlib_worker_thread_barrier_sync (vm);

  vec_validate_init_empty (cm->agg_index_by_sw_if_index, sw_if_index, ~0);
  port_idx = cm->agg_index_by_sw_if_index[sw_if_index];
  if (port_idx == ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  port = pool_elt_at_index (cm->aggregates, port_idx);

  if (rate_bytes_per_sec > port->rate_bytes_per_sec)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_INVALID_VALUE;
    }

  /* S-VLAN sets are disjoint per port. */
  for (u32 t = svlan_id; t <= svlan_id_end; t++)
    if (port->svlan_map[t] != ~0)
      {
	vlib_worker_thread_barrier_release (vm);
	return VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
      }

  pool_get_aligned_zero (cm->aggregates, agg, CLIB_CACHE_LINE_BYTES);
  agg_idx = agg - cm->aggregates;

  /* pool_get may have moved the pool. */
  port = pool_elt_at_index (cm->aggregates, port_idx);

  vec_validate_aligned (agg->stats, vlib_get_n_threads () - 1,
			CLIB_CACHE_LINE_BYTES);

  agg->sw_if_index = sw_if_index;
  agg->agg_index = agg_idx;
  agg->level = CAKE_AGG_LEVEL_SVLAN;
  agg->parent_index = port_idx;
  agg->svlan_id = svlan_id;
  agg->svlan_id_end = svlan_id_end;
  agg->svlan_map = NULL;
  agg->weight = weight ? weight : 1;
  agg->burst_ns = burst_ns;
  agg->rate_bytes_per_sec = rate_bytes_per_sec;
  agg->rate_ns_per_byte_scaled = cake_rate_scaled (rate_bytes_per_sec);
  agg->drr_round_bytes = cake_drr_round_bytes (rate_bytes_per_sec);
  agg->global_shaper_time_ns = (u64) (vlib_time_now (vm) * 1e9);
  cake_drr_shared_init (&agg->drr,
			cake_effective_weight (rate_bytes_per_sec,
					       agg->weight),
			agg->global_shaper_time_ns);

  if (buffer_limit == 0)
    {
      u64 derived = (rate_bytes_per_sec * 100000 * 3) / (1000000 * 2);
      if (derived < 1048576)
	derived = 1048576;
      /* 0.15 s of rate passes 4 GiB at ~229 Gbit/s; saturate rather than
       * wrap the u32 into a tiny limit. */
      agg->buffer_limit = derived > ~0U ? ~0U : (u32) derived;
    }
  else
    agg->buffer_limit = buffer_limit;

  for (u32 t = svlan_id; t <= svlan_id_end; t++)
    port->svlan_map[t] = agg_idx;

  cake_agg_reparent_all (cm);

  vlib_worker_thread_barrier_release (vm);

  vlib_log_notice (cm->log_class,
		   "svlan aggregate created on sw_if_index %u tags %u-%u "
		   "(rate %llu B/s)",
		   sw_if_index, svlan_id, svlan_id_end, rate_bytes_per_sec);

  return 0;
}

int
cake_svlan_aggregate_delete (vlib_main_t *vm, u32 sw_if_index, u16 svlan_id)
{
  cake_main_t *cm = &cake_main;
  cake_aggregate_t *port, *agg;
  u32 port_idx, agg_idx;
  u16 first, last;

  if (svlan_id >= CAKE_SVLAN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  vlib_worker_thread_barrier_sync (vm);

  vec_validate_init_empty (cm->agg_index_by_sw_if_index, sw_if_index, ~0);
  port_idx = cm->agg_index_by_sw_if_index[sw_if_index];
  if (port_idx == ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  port = pool_elt_at_index (cm->aggregates, port_idx);
  agg_idx = port->svlan_map[svlan_id];
  if (agg_idx == ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  agg = pool_elt_at_index (cm->aggregates, agg_idx);
  first = agg->svlan_id;
  last = agg->svlan_id_end;

  for (u32 t = first; t <= last; t++)
    if (port->svlan_map[t] == agg_idx)
      port->svlan_map[t] = ~0;

  /* Members fall back to the port, taking their weight and their outstanding
   * charge with them, before the pool entry goes away. */
  cake_agg_reparent_all (cm);

  vec_free (agg->stats);
  pool_put_index (cm->aggregates, agg_idx);

  vlib_worker_thread_barrier_release (vm);

  vlib_log_notice (cm->log_class,
		   "svlan aggregate deleted on sw_if_index %u tags %u-%u",
		   sw_if_index, first, last);

  return 0;
}

int
cake_sched_enable_disable (vlib_main_t *vm, u32 sw_if_index, u8 is_enable,
			   u64 rate_bytes_per_sec, u8 tin_mode,
			   i16 overhead_bytes, u8 atm_mode, u8 mpu,
			   u32 buffer_limit, u32 target_us, u32 interval_us,
			   u32 flags, u32 weight)
{
  cake_main_t *cm = &cake_main;

  /* Disable must accept a hidden interface: PPPoE parks session
   * interfaces hidden for reuse instead of deleting them, and the
   * teardown disable races that hiding. Rejecting it leaks the
   * scheduler onto the recycled interface's next subscriber. */
  if (is_enable ?
	!vnet_sw_interface_is_api_valid (vnet_get_main (), sw_if_index) :
	!vnet_sw_interface_is_valid (vnet_get_main (), sw_if_index))
    return VNET_API_ERROR_INVALID_SW_IF_INDEX;

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return VNET_API_ERROR_INVALID_VALUE;

  /* One-shot config change under the barrier: both branches mutate state
   * workers read per packet - enable may even realloc the sw_if_index
   * lookup vector, disable frees tins a dequeue walk may be inside. The
   * binapi path already holds the barrier and the sync is
   * recursion-tolerant; this is for the CLI and the interface add/del
   * callback, which arrive barrier-free. */
  vlib_worker_thread_barrier_sync (vm);

  if (is_enable)
    {
      vec_validate_init_empty (cm->sched_index_by_sw_if_index, sw_if_index,
			       ~0);
      if (cm->sched_index_by_sw_if_index[sw_if_index] != ~0)
	{
	  vlib_worker_thread_barrier_release (vm);
	  return VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
	}

      if (tin_mode > CAKE_TIN_MODE_DIFFSERV8)
	tin_mode = CAKE_TIN_MODE_BESTEFFORT;

      cake_sched_t *cs;
      pool_get_zero (cm->schedulers, cs);
      u32 pool_index = cs - cm->schedulers;

      cs->sw_if_index = sw_if_index;
      cs->sched_index = pool_index;
      cs->rate_bytes_per_sec = rate_bytes_per_sec;
      cs->rate_ns_per_byte_scaled = cake_rate_scaled (rate_bytes_per_sec);

      /* Weight defaults to the subscriber's own configured rate, so with no
       * configuration at all a 100 Mbit/s subscriber gets ten times the
       * congested share of a 10 Mbit/s one. The operator multiplier scales
       * that rather than replacing it: replacing would make mixed
       * configuration nonsensical, since a child set to weight 2 alongside
       * children defaulting to their rate would be crushed. */
      cs->weight = weight ? weight : 1;
      cs->drr.effective_weight =
	cake_effective_weight (rate_bytes_per_sec, cs->weight);
      /* Seeded, not left at pool_get_zero's 0: past day 25 of uptime a zero
       * tag sits in the future half-space where the refill test refuses it. */
      cs->drr.round = cake_drr_round ((u64) (vlib_time_now (vm) * 1e9));
      cs->overhead_bytes = overhead_bytes;
      cs->atm_mode = atm_mode;
      cs->mpu = mpu;
      cs->global_shaper_time_ns = (u64) (vlib_time_now (vm) * 1e9);
      cs->owner_thread = CAKE_OWNER_UNSET;
      cs->tin_mode = tin_mode;
      cs->n_tins = cake_tin_counts[tin_mode];
      cs->dscp_to_tin = cake_dscp_tables[tin_mode];

      if (buffer_limit == 0)
	{
	  u32 interval = interval_us > 0 ? interval_us : 100000;
	  cs->buffer_limit =
	    (u32) ((rate_bytes_per_sec * interval * 3) / (1000000 * 2));
	  if (cs->buffer_limit < 65536)
	    cs->buffer_limit = 65536;
	}
      else
	cs->buffer_limit = buffer_limit;

      cs->buffer_usage = 0;
      cs->queued_buffers = 0;

      cs->target_us = target_us > 0 ? target_us : CAKE_TARGET_US_DEFAULT;
      cs->interval_us =
	interval_us > 0 ? interval_us : CAKE_INTERVAL_US_DEFAULT;

      {
	u32 mtu = 1514;
	vnet_sw_interface_t *swif =
	  vnet_get_sw_interface (vnet_get_main (), sw_if_index);
	if (swif)
	  {
	    u32 iface_mtu = swif->mtu[VNET_MTU_IP4];
	    if (iface_mtu > 0)
	      mtu = iface_mtu + 14;
	  }
	u32 wire_mtu = cake_overhead_adjust (cs, mtu);
	cs->mtu_time_us =
	  rate_bytes_per_sec > 0
	    ? (u32) ((u64) wire_mtu * 1000000ULL / rate_bytes_per_sec)
	    : 0;
      }

      cs->p_inc = ~0U / 256;
      cs->p_dec = ~0U / 4096;

      cs->tins =
	clib_mem_alloc_aligned (cs->n_tins * sizeof (cake_tin_t),
				CLIB_CACHE_LINE_BYTES);
      for (u8 t = 0; t < cs->n_tins; t++)
	{
	  cake_tin_init (&cs->tins[t], CAKE_QUANTUM_DEFAULT);
	  cs->tins[t].tin_quantum = 65535 / cs->n_tins;
	}

      cake_sched_resolve_attachment (cm, cs);

      cm->sched_index_by_sw_if_index[sw_if_index] = pool_index;

      vnet_feature_enable_disable ("ip4-output", "ip4-cake-enqueue",
				   sw_if_index, 1, 0, 0);
      vnet_feature_enable_disable ("ip6-output", "ip6-cake-enqueue",
				   sw_if_index, 1, 0, 0);

      cm->n_schedulers++;

      u32 n_threads = vlib_get_n_threads ();
      vec_validate (cm->per_thread, n_threads - 1);

      for (u32 ti = 0; ti < n_threads; ti++)
	{
	  if (!cm->per_thread[ti].random_seed)
	    cm->per_thread[ti].random_seed = 0xdeadbeef ^ (ti + 1);
	}

      vlib_log_notice (cm->log_class,
		       "scheduler enabled on sw_if_index %u "
		       "(rate %llu B/s, overhead %d, tins %u, mode %u)",
		       sw_if_index, rate_bytes_per_sec, (int) overhead_bytes,
		       cs->n_tins, tin_mode);
    }
  else
    {
      if (sw_if_index >= vec_len (cm->sched_index_by_sw_if_index))
	{
	  vlib_worker_thread_barrier_release (vm);
	  return VNET_API_ERROR_NO_SUCH_ENTRY;
	}
      u32 pool_index = cm->sched_index_by_sw_if_index[sw_if_index];
      if (pool_index == ~0)
	{
	  vlib_worker_thread_barrier_release (vm);
	  return VNET_API_ERROR_NO_SUCH_ENTRY;
	}

      cake_sched_t *cs = pool_elt_at_index (cm->schedulers, pool_index);
      u32 owner = cs->owner_thread;

      vnet_feature_enable_disable ("ip4-output", "ip4-cake-enqueue",
				   sw_if_index, 0, 0, 0);
      vnet_feature_enable_disable ("ip6-output", "ip6-cake-enqueue",
				   sw_if_index, 0, 0, 0);

      for (u8 t = 0; t < cs->n_tins; t++)
	cake_tin_drain (vm, cm, cs, &cs->tins[t]);
      clib_mem_free (cs->tins);
      cs->tins = NULL;

      /* Teardown deactivation: the second of the two weight-bearing sites.
       * A dispatch mid-walk may still clear this scheduler's activity bit
       * defensively afterwards; that path deliberately moves no weight, so
       * this subtraction happens exactly once. */
      cake_sched_drr_deactivate (cm, cs);

      for (u32 ti = 0; ti < vec_len (cm->per_thread); ti++)
	{
	  cake_per_thread_t *pt = vec_elt_at_index (cm->per_thread, ti);
	  pt->active_bitmap =
	    clib_bitmap_set (pt->active_bitmap, pool_index, 0);
	}

      cm->sched_index_by_sw_if_index[sw_if_index] = ~0;
      pool_put (cm->schedulers, cs);

      cm->n_schedulers--;

      if (owner != CAKE_OWNER_UNSET &&
	  !cake_worker_has_other_schedulers (cm, NULL, owner))
	{
	  vlib_main_t *owner_vm = vlib_get_main_by_index (owner);
	  vlib_node_set_state (owner_vm, cm->dequeue_node_index,
			       VLIB_NODE_STATE_DISABLED);
	}

      vlib_log_notice (cm->log_class,
		       "scheduler disabled on sw_if_index %u", sw_if_index);
    }

  vlib_worker_thread_barrier_release (vm);
  return 0;
}

void
cake_sched_reset_stats (u32 sw_if_index)
{
  cake_main_t *cm = &cake_main;

  if (sw_if_index >= vec_len (cm->sched_index_by_sw_if_index))
    return;

  u32 pool_index = cm->sched_index_by_sw_if_index[sw_if_index];
  if (pool_index == ~0)
    return;

  cake_sched_t *cs = pool_elt_at_index (cm->schedulers, pool_index);

  cs->enqueued_pkts = 0;
  cs->enqueued_bytes = 0;
  cs->dequeued_pkts = 0;
  cs->dequeued_bytes = 0;
  cs->dropped_pkts = 0;

  for (u8 t = 0; t < cs->n_tins; t++)
    {
      cs->tins[t].packets = 0;
      cs->tins[t].bytes = 0;
      cs->tins[t].drops = 0;
      cs->tins[t].ecn_marks = 0;
    }
}

int
cake_aggregate_create (vlib_main_t *vm, u32 sw_if_index,
		        u64 rate_bytes_per_sec, u32 weight, u32 burst_ns,
		        u32 buffer_limit)
{
  cake_main_t *cm = &cake_main;

  if (!vnet_sw_interface_is_api_valid (vnet_get_main (), sw_if_index))
    return VNET_API_ERROR_INVALID_SW_IF_INDEX;

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return VNET_API_ERROR_INVALID_VALUE;

  if (burst_ns == 0)
    burst_ns = CAKE_AGG_BURST_NS;
  if (burst_ns < CAKE_AGG_BURST_NS_MIN || burst_ns > CAKE_AGG_BURST_NS_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  vlib_worker_thread_barrier_sync (vm);

  vec_validate_init_empty (cm->agg_index_by_sw_if_index, sw_if_index, ~0);
  if (cm->agg_index_by_sw_if_index[sw_if_index] != ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_ENTRY_ALREADY_EXISTS;
    }

  cake_aggregate_t *agg;
  pool_get_aligned_zero (cm->aggregates, agg, CLIB_CACHE_LINE_BYTES);
  u32 agg_idx = agg - cm->aggregates;

  vec_validate_aligned (agg->stats, vlib_get_n_threads () - 1,
			CLIB_CACHE_LINE_BYTES);

  agg->sw_if_index = sw_if_index;
  agg->agg_index = agg_idx;
  agg->level = CAKE_AGG_LEVEL_PORT;
  agg->parent_index = ~0;
  agg->weight = weight ? weight : 1;
  agg->burst_ns = burst_ns;
  agg->rate_bytes_per_sec = rate_bytes_per_sec;
  agg->rate_ns_per_byte_scaled = cake_rate_scaled (rate_bytes_per_sec);
  agg->drr_round_bytes = cake_drr_round_bytes (rate_bytes_per_sec);
  agg->global_shaper_time_ns = (u64) (vlib_time_now (vm) * 1e9);
  cake_drr_shared_init (&agg->drr,
			cake_effective_weight (rate_bytes_per_sec,
					       agg->weight),
			agg->global_shaper_time_ns);

  vec_validate_init_empty (agg->svlan_map, CAKE_SVLAN_MAX - 1, ~0);

  if (buffer_limit == 0)
    {
      u64 derived = (rate_bytes_per_sec * 100000 * 3) / (1000000 * 2);
      if (derived < 1048576)
	derived = 1048576;
      agg->buffer_limit = derived > ~0U ? ~0U : (u32) derived;
    }
  else
    agg->buffer_limit = buffer_limit;

  agg->buffer_usage = 0;

  cm->agg_index_by_sw_if_index[sw_if_index] = agg_idx;

  cake_agg_reparent_all (cm);

  vlib_worker_thread_barrier_release (vm);

  vlib_log_notice (cm->log_class,
		   "aggregate created on sw_if_index %u (rate %llu B/s)",
		   sw_if_index, rate_bytes_per_sec);

  return 0;
}

int
cake_aggregate_delete (vlib_main_t *vm, u32 sw_if_index)
{
  cake_main_t *cm = &cake_main;

  vlib_worker_thread_barrier_sync (vm);

  vec_validate_init_empty (cm->agg_index_by_sw_if_index, sw_if_index, ~0);
  u32 agg_idx = cm->agg_index_by_sw_if_index[sw_if_index];
  if (agg_idx == ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  cake_aggregate_t *agg = pool_elt_at_index (cm->aggregates, agg_idx);
  cake_aggregate_t *child;

  /* A port cannot be deleted out from under its S-VLANs: their parent_index
   * would dangle and their weight contribution would never be unwound. */
  pool_foreach (child, cm->aggregates)
    {
      if (child->parent_index == agg_idx)
	{
	  vlib_worker_thread_barrier_release (vm);
	  return VNET_API_ERROR_INSTANCE_IN_USE;
	}
    }

  cm->agg_index_by_sw_if_index[sw_if_index] = ~0;

  /* Detach before the pool entry goes away, so each member's weight leaves by
   * the same path it arrived on. */
  cake_agg_reparent_all (cm);

  vec_free (agg->stats);
  vec_free (agg->svlan_map);
  pool_put_index (cm->aggregates, agg_idx);

  vlib_worker_thread_barrier_release (vm);

  vlib_log_notice (cm->log_class, "aggregate deleted on sw_if_index %u",
		   sw_if_index);

  return 0;
}

/* Change rate, weight, burst or buffer limit in place. Any field left zero
 * keeps its current value.
 *
 * This exists so a rate change is not delete-and-recreate, which would drop
 * the child out of its parent's W and back in, and would take every member's
 * attachment and outstanding charge with it. The parent's active_weight moves
 * by the delta instead, and only while this aggregate is actually counted in
 * it - an aggregate with no active children contributes nothing to adjust. */
int
cake_aggregate_update (vlib_main_t *vm, u32 sw_if_index, u8 level,
		       u16 svlan_id, u64 rate_bytes_per_sec, u32 weight,
		       u32 burst_ns, u32 buffer_limit)
{
  cake_main_t *cm = &cake_main;
  cake_aggregate_t *agg, *port;
  u32 port_idx, agg_idx;
  u64 old_weight, new_weight;

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return VNET_API_ERROR_INVALID_VALUE;

  if (burst_ns && (burst_ns < CAKE_AGG_BURST_NS_MIN ||
		   burst_ns > CAKE_AGG_BURST_NS_MAX))
    return VNET_API_ERROR_INVALID_VALUE;

  if (level == CAKE_AGG_LEVEL_SVLAN && svlan_id >= CAKE_SVLAN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  vlib_worker_thread_barrier_sync (vm);

  vec_validate_init_empty (cm->agg_index_by_sw_if_index, sw_if_index, ~0);
  port_idx = cm->agg_index_by_sw_if_index[sw_if_index];
  if (port_idx == ~0)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  if (level == CAKE_AGG_LEVEL_SVLAN)
    {
      port = pool_elt_at_index (cm->aggregates, port_idx);
      agg_idx = port->svlan_map[svlan_id];
      if (agg_idx == ~0)
	{
	  vlib_worker_thread_barrier_release (vm);
	  return VNET_API_ERROR_NO_SUCH_ENTRY;
	}
    }
  else
    agg_idx = port_idx;

  agg = pool_elt_at_index (cm->aggregates, agg_idx);

  /* An S-VLAN may not be shaped above the port it hangs off. */
  if (rate_bytes_per_sec && agg->parent_index != ~0 &&
      rate_bytes_per_sec >
	pool_elt_at_index (cm->aggregates, agg->parent_index)
	  ->rate_bytes_per_sec)
    {
      vlib_worker_thread_barrier_release (vm);
      return VNET_API_ERROR_INVALID_VALUE;
    }

  /* The same invariant from the other side: a port may not be lowered
   * beneath an S-VLAN it carries. */
  if (rate_bytes_per_sec && agg->parent_index == ~0)
    {
      cake_aggregate_t *child;
      pool_foreach (child, cm->aggregates)
	{
	  if (child->parent_index == agg_idx &&
	      child->rate_bytes_per_sec > rate_bytes_per_sec)
	    {
	      vlib_worker_thread_barrier_release (vm);
	      return VNET_API_ERROR_INVALID_VALUE;
	    }
	}
    }

  old_weight = agg->drr.effective_weight;

  if (rate_bytes_per_sec)
    {
      agg->rate_bytes_per_sec = rate_bytes_per_sec;
      agg->rate_ns_per_byte_scaled = cake_rate_scaled (rate_bytes_per_sec);
      agg->drr_round_bytes = cake_drr_round_bytes (rate_bytes_per_sec);
    }
  if (weight)
    agg->weight = weight;
  if (burst_ns)
    agg->burst_ns = burst_ns;
  if (buffer_limit)
    agg->buffer_limit = buffer_limit;

  new_weight =
    cake_effective_weight (agg->rate_bytes_per_sec, agg->weight);
  agg->drr.effective_weight = new_weight;

  if (agg->parent_index != ~0 && agg->n_active_children > 0 &&
      new_weight != old_weight)
    {
      cake_aggregate_t *parent =
	pool_elt_at_index (cm->aggregates, agg->parent_index);

      if (new_weight > old_weight)
	__atomic_fetch_add (&parent->active_weight, new_weight - old_weight,
			    __ATOMIC_RELAXED);
      else
	__atomic_fetch_sub (&parent->active_weight, old_weight - new_weight,
			    __ATOMIC_RELAXED);
    }

  vlib_worker_thread_barrier_release (vm);

  vlib_log_notice (cm->log_class,
		   "aggregate updated on sw_if_index %u level %u svlan %u "
		   "(rate %llu B/s, weight %u)",
		   sw_if_index, level, svlan_id, agg->rate_bytes_per_sec,
		   agg->weight);

  return 0;
}

static clib_error_t *
cake_aggregate_set_command_fn (vlib_main_t *vm, unformat_input_t *input,
				vlib_cli_command_t *cmd)
{
  u32 sw_if_index = ~0;
  u64 rate_kbps = 0;
  u32 svlan_id = ~0, svlan_id_end = ~0;
  u32 weight = 0, burst_ms = 0;
  u8 is_disable = 0, is_update = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface,
		    vnet_get_main (), &sw_if_index))
	;
      else if (unformat (input, "svlan %u-%u", &svlan_id, &svlan_id_end))
	;
      else if (unformat (input, "svlan %u", &svlan_id))
	svlan_id_end = svlan_id;
      else if (unformat (input, "rate %llu", &rate_kbps))
	;
      else if (unformat (input, "weight %u", &weight))
	;
      else if (unformat (input, "burst %u", &burst_ms))
	;
      else if (unformat (input, "update"))
	is_update = 1;
      else if (unformat (input, "disable"))
	is_disable = 1;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  if (sw_if_index == ~0)
    return clib_error_return (0, "interface required");

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return clib_error_return (0, "weight must be %u-%u", CAKE_WEIGHT_MIN,
			      CAKE_WEIGHT_MAX);

  /* Widened before the multiply: a u32 product wraps at ~4295 ms, and some
   * wrapped values land back inside the valid burst range. */
  u64 burst_ns_wide = (u64) burst_ms * 1000000;
  if (burst_ns_wide > CAKE_AGG_BURST_NS_MAX)
    return clib_error_return (0, "burst must be %llu-%llu ms",
			      CAKE_AGG_BURST_NS_MIN / 1000000,
			      CAKE_AGG_BURST_NS_MAX / 1000000);
  u32 burst_ns = (u32) burst_ns_wide;
  int rv;

  if (is_update)
    rv = cake_aggregate_update (
      vm, sw_if_index,
      svlan_id != ~0 ? CAKE_AGG_LEVEL_SVLAN : CAKE_AGG_LEVEL_PORT,
      svlan_id != ~0 ? (u16) svlan_id : 0, rate_kbps * 1000 / 8, weight,
      burst_ns, 0);
  else if (svlan_id != ~0)
    {
      if (svlan_id_end < svlan_id || svlan_id_end >= CAKE_SVLAN_MAX)
	return clib_error_return (0, "svlan range must be 0-%u ascending",
				  CAKE_SVLAN_MAX - 1);
      if (is_disable)
	rv = cake_svlan_aggregate_delete (vm, sw_if_index, (u16) svlan_id);
      else
	{
	  if (rate_kbps == 0)
	    return clib_error_return (0, "rate required (kbps)");
	  rv = cake_svlan_aggregate_create (vm, sw_if_index, (u16) svlan_id,
					    (u16) svlan_id_end,
					    rate_kbps * 1000 / 8, weight,
					    burst_ns, 0);
	}
    }
  else if (is_disable)
    rv = cake_aggregate_delete (vm, sw_if_index);
  else
    {
      if (rate_kbps == 0)
	return clib_error_return (0, "rate required (kbps)");
      rv = cake_aggregate_create (vm, sw_if_index, rate_kbps * 1000 / 8,
				  weight, burst_ns, 0);
    }

  if (rv)
    return clib_error_return (0, "cake_aggregate returned %d", rv);

  return 0;
}

VLIB_CLI_COMMAND (cake_aggregate_set_command, static) = {
  .path = "set cake aggregate",
  .short_help = "set cake aggregate <interface> [svlan <id>[-<id>]] "
		"rate <kbps> [weight <1-256>] [burst <ms>] [update|disable]",
  .function = cake_aggregate_set_command_fn,
};

static void
cake_agg_show_children (vlib_main_t *vm, cake_main_t *cm,
			cake_aggregate_t *agg, const char *indent)
{
  u64 active_weight =
    __atomic_load_n (&agg->active_weight, __ATOMIC_RELAXED);
  u32 n_active =
    __atomic_load_n (&agg->n_active_children, __ATOMIC_RELAXED);
  u64 shaped_pkts, shaped_bytes, backpressure, drr_blocked, parent_blocked;
  cake_sched_t *cs;

  cake_agg_stats_sum (agg, &shaped_pkts, &shaped_bytes, &backpressure,
		      &drr_blocked, &parent_blocked);

  vlib_cli_output (vm, "%sactive %u children, W %llu, round %llu bytes",
		   indent, n_active, active_weight, agg->drr_round_bytes);
  vlib_cli_output (vm,
		   "%sbuffer %u/%u bytes, shaped %llu pkts %llu bytes",
		   indent, agg->buffer_usage, agg->buffer_limit, shaped_pkts,
		   shaped_bytes);
  vlib_cli_output (vm,
		   "%sbackpressure %llu, drr_blocked %llu, parent_blocked %llu",
		   indent, backpressure, drr_blocked, parent_blocked);

  /* No child list exists - arbitration is child-driven, so nothing on the hot
   * path ever walks one. Rebuilding it is a CLI cost only. */
  pool_foreach (cs, cm->schedulers)
    {
      u64 share_x10;

      if (cs->aggregate_index != agg->agg_index)
	continue;

      share_x10 = (cs->drr.active && active_weight)
		    ? (cs->drr.effective_weight * 1000) / active_weight
		    : 0;

      vlib_cli_output (
	vm,
	"%s  %U: weight %llu (x%u), share %llu.%llu%%, %s, quantum %llu bytes, "
	"deficit %lld, drr_blocked %llu, parent_blocked %llu",
	indent, format_vnet_sw_if_index_name, vnet_get_main (),
	cs->sw_if_index, cs->drr.effective_weight, cs->weight, share_x10 / 10,
	share_x10 % 10, cs->drr.active ? "active" : "idle",
	cake_drr_quantum (agg->drr_round_bytes, cs->drr.effective_weight,
			  active_weight),
	cs->drr.deficit, cs->drr_blocked, cs->parent_blocked);
    }
}

static clib_error_t *
cake_aggregate_show_command_fn (vlib_main_t *vm, unformat_input_t *input,
				 vlib_cli_command_t *cmd)
{
  cake_main_t *cm = &cake_main;
  u32 sw_if_index = ~0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface,
		    vnet_get_main (), &sw_if_index))
	;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  u32 found = 0;
  cake_aggregate_t *agg;
  pool_foreach (agg, cm->aggregates)
    {
      cake_aggregate_t *svlan;
      u64 port_weight;

      if (agg->level != CAKE_AGG_LEVEL_PORT)
	continue;
      if (sw_if_index != ~0 && agg->sw_if_index != sw_if_index)
	continue;

      vlib_cli_output (vm, "  %U: rate %llu B/s (%llu kbps), burst %u ms",
		       format_vnet_sw_if_index_name, vnet_get_main (),
		       agg->sw_if_index, agg->rate_bytes_per_sec,
		       agg->rate_bytes_per_sec * 8 / 1000,
		       agg->burst_ns / 1000000);
      cake_agg_show_children (vm, cm, agg, "    ");

      port_weight = __atomic_load_n (&agg->active_weight, __ATOMIC_RELAXED);

      pool_foreach (svlan, cm->aggregates)
	{
	  u64 share_x10;

	  if (svlan->parent_index != agg->agg_index)
	    continue;

	  share_x10 =
	    (svlan->n_active_children && port_weight)
	      ? (svlan->drr.effective_weight * 1000) / port_weight
	      : 0;

	  vlib_cli_output (
	    vm,
	    "    svlan %u-%u: rate %llu B/s (%llu kbps), weight x%u, "
	    "burst %u ms, share %llu.%llu%%",
	    svlan->svlan_id, svlan->svlan_id_end, svlan->rate_bytes_per_sec,
	    svlan->rate_bytes_per_sec * 8 / 1000, svlan->weight,
	    svlan->burst_ns / 1000000, share_x10 / 10, share_x10 % 10);
	  cake_agg_show_children (vm, cm, svlan, "      ");
	}

      found++;
    }

  if (found == 0)
    vlib_cli_output (vm, "  no aggregates configured");

  return 0;
}

VLIB_CLI_COMMAND (cake_aggregate_show_command, static) = {
  .path = "show cake aggregate",
  .short_help = "show cake aggregate [<interface>]",
  .function = cake_aggregate_show_command_fn,
};

static const char *cake_tin_mode_names[] = {
  [CAKE_TIN_MODE_BESTEFFORT] = "besteffort",
  [CAKE_TIN_MODE_DIFFSERV3] = "diffserv3",
  [CAKE_TIN_MODE_DIFFSERV4] = "diffserv4",
  [CAKE_TIN_MODE_DIFFSERV8] = "diffserv8",
};

static clib_error_t *
cake_sched_set_command_fn (vlib_main_t *vm, unformat_input_t *input,
			   vlib_cli_command_t *cmd)
{
  u32 sw_if_index = ~0;
  u64 rate_kbps = 0;
  i16 overhead_bytes = 0;
  u8 atm_mode = 0;
  u8 mpu = 64;
  u8 is_disable = 0;
  u8 tin_mode = CAKE_TIN_MODE_BESTEFFORT;
  u32 weight = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface,
		    vnet_get_main (), &sw_if_index))
	;
      else if (unformat (input, "rate %llu", &rate_kbps))
	;
      else if (unformat (input, "overhead %d", &overhead_bytes))
	;
      else if (unformat (input, "atm"))
	atm_mode = 1;
      else if (unformat (input, "ptm"))
	atm_mode = 2;
      else if (unformat (input, "noatm"))
	atm_mode = 0;
      else if (unformat (input, "mpu %d", &mpu))
	;
      else if (unformat (input, "weight %u", &weight))
	;
      else if (unformat (input, "besteffort"))
	tin_mode = CAKE_TIN_MODE_BESTEFFORT;
      else if (unformat (input, "diffserv3"))
	tin_mode = CAKE_TIN_MODE_DIFFSERV3;
      else if (unformat (input, "diffserv4"))
	tin_mode = CAKE_TIN_MODE_DIFFSERV4;
      else if (unformat (input, "diffserv8"))
	tin_mode = CAKE_TIN_MODE_DIFFSERV8;
      else if (unformat (input, "dsl-pppoe-atm"))
	{
	  overhead_bytes = 32;
	  atm_mode = 1;
	  mpu = 64;
	}
      else if (unformat (input, "dsl-pppoe-ptm"))
	{
	  overhead_bytes = 27;
	  atm_mode = 2;
	  mpu = 64;
	}
      else if (unformat (input, "docsis"))
	{
	  overhead_bytes = 18;
	  mpu = 64;
	}
      else if (unformat (input, "gpon"))
	{
	  overhead_bytes = 4;
	  mpu = 64;
	}
      else if (unformat (input, "ethernet"))
	{
	  overhead_bytes = 14;
	  mpu = 64;
	}
      else if (unformat (input, "disable"))
	is_disable = 1;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  if (sw_if_index == ~0)
    return clib_error_return (0, "interface required");

  if (!is_disable && rate_kbps == 0)
    return clib_error_return (0, "rate required (kbps)");

  u64 rate_bytes = rate_kbps * 1000 / 8;

  if (weight && (weight < CAKE_WEIGHT_MIN || weight > CAKE_WEIGHT_MAX))
    return clib_error_return (0, "weight must be %u-%u", CAKE_WEIGHT_MIN,
			      CAKE_WEIGHT_MAX);

  int rv = cake_sched_enable_disable (vm, sw_if_index, !is_disable,
				      rate_bytes, tin_mode, overhead_bytes,
				      atm_mode, mpu, 0, 0, 0, 0, weight);

  if (rv)
    return clib_error_return (0, "cake_sched_enable_disable returned %d", rv);

  return 0;
}

VLIB_CLI_COMMAND (cake_sched_set_command, static) = {
  .path = "set cake scheduler",
  .short_help = "set cake scheduler <interface> rate <kbps> "
		"[besteffort|diffserv3|diffserv4|diffserv8] "
		"[overhead <bytes>|ethernet|docsis|dsl-pppoe-atm|dsl-pppoe-ptm|gpon] "
		"[atm|ptm|noatm] [mpu <bytes>] [weight <1-256>] [disable]",
  .function = cake_sched_set_command_fn,
};

static clib_error_t *
cake_sched_show_command_fn (vlib_main_t *vm, unformat_input_t *input,
			    vlib_cli_command_t *cmd)
{
  cake_main_t *cm = &cake_main;
  u32 sw_if_index = ~0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface,
		    vnet_get_main (), &sw_if_index))
	;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  u32 found = 0;
  cake_sched_t *cs;
  pool_foreach (cs, cm->schedulers)
    {
      if (sw_if_index != ~0 && cs->sw_if_index != sw_if_index)
	continue;

      vlib_cli_output (
	vm,
	"  %U: rate %llu B/s (%llu kbps), overhead %d, "
	"mode %s, tins %u, owner thread %u",
	format_vnet_sw_if_index_name, vnet_get_main (), cs->sw_if_index,
	cs->rate_bytes_per_sec, cs->rate_bytes_per_sec * 8 / 1000,
	(int) cs->overhead_bytes, cake_tin_mode_names[cs->tin_mode],
	cs->n_tins, cs->owner_thread);

      vlib_cli_output (vm,
		       "    queue %u pkts %u/%u bytes",
		       cs->queued_buffers, cs->buffer_usage, cs->buffer_limit);

      vlib_cli_output (vm,
		       "    enqueued: %llu pkts %llu bytes, "
		       "dequeued: %llu pkts %llu bytes, "
		       "dropped: %llu pkts",
		       cs->enqueued_pkts, cs->enqueued_bytes,
		       cs->dequeued_pkts, cs->dequeued_bytes,
		       cs->dropped_pkts);

      for (u8 t = 0; t < cs->n_tins; t++)
	{
	  cake_tin_t *tin = &cs->tins[t];
	  vlib_cli_output (vm,
			   "    tin %u: flows %u (%u sparse, %u bulk), "
			   "pkts %llu, drops %llu, ecn %llu",
			   t, tin->flow_count, tin->sparse_flow_count,
			   tin->bulk_flow_count, tin->packets, tin->drops,
			   tin->ecn_marks);
	}

      found++;
    }

  if (found == 0)
    vlib_cli_output (vm, "  no schedulers configured");

  return 0;
}

VLIB_CLI_COMMAND (cake_sched_show_command, static) = {
  .path = "show cake scheduler",
  .short_help = "show cake scheduler [<interface>]",
  .function = cake_sched_show_command_fn,
};

static clib_error_t *
osvbng_qos_sched_init (vlib_main_t *vm)
{
  cake_main_t *cm = &cake_main;

  cm->vlib_main = vm;
  cm->vnet_main = vnet_get_main ();
  cm->log_class = vlib_log_register_class ("osvbng_qos_sched", 0);
  cm->n_schedulers = 0;

  cm->ip4_enqueue_node_index = ip4_cake_enqueue_node.index;
  cm->ip6_enqueue_node_index = ip6_cake_enqueue_node.index;
  cm->dequeue_node_index = cake_dequeue_node.index;

  cm->ip4_output_arc_index = vnet_get_feature_arc_index ("ip4-output");
  cm->ip6_output_arc_index = vnet_get_feature_arc_index ("ip6-output");

  cm->fq_ip4_index =
    vlib_frame_queue_main_init (ip4_cake_enqueue_node.index, 0);
  cm->fq_ip6_index =
    vlib_frame_queue_main_init (ip6_cake_enqueue_node.index, 0);

  cake_cobalt_cache_init ();

  cake_quantum_div[0] = ~0;
  for (u32 i = 1; i <= CAKE_QUEUES; i++)
    cake_quantum_div[i] = 65535 / i;

  vlib_log_notice (
    cm->log_class,
    "initialized (Phase 6: FQ + DRR + COBALT + DiffServ + Triple Isolation)");

  return 0;
}

VLIB_INIT_FUNCTION (osvbng_qos_sched_init);

/* sw_if_index values are recycled, so a scheduler outliving its interface
 * blocks the next one to reuse the index: enable returns ENTRY_ALREADY_EXISTS
 * while the stale entry, its feature config gone, shapes nothing. */
static clib_error_t *
cake_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index, u32 is_add)
{
  cake_main_t *cm = &cake_main;

  if (is_add)
    return 0;

  if (sw_if_index < vec_len (cm->sched_index_by_sw_if_index) &&
      cm->sched_index_by_sw_if_index[sw_if_index] != ~0)
    cake_sched_enable_disable (cm->vlib_main, sw_if_index, 0 /* disable */, 0,
			       0, 0, 0, 0, 0, 0, 0, 0, 0);

  if (sw_if_index < vec_len (cm->agg_index_by_sw_if_index) &&
      cm->agg_index_by_sw_if_index[sw_if_index] != ~0)
    {
      u32 agg_idx = cm->agg_index_by_sw_if_index[sw_if_index];
      cake_aggregate_t *child;
      u16 *tags = NULL, *tag;

      /* Port delete refuses while S-VLANs hang off it, so take them down
       * first. Collect before deleting: the pool moves underneath. */
      pool_foreach (child, cm->aggregates)
	{
	  if (child->parent_index == agg_idx)
	    vec_add1 (tags, child->svlan_id);
	}

      vec_foreach (tag, tags)
	cake_svlan_aggregate_delete (cm->vlib_main, sw_if_index, *tag);
      vec_free (tags);

      cake_aggregate_delete (cm->vlib_main, sw_if_index);
    }

  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (cake_sw_interface_add_del);

VLIB_PLUGIN_REGISTER () = {
  .version = "6.0.0",
  .description = "osvbng QoS Scheduler Plugin (CAKE)",
};

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
