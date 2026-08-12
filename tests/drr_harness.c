/* Copyright 2026 Veesix Networks Ltd
 * Licensed under the GNU General Public License v3.0 or later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DRR and shaper harness (IMPLEMENTATION_SPEC.md section 9.1).
 *
 * The repo has no C test suite and TESTING.md records that the af-packet/veth
 * environment has no downstream bottleneck, so nothing existing can prove an
 * arithmetic claim. This links the shipped inlines from src/cake_drr.h and
 * src/cake_shaper.h - not a copy of them - against a clock it controls, and
 * runs the multi-writer paths under pthreads.
 *
 * Scope, learned the hard way (PHASE5_FINDINGS.md F5-1): this harness asserts
 * arithmetic and concurrency invariants only. It must not be used to predict
 * bandwidth shares, because it cannot model VPP's dispatch loop and a model
 * that tried was confidently wrong by an order of magnitude. Share
 * measurement belongs to tests/fairness-rig.sh against a running VPP.
 *
 * Build: tests/CMakeLists.txt, or
 *   cc -O2 -Wall -Wextra -Isrc -pthread tests/drr_harness.c -o drr_harness
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The dependency contract both headers declare. */
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;
#define static_always_inline static inline __attribute__ ((always_inline))

#include "cake_drr.h"
#include "cake_shaper.h"

#define MTU_ADJ	      1514
#define JUMBO_ATM_ADJ 9976 /* MTU 9000 under the dsl-pppoe-atm preset */

/* A parent virtual time far enough ahead of any clock this harness uses that
 * its gate never opens. Not ~0: the escape comparison is written in addition
 * form, so a sentinel near the u64 ceiling wraps and reads as a parent that is
 * idle. Real virtual times are clock values and cannot get near it. */
#define PARENT_SATURATED (1000000000000000ULL)

static int failures;
static int checks;

static void
check (int ok, const char *what, const char *fmt, ...)
{
  checks++;
  if (!ok)
    {
      failures++;
      printf ("  FAIL  %s\n", what);
      if (fmt && *fmt)
	{
	  va_list ap;
	  va_start (ap, fmt);
	  printf ("        ");
	  vprintf (fmt, ap);
	  printf ("\n");
	  va_end (ap);
	}
    }
}

static void
pass (const char *what, const char *detail)
{
  printf ("  ok    %-46s %s\n", what, detail ? detail : "");
}

static i64
cap_for (u64 round_bytes, u64 weight, u64 W)
{
  i64 q = (i64) cake_drr_quantum (round_bytes, weight, W);
  i64 cap = 2 * q;
  return cap < CAKE_MAX_PKT_BYTES ? CAKE_MAX_PKT_BYTES : cap;
}

/* Section 4.4: the refill is signed throughout. A u64 operand anywhere would
 * promote a negative deficit to a huge unsigned value, the clamp would return
 * cap, and every refill would silently forgive the debt. */
static void
test_refill_does_not_forgive_debt (void)
{
  cake_drr_child_t c = { .effective_weight = 625000, .deficit = -5000 };
  u64 W = 2500000, rb = 1000;
  i64 q = (i64) cake_drr_quantum (rb, c.effective_weight, W);

  cake_drr_refill (&c, rb, W, 1);

  check (c.deficit == -5000 + q, "refill does not forgive debt",
	 "deficit %" PRId64 ", expected %" PRId64, c.deficit, -5000 + q);
  check (c.deficit < 0, "debt survives a refill", "deficit %" PRId64,
	 c.deficit);
  pass ("refill carries debt (signed arithmetic)", "deficit stays negative");
}

static void
test_refill_cap (void)
{
  cake_drr_child_t c = { .effective_weight = 625000, .deficit = 0 };
  u64 W = 2500000, rb = 1000;
  i64 cap = cap_for (rb, c.effective_weight, W);

  for (u32 r = 1; r < 10000; r++)
    cake_drr_refill (&c, rb, W, r);

  check (c.deficit == cap, "refill clamps at the cap",
	 "deficit %" PRId64 ", cap %" PRId64, c.deficit, cap);
  pass ("refill clamps at max(2*quantum, CAKE_MAX_PKT_BYTES)", "");
}

/* Section 4.3: weight is validated 1-256 and the quantum carries a 128-bit
 * intermediate, because round_bytes * effective_weight overflows u64 for
 * permitted configurations. */
