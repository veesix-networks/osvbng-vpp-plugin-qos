/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - API message handlers
 */

#include <vnet/vnet.h>
#include <vlibmemory/api.h>

#include <osvbng_qos_sched/osvbng_qos_sched.h>

#include <vnet/format_fns.h>

#include <osvbng_qos_sched/osvbng_qos_sched.api_enum.h>
#include <osvbng_qos_sched/osvbng_qos_sched.api_types.h>

#define REPLY_MSG_ID_BASE cm->msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_osvbng_cake_sched_enable_disable_t_handler (
  vl_api_osvbng_cake_sched_enable_disable_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_sched_enable_disable_reply_t *rmp;
  int rv = 0;

  u32 sw_if_index = ntohl (mp->sw_if_index);
  u64 rate_bytes_per_sec = clib_net_to_host_u64 (mp->rate_bytes_per_sec);
  u8 tin_mode = (u8) mp->tin_mode;
  i16 overhead_bytes = (i16) ntohs (mp->overhead_bytes);
  u8 atm_mode = (u8) mp->atm_mode;
  u8 mpu = mp->mpu;
  u32 buffer_limit = ntohl (mp->buffer_limit);
  u32 target_us = ntohl (mp->target_us);
  u32 interval_us = ntohl (mp->interval_us);
  u32 flags = ntohl (mp->flags);

  /* v1 carries no weight, so the child's share is its rate alone. */
  rv = cake_sched_enable_disable (cm->vlib_main, sw_if_index, mp->is_enable,
				  rate_bytes_per_sec, tin_mode,
				  overhead_bytes, atm_mode, mpu, buffer_limit,
				  target_us, interval_us, flags, 0);

  REPLY_MACRO (VL_API_OSVBNG_CAKE_SCHED_ENABLE_DISABLE_REPLY);
}

static void
send_cake_sched_details (cake_sched_t *cs, vl_api_registration_t *reg,
			 u32 context)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_sched_details_t *rmp;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));

  rmp->_vl_msg_id =
    ntohs (VL_API_OSVBNG_CAKE_SCHED_DETAILS + cm->msg_id_base);
  rmp->context = context;
  rmp->sw_if_index = ntohl (cs->sw_if_index);
  rmp->rate_bytes_per_sec = clib_host_to_net_u64 (cs->rate_bytes_per_sec);
  rmp->tin_mode = cs->tin_mode;
  rmp->tin_cnt = cs->n_tins;
  rmp->buffer_usage = ntohl (cs->buffer_usage);
  rmp->buffer_limit = ntohl (cs->buffer_limit);

  for (u8 t = 0; t < cs->n_tins; t++)
    {
      rmp->tin_packets[t] = clib_host_to_net_u64 (cs->tins[t].packets);
      rmp->tin_bytes[t] = clib_host_to_net_u64 (cs->tins[t].bytes);
      rmp->tin_drops[t] = clib_host_to_net_u64 (cs->tins[t].drops);
      rmp->tin_ecn_marks[t] = clib_host_to_net_u64 (cs->tins[t].ecn_marks);
      rmp->tin_sparse_flows[t] = ntohl (cs->tins[t].sparse_flow_count);
      rmp->tin_bulk_flows[t] = ntohl (cs->tins[t].bulk_flow_count);
    }

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
vl_api_osvbng_cake_sched_dump_t_handler (
  vl_api_osvbng_cake_sched_dump_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_registration_t *reg;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  u32 filter_sw_if_index = ntohl (mp->sw_if_index);

  cake_sched_t *cs;
  pool_foreach (cs, cm->schedulers)
    {
      if (filter_sw_if_index != ~0 && cs->sw_if_index != filter_sw_if_index)
	continue;
      send_cake_sched_details (cs, reg, mp->context);
    }
}

static void
vl_api_osvbng_cake_sched_reset_stats_t_handler (
  vl_api_osvbng_cake_sched_reset_stats_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_sched_reset_stats_reply_t *rmp;
  int rv = 0;

  u32 sw_if_index = ntohl (mp->sw_if_index);
  cake_sched_reset_stats (sw_if_index);

  REPLY_MACRO (VL_API_OSVBNG_CAKE_SCHED_RESET_STATS_REPLY);
}

static void
vl_api_osvbng_cake_aggregate_create_t_handler (
  vl_api_osvbng_cake_aggregate_create_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_create_reply_t *rmp;
  int rv = 0;

  /* v1 carries neither weight nor burst; both take their defaults. */
  rv = cake_aggregate_create (cm->vlib_main, ntohl (mp->sw_if_index),
			       clib_net_to_host_u64 (mp->rate_bytes_per_sec), 0,
			       0, ntohl (mp->buffer_limit));

  REPLY_MACRO (VL_API_OSVBNG_CAKE_AGGREGATE_CREATE_REPLY);
}

static void
vl_api_osvbng_cake_aggregate_delete_t_handler (
  vl_api_osvbng_cake_aggregate_delete_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_delete_reply_t *rmp;
  int rv = 0;

  rv = cake_aggregate_delete (cm->vlib_main, ntohl (mp->sw_if_index));

  REPLY_MACRO (VL_API_OSVBNG_CAKE_AGGREGATE_DELETE_REPLY);
}

