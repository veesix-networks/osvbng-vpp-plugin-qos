# Phase 2 Review: hqos-svlan (Claude Fable 5, substituting for Gemini)

**Date:** 2026-08-12
**Input:** `IMPLEMENTATION_SPEC.md` @ `b381405`, `DECISIONS.md`, plugin source on
`spec/hqos-svlan` (= `main`), fix branches `fix/agg-buffer-accounting` (#5),
`fix/agg-gate-livelock` (#4), `fix/shaper-rate-precision` (#6),
`fix/sched-interface-teardown` (#7), `fix/agg-walk-rotation` (#9), and the
osvbng Go control plane.
**Focus (per the human):** design vs VPP best practice, DPDK-adjacent
performance, fairness under congestion for a residential BNG, bufferbloat
impact. The four attack items in `README.md` §"Prompt to Resume" were worked
explicitly; answers are folded into the findings and the closing assessments.

Every source claim in the spec that could be checked locally was checked.
The following are **confirmed against source** and not repeated as findings:
the `clib_bitmap_foreach` stable-ascending starvation mechanism
(`cake_dequeue.c:227`); the owner-thread single-writer property
(`cake_dequeue.c:240`, `cake_enqueue.c:180-193`); handed-off packets being
uncharged until owner-side re-enqueue (`cake_handoff.c:44`, charge only in
`enqueue_local`); the three buffer-accounting leaks in `main` and that PR #5
closes exactly those paths and converts admission to fetch-add-then-verify;
PR #4's gate-CAS termination fix and 25 ms burst clamp (`CAKE_AGG_BURST_NS`);
the Q48.16 rate representation in PR #6; the v1 aggregate messages existing
end-to-end with no Go caller. The §4.9 charge-transfer equality argument
(attack item 3) **holds**: both counters share the raw `pkt_len` basis, the
charge site is owner-thread-only, handoff-congestion drops are uncharged, and
the barrier quiesces everything else — no violating path was found. The §4.4
refund pairing (attack item 2) is sound in structure; F7 below corrects one
arithmetic bound.

---

## Findings

### F1 — HIGH: the DRR admission rule deadlocks a child whose head packet exceeds the deficit cap; the §4.4 progress argument is incomplete

This is the answer to attack item 1: wall-clock rounds prevent the
*replenishment* deadlock, but replenishment is clamped —
`deficit = min(deficit + quantum, cap)` with
`cap = max(2*quantum, CAKE_MAX_PKT_BYTES)` — so progress additionally requires
`cap >= adj_len` for every packet that reaches the check. The spec never
defines `CAKE_MAX_PKT_BYTES` (it appears only in the §6 file plan), and the
natural value (1514, matching `CAKE_QUANTUM_DEFAULT`) breaks it.

Concrete execution path: subscriber provisioned with MTU 9000
(`swif->mtu[VNET_MTU_IP4]` is already read at scheduler create, jumbo is a
supported configuration), `dsl-pppoe-atm` preset → `adj_len =
ceil((9000+32)/48)*53 ≈ 9,976`. Parent saturated, so the §4.4 work-conserving
escape never fires. Refill clamps deficit at `cap < adj_len` every round;
`deficit >= adj_len` is never true; the head packet is never admitted; the DRR
check is per *child*, so the entire subscriber wedges — queues pinned full,
every subsequent packet tail-dropped at admission, indistinguishable from the
issue #8 starvation this spec exists to fix. GSO amplifies it: the API already
declares `OSVBNG_CAKE_FLAG_SPLIT_GSO` but nothing splits, and a coalesced
chain's `adj_len` can reach tens of KB.

**Recommended fix (primary), cross-checked against in-repo precedent:** adopt
the eligibility discipline the plugin's own flow-level DRR already uses at
`cake_dequeue.c:317-320` — eligible while `deficit > 0`, subtract the full
`adj_len`, allow the deficit to go into debt (Linux sch_cake/fq_codel use the
same variant precisely because it cannot deadlock on oversized packets, and
its long-run byte fairness is equivalent; debt is repaid before the child is
eligible again). For the owner-local `cake_drr_child_t` this is a signed
deficit. For the shared packed word, store the deficit biased
(`deficit + 2^31`): eligibility is `> BIAS`, debt is bounded at one packet
below zero because eligibility requires positivity, so the u32 half can never
wrap — the §4.4 carry-into-round-bits argument survives unchanged, and the
refund `fetch_add` is unaffected. This removes the MTU-floor coupling
entirely: no value of `CAKE_MAX_PKT_BYTES` needs to be "big enough".
**Fallback if `>= adj_len` is kept:** define `CAKE_MAX_PKT_BYTES` ≥ the
worst-case `adj_len` the dataplane can present (jumbo × ATM inflation ⇒
16 KB is not paranoid), add a safety-valve admission at `deficit == cap`
regardless of size, and count oversized admissions. Either way §9.1 needs a
harness row: "head packet with adj_len > 2×quantum under saturated parent —
child still progresses."

Performance impact of the fix: none. Same word, same CAS, one comparison
changes.

### F2 — HIGH: quantum arithmetic still overflows u64 for permitted configurations; the §4.3 evaluation-order rule is not sufficient

§4.3 fixes the `agg_rate * ROUND_PERIOD * weight` ordering but the surviving
order still computes `round_bytes * effective_weight_i` before dividing by
`W`. `weight` is an unbounded u32 multiplier and `effective_weight =
rate_bytes_per_sec * weight`. Concrete overflow: 100 Gbit/s port
(`round_bytes = 1.25e7`), 25 Gbit/s child with `weight 1000`
(`effective_weight = 3.125e12`) → product `3.9e19 > 1.8e19`. Silently wraps;
the child's quantum becomes garbage and shares skew arbitrarily — the same
class of bug as the rate truncation the spec itself cites, in the formula the
spec introduces.

**Fix, cross-checked:** both halves. (a) Validate `weight <= 256` at the API
handler and in osvbng commit-time validation — no residential-BNG use case
needs more, and it bounds the product below 5e18 for any ≤100 Gbit/s pair.
(b) Compute the quantum with a 128-bit intermediate
(`(u64)(((unsigned __int128) round_bytes * eff_w) / W)`); this runs at refill
(once per child per round, not per packet), so the wide divide costs nothing
measurable. State both in §4.3/§5.2.

### F3 — MEDIUM: `active_weight` / `n_active_children` on cacheline0 reintroduces the 7c04b13 bounce for sparse traffic; "written on activation transitions only" is traffic-frequency, not config-frequency

§4.7 places the two activation atomics on the aggregate's cacheline0 with the
read-mostly identity fields, on the theory that activation transitions are
rare. They are not rare for exactly the traffic a residential BNG carries: a
scheduler deactivates the moment its queues drain (`cake_dequeue.c:267` empty
detect) and reactivates on the next packet (`cake_enqueue.c:344`), so a
50 pps VoIP-only subscriber toggles once per packet. Each toggle is 2 RMWs on
the parent, plus 2 more on the *port's* line when it is an S-VLAN's 0↔1
child transition (§4.9 refcount propagation — an S-VLAN with one sparse
active member propagates per packet). 1000 sparse sessions ≈ 200k+
invalidations/sec of a line every worker must re-read per packet for
`rate_ns_per_byte_scaled` (gate cost) and `buffer_limit` (admission). That is
the cache-line bounce class commit `7c04b13` exists to remove.

**Fix, cross-checked against the shipped layout rule:** give the two
activation atomics their own cache line (cacheline4 — they may share with
each other, they are written together), exactly as `global_shaper_time_ns`
and `buffer_usage` each own theirs. Cost: 64 bytes per aggregate. If the
harness later shows the toggle *rate* itself matters (the RMWs, not the
placement), a deactivation debounce (clear only after N consecutive empty
visits) is the follow-up — it is safe because a stale-active child only
inflates `W`, and under-utilisation is already covered by the
work-conserving escape — but do not spec it now; measure first.

### F4 — MEDIUM: §10's hot-path cost claim is wrong by ~4 atomic RMWs per packet, and the enqueue-side RMWs run at offered rate, not admitted rate

"This spec adds one atomic read and one local compare per packet per level"
describes only the step-2 owner-local check. Counting the shared-line RMWs
for a scheduler under an S-VLAN versus shipped port-only:

| Path | Shipped | With S-VLAN tier |
|---|---|---|
| enqueue admission | 1 fetch_add (port buffer) | 2 (S-VLAN + port) |
| dequeue gates | 1 CAS (port gate) | 3 (S-VLAN gate CAS, S-VLAN reserve CAS, port gate CAS) |
| free/discharge | 1 fetch_sub | 2 |
| **total, distinct hot lines** | **3 RMWs / 2 lines** | **7 RMWs / 5 lines** |

The dequeue-side RMWs are bounded by aggregate throughput (they only run for
packets the tier actually admits), which is the design's saving grace. The
enqueue-side ones are not: fetch-add-then-verify runs per *offered* packet.
Concrete worst case: incast overload toward one port with the port buffer
pinned full — every offered packet from every worker pays S-VLAN fetch_add +
port fetch_add + port fetch_sub (reject) + S-VLAN fetch_sub (unwind) = 4 RMWs
on two contended lines at offered rate, stealing cycles from DPDK RX polling
on the same cores and thus from unrelated interfaces.

**Fixes, cross-checked:** (a) Correct the §10 claim — reviewers and the
benchmark phases should gate on the honest number. (b) Add a read-only
overload filter in front of the fetch-add-verify pair: relaxed-load the
usage, and if it already exceeds the limit, drop without any RMW. This is not
a regression to the pre-#5 race: the racy load is used only to *reject* (a
stale read can only cause a marginally early or late drop at a full queue,
where dropping is the outcome regardless); admission still goes through #5's
exact fetch-add-then-verify. The sustained-overload path becomes shared-read
scalable. (c) Extend §9 with a `tests/benchmark.py` phase comparing
port-only vs S-VLAN-tier clocks/vector deltas so the ~2.3× RMW growth gets a
measured cycles number before Phase 3 merges.

### F5 — MEDIUM: the work-conserving escape hands the gate's entire burst credit out in walk order; fairness has a bounded hole at every onset of congestion

Attack item 1, second half. Post-#4 the gate clamps idle credit at
`CAKE_AGG_BURST_NS` (25 ms): under sustained sub-saturation, virtual time
sits pinned at `now − 25 ms`, so the §4.4 escape (`parent_shaper_time <=
now − 1 ms`) is *continuously open* — intended, that is work conservation.
But at the transition into saturation, the first `burst_ns × rate` bytes
(31 MB at 10 Gbit/s) are admitted while the escape is still open, i.e. with
no DRR arbitration at all, in bitmap walk order — the issue #8 pattern,
returning for up to 24 ms at every busy-period onset. For cyclic offered
load (e.g. 100 ms on/off periods, which residential traffic approximates),
the unarbitrated fraction of admitted bytes can reach tens of percent, and
the §9.1 "saturated shares within ±2%" rows will not see it because they
measure steady saturation only.

Escape/refund interaction (the specific question in the resume prompt): an
escape admission *can* reach a step-5 port-gate failure — many workers pass
the escape concurrently, their gate CASes serialize, late ones find the gate
shut — and the §4.4 no-refund flag correctly prevents refunding a reserve
that never happened. That part is sound.

**Fixes, cross-checked, in preference order:** (a) Default aggregate
`burst_ns` to 10 ms (already the documented floor of the §8 range) — halves
the unfair window *and* the post-idle line-rate burst injected into the
access network (see bufferbloat assessment), at the cost of slightly less
post-idle catch-up throughput; keep 25 ms available per config. (b) Attempt
the reserve *before* consulting the escape, and use the escape only when the
reserve is refused: the blocked-path CAS is read-only-fail (§4.4), so the
extra cost on the idle path is one shared read of a line the gate CAS touches
anyway, and heavy children then enter the congested period with depleted
deficits, shortening the unfair transient to at most one round of carry.
(c) Add a §9.1 harness row: cyclic offered load, long-run shares within
tolerance; and an onset-convergence row: after idle→saturation transition,
shares converge within N rounds. None of these adds per-packet cost on the
saturated path.

### F6 — MEDIUM: the spec does not pin where the step-2 DRR check sits inside `cake_dequeue_one`; the natural placement (matching the gate) churns COBALT/ECN state on every blocked retry

§4.5 orders the six steps but `cake_dequeue_one` today interleaves AQM into
them: `cobalt_should_drop` runs and the ECN mark is *applied to the buffer*
(`cake_dequeue.c:149-154`) before the gate check at `:158`, and a gate
rejection un-pops the packet. If the new DRR check lands in the same place, a
DRR-blocked child — which retries every polling dispatch, i.e. at µs
frequency, 1000× the round rate — re-runs `cobalt_should_drop` on the same
head packet each time, escalating `codel_count` (so COBALT over-drops when
the child finally unblocks) and re-counting `ecn_marks`.

**Fix, cross-checked against Linux behaviour:** specify that the step-2 check
executes before `cobalt_should_drop` and the ECN mark — the only inputs it
needs are `adj_len` (available from `pkt_len` at `:110`) and owner-local
state. A DRR-blocked visit then mutates nothing at all, which is also the
cheap common case §4.4 wants. This matches sch_cake semantics: under a
closed shaper, Linux does not run the AQM either (dequeue is not called
until the watchdog fires); AQM action is deferred at most one round. Keep
the existing AQM-before-gate order for steps 3–5 so drops still drain under
a closed gate; the pre-gate ECN-mark wart at `:149` predates this spec and
stays out of scope.

### F7 — LOW: the refund-carry bound in §4.4 is per-worker, not per-packet

"Deficit plus one refund is at most `cap + CAKE_MAX_PKT_BYTES`" — one refund
can be in flight *per worker* simultaneously (each worker can sit between
step 4 and step 5 for the same S-VLAN child), so the transient bound is
`cap + n_workers × CAKE_MAX_PKT_BYTES`. Still orders of magnitude under
2^32 (25 MB cap + 64 × 16 KB ≈ 26 MB), so the conclusion stands; correct the
stated bound so the §9.1 linearizability row asserts the right invariant
instead of intermittently failing on a correct implementation.

### F8 — LOW: the escape comparison underflows u64 in the first round after boot

`parent_shaper_time <= now_ns - CAKE_DRR_ROUND_PERIOD_NS` wraps when
`now_ns < 1e6` (vlib time starts near zero), making the escape
unconditionally true for the first millisecond. Harmless in practice, wrong
in form; write it as `parent_shaper_time + CAKE_DRR_ROUND_PERIOD_NS <=
now_ns`, which the harness stub clock (which will start at 0) would
otherwise trip over.

### F9 — MEDIUM: the deactivation table in §4.9 omits the two defensive deactivation paths that share the same mechanism; weight subtraction must distinguish them

Attack item 4. The three listed sites are correct and exhaustive *for live
schedulers* — activation and empty-detect deactivation are both owner-thread,
teardown is under barrier, so no transition races another. But
`cake_dequeue.c` also routes two stale-entry cases through the same
`deactivate[]` array and bitmap-clear loop the spec cites as
"`cake_dequeue.c:267,393`": `pool_is_free_index` at `:232` and the
owner-mismatch check at `:242`. Those bits belong to schedulers whose weight
was already removed at the teardown site (`osvbng_qos_sched.c:305` under
barrier clears bitmaps too, but the walk can race a bit set in the same
dispatch that teardown cleared). An implementation that subtracts
`effective_weight` wherever it clears a bitmap bit double-subtracts and
corrupts `W` (u64 underflow → every sibling's quantum collapses toward
zero → tier-wide starvation); one that never subtracts on the `:393` path
leaks weight on empty-detect. The spec should state the rule explicitly:
weight moves only on the *empty-detect* deactivation of a live, owned
scheduler and on teardown — the two defensive paths clear the bit only. The
§9.1 churn row should include "teardown racing an in-flight dispatch with
the scheduler's bit set".

### F10 — LOW: two §4.9 wordings invite implementation mistakes the spec's own reasoning forbids

(a) "Backfill transfers the charge" — state explicitly that the port side is
**not** decremented on backfill: pre-backfill packets charged the port and
will still discharge it; the transfer only pre-credits the S-VLAN for
discharges it never admitted. The word "transfer" invites a symmetric
subtract, which would under-count the port and over-admit. (The per-tag
detach direction is correctly specified with both movements.)
(b) §4.4 calls `cake_drr_local_admit` "read-only check", but the refill
mutates `child->deficit/round`. It is single-writer and safe; say
"owner-local, no refund obligation" instead of "read-only" so nobody
"fixes" the mutation out of it.

---

## Assessment: fairness under congestion (residential BNG)

The mechanism is sound and the right shape for this codebase. Verified
reasoning, not repeated as findings: child-driven DRR with owner-local state
genuinely needs no pinning and no hot-path child iteration; the one new
shared per-packet word is correctly identified and correctly isolated on its
own cache line; weighted-by-configured-rate is the right default for a BNG
(the shaped rate is what the customer bought); walk order stops mattering in
steady saturation because per-round refill (= `round_bytes` = the gate's
per-round credit, summed over children) balances the gate exactly, so each
backlogged child converges to its quantum per round. The deficit cap bounds
the convergence transient after a disturbance to a few rounds (longer —
~`cap/quantum` rounds — when quantum ≪ MTU; the 1000-child case converges in
~12 ms). Byte-based deficits mean small packets (voice) clear the check
earliest, which is the right emergent priority for the tin-blind design §10
already discloses.

The two real fairness gaps are F5 (burst credit unarbitrated at congestion
onset — the only place the issue #8 pattern survives) and F1 (a wedged child
is the opposite of fair). With those addressed, the headline claim — port
capacity splits by S-VLAN weight, S-VLAN capacity splits by subscriber rate,
under sustained congestion — holds, and the §9.1/§9.2 matrix as extended by
F1/F5 will prove it.

## Assessment: bufferbloat

The architecture is correct by CoDel/CAKE philosophy and should be preserved
exactly as specced: **all real queues stay at the leaf**, the upper tiers are
pure rate/arbitration gates, so time spent gate-blocked or DRR-blocked
accrues as sojourn in the subscriber's COBALT-managed queues. AQM therefore
sees hierarchy congestion without any signal plumbing, per-flow isolation is
preserved inside each subscriber, and ECN marking keeps working under
S-VLAN pressure. Two-level buffer admission bounds worst-case standing queue
per tier; per-level `backpressure` remains distinguishable from gate
pressure. This is materially better than aggregate-side queuing designs
(rte_sched-style), which would re-bloat everything CAKE removes.

Three quantified caveats. (1) `burst_ns` is a downstream bloat budget: after
any ≥burst_ns lull, the gate releases up to `burst_ns × rate` at line rate
into the access network — 25 ms of jitter budget at the OLT if the aggregate
rate equals downstream capacity. F5's 10 ms default addresses this too.
(2) The subscriber tier's own shaper accrues *unbounded* idle credit in
shipped code (no behind-clamp analogous to #4's; `cake_dequeue.c:246` simply
compares against a stale virtual time), so a long-idle subscriber bursts its
whole backlog at parent-gate speed. Under this spec the parent gates cap the
damage, and DRR bounds the intra-aggregate unfairness of the burst — an
incidental improvement worth one sentence in §3; subscriber-only deployments
keep the exposure (out of scope here, worth its own issue). (3)
`mtu_time_us` is computed from the *configured* rate
(`osvbng_qos_sched.c:219`), so for a child squeezed far below its rate,
COBALT's over-target floor understates serialization time and drops slightly
early. Second-order, self-limiting, no change requested — noted so nobody
reports it as a regression of this spec.

## Assessment: VPP/DPDK performance posture

The design follows the codebase's established concurrency discipline:
barrier-time lifecycle, CAS loop shapes that mirror the shipped gate,
per-line isolation of contended atomics, owner-thread state kept
thread-local, and no hot-path child iteration. Rejecting central-arbiter DRR
was correct and consistent with why DPDK's own `rte_sched` model (strict
single-writer per port, thread-pinned) is a poor fit for a VPP BNG with RSS.
The polling INPUT node needs no new wake machinery for DRR-blocked children;
retry-next-dispatch composes with PR #4's spin fixes.

The honest cost of the S-VLAN tier is F4's table: 7 contended RMWs across 5
shared lines per packet, versus 3/2 today — inherent to charging two levels,
bounded by aggregate throughput on the dequeue side, and acceptable *if* the
enqueue-side offered-rate exposure gets F4(b)'s read-only overload filter and
the growth is benchmarked before Phase 3 merges. F3 is the one placement
error. Batching/MULTIARCH deferral (§10) is reasonable for reviewability, but
note the First-Class Requirement #2 debt is now two specs deep; the follow-up
issue should be filed when this spec's issue is, not later.

## Verdict

Approve with changes. F1 and F2 must land in the spec before Phase 4
finalization (both have zero-cost fixes); F3–F6, F9 are spec-text changes
and harness rows that materially de-risk implementation; F7, F8, F10 are
wording. No architectural change is requested: the three-level shape, the
packed-word reserve, the barrier-time charge transfer, and the leaf-queue
bufferbloat posture all survived adversarial reading and source
verification.