static void
test_quantum_no_overflow (void)
{
  u64 port = 12500000000ULL;		     /* 100 Gbit/s */
  u64 rb = cake_drr_round_bytes (port);	     /* 12.5 MB per 1 ms round */
  u64 child = 3125000000ULL;		     /* 25 Gbit/s */
  u64 w = child * 256;			     /* the validated ceiling */
  u64 q = cake_drr_quantum (rb, w, w);
  int monotonic = 1;
  u64 prev = 0;

  check (q == rb, "sole child gets the whole round",
	 "quantum %" PRIu64 ", round_bytes %" PRIu64, q, rb);

  for (u64 k = 1; k <= 256; k++)
    {
      u64 qi = cake_drr_quantum (rb, child * k, child * 256 * 4);
      if (qi < prev)
	monotonic = 0;
      prev = qi;
    }
  check (monotonic, "quantum is monotonic in weight", "");

  /* The shares of a fully subscribed parent must not exceed one round. */
  u64 w1 = 125000, w2 = 250000, w3 = 500000, w4 = 1000000;
  u64 sum_w = w1 + w2 + w3 + w4;
  u64 rb2 = cake_drr_round_bytes (1000000);
  u64 total = cake_drr_quantum (rb2, w1, sum_w) +
	      cake_drr_quantum (rb2, w2, sum_w) +
	      cake_drr_quantum (rb2, w3, sum_w) +
	      cake_drr_quantum (rb2, w4, sum_w);
  check (total <= rb2, "quanta sum to at most one round of bytes",
	 "sum %" PRIu64 " > round_bytes %" PRIu64, total, rb2);
  pass ("quantum: 128-bit intermediate, monotonic, conservative", "");
}

/* Section 4.4 / finding F1: a ">= adj_len" rule would wedge a subscriber whose
 * head packet exceeds the refill cap. Eligibility is positivity, so the child
 * progresses on debt instead. */
static void
test_oversized_head_packet (void)
{
  cake_drr_child_t c = { .effective_weight = 625000 };
  u64 W = 2500000, rb = 1000, saturated_vt = PARENT_SATURATED;
  u64 sent = 0;
  i64 cap = cap_for (rb, c.effective_weight, W);

  check (JUMBO_ATM_ADJ > cap, "the test packet does exceed the cap",
	 "adj_len %d, cap %" PRId64, JUMBO_ATM_ADJ, cap);

  for (u64 now = 0; now < 10ULL * 1000000000ULL; now += CAKE_DRR_ROUND_PERIOD_NS)
    {
      if (cake_drr_local_admit (&c, rb, &W, &saturated_vt, now) ==
	  CAKE_DRR_ADMIT)
	{
	  cake_drr_local_charge (&c, JUMBO_ATM_ADJ);
	  sent++;
	}
    }

  check (sent > 0, "oversized head packet still progresses", "sent %" PRIu64,
	 sent);
  /* 10 s at 250 bytes/round of credit is 2.5 MB, or ~250 jumbo packets. */
  check (sent > 200 && sent < 300, "and at its credit rate, not faster",
	 "sent %" PRIu64 ", expected ~250", sent);
  pass ("oversized head packet rides the debt mechanism", "no wedge");
}

/* Section 4.4 / finding F8: written in addition form because now_ns - PERIOD
 * underflows u64 in the first round after boot and under a stub clock.
 *
 * The escape is charged like any other admission and refused past the debt
 * floor, so it returns CAKE_DRR_ADMIT (PHASE5_FINDINGS.md F5-2). */
static void
test_escape_does_not_underflow_at_zero (void)
{
  cake_drr_child_t c = { .effective_weight = 625000, .deficit = -1 };
  u64 W = 2500000, rb = 1000, vt = 0;

  check (cake_drr_local_admit (&c, rb, &W, &vt, 0) == CAKE_DRR_BLOCKED,
	 "no spurious escape at now_ns = 0", "");

  c.deficit = -1;
  c.round = cake_drr_round (CAKE_DRR_ROUND_PERIOD_NS / 2);
  check (cake_drr_local_admit (&c, rb, &W, &vt, CAKE_DRR_ROUND_PERIOD_NS / 2) ==
	   CAKE_DRR_BLOCKED,
	 "no spurious escape inside the first round", "");

  c.deficit = -1;
  c.round = cake_drr_round (5 * CAKE_DRR_ROUND_PERIOD_NS);
  check (cake_drr_local_admit (&c, rb, &W, &vt, 5 * CAKE_DRR_ROUND_PERIOD_NS) ==
	   CAKE_DRR_ADMIT,
	 "escape fires once the parent is a round behind", "");

  /* Refused past the debt floor, which is what bounds a child's state through
   * an idle period and keeps congestion onset recoverable. */
  c.deficit = -(i64) CAKE_MAX_PKT_BYTES - 1;
  c.round = cake_drr_round (5 * CAKE_DRR_ROUND_PERIOD_NS);
  check (cake_drr_local_admit (&c, rb, &W, &vt, 5 * CAKE_DRR_ROUND_PERIOD_NS) ==
	   CAKE_DRR_BLOCKED,
	 "escape refuses a child already a packet into debt", "");
  pass ("escape: charged, debt-bounded, zero-clock safe", "");
}

