# Claude review: bug hunt

- **Reviewer:** Claude (Opus 5), line-level pass
- **Lens:** races, memory safety, buffer/credit leaks, refund obligations,
  arithmetic edges, control-plane state handling
- **Target:** plugin `ae8ed7c..HEAD` and osvbng `main..feat/hqos-svlan-control-plane`
- **Date:** 2026-08-12

Ordered most severe first. Where a finding overlaps a Codex finding it says
so and adds what the independent read confirmed or extended. Triage lives in
`../DECISIONS.md`.

---

## CL-1 [HIGH] Round tags initialize to zero, wedging children created after ~24.9 days of uptime

Overlaps CODEX-PROTOCOL #1; independently confirmed, and the trigger is
worse than "long-idle": **creation** is enough.

`cake_sched_enable_disable` gets the scheduler via `pool_get_zero`
(`osvbng_qos_sched.c:476`), so `cs->drr.round` starts at 0 and is only ever
written by `cake_drr_refill`. `cake_drr_shared_init` (`cake_drr.h:295-301`)
likewise publishes round 0. At uptime past 2^31 ms (~24.9 days),
`cake_drr_round(now)` exceeds 2^31, so for a freshly created child
`(i32)(round - 0)` is negative: `cake_drr_round_advanced` never fires, the
deficit never refills, and the child is dead on arrival while its parent is
saturated. The escape lends at most `CAKE_MAX_PKT_BYTES` of debt — one or
two packets — then closes. A subscriber provisioned on day 30 of dataplane
uptime gets nothing until the round counter re-enters the positive
half-space, up to another ~24.9 days later.

The idle-reactivation variant (last refill > 2^31 rounds ago) is real too
but needs a subscriber idle for 25 unbroken days; creation-at-late-uptime is
the ordinary BNG case. Every long-lived deployment crosses day 25.

Fix shape: seed both round tags from `cake_drr_round(now)` at create, and
rebase a tag seen in the future half-space by more than a small skew bound
(workers disagree by at most one round; anything further back is staleness,
not leadership) rather than treating it as "not yet advanced". The rebase
must not reopen the F5-4 ping-pong: only a tag *far* behind `round` may
rebase, a tag one round ahead must still refuse.

## CL-2 [MEDIUM] A tier-blocked head packet re-enters COBALT once per polling dispatch

`cake_dequeue_one` orders: subscriber DRR check → `cobalt_should_drop` →
ECN mark → `cake_agg_dequeue_gate` (`cake_dequeue.c:139-207`). The comment
at `cake_dequeue.c:128-138` explains why the subscriber DRR check *must*
run before the AQM: a blocked child retries every polling dispatch, and
post-AQM placement would re-run `cobalt_should_drop` on the same head
packet each retry. The parent gates have exactly that placement: on
`CAKE_PARENT_GATE_CLOSED` or `CAKE_PARENT_DRR_BLOCKED` the packet is
un-popped (`flow->head--`) and the next dispatch re-enters
`cobalt_should_drop` on the same packet with a longer sojourn.

Bounding the harm honestly: re-entry happens at dispatch rate, but the
AQM's *escalating* actions (drop, mark, `codel_count++`) are gated on
`drop_next_us`, so state moves at CoDel cadence, not dispatch rate. What
actually accumulates per blocked head packet: an ECN-capable packet is
re-marked and re-counted in `tin->ecn_marks` once per CoDel deadline while
held (idempotent on the wire, inflated in the counters), and a non-ECN
packet is eventually AQM-dropped while the gate is closed — which is
defensible queue management, though the drop then bypasses the gate charge
it never took. The §9.3 contended pair shows the drop *location* shift, not
net inflation: total drops are 236k in both phases; port-only splits
205k overflow / 31k AQM, the tier splits 55k / 181k, at identical goodput.

