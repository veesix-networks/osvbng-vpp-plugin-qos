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

  rv = cake_sched_enable_disable (cm->vlib_main, sw_if_index, mp->is_enable,
				  rate_bytes_per_sec, tin_mode,
				  overhead_bytes, atm_mode, mpu, buffer_limit,
				  target_us, interval_us, flags);

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

  rv = cake_aggregate_create (cm->vlib_main, ntohl (mp->sw_if_index),
			       clib_net_to_host_u64 (mp->rate_bytes_per_sec),
			       ntohl (mp->buffer_limit));

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