/* Section 4.3: the u32 round tag wraps every ~50 days of uptime, costing at
 * worst one spurious refill-or-skip on an idle child. */
static void
test_round_tag_wrap (void)
{
  cake_drr_child_t c = { .effective_weight = 625000, .deficit = 0 };
  u64 W = 2500000, rb = 1000;
  u64 vt = PARENT_SATURATED;
  i64 cap = cap_for (rb, c.effective_weight, W);
  u64 last = (u64) 0xfffffffeULL * CAKE_DRR_ROUND_PERIOD_NS;

  /* Step across the boundary the way uptime does, one round at a time. */
  c.round = 0xfffffffdU;

  cake_drr_local_admit (&c, rb, &W, &vt, last);
  check (c.round == 0xfffffffeU, "round tag advances toward u32 max",
	 "round %u", c.round);

  cake_drr_local_admit (&c, rb, &W, &vt, last + CAKE_DRR_ROUND_PERIOD_NS);
  check (c.round == 0xffffffffU, "round tag reaches u32 max", "round %u",
	 c.round);

  cake_drr_local_admit (&c, rb, &W, &vt, last + 2 * CAKE_DRR_ROUND_PERIOD_NS);
  check (c.round == 0, "round tag wraps to zero", "round %u", c.round);
  check (c.deficit > 0 && c.deficit <= cap,
	 "deficit stays sane across the wrap", "deficit %" PRId64, c.deficit);

  /* A worker whose clock lags across the boundary must not re-refill. */
  {
    i64 held = c.deficit;
    cake_drr_local_admit (&c, rb, &W, &vt, last + CAKE_DRR_ROUND_PERIOD_NS);
    check (c.deficit == held && c.round == 0,
	   "a lagging clock neither refills nor rewinds the round",
	   "deficit %" PRId64 ", round %u", c.deficit, c.round);
  }
  pass ("u32 round tag wrap is benign", "~50 days of uptime");
}

/* Phase 6 CL-1: a tag >2^31 rounds stale wraps into the future half-space,
 * where plain advancement would never refill it. The rebase must recover it
 * without firing on the one-round lead a concurrent worker can hold. */
static void
test_stale_round_rebases (void)
{
  u64 W = 2500000, rb = 1000;
  u64 vt = PARENT_SATURATED;

  /* Local child created at round 0, consulted at uptime day ~30. */
  {
    cake_drr_child_t c = { .effective_weight = 625000, .deficit = 0,
			   .round = 0 };
    u64 day30 = 2600000000ULL * CAKE_DRR_ROUND_PERIOD_NS;

    check (cake_drr_local_admit (&c, rb, &W, &vt, day30) == CAKE_DRR_ADMIT,
	   "a stale local child refills instead of wedging", "deficit %" PRId64,
	   c.deficit);
    check (c.round == cake_drr_round (day30),
	   "the rebase publishes the current round", "round %u", c.round);
  }

  /* Shared child, same staleness. */
  {
    cake_drr_shared_child_t c;
    u64 day30 = 2600000000ULL * CAKE_DRR_ROUND_PERIOD_NS;

    cake_drr_shared_init (&c, 625000, 0);
    check (cake_drr_shared_reserve (&c, rb, &W, &vt, MTU_ADJ, day30) ==
	     CAKE_DRR_ADMIT,
	   "a stale shared child refills instead of wedging", "");
    check ((u32) (__atomic_load_n (&c.round_deficit, __ATOMIC_RELAXED) >> 32) ==
	     cake_drr_round (day30),
	   "the shared rebase publishes the current round", "");
  }

  /* A one-round lead is a concurrent worker, not staleness (F5-4). */
  {
    cake_drr_child_t c = { .effective_weight = 625000, .deficit = 5,
			   .round = 100 };
    cake_drr_local_admit (&c, rb, &W, &vt, 99 * CAKE_DRR_ROUND_PERIOD_NS);
    check (c.round == 100 && c.deficit == 5,
	   "a one-round lead is not treated as stale",
	   "round %u deficit %" PRId64, c.round, c.deficit);
  }

  /* A child initialized at late uptime starts current; the rebase is a
   * recovery path, not the ordinary case. */
  {
    cake_drr_shared_child_t c;
    u64 day30 = 2600000000ULL * CAKE_DRR_ROUND_PERIOD_NS;

    cake_drr_shared_init (&c, 625000, day30);
    check ((u32) (__atomic_load_n (&c.round_deficit, __ATOMIC_RELAXED) >> 32) ==
	     cake_drr_round (day30),
	   "shared init seeds the current round", "");
  }

  pass ("stale round tags rebase, one-round leads do not", ">2^31 rounds");
}

