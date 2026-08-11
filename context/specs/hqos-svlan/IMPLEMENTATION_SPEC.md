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
- **Per-child DRR state is uncontended.** A scheduler has exactly one
  `owner_thread` and only that thread dequeues it (`cake_dequeue.c:287`), so
  its deficit lives in `cake_sched_t` and costs no shared cache line.

### 4.3 DRR children and weights

Anything competing for a parent's rate is a DRR child, whether it is a
subscriber scheduler or an S-VLAN aggregate:

```c
typedef struct
{
  u64 effective_weight;   /* own rate_bytes_per_sec x weight multiplier */
  u64 deficit;            /* bytes still sendable this round */
  u32 round;              /* round tag the deficit belongs to */
} cake_drr_child_t;
```

Embedded once in `cake_sched_t` and once in `cake_aggregate_t`. A scheduler has
exactly one, for its immediate parent — never one per tier.

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

The default weight is the child's own configured rate, so with no configuration
at all a 100 Mbit/s subscriber gets ten times the congested share of a
10 Mbit/s subscriber, and a 500 Mbit/s S-VLAN gets five times a 100 Mbit/s one.

The explicit `weight` is a **multiplier, not a replacement**. Replacing the
derived rate would make mixed configuration nonsensical: a child set to
`weight 2` alongside children defaulting to `125000000` would be crushed. As a
multiplier it is safe to set on a subset and means "this many times the share
your rate alone earns".

### 4.4 The DRR admission rule

One helper serves both tiers:

```c
static_always_inline u8
cake_drr_admit (cake_aggregate_t *parent, cake_drr_child_t *child,
		u32 adj_len, u64 now_ns);
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
    u64 cap = clib_max (2 * quantum, CAKE_MAX_PKT_BYTES);
    child->deficit = clib_min (child->deficit + quantum, cap);
    child->round = round;
  }
```

Accumulation is required, not a refinement. `quantum_i` falls below one MTU
whenever a parent has many children — a 1 Gbit/s aggregate with 1000 equal
subscribers and 1 ms rounds gives 125 bytes, and a child that reset each round
could never send a 1500-byte packet. The cap stops a child blocked on its own
subscriber shaper from hoarding while idle-ish, and the `CAKE_MAX_PKT_BYTES`
floor guarantees every child can eventually send one full-size packet.

A child resets `deficit = 0` on activation, so going idle earns nothing.

**Work-conserving escape:**

```c
if (parent_shaper_time <= now_ns - CAKE_DRR_ROUND_PERIOD_NS)
  return 1;    /* parent genuinely idle: nothing to arbitrate */
```

Virtual time a full round behind the wall clock means real spare capacity.
Without this an idle child's unused quantum is lost every round. Applied per
tier independently.

**Admission** is then `child->deficit >= adj_len`. Deficit, quantum and the cap
are all **bytes**; `cost_ns` is the rate gate's unit and does not appear in the
DRR path. The check does not mutate; see §4.5 for why.

### 4.5 The two-level dequeue path

```
1. subscriber's own shaper                      (unchanged)
2. DRR check vs S-VLAN       — read only
3. S-VLAN rate gate CAS      — closed: refuse, nothing charged
4. DRR check vs port         — read only (the S-VLAN's own drr_child)
5. port rate gate CAS        — closed: refund the S-VLAN charge
6. success: decrement both deficits
```

Checking deficits without mutating them, and decrementing only at step 6, means
**deficits never need refunding**. The refund problem stays confined to the one
atomic (`svlan->global_shaper_time_ns`), unwound with a single
`__atomic_fetch_sub` of the same `cost_ns` that was added.

When a scheduler has no S-VLAN aggregate, steps 2 and 3 are skipped and step 4
uses the **scheduler's own** `drr_child` against the port. There is then nothing
to refund, and the path reduces to exactly the single-tier behaviour of Phase 1.

Child-first ordering keeps refunds rare: S-VLAN aggregates are normally
provisioned under port rate, so the port gate rarely rejects what the S-VLAN
gate accepted. Refund safety is unchanged from `hqos-qinq`: a concurrent worker
that observed the inflated time waits marginally longer for one packet —
bounded, self-correcting, accuracy not correctness.

Step 6 decrements the S-VLAN's own deficit, which is shared state written per
packet. This is the one genuinely new shared write the design adds; §4.7 places
it. Flat DRR straight to the port would avoid it, but would let a customer with
100 subscribers out-compete a customer with 5, which defeats the purpose of
having an S-VLAN tier at all.

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
| `active_weight` (`u64`), `n_active_children` (`u32`) | cacheline0 | atomics written on activation transitions only, read per packet |
| `drr` | cacheline3 | `cake_drr_child_t` — an S-VLAN's own deficit vs the port, written per packet |

The shipped layout is preserved exactly: `global_shaper_time_ns` on cacheline1,
`buffer_usage` on cacheline2, per-thread stats out of line. Packing the new
per-packet write next to either existing atomic would reintroduce the cache-line
bounce that commit `7c04b13` removed, so `drr` takes its own line.

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
| `cake_dequeue.c:444` | scheduler 1→0 active: reverse |
| `osvbng_qos_sched.c:305` | teardown deactivation: reverse |

