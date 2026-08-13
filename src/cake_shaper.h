/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - Shaper and admission primitives
 *
 * The arithmetic behind an aggregate's rate gate and its buffer admission,
 * split from the plugin types that resolve which aggregate is meant. Both are
 * multi-writer: every worker shaping into an aggregate executes them per
 * packet, so both are written as a single linearizable step rather than
 * check-then-act.
 *
 * DEPENDENCY CONTRACT: same as cake_drr.h. Fixed-width integers and GCC
 * atomic builtins only, no vlib/vnet/vppinfra, so tests/drr_harness.c can
 * exercise the shipped arithmetic under pthreads without a VPP environment.
 * Branch-prediction hints belong at the call sites, where the VPP macros are
 * available.
 *
 * Required from the including translation unit: u8/u32/u64,
 * static_always_inline.
 */

#ifndef __included_cake_shaper_h__
#define __included_cake_shaper_h__

/* ns/byte in fixed point: plain integers truncate to 0 at and above 1e9 B/s
 * and still round 3 Gbit/s up to 4 Gbit/s. 16 fractional bits hold the error
 * under 0.01% to 100 Gbit/s. */
#define CAKE_RATE_FRAC_BITS 16

static_always_inline u64
cake_rate_scaled (u64 rate_bytes_per_sec)
{
  return rate_bytes_per_sec > 0
	   ? (1000000000ULL << CAKE_RATE_FRAC_BITS) / rate_bytes_per_sec
	   : 0;
}

static_always_inline u64
cake_cost_ns (u32 adj_len, u64 rate_ns_per_byte_scaled)
{
  return ((u64) adj_len * rate_ns_per_byte_scaled) >> CAKE_RATE_FRAC_BITS;
}

/* Advance a shaper's virtual time by one packet's cost, or refuse. Returns 0
 * with nothing written when the shaper is ahead of the wall clock.
 *
 * burst_ns caps how much idle credit can accrue: without it a long-idle
 * aggregate releases its whole backlog at line rate on the next packet. */
static_always_inline u8
cake_shaper_gate_take (u64 *virtual_time_ns, u64 cost_ns, u64 now_ns,
		       u64 burst_ns)
{
  u64 old_time, base, new_time;

  do
    {
      old_time = __atomic_load_n (virtual_time_ns, __ATOMIC_ACQUIRE);

      if (old_time > now_ns)
	return 0;

      /* old_time stays as loaded: it is the CAS expected value, so clamping it
       * would guarantee the exchange never succeeds. */
      base = old_time;
      if (now_ns - base > burst_ns)
	base = now_ns - burst_ns;

      new_time = base + cost_ns;
    }
  while (!__atomic_compare_exchange_n (virtual_time_ns, &old_time, new_time, 1,
				       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

  return 1;
}

/* Reserve queue occupancy, or refuse. Add first and verify after: a
 * load-compare-add would let every worker admit against the same under-limit
 * read, overshooting by one packet per worker. Over-admission is bounded to
 * the packets concurrently in flight, and every one of them is unwound. */
static_always_inline u8
cake_agg_admit (u32 *usage, u32 limit, u32 len)
{
  u32 prev = __atomic_fetch_add (usage, len, __ATOMIC_RELAXED);

  if (prev + len > limit)
    {
      __atomic_fetch_sub (usage, len, __ATOMIC_RELAXED);
      return 0;
    }

  return 1;
}

static_always_inline void
cake_agg_release (u32 *usage, u32 len)
{
  __atomic_fetch_sub (usage, len, __ATOMIC_RELAXED);
}

#endif /* __included_cake_shaper_h__ */

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