/* Section 9.1: rate precision at and above 1 Gbit/s, which the fixed-point
 * change (#6) introduced and which was verified by inspection only, because
 * af-packet presents one rx queue and the veth rig cannot offer multi-gigabit. */
static void
test_rate_precision (void)
{
  static const u64 rates[] = { 125000ULL,	  /* 1 Mbit/s */
			       1250000ULL,	  /* 10 Mbit/s */
			       125000000ULL,	  /* 1 Gbit/s */
			       1250000000ULL,	  /* 10 Gbit/s */
			       3125000000ULL,	  /* 25 Gbit/s */
			       12500000000ULL };  /* 100 Gbit/s */
  double worst = 0;

  for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++)
    {
      u64 scaled = cake_rate_scaled (rates[i]);
      /* Cost of a large burst, so the per-packet truncation is visible. */
      u64 cost = cake_cost_ns (60000, scaled);
      double implied = 60000.0 * 1e9 / (double) cost;
      double err = (implied - (double) rates[i]) / (double) rates[i] * 100.0;

      if (err < 0)
	err = -err;
      if (err > worst)
	worst = err;

      check (err < 1.0, "rate precision within 1%",
	     "%" PRIu64 " B/s: implied %.0f, err %.4f%%", rates[i], implied,
	     err);
    }

  check (cake_rate_scaled (0) == 0, "rate 0 does not divide by zero", "");

  char detail[64];
  snprintf (detail, sizeof detail, "worst %.4f%% to 100 Gbit/s", worst);
  pass ("fixed-point rate holds precision", detail);
}

/* Section 9.1: rate accuracy per tier. Drives the shipped gate with a clock
 * this harness owns, so the only thing under test is the shaper arithmetic. */
static void
test_gate_rate_accuracy (void)
{
  u64 rate = 125000000ULL; /* 1 Gbit/s */
  u64 scaled = cake_rate_scaled (rate);
  u64 vt = 0, admitted = 0;
  u64 window = 2ULL * 1000000000ULL;
  double achieved, err;

  for (u64 now = 0; now < window; now += 100)
    while (cake_shaper_gate_take (&vt, cake_cost_ns (MTU_ADJ, scaled), now,
				  10ULL * 1000000ULL))
      admitted += MTU_ADJ;

  achieved = (double) admitted / 2.0;
  err = (achieved - (double) rate) / (double) rate * 100.0;
  if (err < 0)
    err = -err;

  check (err < 1.0, "gate holds its configured rate within 1%",
	 "achieved %.0f B/s vs %" PRIu64 ", err %.3f%%", achieved, rate, err);

  char detail[64];
  snprintf (detail, sizeof detail, "1 Gbit/s, err %.3f%%", err);
  pass ("shaper gate rate accuracy", detail);
}

/* Pure DRR proportionality: no gate, no escape, every child always backlogged.
 * This is the arithmetic claim - that credit is issued in proportion to weight
 * - and not a bandwidth claim. */
