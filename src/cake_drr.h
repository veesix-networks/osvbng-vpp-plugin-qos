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
 * Required from the including translation unit: u8/u32/i32/u64/i64,
 * static_always_inline.
 *
 * Reference: Shreedhar & Varghese, Efficient Fair Queueing using Deficit
 * Round Robin, SIGCOMM '95.
 */

#ifndef __included_cake_drr_h__
#define __included_cake_drr_h__

/* Rounds come from the wall clock, never from bytes sent. A byte clock stops
 * advancing exactly when every child is blocked, so "everyone is blocked"
 * becomes self-sustaining: nobody sends, the clock never moves, nobody
 * unblocks. Wall time advances whether or not a packet moves.
 *
 * Compile-time by decision - a granularity knob with no good operator
 * answer. */
#define CAKE_DRR_ROUND_PERIOD_NS 1000000ULL

/* One standard-MTU-class packet of refill headroom. Deliberately not
 * load-bearing: a packet larger than the cap rides the debt mechanism below
 * rather than needing the cap to cover it. */
#define CAKE_MAX_PKT_BYTES 2048

/* The shared child stores its deficit biased by 2^31 so bounded debt packs
 * unsigned alongside the round tag in one word. The value spans one max packet
 * of debt below the bias up to bias + cap above it: even a 100 Gbit/s parent
 * with 1 ms rounds caps at 25 MB, keeping the word inside
 * [2^31 - 64 KB, 2^31 + 25 MB] against a 4.29e9 ceiling. */
#define CAKE_DRR_DEFICIT_BIAS 2147483648ULL

typedef struct
{
  u64 effective_weight; /* own rate x weight multiplier, always >= 1 */
  i64 deficit;		/* bytes sendable; negative = bounded debt */
  u32 round;		/* round tag the deficit belongs to */
  u8 active;		/* weight is currently counted in the parent */
} cake_drr_child_t;

/* An aggregate's own state as a child of the tier above it. Unlike a
 * scheduler, which has exactly one owner thread, this is written per packet by
 * whichever worker dequeues any of its members, so round and deficit are
 * packed into one word and moved by a single CAS. */
typedef struct
{
  u64 effective_weight; /* read-mostly; lives with the identity group */
  u64 round_deficit;    /* (round u32 << 32) | biased deficit u32; atomic */
} cake_drr_shared_child_t;

typedef enum
{
  CAKE_DRR_BLOCKED = 0,	 /* quantum spent: siblings hold the capacity */
  CAKE_DRR_ADMIT,	 /* charge adj_len once the send succeeds, and for a
			    shared child refund it if a later gate refuses */
  CAKE_DRR_UNARBITRATED, /* no parent to compete for: never charged, never
			    refunded */
} cake_drr_admit_t;

static_always_inline u32
cake_drr_round (u64 now_ns)
{
  /* Wraps every ~50 days of uptime, costing at worst one spurious
   * refill-or-skip on an idle child. */
  return (u32) (now_ns / CAKE_DRR_ROUND_PERIOD_NS);
}

/* Has the round advanced, rather than merely differ?
 *
 * Workers sample the wall clock independently, so two of them can straddle a
 * round boundary and see different rounds for the same instant. Testing
 * inequality lets the lagging one treat the leader's round as new and refill
 * again, and the two then ping-pong, issuing a fresh quantum per crossing
 * instead of per round. Measured on the shared child: eight threads over the
 * same 20000 rounds drew 26409 packets of credit where 3304 had been issued.
 *
 * The signed difference is also what keeps the u32 round tag wrapping
 * correctly every ~50 days. */
static_always_inline u8
cake_drr_round_advanced (u32 round, u32 current)
{
  return (i32) (round - current) > 0;
}

/* The signed test above is valid only within 2^31 rounds: a tag >24.9 days
 * stale wraps into the future half-space and would never refill again. No
 * real lead approaches 2^20 rounds (~17 min), so a larger one is pre-wrap
 * staleness and is due a refill. */
#define CAKE_DRR_ROUND_STALE_LEAD (1u << 20)

static_always_inline u8
cake_drr_round_stale (u32 round, u32 current)
{
  return (i32) (round - current) < -(i32) CAKE_DRR_ROUND_STALE_LEAD;
}

