# Implementation Spec: S-VLAN HQoS with Fair Queueing

## 1. Overview

Add a per-S-VLAN aggregate tier between the subscriber CAKE scheduler and the
per-port aggregate, and make every aggregate share its rate fairly between its
children instead of serving them in pool-index order. Fairness is deficit round
robin weighted by each child's configured rate, with an optional operator
multiplier, and it applies uniformly at both tiers.

## 2. References

- `../hqos-qinq/IMPLEMENTATION_SPEC.md` — the per-S-VLAN design that preceded
  the pivot to per-port. Its §4.7 gate and §4.9 lifecycle are inherited; its
  §4.10 fairness claim is superseded by this spec (see §3).
- `../hqos-qinq/DECISIONS.md` — records "Weighted DRR in Phase 1" as rejected.
  This spec reverses that rejection; the argument is in §4.2.
- `../cake-scheduler/IMPLEMENTATION_SPEC.md` — the leaf level, unchanged here.
- Issue #8 — aggregate starves children by scheduler index. Measured evidence.
- Broadband Forum TR-178 §5.4 (hierarchical scheduling at the access node),
  TR-101 §3.2 (S-VLAN per customer / per service model).
- Shreedhar & Varghese, *Efficient Fair Queueing using Deficit Round Robin*,
  SIGCOMM '95 — the DRR quantum and deficit-carry rules in §4.4.
- VPP `src/vppinfra/bitmap.h` — `clib_bitmap_next_set`, the walk this spec
  makes order-independent.

## 3. Current State

Two levels ship today: the subscriber `cake_sched_t` (real queues, DiffServ
tins, COBALT AQM, virtual-time shaper) and a per-port `cake_aggregate_t`
(lockless, shared across workers, virtual-time gate plus buffer admission).

The aggregate has no child list. `hqos-qinq` §4.10 justified this on two
claims, both of which are now measured to be false:

1. *"Natural worker distribution ... no single subscriber gets preferential
   access to the aggregate token bucket."* `cake_dequeue.c` walks its
   active-scheduler bitmap with `clib_bitmap_foreach`, which yields ascending
   pool index and is stable across dispatches. Within a worker, the first
   scheduler polled consumes the available credit and the rest find the gate
   shut — every dispatch, indefinitely. RSS does not enter into it.

2. *"Perfect per-subscriber aggregate fairness would require the thread-pinned
   DRR model which defeats VPP's threading."* It does not. §4 gives DRR with no
   pinning, no child iteration on the hot path, and one shared atomic that
   moves on activation rather than per packet.

Measured on VPP v26.06, four 5 Mbit/s schedulers under one 8 Mbit/s aggregate,
all offered far above their rate:

```
host-veth0.100   dequeued 3743 pkts  5,412,378 bytes   ~676 KB/s
host-veth0.101   dequeued 1840 pkts  2,660,640 bytes   ~332 KB/s
host-veth0.102   dequeued    0 pkts          0 bytes        0
host-veth0.103   dequeued    0 pkts          0 bytes        0

host-veth0 (agg) shaped  5583 pkts  8,073,018 bytes  ~1,000 KB/s
```

The aggregate holds its configured rate precisely. The split is strict priority
by index: `.100` takes its full subscriber rate, `.101` takes the exact
remainder (625,000 + 375,000 = 1,000,000 B/s), and the tail starves. There is
no operator signal — `backpressure` counts buffer admission, not the gate, so
the starved children report ordinary AQM and overflow drops.

### Prerequisites