static void
send_cake_aggregate_details (cake_aggregate_t *agg,
			      vl_api_registration_t *reg, u32 context)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_details_t *rmp;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));

  rmp->_vl_msg_id =
    ntohs (VL_API_OSVBNG_CAKE_AGGREGATE_DETAILS + cm->msg_id_base);
  rmp->context = context;
  rmp->sw_if_index = ntohl (agg->sw_if_index);
  rmp->rate_bytes_per_sec = clib_host_to_net_u64 (agg->rate_bytes_per_sec);
  rmp->buffer_usage = ntohl (agg->buffer_usage);
  rmp->buffer_limit = ntohl (agg->buffer_limit);
  /* The v1 message has no field for the gate-side counters; they reach an
   * operator through the CLI until the _v2 messages land. */
  u64 shaped_pkts, shaped_bytes, backpressure_events;
  u64 drr_blocked, parent_blocked;
  cake_agg_stats_sum (agg, &shaped_pkts, &shaped_bytes, &backpressure_events,
		      &drr_blocked, &parent_blocked);

  rmp->shaped_pkts = clib_host_to_net_u64 (shaped_pkts);
  rmp->shaped_bytes = clib_host_to_net_u64 (shaped_bytes);
  rmp->backpressure_events = clib_host_to_net_u64 (backpressure_events);

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
vl_api_osvbng_cake_aggregate_dump_t_handler (
  vl_api_osvbng_cake_aggregate_dump_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_registration_t *reg;

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  u32 filter_sw_if_index = ntohl (mp->sw_if_index);

  cake_aggregate_t *agg;
  pool_foreach (agg, cm->aggregates)
    {
      if (filter_sw_if_index != ~0 && agg->sw_if_index != filter_sw_if_index)
	continue;
      send_cake_aggregate_details (agg, reg, mp->context);
    }
}


static void
vl_api_osvbng_cake_sched_v2_enable_disable_t_handler (
  vl_api_osvbng_cake_sched_v2_enable_disable_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_sched_v2_enable_disable_reply_t *rmp;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  rv = cake_sched_enable_disable (
    cm->vlib_main, ntohl (mp->sw_if_index), mp->is_enable,
    clib_net_to_host_u64 (mp->rate_bytes_per_sec), mp->tin_mode,
    (i16) ntohs (mp->overhead_bytes), mp->atm_mode, mp->mpu,
    ntohl (mp->buffer_limit), ntohl (mp->target_us), ntohl (mp->interval_us),
    ntohl (mp->flags), ntohl (mp->weight));

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_OSVBNG_CAKE_SCHED_V2_ENABLE_DISABLE_REPLY);
}

static void
vl_api_osvbng_cake_aggregate_v2_create_t_handler (
  vl_api_osvbng_cake_aggregate_v2_create_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_v2_create_reply_t *rmp;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  /* An unknown level must not fall through to the port path. */
  if (mp->level == OSVBNG_CAKE_AGG_LEVEL_SVLAN)
    rv = cake_svlan_aggregate_create (
      cm->vlib_main, ntohl (mp->sw_if_index), ntohs (mp->svlan_id),
      ntohs (mp->svlan_id_end), clib_net_to_host_u64 (mp->rate_bytes_per_sec),
      ntohl (mp->weight), ntohl (mp->burst_ns), ntohl (mp->buffer_limit));
  else if (mp->level == OSVBNG_CAKE_AGG_LEVEL_PORT)
    rv = cake_aggregate_create (
      cm->vlib_main, ntohl (mp->sw_if_index),
      clib_net_to_host_u64 (mp->rate_bytes_per_sec), ntohl (mp->weight),
      ntohl (mp->burst_ns), ntohl (mp->buffer_limit));
  else
    rv = VNET_API_ERROR_INVALID_VALUE;

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_OSVBNG_CAKE_AGGREGATE_V2_CREATE_REPLY);
}

static void
vl_api_osvbng_cake_aggregate_v2_delete_t_handler (
  vl_api_osvbng_cake_aggregate_v2_delete_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_v2_delete_reply_t *rmp;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  if (mp->level == OSVBNG_CAKE_AGG_LEVEL_SVLAN)
    rv = cake_svlan_aggregate_delete (cm->vlib_main, ntohl (mp->sw_if_index),
				      ntohs (mp->svlan_id));
  else if (mp->level == OSVBNG_CAKE_AGG_LEVEL_PORT)
    rv = cake_aggregate_delete (cm->vlib_main, ntohl (mp->sw_if_index));
  else
    rv = VNET_API_ERROR_INVALID_VALUE;

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_OSVBNG_CAKE_AGGREGATE_V2_DELETE_REPLY);
}