Propagation upward is a refcount. On `fetch_add`, only the worker observing
`prev == 0` adds the aggregate's own `effective_weight` to *its* parent; on
`fetch_sub`, only the one observing `prev == 1` subtracts. An S-VLAN therefore
counts toward the port's `W` exactly while it has at least one active child.

The scheduler-side active flag stays owner-thread-local. Only the parent
counters are atomic, and they move on activation transitions rather than per
packet.

Lifecycle operations, all under the worker barrier:

- **Create backfills.** Creating an S-VLAN aggregate on a port with live
  sessions walks the scheduler pool, re-resolves attachment for schedulers whose
  encap carries that tag, and **moves their weight contribution from the port to
  the new S-VLAN**. Without the move the port double-counts them.
- **S-VLAN delete detaches members to the port**, moving weight the other way.
- **Port delete with children is refused.**
- **Rate or weight update** adjusts the parent's `active_weight` by the delta if
  the child is currently active. This is why `osvbng_cake_aggregate_update`
  exists: without it a rate change is delete-and-recreate, which drops the child
  out of `W` and back in.
- **Interface deletion** reuses the existing `VNET_SW_INTERFACE_ADD_DEL_FUNCTION`
  hook, extended to unwind the parent chain.

All lifecycle mutation runs under the API/CLI worker barrier. No handler in this
plugin may be marked mp-safe without re-review.

**Invariant, asserted by the harness:** after arbitrary churn,
`agg->active_weight == SUM effective_weight of active children` and
`agg->n_active_children == |active children|`. Every path above can break it and
a leak is silent — too high and every child under-sends, too low and shares
skew.

### 4.10 Enqueue admission

Unchanged in shape: charge the S-VLAN's `buffer_usage`, then the port's; unwind
the S-VLAN on port rejection. Both use the atomic fetch-add-then-verify pattern
already shipped, so over-admission stays bounded at one packet per worker per
level. The existing discharge helper generalises to walk the parent chain, so
every charged-buffer free path releases at both levels.

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
osvbng_cake_aggregate_create_v2 {
  u32 sw_if_index;          /* port */
  u8  level;                /* 0 = port, 1 = svlan */
  u16 svlan_id;             /* level 1 only */
  u16 svlan_id_end;         /* range end, == svlan_id for a single tag */
  u64 rate_bytes_per_sec;
  u32 weight;               /* multiplier, 0 or 1 = default */
  u32 burst_ns;
  u32 buffer_limit;         /* 0 = derive from rate */
}
osvbng_cake_aggregate_delete_v2 { u32 sw_if_index; u8 level; u16 svlan_id; }
osvbng_cake_aggregate_update    { u32 sw_if_index; u8 level; u16 svlan_id;
                                  u64 rate_bytes_per_sec; u32 weight;
                                  u32 buffer_limit; }
osvbng_cake_aggregate_dump_v2   { u32 sw_if_index; }
osvbng_cake_aggregate_details_v2 { ... level, parent_sw_if_index, svlan_id,
                                   weight, active_weight, n_active_children,
                                   shaped_pkts, shaped_bytes, backpressure,
                                   drr_blocked, parent_blocked ... }
osvbng_cake_sched_enable_disable_v2 { ...v1 fields..., u32 weight; }
osvbng_cake_capabilities { }  /* -> version, max_levels, features bitmap */
```

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
a child's rate does not exceed its parent's; oversubscription across children is
allowed and expected.

## 6. File Plan

### Plugin (`osvbng-vpp-plugin-qos`)

| File | Change |
|---|---|
| `src/osvbng_qos_sched.h` | `cake_drr_child_t`; aggregate `level`/`parent_index`/`svlan_id`/`weight`/`active_weight`/`n_active_children`/`drr`; scheduler `drr`/`weight`/`agg_svlan_index`; `cake_drr_admit()`; `CAKE_DRR_ROUND_PERIOD_NS`, `CAKE_MAX_PKT_BYTES` |
| `src/osvbng_qos_sched.c` | two-level create/delete/update, S-VLAN map, backfill and detach walks, attachment walk extension, weight accounting on teardown, CLI |
| `src/osvbng_qos_sched.api` | messages in §5.2 |
| `src/osvbng_qos_sched_api.c` | handlers for the new messages |
| `src/cake_dequeue.c` | two-level gate with refund, `CAKE_DEQ_DRR_BLOCKED`, deficit decrement on success, weight accounting on deactivation |
| `src/cake_enqueue.c` | two-level buffer admission, weight accounting on activation |
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

**Phase 1 — DRR on the existing port tier.** `cake_drr_child_t`, `cake_drr_admit()`,
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
| (derived) | `burst_ns` | `agg->burst_ns` | default 25 ms, range 10–150 ms |
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
| Two-level refund | every charge paired with exactly one refund; accounting balances |
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
- Backfill: create an S-VLAN aggregate while sessions are live and passing
  traffic; members move from the port to the S-VLAN with no accounting drift.
- Teardown under load at both levels: quiescent `buffer_usage == 0`.

### 9.3 containerlab

Suites 18 and 19 green throughout, proving zero behaviour change for
subscriber-only and port-only configurations.

### 9.4 Explicitly not covered

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
  path has no batch loop. This spec adds one atomic read and one local
  compare per packet per level and neither adds nor removes batching. Batching
  the existing paths is worth its own issue; folding it into this one would
  make the fairness change unreviewable.