static void
test_credit_is_proportional_to_weight (void)
{
  enum { N = 8 };
  static const u64 weights[N] = { 125000,  125000,	125000,	 125000,
				  250000,  500000,	1000000, 2000000 };
  cake_drr_child_t c[N];
  u64 sent[N];
  u64 W = 0, rb = cake_drr_round_bytes (1000000), total = 0;
  u64 saturated_vt = ~0ULL;
  double worst = 0;
  u64 first_send_round[N];

  memset (c, 0, sizeof c);
  memset (sent, 0, sizeof sent);
  for (int i = 0; i < N; i++)
    {
      c[i].effective_weight = weights[i];
      c[i].active = 1;
      W += weights[i];
      first_send_round[i] = 0;
    }

  for (u64 r = 1; r < 200000; r++)
    {
      u64 now = r * CAKE_DRR_ROUND_PERIOD_NS;
      for (int i = 0; i < N; i++)
	if (cake_drr_local_admit (&c[i], rb, &W, &saturated_vt, now) ==
	    CAKE_DRR_ADMIT)
	  {
	    cake_drr_local_charge (&c[i], MTU_ADJ);
	    sent[i] += MTU_ADJ;
	    if (!first_send_round[i])
	      first_send_round[i] = r;
	  }
    }

  for (int i = 0; i < N; i++)
    total += sent[i];

  for (int i = 0; i < N; i++)
    {
      double got = 100.0 * (double) sent[i] / (double) total;
      double want = 100.0 * (double) weights[i] / (double) W;
      double err = got - want;

      if (err < 0)
	err = -err;
      if (err > worst)
	worst = err;

      /* Section 9.1 progress row: no active child may starve. */
      check (sent[i] > 0, "every active child sends", "child %d sent nothing",
	     i);
      check (first_send_round[i] <= 64,
	     "every active child sends within a bounded number of rounds",
	     "child %d first sent in round %" PRIu64, i, first_send_round[i]);
    }

  check (worst < 2.0, "credit tracks weight within 2 points",
	 "worst %.2f points", worst);

  char detail[64];
  snprintf (detail, sizeof detail, "8 children, worst %.2f pts", worst);
  pass ("DRR credit is proportional to weight", detail);
}

/* Section 4.9: activation and deactivation are transitions, so a parent's
 * weight moves exactly once however many times they are called. This is what
 * lets the dequeue walk's defensive paths clear an activity bit for a
 * scheduler whose teardown already ran without double-subtracting. */
static void
test_activation_is_idempotent (void)
{
  cake_drr_child_t c = { .effective_weight = 625000, .deficit = -400 };

  check (cake_drr_child_activate (&c) == 1, "first activate transitions", "");
  check (c.deficit == -400, "activation clamps min(deficit,0), keeping debt",
	 "deficit %" PRId64, c.deficit);
  check (cake_drr_child_activate (&c) == 0, "second activate does not", "");

  c.deficit = 900;
  c.active = 0;
  cake_drr_child_activate (&c);
  check (c.deficit == 0, "activation forfeits idle credit", "deficit %" PRId64,
	 c.deficit);

  check (cake_drr_child_deactivate (&c) == 1, "first deactivate transitions",
	 "");
  check (cake_drr_child_deactivate (&c) == 0, "second deactivate does not",
	 "");
  pass ("activation/deactivation are one-shot transitions", "");
}

/* Section 9.1: weight invariant after randomised churn, run concurrently.
 * Each thread owns a disjoint slice of children, as the owner-thread model
 * guarantees, while the parent counters are shared. */
#define CHURN_THREADS  8
#define CHURN_PER_THREAD 64
#define CHURN_ITERS    20000

typedef struct
{
  cake_drr_child_t *children;
  u64 *active_weight;
  u32 *n_active;
  u32 seed;
} churn_arg_t;

static void *
churn_thread (void *raw)
{
  churn_arg_t *a = raw;

  for (int it = 0; it < CHURN_ITERS; it++)
    {
      a->seed = a->seed * 1103515245u + 12345u;
      cake_drr_child_t *c = &a->children[(a->seed >> 16) % CHURN_PER_THREAD];

      if ((a->seed >> 8) & 1)
	{
	  if (cake_drr_child_activate (c))
	    cake_drr_parent_join (a->active_weight, a->n_active,
				  c->effective_weight);
	}
      else
	{
	  if (cake_drr_child_deactivate (c))
	    cake_drr_parent_leave (a->active_weight, a->n_active,
				   c->effective_weight);
	}
    }
  return NULL;
}