So: a structural inconsistency with the file's own stated rule and an
ECN-counter inaccuracy, not a throughput or fairness defect at measured
scales. Worth recording because the S-VLAN tier adds two new refusal
sources (tier reserve, port gate) that widen the shipped v1 window, and
because whether AQM-at-CoDel-cadence under a closed gate over-drops on
unblock is exactly the kind of claim the af-packet rig cannot test
(TESTING.md's latency-under-load boundary). Revisit with a real
bottlenecked rig before treating as shippable-forever.

## CL-3 [MEDIUM] Conf handler shape change leaks the old dataplane objects

`AggregateHandler.Apply` (`osvbng/pkg/handlers/conf/qos/aggregate.go:100-104`):
when the old and new revisions differ in shape (`!sameShape` — interface or
tag set changed), the code path falls through to `ApplyAggregate` for the
new shape **without removing the old objects first**, despite the
`sameShape` doc comment saying a shape change "has to be torn down and
rebuilt". Change `svlans: ["100-103"]` to `["200-203"]` and tags 100-103
keep shaping forever. Worse, change `["100-103"]` to `["100-107"]`:
`ApplyAggregate` treats the dataplane's `ENTRY_ALREADY_EXISTS` for the
overlapping range as benign replay (`qos_aggregate.go:117-121`) and reports
success while the dataplane still holds the old 100-103 object with the old
rate. `Rollback` mirrors the same gap in reverse.

Fix shape: on `!sameShape`, run `remove(old)` before `ApplyAggregate(new)`.
The already-exists tolerance is correct for checkpoint replay and can stay.

## CL-4 [MEDIUM] Capability probe caches its own failure for the process lifetime

`capabilities()` (`osvbng/pkg/southbound/vpp/qos_aggregate.go:36-63`) runs
under a package-level `sync.Once`. If the first call happens while the
dataplane is down or the channel fails — exactly the startup race
`StateRestoring` exists for — `caps` stays zero, `ApplyAggregate` refuses
every S-VLAN entry with "dataplane has no S-VLAN aggregate tier", and
nothing ever re-probes: `caps.known` is set but never consulted. The
control plane then needs a process restart to program QoS against a
dataplane that supports it. A dataplane restart/upgrade under a running
control plane keeps stale capabilities the same way.

Fix shape: retry the probe while `!caps.known` (only a *successful reply or
a definitive "message unknown"* should latch), and reset on southbound
reconnect. Package-level state should move onto the `*VPP` receiver.

## CL-5 [MEDIUM] Port rate update below child rates

Confirms CODEX #2 from the source: `cake_aggregate_update`
(`osvbng_qos_sched.c:834-841`) checks `rate > parent's rate` only when the
updated aggregate *has* a parent. Lowering a port under its S-VLANs
succeeds, leaving children the create path would have refused, and the Go
`ValidateAggregates` guard only protects changes that arrive through a full
config commit — the CLI and direct API path have no second line of defence.

## CL-6 [LOW] v2 handlers alias unknown levels to the port operation

Confirms CODEX #1: `osvbng_qos_sched_api.c:232/257` and
`cake_aggregate_update` branch `level == SVLAN ? ... : port-path`. A
corrupted or future-versioned `level` byte executes the port operation;
for delete that removes the port tier (when childless) instead of
returning `INVALID_VALUE`. One-line validation in each of the three
handlers.

## CL-7 [LOW] Per-packet cost floor overspeeds high-rate shapers — pre-existing, not introduced by this branch

CODEX-PROTOCOL #2 is arithmetically right: `cake_cost_ns` floors to whole
ns per packet, and at 100 Gbit/s with 64-byte frames that admits 2.4% over
rate (5 ns charged vs 5.12 true). Scope correction from the git history:
the identical arithmetic shipped in v1 (`ae8ed7c:src/osvbng_qos_sched.h:709-723`)
for both the subscriber shaper and the port aggregate — this branch
extracted it into `cake_shaper.h` unchanged. The S-VLAN tier applies it at
no higher a rate than the v1 port aggregate already did. At BNG subscriber
and port rates with real frame mixes the error is well under 1%; it becomes
material only for >10 Gbit/s aggregates dominated by small frames.

Fix shape if accepted: per-thread fractional-remainder accumulation in the
existing per-thread stats cache line (long-run exact, no new shared RMW),
or fold the fraction into the gate CAS word. Not a one-liner; deserves its
own issue rather than a rider on this branch.

## CL-8 [LOW] Stale comment says the escape is uncharged

`cake_dequeue.c:223-225` ("An escape admission is not charged...") describes
pre-F5-2 semantics. The code below it is correct — escape admissions return
`CAKE_DRR_ADMIT` and are charged — but the comment now asserts the exact
opposite of F5-2 at the one call site a reviewer will read. Same for the
half of the `cake_agg_dequeue_gate` refund comment
(`osvbng_qos_sched.h:1106-1107`) claiming "a work-conserving escape admits
without reserving": inside `cake_drr_shared_reserve` the escape *is*
charged and *is* refundable, and the `reserved == CAKE_DRR_ADMIT` test it
justifies is vacuous (BLOCKED already returned early). Comment-only fixes,
but they misdocument the invariant F5-2 exists to protect.

## CL-9 [LOW] Derived buffer_limit truncates above ~229 Gbit/s

`(u32)((rate_bytes_per_sec * 100000 * 3) / (1000000 * 2))`
(`osvbng_qos_sched.c:374,710`): the u64 product is fine but the u32 cast
wraps once 0.15 s of rate exceeds 4 GiB (~229 Gbit/s), silently producing a
tiny limit. Unreachable for shipping hardware; cheap to clamp.

## Checked and clean

Things this pass specifically went looking for and did not find broken:

- **Refund obligations in `cake_agg_dequeue_gate`**: both unwind paths
  restore gate charge, reserve, and per-thread stats symmetrically; the
  fetch_sub refund racing a concurrent burst-clamp rebase loses at most
  accrued idle credit, bounded by `burst_ns`, self-correcting.
- **`cake_drr_shared_reserve` CAS**: refill/admit/decrement is genuinely
  linearizable; the read-only blocked path writes nothing; the biased word
  cannot carry into the round bits for any permitted configuration
  (bias 2^31 + cap ≤ 2×25 MB + refund ≤ 2048 stays under 2^32); a jumbo
  `adj_len` can push debt past the escape floor but only deeper into
  bounded, repayable debt.
- **Weight accounting**: join/leave refcount propagation is
  transition-exact under interleaving; `cake_aggregate_update` moves the
  parent delta only while counted; teardown vs defensive-deactivate
  double-subtract is guarded as documented.
- **Buffer charge/discharge**: admit and discharge both use raw `pkt_len`
  end to end (chain admit, dequeue, AQM drop, evict/teardown via
  `backlog_bytes`); handoff-congestion drops occur before any charge
  exists, matching the reparent proof's assumption.
- **`cake_agg_reparent_all`**: chain set-difference transfer is exact for
  the shipped two-level topology, including S-VLAN delete (map cleared
  before reparent, pool entry freed after) and port delete (children
  refused via `INSTANCE_IN_USE` first).
- **Interface deletion**: `cake_sw_interface_add_del` tears down S-VLANs
  before the port and collects tags before mutating the pool.
- **IPv6 parity**: both directions share every inline; ECN, DSCP, hashing
  and reinjection all have v6 paths in the same commit.
