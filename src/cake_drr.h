/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osvbng QoS Scheduler Plugin - Deficit round robin core
 *
 * Arbitration is child-driven: every child carries its own deficit and
 * refuses itself when it is spent. No parent walks a child list on the hot
 * path, no ordering is imposed, and workers stay independent. What a parent
 * publishes is only the sum of its active children's weights, which moves on
 * activation transitions rather than per packet.
 *
 * DEPENDENCY CONTRACT: this header uses fixed-width integers and GCC atomic
 * builtins only. It must not reach for vlib, vnet or vppinfra, so that
 * tests/drr_harness.c can drive the shipped arithmetic under pthreads on a
 * plain host toolchain, against a stub clock it controls. Callers hand in the
 * parent's state as values and pointers.
 *
 * Required from the including translation unit: u8/u32/u64/i64,
 * static_always_inline.
 *
 * Reference: context/specs/hqos-svlan/IMPLEMENTATION_SPEC.md sections 4.3-4.5.
 * Shreedhar & Varghese, Efficient Fair Queueing using Deficit Round Robin,
 * SIGCOMM '95.
 */

#ifndef __included_cake_drr_h__
#define __included_cake_drr_h__

/* Rounds come from the wall clock, never from bytes sent. A byte clock stops
 * advancing exactly when every child is blocked, so "everyone is blocked"
 * becomes self-sustaining: nobody sends, the clock never moves, nobody
 * unblocks. Wall time advances whether or not a packet moves.
 *
 * Compile-time by decision - a granularity knob with no good operator answer
 * (DECISIONS.md, "Configurable DRR round period"). */
#define CAKE_DRR_ROUND_PERIOD_NS 1000000ULL

/* One standard-MTU-class packet of refill headroom. Deliberately not
 * load-bearing: a packet larger than the cap rides the debt mechanism below
 * rather than needing the cap to cover it. */
#define CAKE_MAX_PKT_BYTES 2048

typedef struct
{
  u64 effective_weight; /* own rate x weight multiplier, always >= 1 */
  i64 deficit;		/* bytes sendable; negative = bounded debt */
  u32 round;		/* round tag the deficit belongs to */
  u8 active;		/* weight is currently counted in the parent */
} cake_drr_child_t;

typedef enum
{
  CAKE_DRR_BLOCKED = 0, /* quantum spent: siblings hold the capacity */
  CAKE_DRR_ADMIT,	/* eligible: charge adj_len once the send succeeds */
  CAKE_DRR_ESCAPE,	/* parent genuinely idle: admit without charging */
} cake_drr_admit_t;

static_always_inline u32
cake_drr_round (u64 now_ns)
{
  /* Wraps every ~50 days of uptime, costing at worst one spurious
   * refill-or-skip on an idle child. */
  return (u32) (now_ns / CAKE_DRR_ROUND_PERIOD_NS);
}

/* Bytes a parent passes in one round. Precomputed on configuration so the
 * refill path never divides by 1e9. */
static_always_inline u64
cake_drr_round_bytes (u64 rate_bytes_per_sec)
{
  return (rate_bytes_per_sec * CAKE_DRR_ROUND_PERIOD_NS) / 1000000000ULL;
}

/* quantum_i = round_bytes * effective_weight_i / W.
 *
 * Reducing to round_bytes before multiplying is mandatory: rate x period x
 * weight overflows u64 outright. That alone is not sufficient - round_bytes x
 * effective_weight still wraps for permitted configurations (a 25 Gbit/s
 * child at weight 1000 under a 100 Gbit/s port reaches 3.9e19), so the
 * multiply carries a 128-bit intermediate. It runs once per child per round,
 * not per packet, so the wide divide costs nothing measurable. */
static_always_inline u64
cake_drr_quantum (u64 round_bytes, u64 effective_weight, u64 active_weight)
{
  /* W is read without the parent's cooperation and a child is meant to be
   * counted in it, so a lagging or zeroed read must never hand this child
   * more than its whole-parent share. */
  if (active_weight < effective_weight)
    active_weight = effective_weight;

  return (u64) (((unsigned __int128) round_bytes * effective_weight) /
		active_weight);
}

/* Accumulate rather than reset: quantum_i falls below one MTU whenever a
 * parent has many children (1 Gbit/s over 1000 equal children gives 125
 * bytes per round), and accumulation lets such a child assemble a full packet
 * of credit. The cap stops a child blocked on its own subscriber shaper from
 * hoarding while idle-ish. */