static void
test_weight_accounting_under_churn (void)
{
  static cake_drr_child_t children[CHURN_THREADS][CHURN_PER_THREAD];
  pthread_t t[CHURN_THREADS];
  churn_arg_t args[CHURN_THREADS];
  u64 active_weight = 0;
  u32 n_active = 0;
  u64 expect_w = 0;
  u32 expect_n = 0;

  memset (children, 0, sizeof children);
  for (int i = 0; i < CHURN_THREADS; i++)
    {
      for (int j = 0; j < CHURN_PER_THREAD; j++)
	children[i][j].effective_weight = 125000 + (u64) (i * 1000 + j);
      args[i] = (churn_arg_t){ .children = children[i],
			       .active_weight = &active_weight,
			       .n_active = &n_active,
			       .seed = 0x9e3779b9u ^ (u32) (i + 1) };
      pthread_create (&t[i], NULL, churn_thread, &args[i]);
    }
  for (int i = 0; i < CHURN_THREADS; i++)
    pthread_join (t[i], NULL);

  for (int i = 0; i < CHURN_THREADS; i++)
    for (int j = 0; j < CHURN_PER_THREAD; j++)
      if (children[i][j].active)
	{
	  expect_w += children[i][j].effective_weight;
	  expect_n++;
	}

  check (active_weight == expect_w,
	 "active_weight equals the sum of active children",
	 "got %" PRIu64 ", expected %" PRIu64, active_weight, expect_w);
  check (n_active == expect_n, "n_active_children is exact",
	 "got %u, expected %u", n_active, expect_n);

  /* Teardown drains it to zero, the quiescent invariant the rig observes. */
  for (int i = 0; i < CHURN_THREADS; i++)
    for (int j = 0; j < CHURN_PER_THREAD; j++)
      if (cake_drr_child_deactivate (&children[i][j]))
	cake_drr_parent_leave (&active_weight, &n_active,
			       children[i][j].effective_weight);

  check (active_weight == 0 && n_active == 0,
	 "teardown returns the parent to zero",
	 "weight %" PRIu64 ", n %u", active_weight, n_active);

  char detail[64];
  snprintf (detail, sizeof detail, "%d threads x %d iters", CHURN_THREADS,
	    CHURN_ITERS);
  pass ("weight accounting exact under concurrent churn", detail);
}

/* Section 9.1: admission race under contention. Add-then-verify may transiently
 * exceed the limit by the packets concurrently in flight, but every one of them
 * is unwound, so no admitted packet is ever left above the limit. */
#define ADMIT_THREADS 8
#define ADMIT_LEN     1514

typedef struct
{
  u32 *usage;
  u32 limit;
  u64 admitted;
  u32 held;
  u32 peak;
} admit_arg_t;

/* Admit and release continuously, so usage churns around the limit for the
 * whole run rather than filling once and refusing thereafter. That is the
 * state the race lives in. */
static void *
admit_thread (void *raw)
{
  admit_arg_t *a = raw;

  for (int i = 0; i < 200000; i++)
    {
      u32 seen;

      if (cake_agg_admit (a->usage, a->limit, ADMIT_LEN))
	{
	  a->admitted++;
	  a->held++;
	}

      seen = __atomic_load_n (a->usage, __ATOMIC_RELAXED);
      if (seen > a->peak)
	a->peak = seen;

      if (a->held && (i & 3) == 0)
	{
	  cake_agg_release (a->usage, ADMIT_LEN);
	  a->held--;
	}
    }

  return NULL;
}

static void
test_admission_race (void)
{
  u32 usage = 0;
  u32 limit = 200 * ADMIT_LEN;
  pthread_t t[ADMIT_THREADS];
  admit_arg_t args[ADMIT_THREADS];
  u64 total = 0, still_held = 0;
  u32 peak = 0;

  for (int i = 0; i < ADMIT_THREADS; i++)
    {
      args[i] = (admit_arg_t){ .usage = &usage, .limit = limit };
      pthread_create (&t[i], NULL, admit_thread, &args[i]);
    }
  for (int i = 0; i < ADMIT_THREADS; i++)
    pthread_join (t[i], NULL);

  for (int i = 0; i < ADMIT_THREADS; i++)
    {
      total += args[i].admitted;
      still_held += args[i].held;
      if (args[i].peak > peak)
	peak = args[i].peak;
    }

  /* Add-then-verify can transiently exceed the limit, but only by the packets
   * concurrently in flight - one per worker - and every one is unwound. */
  check (peak <= limit + ADMIT_THREADS * ADMIT_LEN,
	 "over-admission is bounded by one packet per worker",
	 "peak %u, limit %u, bound %u", peak, limit,
	 limit + ADMIT_THREADS * ADMIT_LEN);
  check ((u64) usage == still_held * ADMIT_LEN,
	 "usage equals what the workers still hold",
	 "usage %u, held %" PRIu64 " x %d", usage, still_held, ADMIT_LEN);

  /* Releasing the remainder returns it to zero, with no lost fetch_sub. */
  for (u64 i = 0; i < still_held; i++)
    cake_agg_release (&usage, ADMIT_LEN);
  check (usage == 0, "release drains usage to zero", "usage %u", usage);

  char detail[80];
  snprintf (detail, sizeof detail, "%d threads, %" PRIu64 " admits, peak %u/%u",
	    ADMIT_THREADS, total, peak, limit);
  pass ("aggregate admission is exact under contention", detail);
}