This spec is unimplementable until the session-parentage fixes merge
(`osvbng-vpp-plugin-ipoe` #7, `osvbng-vpp-plugin-pppoe-control` #3). Session
interfaces are created by `vnet_register_interface`, which leaves
`sup_sw_if_index == sw_if_index`, so the attachment walk terminates at the
session and no scheduler ever resolves an S-VLAN or a port. State this in the
issue before starting Phase 5.

This spec does **not** depend on PR #9 (walk rotation). §4.4 makes walk order
irrelevant on its own; if #9 merges, the two are compatible and #9 becomes
redundant rather than conflicting.

PR #5 (`fix/agg-buffer-accounting`) is a **hard prerequisite**, not merely
preferred. §4.9's charge transfer and the §9.1 accounting invariants are exact
only because #5 makes `cs->buffer_usage` track precisely what a scheduler has
charged to its parent: it discharges the three free paths `main` leaks (the
ring-full drop after the aggregate charge at `cake_enqueue.c:274`, flow
eviction, and teardown drain — all routed through its `cake_flow_discard`
helper) and converts buffer admission to fetch-add-then-verify. §4.10 and §8
describe the post-#5/#6 code, not `main`.

An incidental effect worth recording: the shipped *subscriber* shaper accrues
unbounded idle credit (`cake_dequeue.c:246` compares against a virtual time
with no behind-clamp analogous to PR #4's `CAKE_AGG_BURST_NS`), so a
long-idle subscriber can burst its whole backlog at parent-gate speed. Under
this spec the parent gates cap that burst and DRR bounds its intra-aggregate
unfairness. Subscriber-only deployments retain the exposure — a separate
issue, not this spec's scope.

## 4. Design

### 4.1 Architecture

```
subscriber cake_sched_t ──gate+DRR──▶ S-VLAN cake_aggregate_t ──gate+DRR──▶ port cake_aggregate_t
     (real queues)                      (virtual time)                       (virtual time)
```

Three levels, one arbitration mechanism used twice. A scheduler competes with
its siblings inside its S-VLAN; an S-VLAN competes with other S-VLANs for the
port. A scheduler whose S-VLAN has no aggregate competes directly for the port.

### 4.2 Why DRR, and why it does not need pinning

The `hqos-qinq` rejection assumed DRR requires a central arbiter iterating
children in order, which in VPP means every child of an aggregate shares one
owner thread. That would cap an aggregate at one core and fight RSS, and
rejecting it was correct.

DRR does not require that. Arbitration can be **child-driven**: each child
carries its own deficit and refuses itself when it is spent. No child list is
walked on the hot path, no ordering is imposed, and workers stay independent.
What the parent needs to know is only the *sum* of its active children's
weights, which changes on activation transitions, not per packet.

Two properties follow:

- **Walk order stops mattering.** Whichever scheduler `clib_bitmap_foreach`
  reaches first can only take its quantum, not all the credit. This is what
  closes issue #8 without the rotation fix.
- **Scheduler-side DRR state is uncontended.** A scheduler has exactly one
  `owner_thread` and only that thread dequeues it (`cake_dequeue.c:240`), so
  its deficit lives in `cake_sched_t` and costs no shared cache line. The one
  exception is the S-VLAN's own child state versus the port, which any worker
  owning one of its members touches per packet; §4.4 makes that word
  linearizable rather than pretending it is uncontended.

### 4.3 DRR children and weights

Anything competing for a parent's rate is a DRR child, whether it is a
subscriber scheduler or an S-VLAN aggregate:

```c
typedef struct
{
  u64 effective_weight;   /* own rate_bytes_per_sec x weight multiplier */
  i64 deficit;            /* bytes sendable; negative = bounded debt (§4.4) */
  u32 round;              /* round tag the deficit belongs to */
} cake_drr_child_t;       /* owner-thread-local: schedulers only */

typedef struct
{
  u64 effective_weight;   /* read-mostly; lives with the identity group */
  u64 round_deficit;      /* (round u32 << 32) | biased deficit u32; atomic.
                             deficit stored as value + CAKE_DRR_DEFICIT_BIAS
                             (2^31) so bounded debt packs unsigned (§4.4) */
} cake_drr_shared_child_t; /* multi-writer: an aggregate's own child state */
```

The plain form is embedded in `cake_sched_t` — single writer, the owner
thread. The shared form is embedded in `cake_aggregate_t`, because an S-VLAN's
deficit versus the port is written by whichever worker dequeues one of its
members. Packing round and deficit into one word is what lets §4.4 refill,
admit and decrement in a single CAS. Both halves fit u32 comfortably: the
deficit is stored biased (`value + CAKE_DRR_DEFICIT_BIAS`, bias 2^31) so
bounded debt packs unsigned — it spans one max packet of debt (eligibility
requires positivity, §4.4) up to `cap = max(2 * quantum,
CAKE_MAX_PKT_BYTES)`, and even a 100 Gbit/s parent with 1 ms rounds caps at
25 MB, keeping the biased value inside [2^31 − 64 KB, 2^31 + 25 MB] against a
4.29e9 ceiling. The u32 round tag wraps every ~50 days of uptime, costing at
worst one spurious refill-or-skip on an idle child — benign. A scheduler has
exactly one child struct, for its immediate parent — never one per tier.

```
effective_weight_i = rate_bytes_per_sec_i * weight_i        (weight_i default 1)
W                  = SUM effective_weight over active children
share_i            = effective_weight_i / W
round_bytes        = agg_rate_bytes_per_sec * ROUND_PERIOD_NS / 1e9
quantum_i          = round_bytes * effective_weight_i / W
```

**Evaluate in that order.** Computing `agg_rate * ROUND_PERIOD_NS *
effective_weight` before dividing overflows `u64`: at 1 Gbit/s with a 1 ms round
and a weight of 1.25e8, the intermediate reaches ~1.5e22 against a `u64` ceiling
of 1.8e19. Reducing to `round_bytes` first keeps every intermediate under 1e14.
This is the same class of arithmetic bug as the rate truncation already fixed in
this plugin, so the spec states the order rather than leaving it to the
implementer.

The order alone is not sufficient: `round_bytes * effective_weight_i` still
overflows `u64` when the multiplier is large — a 25 Gbit/s child with
`weight 1000` under a 100 Gbit/s port reaches 3.9e19 and wraps silently,
turning the child's quantum into garbage. Two guards, both mandatory:
`weight` is validated to 1–256 at the API handler and at osvbng commit time
(no residential-BNG case needs more, and it bounds the product below 5e18 for
any pair of ≤100 Gbit/s rates), and the quantum is computed with a 128-bit
intermediate:

```c
quantum_i = (u64) (((unsigned __int128) round_bytes * effective_weight_i) / W);
```

This runs at refill — once per child per round, not per packet — so the wide
divide costs nothing measurable.

The default weight is the child's own configured rate, so with no configuration
at all a 100 Mbit/s subscriber gets ten times the congested share of a
10 Mbit/s subscriber, and a 500 Mbit/s S-VLAN gets five times a 100 Mbit/s one.

The explicit `weight` is a **multiplier, not a replacement**. Replacing the
derived rate would make mixed configuration nonsensical: a child set to
`weight 2` alongside children defaulting to `125000000` would be crushed. As a
multiplier it is safe to set on a subset and means "this many times the share
your rate alone earns".

### 4.4 The DRR admission rule

One state layout serves both tiers; the access discipline differs by writer
count:

```c
static_always_inline u8
cake_drr_local_admit (cake_aggregate_t *parent, cake_drr_child_t *child,
		      u32 adj_len, u64 now_ns);   /* owner-local, no refund
						     obligation */
static_always_inline u8
cake_drr_shared_reserve (cake_aggregate_t *parent,
			 cake_drr_shared_child_t *child, u32 adj_len,
			 u64 now_ns);   /* refill+admit+decrement, one CAS */
static_always_inline void
cake_drr_shared_refund (cake_drr_shared_child_t *child, u32 adj_len);
```

**Rounds come from the wall clock**, not from bytes sent:

```c
u32 round = now_ns / CAKE_DRR_ROUND_PERIOD_NS;   /* 1 ms */
```

This is the single most important choice in the design. If replenishment were
driven by bytes sent — a byte-clock round, or a naive virtual-time eligibility
bound — then "every child is blocked" becomes self-sustaining: nobody sends, so
the clock never advances, so nobody unblocks. Wall time advances regardless of
whether any packet moves, so that deadlock class does not exist.

**Deficit accumulates and is capped:**

```c
if (child->round != round)
  {
    i64 cap = clib_max (2 * (i64) quantum, CAKE_MAX_PKT_BYTES);
    child->deficit = clib_min (child->deficit + (i64) quantum, cap);
    child->round = round;
  }
```

`cap` and the addend are **signed** on purpose: `deficit` may be negative
(debt, below), and a `u64` operand would promote it to a huge unsigned value,
making `clib_min` return `cap` and silently forgive the debt on every refill.
The harness's oversized-packet row catches this mistake.

Accumulation smooths bursts; it is no longer what guarantees progress
(eligibility is positivity — below). `quantum_i` falls below one MTU whenever
a parent has many children — a 1 Gbit/s aggregate with 1000 equal subscribers
and 1 ms rounds gives 125 bytes — and accumulation up to the cap lets such a
child assemble a full packet of credit rather than cycling through debt for
every packet. The cap stops a child blocked on its own subscriber shaper from
hoarding while idle-ish. `CAKE_MAX_PKT_BYTES` is 2048 — one standard-MTU-class
packet of headroom; its value is deliberately *not* load-bearing, because
packets larger than the cap (jumbo × ATM inflation, unsplit GSO chains) ride
the debt mechanism below instead of needing the cap to cover them.

A scheduler's child clamps `deficit = clib_min (deficit, 0)` on activation:
going idle earns nothing, and debt survives. Both halves matter — resetting
debt too would let a sparse child (whose queue drains between packets,
re-activating on each one) send one full-size packet per round regardless of
its quantum, a `pkt_size / quantum`-fold overshare under saturation. The
shared child does **not** reset on activation: the 0→1 transition happens on
the enqueue hot path, where a racing reset against a concurrent reserve from
another worker would lose a decrement. Carryover across an idle period is
instead bounded by the refill cap — at most `cap` bytes, the same two-quantum
burst allowance every child already has — and its debt likewise persists in
the biased word.

**Admission** is `deficit > 0` — the discipline the plugin's own flow-level
DRR (`cake_dequeue.c:317-320`) and Linux sch_cake already use. On success the
full `adj_len` is subtracted and the deficit may go negative: debt, bounded at
one packet because eligibility requires positivity, repaid from subsequent
refills before the child is eligible again. Long-run byte fairness is
identical to the `>= adj_len` variant, and no packet size can stall a child.
A `>= adj_len` rule cannot be used here: refills clamp at the cap, so any
head packet with `adj_len > cap` — MTU 9000 with the `dsl-pppoe-atm` preset
gives `adj_len ≈ 9,976`; an unsplit GSO chain reaches tens of KB
(`OSVBNG_CAKE_FLAG_SPLIT_GSO` is declared but nothing splits) — would never
be admitted while the parent is saturated and the escape never fires,
wedging the entire subscriber permanently. Deficit, quantum and the cap are
all **bytes**; `cost_ns` is the rate gate's unit and does not appear in the
DRR path.

**Work-conserving escape**, consulted only after the deficit check or the
reserve refuses:

```c
if (parent_shaper_time + CAKE_DRR_ROUND_PERIOD_NS <= now_ns)
  return 1;    /* parent genuinely idle: admit without reserving */
```

Virtual time a full round behind the wall clock means real spare capacity.
Without this an idle child's unused quantum is lost every round. Applied per
tier independently. The addition form matters: `now_ns - PERIOD` underflows
u64 in the first round after boot, and in the harness, whose stub clock
starts at zero.

Ordering the escape *after* the eligibility check keeps DRR state honest
through idle periods: an eligible child decrements normally even when the
parent is idle, so deficits drain toward zero and every child enters a
congestion onset with bounded state — roughly zero to `cap`, never a full
allowance and never deep debt — with the escape covering only children whose
deficit is spent. The blocked-path reserve is read-only-fail (below), so the
idle-path cost is one shared read of a line the gate CAS touches anyway. An
escape admission neither decrements nor reserves: usage under an idle parent
does not count against the child's congested share, which is also what keeps
debt bounded at one packet.

**The two disciplines.** For a scheduler's own child the check does not mutate
— the owner thread checks early and decrements only on final success (§4.5), so
no refund can ever be needed. That discipline is unsound for the shared child:
two workers passing a read-only check against the last quantum would both
decrement later, either losing an update or wrapping the deficit below zero and
holding the tier's admission open until the next refill.
`cake_drr_shared_reserve` therefore linearizes the whole step in one CAS on
`round_deficit`, the same loop shape as the shipped
`cake_agg_dequeue_gate` (`osvbng_qos_sched.h:702`):

```c
do
  {
    old = __atomic_load_n (&child->round_deficit, __ATOMIC_ACQUIRE);
    (cur_round, biased) = unpack (old);        /* biased = deficit + BIAS */
    if (cur_round != round)
      biased = clib_min (biased + quantum,
			 CAKE_DRR_DEFICIT_BIAS + cap);       /* refill */
    admitted = biased > CAKE_DRR_DEFICIT_BIAS;               /* deficit > 0 */
    if (!admitted && cur_round == unpack_round (old))
      return 0;                       /* nothing to publish: fail read-only */
    new = pack (round, admitted ? biased - adj_len : biased);
  }
while (!CAS (&child->round_deficit, old, new));
```

A failed admission in an unchanged round touches nothing, so the common
blocked path stays read-only. A refill is published even when admission fails,
so the round advances without requiring a successful send. `biased - adj_len`
cannot cross zero: admission requires `biased > 2^31` and `adj_len` is far
below 2^31, so the u32 half never wraps.

`cake_drr_shared_refund` is an `__atomic_fetch_add` of `adj_len` to the low
half — it cannot carry into the round bits. One refund can be in flight per
*worker* (each worker holds at most one packet between reserve and refund),
so the transient ceiling is `BIAS + cap + n_workers × max adj_len`: with a
25 MB cap, 512 workers and 64 KB chains that is still under 2.2e9 against the
4.29e9 ceiling. If a round boundary lands between reserve and refund, the
refund credits the new round instead: bounded at one packet, clamped by the
next refill's cap, self-correcting — the same accuracy class as the
shaper-time refund §4.5 already accepts. Two rules keep refunds exact: a
reserve is refunded **iff it succeeded** — the work-conserving escape admits
without reserving, and the dequeue path carries a local flag recording
whether the reserve happened — and each reserve is refunded at most once.

### 4.5 The two-level dequeue path

```
1. subscriber's own shaper                        (unchanged)
2. DRR eligibility vs immediate parent — owner-local, deficit > 0
3. S-VLAN rate gate CAS          — closed: refuse, nothing charged
4. S-VLAN's DRR reserve vs port  — one CAS; blocked: refund the S-VLAN gate
5. port rate gate CAS            — closed: refund the S-VLAN gate
                                   and the step-4 reserve
6. success: decrement the scheduler's own deficit (owner-local,
   may go negative — debt, §4.4)
```

**Scheduler deficits never need refunding.** Checked at step 2, mutated only at
step 6, by a single writer — the owner thread — so nothing can interleave.

**Placement inside `cake_dequeue_one`.** The step-2 check executes before
`cobalt_should_drop` and before the ECN mark — its only inputs are `pkt_len`
(already read) and owner-local state — so a DRR-blocked visit mutates nothing
at all. This is load-bearing: a blocked child retries every polling dispatch,
three orders of magnitude more often than rounds turn, and placed after the
AQM like today's gate check each retry would re-run COBALT on the same head
packet, escalate `codel_count`, inflate `ecn_marks`, and over-drop once the
child unblocks. It also matches sch_cake, which does not run the AQM while
the shaper holds the qdisc closed; AQM action is deferred at most one round.
Steps 3–5 keep the existing AQM-before-gate order, so COBALT continues to
drop and drain under a closed gate.

The S-VLAN's shared deficit cannot use that discipline (§4.4): step 4 is a
reserve, and it acquires a refund obligation on the two later failure paths.
The failure ordering is deliberate. The most common rejection under saturation
— the S-VLAN sitting at its own configured rate — fails at step 3 and unwinds
nothing. A DRR block at step 4 refunds only the S-VLAN gate charge, a single
`__atomic_fetch_sub` of the same `cost_ns` that was added. Only the rare port
rejection at step 5 unwinds two things.

When a scheduler has no S-VLAN aggregate, step 2 checks the scheduler's own
`drr_child` against the port, steps 3 and 4 are skipped, and a port-gate
failure at step 5 unwinds nothing. The path reduces to exactly the single-tier
behaviour of Phase 1.

Child-first ordering keeps the step-5 refunds rare: S-VLAN aggregates are
normally provisioned under port rate, so the port gate rarely rejects what the
S-VLAN gate accepted. Refund safety is unchanged from `hqos-qinq` for the gate
and bounded per §4.4 for the reserve: a concurrent worker that observed the
inflated value waits marginally longer for one packet — bounded,
self-correcting, accuracy not correctness.

Step 4 writes the S-VLAN's own deficit, shared state written per packet. This
is the one genuinely new shared write the design adds; §4.7 places it. Flat DRR
straight to the port would avoid it, but would let a customer with 100
subscribers out-compete a customer with 5, which defeats the purpose of having
an S-VLAN tier at all.

### 4.6 Dequeue outcomes

`cake_dequeue_one` gains one outcome:

```c
typedef enum
{
  CAKE_DEQ_EMPTY = 0,
  CAKE_DEQ_SENT,
  CAKE_DEQ_DROPPED,
  CAKE_DEQ_GATE_CLOSED,   /* parent at its configured rate */
  CAKE_DEQ_DRR_BLOCKED,   /* this child has spent its quantum */
} cake_deq_result_t;
```

Both blocked outcomes leave the scheduler and let the polling node retry on the
next dispatch with a fresh clock. They are counted separately because they mean
different things to an operator: `drr_blocked` is "your siblings are using the
capacity", `parent_blocked` is "the parent is full". Today neither is
distinguishable from an ordinary drop.

### 4.7 Data structures

`cake_sched_t` gains, all owner-thread-local:

| Field | Purpose |
|---|---|
| `drr` | `cake_drr_child_t` for the immediate parent |
| `weight` | operator multiplier, default 1 |
| `agg_svlan_index`, `agg_port_index` | cached attachment, either may be `~0` |

`cake_aggregate_t` gains:

| Field | Cache line | Purpose |
|---|---|---|
| `level`, `parent_index`, `svlan_id`, `weight` | cacheline0 (read-mostly) | identity and hierarchy |
| `drr.effective_weight` | cacheline3, with the packed word | the aggregate's own weight as a child; see amendment below |
| `active_weight` (`u64`), `n_active_children` (`u32`) | cacheline4 | activation atomics, written on activation transitions, read at refill |
| `drr.round_deficit` | cacheline3 | packed `(round, biased deficit)` word — an S-VLAN's own deficit vs the port, CAS-written per packet |

**Amended at Phase 6** (this table originally placed `drr.effective_weight`
on cacheline0): the implementation keeps the whole
`cake_drr_shared_child_t` on cacheline3. The weight is read on exactly one
path — the refill inside `cake_drr_shared_reserve` — by the same worker that
is about to CAS `round_deficit` beside it, so splitting the pair would touch
two lines where one does; and parking it on cacheline0 would put a field
that is semantically part of the CAS-contended object on the line every
worker reads per packet. Raised as an unrecorded deviation by the Phase 6
spec-compliance review (`code-reviews/CODEX.md` #3); accepted as the better
layout and recorded here rather than reverted.

The shipped layout is preserved exactly: `global_shaper_time_ns` on cacheline1,
`buffer_usage` on cacheline2, per-thread stats out of line. Packing the new
per-packet write next to either existing atomic would reintroduce the cache-line
bounce that commit `7c04b13` removed, so `round_deficit` takes its own line.

The activation pair takes its own line for the same reason, because
activation transitions track *traffic*, not configuration: a scheduler
deactivates the moment its queues drain and re-activates on the next packet,
so a 50 pps VoIP-only subscriber toggles once per packet, and a single-member
S-VLAN propagates each toggle to the port (§4.9). Parked on cacheline0, those
writes would invalidate — at sparse-traffic rate — the line every worker
reads per packet for `rate_ns_per_byte_scaled` (gate cost) and `buffer_limit`
(admission). The two share a line with each other (they are always written
together), never with the read-mostly identity group.

Per-port S-VLAN map: 4096-entry `u32` vector hung off the port aggregate
(~16 KB per port, O(1) lookup), `~0` for unmapped tags.

### 4.8 Attachment

Unchanged in mechanism. `cake_sched_enable_disable` walks `sup_sw_if_index`
toward the physical port. At each hop, if the interface is
`VNET_SW_INTERFACE_TYPE_SUB` with `sub.eth.flags.one_tag`, read
`sub.eth.outer_vlan_id` and look it up in the port's S-VLAN map. Hit caches
`agg_svlan_index` and points `drr` at the S-VLAN; miss leaves the scheduler
competing directly for the port. Disable clears both indices and removes the
child's weight from whichever parent held it.

osvbng's S-VLAN sub-interfaces are single-tagged (`parent.svlan` via
`CreateSubif`; the C-VLAN is never a VPP sub-interface), so this reads
`one_tag`, not a QinQ pair.

The control plane manages aggregates only. Attachment stays automatic — no bind
API, no per-session call.

### 4.9 Weight accounting and lifecycle

`active_weight` is maintained at exactly three sites:

| Site | Transition |
|---|---|
| `cake_enqueue.c:347` | scheduler 0→1 active: add `effective_weight` to parent, `fetch_add` parent's `n_active_children` |
| `cake_dequeue.c:267,393` | scheduler 1→0 active: reverse |
| `osvbng_qos_sched.c:305` | teardown deactivation: reverse |

Propagation upward is a refcount. On `fetch_add`, only the worker observing
`prev == 0` adds the aggregate's own `effective_weight` to *its* parent; on
`fetch_sub`, only the one observing `prev == 1` subtracts. An S-VLAN therefore
counts toward the port's `W` exactly while it has at least one active child.

The scheduler-side active flag stays owner-thread-local. Only the parent
counters are atomic, and they move on activation transitions rather than per
packet (though for sparse traffic activation transitions *are* per-packet —
§4.7's placement rationale).

Two further paths clear activity bits without a weight transition, and the
distinction is load-bearing. The dequeue walk routes stale entries through
the same `deactivate[]` array and bitmap-clear loop as the empty-detect site:
`pool_is_free_index` (`cake_dequeue.c:232`) and the owner-mismatch check
(`cake_dequeue.c:242`), both reachable when teardown cleared state while a
dispatch was mid-walk. Weight moves **only** on the empty-detect deactivation
of a live, owned scheduler and at the teardown site; the defensive paths
clear the bit and touch nothing else. Subtracting there double-counts a
teardown that already ran — `active_weight` underflows and every sibling's
quantum collapses — while never subtracting on empty-detect leaks weight
upward instead. The §9.1 churn row asserts both directions.

Lifecycle operations, all under the worker barrier:

- **Create backfills.** Creating an S-VLAN aggregate on a port with live
  sessions walks the scheduler pool, re-resolves attachment for schedulers whose
  encap carries that tag, and **moves their weight contribution from the port to
  the new S-VLAN**. Without the move the port double-counts them.
- **Backfill also transfers each moved member's outstanding buffer charge:**
  `svlan->buffer_usage += cs->buffer_usage`. The port side is **not**
  decremented: those packets charged the port at enqueue and still discharge
  it at free time — the transfer only pre-credits the S-VLAN for discharges
  it never admitted, and a symmetric subtract would under-count the port and
  over-admit. Discharge resolves through the
  scheduler's *current* attachment at free time (`cake_agg_discharge`,
  `osvbng_qos_sched.h:691`), and a member's queued packets were charged to the
  port only. Without the transfer, the first pre-backfill packet freed
  underflows the S-VLAN's `u32 buffer_usage`, which wraps and pins the
  admission check (`cake_enqueue.c:263`) permanently shut — one backfill under
  backlog bricks the tier. Shipped code never hits this because it never
  reparents a scheduler that holds charged packets; backfill is what introduces
  the hazard, and the transfer is what discharges it.
- **S-VLAN delete detaches members to the port**, moving weight the other way
  and reversing the charge transfer (`svlan->buffer_usage -= cs->buffer_usage`)
  per member. Required for per-tag detach (`svlan <id> disable` on a
  multi-range aggregate), where the aggregate survives with other members and
  stale charges would otherwise hold backpressure on forever. On full delete
  the counter dies with the aggregate, and the port side stays balanced without
  adjustment: those packets charged the port at enqueue and still discharge it
  after the detach.

The transfer is exact, not approximate. `cs->buffer_usage` and the aggregate
charge share the same basis — raw `pkt_len` (`cake_enqueue.c:274` and `:310`)
— and the charge site runs only on the owner thread (`enqueue_local` is
reached solely via the owner check or the one-time owner CAS,
`cake_enqueue.c:180-193`; handed-off packets are uncharged until re-enqueued
owner-side). Under the worker barrier no charge or discharge is in flight, so
at transfer time `cs->buffer_usage` equals the member's outstanding parent
charge to the byte. This equality is PR #5's invariant — the reason it is a
hard prerequisite (§3).
- **Port delete with children is refused.**
- **Rate or weight update** adjusts the parent's `active_weight` by the delta if
  the child is currently active. This is why `osvbng_cake_aggregate_update`
  exists: without it a rate change is delete-and-recreate, which drops the child
  out of `W` and back in.
- **Interface deletion** reuses the existing `VNET_SW_INTERFACE_ADD_DEL_FUNCTION`
  hook, extended to unwind the parent chain.

All lifecycle mutation runs under the API/CLI worker barrier. No handler in this
plugin may be marked mp-safe without re-review.

**Invariants, asserted by the harness:** after arbitrary churn,
`agg->active_weight == SUM effective_weight of active children`,
`agg->n_active_children == |active children|`, and
`agg->buffer_usage == SUM cs->buffer_usage` over the schedulers currently
attached beneath it (direct members for an S-VLAN; every scheduler under the
port for a port). Every path above can break them and a leak is silent — too
high and every child under-sends or the tier back-pressures spuriously, too low
and shares skew or admission over-commits.

### 4.10 Enqueue admission

Unchanged in shape: charge the S-VLAN's `buffer_usage`, then the port's; unwind
the S-VLAN on port rejection. Both use the atomic fetch-add-then-verify pattern
PR #5 establishes, so over-admission stays bounded at one packet per worker per
level. The discharge helper generalises to walk the parent chain, so every
charged-buffer free path — including PR #5's `cake_flow_discard` paths
(ring-full drop, flow eviction, teardown drain) — releases at both levels.

A read-only overload filter fronts the fetch-add-verify pair at each level:
relaxed-load the usage and drop without any RMW when it already exceeds the
limit. This is not the pre-#5 race returning — the stale read is used only to
*reject* at an already-full queue, where a marginally early or late drop is
the outcome regardless; every admission still goes through the exact
fetch-add-then-verify. Without it, sustained incast overload pays four RMWs
per rejected packet (S-VLAN add, port add, port sub, S-VLAN unwind) on the
two hottest lines, from every worker, at *offered* rate — cycles taken
directly from DPDK RX polling. With it, the overload path is shared reads,
which scale.

Per-level `backpressure` counters remain the buffer-side congestion signal, kept
distinct from the gate-side `drr_blocked` and `parent_blocked`.

## 5. Configuration

### 5.1 vppctl

```
set cake aggregate <interface> rate <kbps> [weight <n>] [burst <ms>]
set cake aggregate <interface> svlan <id|range> rate <kbps> [weight <n>]
set cake aggregate <interface> svlan <id> disable
show cake aggregate [<interface>]
```

`show cake aggregate` renders the tree:

```
TenGigabitEthernet0/0/0: rate 1250000000 B/s, weight 1
  active 2/2 children, W 750000000
  buffer 0/18750000, shaped 421 pkts, backpressure 0
  svlan 100: rate 62500000 B/s (500000 kbps), weight 2, share 66.7%
    active 3/12 children, W 37500000
    drr_blocked 1842, parent_blocked 97
    ipoe_session0:  rate 12500000 B/s, weight 1, share 33.3%, drr_blocked 612
    ipoe_session1:  rate 12500000 B/s, weight 1, share 33.3%, drr_blocked 588
    ipoe_session4:  rate 12500000 B/s, weight 1, share 33.3%, drr_blocked 642
  svlan 200: rate 12500000 B/s (100000 kbps), weight 1, share 33.3%
    active 1/4 children, W 12500000
    drr_blocked 0, parent_blocked 12
```

### 5.2 Binary API

New messages; existing ones are retained unchanged so CRCs stay valid for
current control planes.

```
osvbng_cake_aggregate_v2_create {
  u32 sw_if_index;          /* port, for both levels */
  u8  level;                /* 0 = port, 1 = svlan */
  u16 svlan_id;             /* level 1 only */
  u16 svlan_id_end;         /* range end, == svlan_id for a single tag */
  u64 rate_bytes_per_sec;
  u32 weight;               /* multiplier, 0 or 1 = default */
  u32 burst_ns;             /* 10-150 ms, 0 = default */
  u32 buffer_limit;         /* 0 = derive from rate */
}
osvbng_cake_aggregate_v2_delete { u32 sw_if_index; u8 level; u16 svlan_id; }
osvbng_cake_aggregate_v2_update { u32 sw_if_index; u8 level; u16 svlan_id;
                                  u64 rate_bytes_per_sec; u32 weight;
                                  u32 burst_ns; u32 buffer_limit; }
osvbng_cake_aggregate_v2_dump   { u32 sw_if_index; }
osvbng_cake_aggregate_v2_details { ... level, parent_sw_if_index, svlan_id,
                                   weight, burst_ns, effective_weight,
                                   active_weight, n_active_children,
                                   shaped_pkts, shaped_bytes, backpressure,
                                   drr_blocked, parent_blocked ... }
osvbng_cake_sched_v2_enable_disable { ...v1 fields..., u32 weight; }
osvbng_cake_capabilities { }  /* -> version, max_levels, features bitmap */
```

Every two-tier message is named `<object>_v2_<verb>`. VPP upstream mixes the
two forms — `gre_tunnel_add_del_v2` sits beside `gre_tunnel_v2_dump` in one
file, because only the dump/details pairing is inferred from the name — but
one form throughout reads better than matching that inconsistency.

`weight` is validated to 1–256 wherever it appears (`v2_create`, `v2_update`,
`sched_v2`); 0 means default. `burst_ns` is validated to 10–150 ms in both
`v2_create` and `v2_update`: an aggregate's burst is configurable for its
whole life, not only at creation. The bound is what §4.3's overflow guard relies
on, so the handlers reject rather than clamp.

**Why `_v2` on the scheduler message rather than a separate set-weight call.**
Message CRCs are checked at runtime, so adding `weight` to the shipped
`osvbng_cake_sched_enable_disable` breaks every existing control plane. A
separate `osvbng_cake_sched_set_weight` would preserve CRCs but costs a second
binapi round trip on every session bring-up, and session setup is a declared hot
path in osvbng's contributing guidelines. `_v2` keeps it at one call and leaves
v1 working, deprecated.

`osvbng_cake_capabilities` is new because the plugin has none today, and a
control plane should be able to discover whether the dataplane it is talking to
supports the S-VLAN level rather than assuming.

### 5.3 osvbng config schema

```yaml
qos-aggregates:
  port-te0:
    interface: TenGigabitEthernet0/0/0
    rate: 10000000            # kbps
  cust-acme:
    interface: TenGigabitEthernet0/0/0
    svlans: ["100", "200-299"]
    rate: 500000              # kbps
    weight: 2                 # optional multiplier, default 1
```

`svlans` is a list of range strings. `ParseVLANRange` accepts a single value or
one hyphen range only, so comma syntax inside an entry is invalid — the list is
the way to express disjoint sets.

Commit-time validation: the interface exists; S-VLAN sets are disjoint per port;
a child's rate does not exceed its parent's; `weight` is 1–256 (§4.3);
oversubscription across children is allowed and expected.

## 6. File Plan

### Plugin (`osvbng-vpp-plugin-qos`)

| File | Change |
|---|---|
| `src/osvbng_qos_sched.h` | `cake_drr_child_t`, `cake_drr_shared_child_t`; aggregate `level`/`parent_index`/`svlan_id`/`weight`/`active_weight`/`n_active_children`/`drr`; scheduler `drr`/`weight`/`agg_svlan_index`; `cake_drr_local_admit()`, `cake_drr_shared_reserve()`, `cake_drr_shared_refund()`; `CAKE_DRR_ROUND_PERIOD_NS`, `CAKE_MAX_PKT_BYTES`, `CAKE_DRR_DEFICIT_BIAS` |
| `src/osvbng_qos_sched.c` | two-level create/delete/update, S-VLAN map, backfill and detach walks with weight and buffer-charge transfer, attachment walk extension, weight accounting on teardown, CLI |
| `src/osvbng_qos_sched.api` | messages in §5.2 |
| `src/osvbng_qos_sched_api.c` | handlers for the new messages |
| `src/cake_dequeue.c` | two-level gate with gate and reserve refunds, escape-path no-refund flag, `CAKE_DEQ_DRR_BLOCKED`, scheduler deficit decrement on success, weight accounting on deactivation |
| `src/cake_enqueue.c` | two-level buffer admission with read-only overload filter, weight accounting on activation |
| `src/osvbng_qos_sched_error.def` | `DRR_BLOCKED`, `PARENT_BLOCKED`; wire the existing unincremented `AGG_SHAPED`/`AGG_BACKPRESSURE` |
| `tests/drr_harness.c` (new) | multi-threaded harness, §9 |
| `tests/CMakeLists.txt` (new) | builds the harness standalone |

### Control plane (`osvbng`)

| File | Change |
|---|---|
| `pkg/vpp/binapi/osvbng_qos_sched/` | regenerated from the new API JSON |
| `pkg/config/qos_aggregate.go` (new) | `qos-aggregates` schema and validation |
| `pkg/conf/handlers/qos_aggregate.go` (new) | `conf.Handler` with `Dependencies()` on the interface path |
| `pkg/southbound/vpp/qos.go` | `ApplyAggregate`/`RemoveAggregate`/`UpdateAggregate`; `ApplyScheduler` moves to `_v2` and passes weight; re-assert on `ENTRY_ALREADY_EXISTS` |
| `pkg/handlers/show/qos/aggregate.go` (new) | CLI + `telemetry.RegisterMetric[southbound.AggregateState]` |
| `pkg/svcgroup/apply.go` | pass the service-group weight through to `ApplyScheduler` |

## 7. Implementation Order

Each phase is independently testable and leaves the tree working.

**Phase 1 — DRR on the existing port tier.** `cake_drr_child_t`, `cake_drr_local_admit()`,
weight accounting at the three sites, `CAKE_DEQ_DRR_BLOCKED`, counters, CLI
display of weight and share. Weights are **derived from configured rate only** —
the explicit multiplier needs the `_v2` message and arrives in Phase 4. No new
API, no S-VLAN. Closes issue #8 on its own
and is the phase that proves the mechanism. Testable: §9 fairness matrix at one
level.

**Phase 2 — the harness.** `tests/drr_harness.c` and its build. Deliberately
before the second tier: the invariants it asserts are what make Phase 3
reviewable, and it retroactively covers the untested admission race and rate
precision already merged.

**Phase 3 — the S-VLAN tier.** `level`/`parent_index`, S-VLAN map, two-level
gate with refund, two-level buffer admission, attachment walk extension,
backfill and detach. Still CLI-only.

**Phase 4 — binary API.** Messages from §5.2, handlers, capabilities query. API
freeze at this phase's exit unblocks the control-plane work.

**Phase 5 — control plane.** Regenerated bindings, config schema, conf handler,
southbound, telemetry.

## 8. Attribute Mappings

| Config (YAML) | API field | VPP internal | Notes |
|---|---|---|---|
| `rate` (kbps) | `rate_bytes_per_sec` | `rate_ns_per_byte_scaled` | `kbps * 1000 / 8`; stored Q48.16 |
| `weight` | `weight` (u32) | `drr.effective_weight` | `rate_bytes_per_sec * max(weight, 1)` |
| `svlans: ["100","200-299"]` | `svlan_id`, `svlan_id_end` | S-VLAN map entries | one message per range entry |
| `interface` | `sw_if_index` | `agg_index_by_sw_if_index` | resolved at commit time |
| (derived) | `burst_ns` | `agg->burst_ns` | default 10 ms, range 10–150 ms. Deliberately the range floor: burst credit is DRR-unarbitrated (§4.4 escape stays open while virtual time catches up), and it is the post-idle line-rate burst released into the access network — 25 ms would buy back a walk-order window at every congestion onset plus 25 ms of downstream jitter budget |
| (derived) | `buffer_limit` | `agg->buffer_limit` | 0 means derive from rate, as today |
| (none) | — | `CAKE_DRR_ROUND_PERIOD_NS` | 1 ms, compile-time; a granularity knob with no good operator answer |

## 9. Testing

### 9.1 Harness (`tests/drr_harness.c`)

The repo has no C test suite and `TESTING.md` states the af-packet/veth
environment has no downstream bottleneck. Nothing existing can prove a fairness
claim, so the harness is load-bearing rather than optional. It links the header
inlines against a stub `vlib` clock and runs N pthreads.

| Assertion | Pass criterion |
|---|---|
| Equal weights, 4 children, saturated | shares within ±2% |
| Weights 1:2:4:8, saturated | shares track ratio within ±5% |
| One child idle | aggregate utilisation ≥ 98% (work-conserving escape) |
| Quantum below MTU: 1000 children, 1 Gbit/s | every child sends, none starves |
| Progress | every active child sends within a bounded number of rounds |
| Weight invariant after randomised churn | `active_weight == SUM active children`, `n_active_children` exact |
| Teardown racing an in-flight dispatch (bit still set) | defensive deactivations (§4.9) subtract nothing; no double weight subtraction; `W` exact |
| Shared-reserve linearizability: N threads on one S-VLAN child | biased word never wraps, no double refill, debt bounded at one packet, bytes admitted per round ≤ quantum + cap + one max packet; transient refund ceiling per §4.4 |
| Two-level refund | gate and reserve refunds pair exactly once per successful reserve; escape-path admissions never refunded; accounting balances |
| Oversized head packet: `adj_len > cap`, parent saturated | child still progresses via debt; long-run share unchanged |
| Cyclic offered load (~100 ms on/off) | long-run shares track weights within ±5% |
| Onset convergence: idle → saturated transition | shares within tolerance within `cap/quantum + 2` rounds |
| Reparent under backlog: backfill and per-tag detach with queued packets | charge transfer exact, no `u32` wrap, `buffer_usage == SUM member cs->buffer_usage` afterwards |
| Rate accuracy per tier | within ±1% |
| Admission race under contention | no over-admission beyond one packet per worker |
| Rate precision at ≥1 Gbit/s | within ±1% of configured |

The last two are not new to this spec — they cover the already-merged admission
and fixed-point changes, which were verified by inspection only because
af-packet presents a single rx queue and the veth rig cannot offer
multi-gigabit. The harness is the first place either becomes provable.

### 9.2 Container rig

veth pair, af-packet, VLAN sub-interfaces, UDP offered load to an off-subnet
destination via a static neighbour with a nonexistent MAC so nothing
rate-limits the source. Note that `ping -f` self-limits (it waits for each
reply) and will silently fail to build any queue.

- Two S-VLANs of different rates under one port, several subscribers each: port
  capacity splits by S-VLAN rate, and within each S-VLAN by subscriber rate.
  This is the headline HQoS assertion.
- Weight multiplier set on one subscriber and one S-VLAN: share moves as
  configured.
- `show cake aggregate` renders the tree; `drr_blocked` and `parent_blocked`
  move independently and distinguishably.
- Backfill: create an S-VLAN aggregate while sessions are live **with standing
  backlog** (offered load above subscriber rate, so queues are provably
  nonempty); members move from the port to the S-VLAN with no accounting
  drift. Repeat for per-tag detach. Active-but-drained traffic does not
  exercise the charge transfer and does not count.
- Teardown under load at both levels: quiescent `buffer_usage == 0`.

### 9.3 Benchmark

`tests/benchmark.py` gains a phase pair: identical offered load, port-only
aggregate versus port + S-VLAN tier. The honest hot-path delta is 7 shared
RMWs across 5 lines per packet versus 3 across 2 (§10), and this is where it
becomes a measured cycles-per-packet number. Phase 3 (the S-VLAN tier) does
not merge without one.

### 9.4 containerlab

Suites 18 and 19 green throughout, proving zero behaviour change for
subscriber-only and port-only configurations.

### 9.5 Explicitly not covered

- **Latency under load.** `TESTING.md` is explicit that the af-packet/veth
  environment has no real bottleneck. No claim here depends on it.
- **RSS spreading children across owner threads.** af-packet presents one rx
  queue, so every scheduler lands on one owner thread. Cross-thread DRR is
  harness-proven and hardware-unproven until a lab window with a real NIC.

## 10. Not In Scope

- **Upstream direction.** Ingress stays a policer.
- **Priority propagation across children.** Under a closed gate the next credit
  is tin-blind: one subscriber's bulk can delay another's voice. Each
  subscriber's own tins protect their mix within whatever share they win. DRR
  could carry priority if specified to; this spec does not.
- **CIR floors and guaranteed minimums.** Weights give proportional share, not a
  guaranteed rate. A child's floor under congestion is its weighted share, which
  falls as siblings activate.
- **More than three levels.** `level` is an enum, not a depth counter. A
  C-VLAN or per-service tier would need this spec's parent chain generalised.
- **Per-aggregate overhead accounting.** Both gates charge the subscriber's
  overhead-adjusted `adj_len`, matching shipped behaviour. Ethernet-family
  presets only: ATM DSL presets under an aggregate misstate utilisation 10–20%
  on small-packet mixes and are unsupported until a per-aggregate overhead delta
  exists.
- **Configurable round period.** Compile-time constant. Revisit only if
  measurement demands it.
- **NIC offload and `vnet/tm`.** No hardware scheduler integration.
- **Hot-path batching.** `PROCESS.md` First-Class Requirement #2 mandates
  dual/quad-loop hot paths with `MULTIARCH_SOURCES` SIMD variants. The existing
  enqueue path is a scalar single loop with one 4-ahead prefetch and the dequeue
  path has no batch loop; this spec neither adds nor removes batching. Its
  honest per-packet cost for a scheduler under an S-VLAN is four additional
  shared-line RMWs — S-VLAN gate CAS, S-VLAN reserve CAS, S-VLAN buffer
  fetch_add, S-VLAN discharge fetch_sub — taking the end-to-end count from
  3 RMWs on 2 hot lines (shipped port-only) to 7 on 5. The dequeue-side RMWs
  run only for packets the tier admits, so they scale with aggregate rate,
  not line rate; the enqueue-side pair runs at *offered* rate, which is why
  §4.10's overload filter exists. The owner-local DRR check itself is one
  compare. §9.3 turns this into a measured number. Batching the existing
  paths is worth its own issue — now two specs deep against Requirement #2,
  file it alongside this one — but folding it into this spec would make the
  fairness change unreviewable.