static_always_inline void
cake_drr_refill (cake_drr_child_t *child, u64 round_bytes, u64 active_weight,
		 u32 round)
{
  i64 quantum =
    (i64) cake_drr_quantum (round_bytes, child->effective_weight,
			    active_weight);
  i64 cap = 2 * quantum;
  i64 refilled;

  if (cap < CAKE_MAX_PKT_BYTES)
    cap = CAKE_MAX_PKT_BYTES;

  /* Signed throughout on purpose: deficit may be negative, and a u64 operand
   * anywhere here promotes it to a huge unsigned value, making the clamp
   * return cap and silently forgive the debt on every refill. */
  refilled = child->deficit + quantum;
  child->deficit = refilled < cap ? refilled : cap;
  child->round = round;
}

/* Owner-local admission. No refund obligation - the caller checks here and
 * charges only once the send has cleared every gate - but the refill does
 * mutate, so this is not a read-only test.
 *
 * active_weight and parent_shaper_time_ns are read through pointers, and only
 * on the paths that need them: a child with credit left in the current round
 * touches no shared cache line at all. */
static_always_inline cake_drr_admit_t
cake_drr_local_admit (cake_drr_child_t *child, u64 round_bytes,
		      const u64 *active_weight,
		      const u64 *parent_shaper_time_ns, u64 now_ns)
{
  u32 round = cake_drr_round (now_ns);

  if (child->round != round)
    cake_drr_refill (child, round_bytes,
		     __atomic_load_n (active_weight, __ATOMIC_RELAXED), round);

  /* Eligibility is positivity, the discipline the plugin's own flow-level DRR
   * and sch_cake already use. A ">= adj_len" rule cannot be used: refills
   * clamp at the cap, so any head packet larger than the cap - MTU 9000 under
   * the dsl-pppoe-atm preset, or an unsplit GSO chain - would never be
   * admitted while the parent is saturated, wedging the subscriber
   * permanently. */
  if (child->deficit > 0)
    return CAKE_DRR_ADMIT;

  /* Work-conserving escape, consulted only after eligibility refuses. Virtual
   * time a full round behind the wall clock means real spare capacity.
   * Ordering it last keeps deficits honest through idle periods, so every
   * child enters a congestion onset with bounded state.
   *
   * Addition form: now_ns - PERIOD underflows u64 in the first round after
   * boot, and under the harness stub clock, which starts at zero. */
  if (__atomic_load_n (parent_shaper_time_ns, __ATOMIC_RELAXED) +
	CAKE_DRR_ROUND_PERIOD_NS <=
      now_ns)
    return CAKE_DRR_ESCAPE;

  return CAKE_DRR_BLOCKED;
}

/* Charged only on a send that cleared every gate, and only for a
 * CAKE_DRR_ADMIT: usage under an idle parent does not count against the
 * child's congested share, which is what keeps debt bounded at one packet.
 * The full adj_len goes, so the deficit may go negative - debt, repaid from
 * subsequent refills before the child is eligible again. */
static_always_inline void
cake_drr_local_charge (cake_drr_child_t *child, u32 adj_len)
{
  child->deficit -= (i64) adj_len;
}

/* Going idle earns nothing, and forgives no debt. Both halves matter:
 * resetting debt too would let a sparse child - one whose queue drains
 * between packets, re-activating on each - send a full-size packet per round
 * regardless of its quantum.
 *
 * Returns 1 only for the caller that performed the transition, so a parent's
 * weight is moved exactly once however many times these are called. That is
 * what lets the dequeue walk's defensive deactivations - a scheduler whose
 * teardown already ran, or one owned by another thread - clear an activity
 * bit without double-subtracting a weight that has already left. */
static_always_inline u8
cake_drr_child_activate (cake_drr_child_t *child)
{
  if (child->active)
    return 0;

  if (child->deficit > 0)
    child->deficit = 0;
  child->active = 1;
  return 1;
}

static_always_inline u8
cake_drr_child_deactivate (cake_drr_child_t *child)
{
  if (!child->active)
    return 0;

  child->active = 0;
  return 1;
}

/* The parent side of the same transition. Only these two counters are shared;
 * they move on activation transitions rather than per packet, and they are
 * always written together, which is why they share a cache line with each
 * other and with nothing else. */
static_always_inline void
cake_drr_parent_join (u64 *active_weight, u32 *n_active_children,
		      u64 effective_weight)
{
  __atomic_fetch_add (active_weight, effective_weight, __ATOMIC_RELAXED);
  __atomic_fetch_add (n_active_children, 1, __ATOMIC_RELAXED);
}

static_always_inline void
cake_drr_parent_leave (u64 *active_weight, u32 *n_active_children,
		       u64 effective_weight)
{
  __atomic_fetch_sub (active_weight, effective_weight, __ATOMIC_RELAXED);
  __atomic_fetch_sub (n_active_children, 1, __ATOMIC_RELAXED);
}

#endif /* __included_cake_drr_h__ */

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