/* Concurrent gate: the CAS loop must never let the shaper hand out more than
 * its configured rate however many workers push at it. */
#define GATE_THREADS 8

typedef struct
{
  u64 *vt;
  u64 cost;
  u64 deadline;
  u64 admitted;
} gate_arg_t;

static void *
gate_thread (void *raw)
{
  gate_arg_t *a = raw;

  for (u64 now = 0; now < a->deadline; now += 1000)
    if (cake_shaper_gate_take (a->vt, a->cost, now, 10ULL * 1000000ULL))
      a->admitted++;

  return NULL;
}

static void
test_gate_under_contention (void)
{
  u64 rate = 125000000ULL; /* 1 Gbit/s */
  u64 scaled = cake_rate_scaled (rate);
  u64 vt = 0;
  u64 window = 1000000000ULL;
  pthread_t t[GATE_THREADS];
  gate_arg_t args[GATE_THREADS];
  u64 total = 0;
  double achieved, over;

  for (int i = 0; i < GATE_THREADS; i++)
    {
      args[i] = (gate_arg_t){ .vt = &vt,
			      .cost = cake_cost_ns (MTU_ADJ, scaled),
			      .deadline = window,
			      .admitted = 0 };
      pthread_create (&t[i], NULL, gate_thread, &args[i]);
    }
  for (int i = 0; i < GATE_THREADS; i++)
    pthread_join (t[i], NULL);

  for (int i = 0; i < GATE_THREADS; i++)
    total += args[i].admitted;

  /* Threads share one wall clock window, so the shaper must still cap total
   * admission at rate x window plus one burst. */
  achieved = (double) (total * MTU_ADJ);
  over = achieved - ((double) rate + 10.0e-3 * (double) rate);
  check (over <= 0, "concurrent gate does not exceed rate plus burst",
	 "admitted %.0f bytes, rate+burst %.0f", achieved,
	 (double) rate + 10.0e-3 * (double) rate);

  char detail[64];
  snprintf (detail, sizeof detail, "%d threads, %" PRIu64 " pkts", GATE_THREADS,
	    total);
  pass ("shaper gate is linearizable", detail);
}

/* Section 4.3: a zeroed word reads as a deficit of -2^31. */
static i64
shared_deficit (cake_drr_shared_child_t *c)
{
  u64 w = __atomic_load_n (&c->round_deficit, __ATOMIC_RELAXED);
  return (i64) (u32) w - (i64) CAKE_DRR_DEFICIT_BIAS;
}

static void
test_shared_reserve_basics (void)
{
  cake_drr_shared_child_t c;
  u64 W = 2500000, rb = 1000, saturated = PARENT_SATURATED;
  u64 admits = 0;
  i64 lo = 0, hi = 0;
  i64 cap = cap_for (rb, 625000, W);

  memset (&c, 0xff, sizeof c);
  cake_drr_shared_init (&c, 625000, 0);
  check (shared_deficit (&c) == 0, "init leaves a zero deficit, not -2^31",
	 "deficit %" PRId64, shared_deficit (&c));

  for (u64 r = 1; r < 20000; r++)
    {
      u64 now = r * CAKE_DRR_ROUND_PERIOD_NS;
      i64 d;

      if (cake_drr_shared_reserve (&c, rb, &W, &saturated, MTU_ADJ, now) ==
	  CAKE_DRR_ADMIT)
	admits++;

      d = shared_deficit (&c);
      if (d < lo)
	lo = d;
      if (d > hi)
	hi = d;
    }

  check (lo >= -(i64) MTU_ADJ, "debt stays bounded at one packet",
	 "low water %" PRId64, lo);
  check (hi <= cap, "credit stays bounded by the refill cap",
	 "high water %" PRId64 ", cap %" PRId64, hi, cap);

  /* 20000 rounds at 250 bytes of credit is 5 MB, or ~3300 MTU packets. */
  check (admits > 3200 && admits < 3400, "admits at its credit rate",
	 "admits %" PRIu64, admits);

  char detail[80];
  snprintf (detail, sizeof detail, "deficit in [%" PRId64 ", %" PRId64 "]", lo,
	    hi);
  pass ("shared reserve: init, debt and credit bounds", detail);
}