static_always_inline u8
cake_drr_round_refill_due (u32 round, u32 current)
{
  return cake_drr_round_advanced (round, current) ||
	 cake_drr_round_stale (round, current);
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

/* Same rule as the local child's escape, for the shared word. Kept identical
 * on purpose: an unarbitrated escape at the S-VLAN tier is the same defect one
 * level up, and it showed up the same way - two S-VLANs whose rates asked for
 * a 75/25 split of a contended port took 83.5/16.5, because whichever tier the
 * walk reached first collected the uncharged admissions. */
static_always_inline u8
cake_drr_escape_open (i64 deficit, const u64 *parent_shaper_time_ns,
		      u64 now_ns)
{
  return deficit > -(i64) CAKE_MAX_PKT_BYTES &&
	 __atomic_load_n (parent_shaper_time_ns, __ATOMIC_RELAXED) +
	     CAKE_DRR_ROUND_PERIOD_NS <=
	   now_ns;
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

  if (cake_drr_round_refill_due (round, child->round))
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
   *
   * Charged like any other admission, and refused once the child is a packet
   * into debt. The original design left escape admissions uncharged, on the
   * reasoning that capacity taken while the parent is idle should not count
   * against a child's congested share. Measured, that breaks the S-VLAN tier:
   * a parent's virtual time also lags while the parent is itself blocked from
   * above, every child reads the lag as spare capacity, and because nothing is
   * charged the deficits stop moving and walk order decides everything. Two of
   * four children under one S-VLAN got nothing at all in 30 s.
   *
   * Charging keeps the books straight; the debt floor keeps the mechanism
   * work-conserving, because a child that has repaid its debt can take idle
   * capacity again immediately. Utilisation with a deliberately under-using
   * sibling measures 100.6% of port rate either way, so nothing is lost.
   *
   * Addition form: now_ns - PERIOD underflows u64 in the first round after
   * boot, and under the harness stub clock, which starts at zero. */
  if (cake_drr_escape_open (child->deficit, parent_shaper_time_ns, now_ns))
    return CAKE_DRR_ADMIT;

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
 * other and with nothing else.
 *
 * Both return the previous child count, because that is the refcount a tier
 * propagates on: an aggregate counts toward its own parent's W exactly while
 * it has at least one active child, so only the caller observing 0 (join) or
 * 1 (leave) carries the transition upward. */
static_always_inline u32
cake_drr_parent_join (u64 *active_weight, u32 *n_active_children,
		      u64 effective_weight)
{
  __atomic_fetch_add (active_weight, effective_weight, __ATOMIC_RELAXED);
  return __atomic_fetch_add (n_active_children, 1, __ATOMIC_RELAXED);
}

static_always_inline u32
cake_drr_parent_leave (u64 *active_weight, u32 *n_active_children,
		       u64 effective_weight)
{
  __atomic_fetch_sub (active_weight, effective_weight, __ATOMIC_RELAXED);
  return __atomic_fetch_sub (n_active_children, 1, __ATOMIC_RELAXED);
}

/* A zeroed word would read as a deficit of -2^31, so a shared child is never
 * simply memset into service; the round tag is seeded so a child created past
 * day 25 of uptime does not start in the stale half-space. */
static_always_inline void
cake_drr_shared_init (cake_drr_shared_child_t *child, u64 effective_weight,
		      u64 now_ns)
{
  child->effective_weight = effective_weight;
  __atomic_store_n (&child->round_deficit,
		    ((u64) cake_drr_round (now_ns) << 32) |
		      CAKE_DRR_DEFICIT_BIAS,
		    __ATOMIC_RELEASE);
}

/* Refill, admit and decrement as one linearizable step.
 *
 * The owner-local discipline is unsound here: two workers passing a read-only
 * check against the last quantum would both decrement later, either losing an
 * update or wrapping the deficit open until the next refill. This mirrors the
 * CAS shape of cake_shaper_gate_take.
 *
 * On CAKE_DRR_ADMIT the caller owes exactly one cake_drr_shared_refund if a
 * later step refuses the packet. */
static_always_inline cake_drr_admit_t
cake_drr_shared_reserve (cake_drr_shared_child_t *child, u64 round_bytes,
			 const u64 *active_weight,
			 const u64 *parent_shaper_time_ns, u32 adj_len,
			 u64 now_ns)
{
  u32 round = cake_drr_round (now_ns);
  u64 old, updated;
  u32 biased;
  u8 admitted = 0;

  for (;;)
    {
      u32 cur_round;
      u8 refilled = 0;

      old = __atomic_load_n (&child->round_deficit, __ATOMIC_ACQUIRE);
      cur_round = (u32) (old >> 32);
      biased = (u32) old;

      if (cake_drr_round_refill_due (round, cur_round))
	{
	  u64 quantum = cake_drr_quantum (
	    round_bytes, child->effective_weight,
	    __atomic_load_n (active_weight, __ATOMIC_RELAXED));
	  i64 cap = 2 * (i64) quantum;
	  u64 topped_up, ceiling;

	  if (cap < CAKE_MAX_PKT_BYTES)
	    cap = CAKE_MAX_PKT_BYTES;

	  topped_up = (u64) biased + quantum;
	  ceiling = CAKE_DRR_DEFICIT_BIAS + (u64) cap;
	  biased = (u32) (topped_up < ceiling ? topped_up : ceiling);
	  refilled = 1;
	}

      admitted = biased > CAKE_DRR_DEFICIT_BIAS;

      /* Work-conserving escape, inside the CAS so that it is charged like any
       * other admission and stays refundable. */
      if (!admitted &&
	  cake_drr_escape_open ((i64) biased - (i64) CAKE_DRR_DEFICIT_BIAS,
				parent_shaper_time_ns, now_ns))
	admitted = 1;

      /* No refill to publish and no admission to charge: the common blocked
       * path writes nothing at all. */
      if (!admitted && !refilled)
	break;

      /* biased - adj_len cannot cross zero: admission requires biased above
       * the bias, and adj_len is far below it. A refill is published even when
       * admission fails, so the round advances without needing a send. The
       * round tag only ever moves forward. */
      updated = ((u64) (refilled ? round : cur_round) << 32) |
		(u64) (admitted ? biased - adj_len : biased);

      if (__atomic_compare_exchange_n (&child->round_deficit, &old, updated, 1,
				       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	break;
    }

  return admitted ? CAKE_DRR_ADMIT : CAKE_DRR_BLOCKED;
}

/* Give back a reservation a later gate refused. Adding to the whole word adds
 * to the low half and cannot carry into the round bits, because the low half
 * stays far below 2^32. If a round boundary lands between reserve and refund
 * the credit lands in the new round instead: bounded at one packet, clamped by
 * the next refill, self-correcting - the same accuracy class as the shaper
 * refund beside it. */
static_always_inline void
cake_drr_shared_refund (cake_drr_shared_child_t *child, u32 adj_len)
{
  __atomic_fetch_add (&child->round_deficit, (u64) adj_len, __ATOMIC_RELAXED);
}

#endif /* __included_cake_drr_h__ */

/*
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