static void
vl_api_osvbng_cake_aggregate_v2_update_t_handler (
  vl_api_osvbng_cake_aggregate_v2_update_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_aggregate_v2_update_reply_t *rmp;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  if (mp->level == OSVBNG_CAKE_AGG_LEVEL_PORT ||
      mp->level == OSVBNG_CAKE_AGG_LEVEL_SVLAN)
    rv = cake_aggregate_update (cm->vlib_main, ntohl (mp->sw_if_index),
				mp->level, ntohs (mp->svlan_id),
				clib_net_to_host_u64 (mp->rate_bytes_per_sec),
				ntohl (mp->weight), ntohl (mp->burst_ns),
				ntohl (mp->buffer_limit));
  else
    rv = VNET_API_ERROR_INVALID_VALUE;

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_OSVBNG_CAKE_AGGREGATE_V2_UPDATE_REPLY);
}

static void
send_cake_aggregate_v2_details (cake_main_t *cm, cake_aggregate_t *agg,
				vl_api_registration_t *reg, u32 context)
{
  vl_api_osvbng_cake_aggregate_v2_details_t *rmp;
  u64 shaped_pkts, shaped_bytes, backpressure, drr_blocked, parent_blocked;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));

  rmp->_vl_msg_id =
    ntohs (VL_API_OSVBNG_CAKE_AGGREGATE_V2_DETAILS + cm->msg_id_base);
  rmp->context = context;
  rmp->sw_if_index = ntohl (agg->sw_if_index);
  rmp->level = agg->level;
  rmp->parent_sw_if_index =
    agg->parent_index == ~0
      ? ~0
      : ntohl (pool_elt_at_index (cm->aggregates, agg->parent_index)
		 ->sw_if_index);
  rmp->svlan_id = ntohs (agg->svlan_id);
  rmp->svlan_id_end = ntohs (agg->svlan_id_end);
  rmp->rate_bytes_per_sec = clib_host_to_net_u64 (agg->rate_bytes_per_sec);
  rmp->weight = ntohl (agg->weight);
  rmp->burst_ns = ntohl (agg->burst_ns);
  rmp->buffer_usage = ntohl (agg->buffer_usage);
  rmp->buffer_limit = ntohl (agg->buffer_limit);
  rmp->effective_weight =
    clib_host_to_net_u64 (agg->drr.effective_weight);
  rmp->active_weight = clib_host_to_net_u64 (
    __atomic_load_n (&agg->active_weight, __ATOMIC_RELAXED));
  rmp->n_active_children =
    ntohl (__atomic_load_n (&agg->n_active_children, __ATOMIC_RELAXED));

  cake_agg_stats_sum (agg, &shaped_pkts, &shaped_bytes, &backpressure,
		      &drr_blocked, &parent_blocked);
  rmp->shaped_pkts = clib_host_to_net_u64 (shaped_pkts);
  rmp->shaped_bytes = clib_host_to_net_u64 (shaped_bytes);
  rmp->backpressure_events = clib_host_to_net_u64 (backpressure);
  rmp->drr_blocked = clib_host_to_net_u64 (drr_blocked);
  rmp->parent_blocked = clib_host_to_net_u64 (parent_blocked);

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
vl_api_osvbng_cake_aggregate_v2_dump_t_handler (
  vl_api_osvbng_cake_aggregate_v2_dump_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_registration_t *reg;
  cake_aggregate_t *agg;
  u32 sw_if_index = ntohl (mp->sw_if_index);

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  /* A port and its S-VLANs both report against the port's sw_if_index, so a
   * filtered dump has to compare the whole chain, not just this entry. */
  pool_foreach (agg, cm->aggregates)
    {
      if (sw_if_index != ~0 && agg->sw_if_index != sw_if_index)
	continue;
      send_cake_aggregate_v2_details (cm, agg, reg, mp->context);
    }
}

static void
vl_api_osvbng_cake_capabilities_t_handler (
  vl_api_osvbng_cake_capabilities_t *mp)
{
  cake_main_t *cm = &cake_main;
  vl_api_osvbng_cake_capabilities_reply_t *rmp;
  int rv = 0;

  REPLY_MACRO2 (
    VL_API_OSVBNG_CAKE_CAPABILITIES_REPLY, ({
      rmp->version = ntohl (3);
      rmp->max_levels = 2;
      rmp->max_svlan_id = ntohl (CAKE_SVLAN_MAX - 1);
      rmp->weight_min = ntohl (CAKE_WEIGHT_MIN);
      rmp->weight_max = ntohl (CAKE_WEIGHT_MAX);
      rmp->features = ntohl (OSVBNG_CAKE_FEATURE_SVLAN_TIER |
			     OSVBNG_CAKE_FEATURE_WEIGHTED_DRR |
			     OSVBNG_CAKE_FEATURE_AGG_BURST |
			     OSVBNG_CAKE_FEATURE_AGG_UPDATE);
    }));
}

#include <osvbng_qos_sched/osvbng_qos_sched.api.c>

static clib_error_t *
osvbng_qos_sched_api_init (vlib_main_t *vm)
{
  cake_main_t *cm = &cake_main;

  cm->msg_id_base = setup_message_id_table ();

  return 0;
}

VLIB_INIT_FUNCTION (osvbng_qos_sched_api_init) = {
  .runs_after = VLIB_INITS ("osvbng_qos_sched_init"),
};

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