static void
test_shared_refund_pairs (void)
{
  cake_drr_shared_child_t c;
  /* A round's credit well above one packet, so the second reserve is a
   * genuine admission rather than a debt-blocked one. */
  u64 W = 2500000, rb = 100000, saturated = PARENT_SATURATED;
  u64 before, after;
  u64 now = 5 * CAKE_DRR_ROUND_PERIOD_NS;

  cake_drr_shared_init (&c, 625000, 0);
  cake_drr_shared_reserve (&c, rb, &W, &saturated, MTU_ADJ, now);

  before = __atomic_load_n (&c.round_deficit, __ATOMIC_RELAXED);
  check (cake_drr_shared_reserve (&c, rb, &W, &saturated, MTU_ADJ, now) ==
	   CAKE_DRR_ADMIT,
	 "a second reserve inside the same round is admitted", "");
  cake_drr_shared_refund (&c, MTU_ADJ);
  after = __atomic_load_n (&c.round_deficit, __ATOMIC_RELAXED);

  check (before == after, "refund exactly reverses its reserve",
	 "before %llx after %llx", (unsigned long long) before,
	 (unsigned long long) after);
  check ((u32) (before >> 32) == (u32) (after >> 32),
	 "refund cannot carry into the round tag", "");
  pass ("two-level refund pairs exactly", "");
}

/* Section 9.1: shared-reserve linearizability. Every worker owning a member of
 * the same S-VLAN CASes this one word per packet. */
#define RESERVE_THREADS 8

typedef struct
{
  cake_drr_shared_child_t *child;
  u64 *W;
  u64 *parent_vt;
  u64 rb;
  u64 admitted;
} reserve_arg_t;

static void *
reserve_thread (void *raw)
{
  reserve_arg_t *a = raw;

  for (u64 r = 1; r < 20000; r++)
    if (cake_drr_shared_reserve (a->child, a->rb, a->W, a->parent_vt, MTU_ADJ,
				 r * CAKE_DRR_ROUND_PERIOD_NS) ==
	CAKE_DRR_ADMIT)
      a->admitted++;

  return NULL;
}

static void
test_shared_reserve_linearizable (void)
{
  cake_drr_shared_child_t c;
  u64 W = 2500000, rb = 1000, saturated = PARENT_SATURATED;
  pthread_t t[RESERVE_THREADS];
  reserve_arg_t args[RESERVE_THREADS];
  u64 total = 0, ceiling;
  i64 d;

  cake_drr_shared_init (&c, 625000, 0);

  for (int i = 0; i < RESERVE_THREADS; i++)
    {
      args[i] = (reserve_arg_t){ .child = &c,
				 .W = &W,
				 .parent_vt = &saturated,
				 .rb = rb,
				 .admitted = 0 };
      pthread_create (&t[i], NULL, reserve_thread, &args[i]);
    }
  for (int i = 0; i < RESERVE_THREADS; i++)
    pthread_join (t[i], NULL);

  for (int i = 0; i < RESERVE_THREADS; i++)
    total += args[i].admitted;

  d = shared_deficit (&c);

  /* Threads race the same 20000 rounds, so the total admitted must still be
   * what one round-sequence's worth of credit buys: a lost update or a double
   * refill would show up here as extra bytes. */
  ceiling = (20000 * 250 + (u64) cap_for (rb, 625000, W) + MTU_ADJ) / MTU_ADJ;
  check (total <= ceiling, "no more admitted than the credit issued",
	 "admitted %" PRIu64 ", ceiling %" PRIu64, total, ceiling);
  check (d >= -(i64) MTU_ADJ && d <= cap_for (rb, 625000, W),
	 "the packed word never wraps under contention", "deficit %" PRId64,
	 d);

  char detail[80];
  snprintf (detail, sizeof detail, "%d threads, %" PRIu64 "/%" PRIu64 " admits",
	    RESERVE_THREADS, total, ceiling);
  pass ("shared reserve is linearizable", detail);
}

int
main (void)
{
  printf ("osvbng_qos_sched DRR/shaper harness\n");
  printf ("arithmetic and concurrency only; shares belong to "
	  "tests/fairness-rig.sh\n\n");

  test_refill_does_not_forgive_debt ();
  test_refill_cap ();
  test_quantum_no_overflow ();
  test_oversized_head_packet ();
  test_escape_does_not_underflow_at_zero ();
  test_round_tag_wrap ();
  test_stale_round_rebases ();
  test_rate_precision ();
  test_gate_rate_accuracy ();
  test_credit_is_proportional_to_weight ();
  test_activation_is_idempotent ();
  test_weight_accounting_under_churn ();
  test_admission_race ();
  test_gate_under_contention ();
  test_shared_reserve_basics ();
  test_shared_refund_pairs ();
  test_shared_reserve_linearizable ();

  printf ("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
