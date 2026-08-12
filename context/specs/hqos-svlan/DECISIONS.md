# Decisions: hqos-svlan

Phase 3 (Codex adversarial critique) ran 2026-08-12; findings under "Phase 3".
Phase 2 ran 2026-08-12 with Claude Fable 5 substituting for Gemini (deep
review: design vs VPP/DPDK practice, fairness under congestion, bufferbloat;
artifact at `spec-reviews/CLAUDE.md`); its ten findings — every one verified
against source in the review itself — were all accepted by the human and are
recorded under "Phase 2". Phase 4 finalization folded both review rounds into
the spec on 2026-08-12. Phase 1 entries are decisions taken during drafting,
recorded here so the review agents attack the reasoning rather than
rediscover it.

## Accepted

### Weighted DRR at the aggregate — reverses the hqos-qinq rejection
- **Source:** Phase 1 (measurement, issue #8)
- **Severity:** HIGH
- **Resolution:** `hqos-qinq/DECISIONS.md` rejected "Weighted DRR in Phase 1"
  with the rationale that fairness is approximate via per-subscriber rate limits
  plus RSS distribution. Both halves are measured false: `clib_bitmap_foreach`
  yields ascending pool index and is stable, so within a worker the first
  scheduler polled takes the credit and the rest starve, with RSS not involved.
  Four children under one aggregate measured 3743 / 1840 / 0 / 0 packets. The
  rejection also assumed DRR requires a central arbiter and therefore thread
  pinning; §4.2 shows child-driven DRR needs neither. Accepted and specified in
  §4.3–4.5.

### Wall-clock rounds rather than byte-clock rounds
- **Source:** Phase 1
- **Severity:** CRITICAL
- **Resolution:** Deficit replenishment driven by bytes sent creates a
  self-sustaining deadlock: if every child is blocked, nothing sends, the clock
  never advances, nothing unblocks. Rounds derive from `now_ns` so progress does
  not depend on any packet moving. §4.4.

### Deficit accumulates across rounds, with a cap and an MTU floor
- **Source:** Phase 1
- **Severity:** HIGH
- **Resolution:** `quantum_i` falls below one MTU whenever a parent has many
  children (1 Gbit/s, 1000 children, 1 ms rounds gives 125 bytes). A per-round
  reset would permanently prevent full-size packets. Deficit accumulates, capped
  at `max(2 * quantum, CAKE_MAX_PKT_BYTES)`, reset to 0 on activation. Deficit,
  quantum and cap are all bytes. §4.4.
- **Superseded in part by Phase 2 (F1):** eligibility is now `deficit > 0`
  with the full `adj_len` subtracted and bounded debt carried, so the cap and
  floor are burst smoothing, no longer load-bearing for progress. Activation
  clamps `deficit = min(deficit, 0)` — idle earns nothing and forgives no
  debt.

### Explicit weight is a multiplier, not a replacement
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** Weight defaults to the child's own `rate_bytes_per_sec`. If an
  explicit weight replaced that, a child set to `weight 2` alongside children
  defaulting to `125000000` would be crushed, so the field would only be usable
  if set on every child. As a multiplier it is safe on a subset. §4.3.

### Deficits checked before the rate gates, decremented only on success
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** Ordering the two-level path so deficits mutate only after both
  gates accept means deficits never need refunding, and the refund path stays
  confined to the single atomic `hqos-qinq` already had to unwind. §4.5.
- **Superseded in part by Phase 3:** holds for scheduler-side deficits only
  (single writer). The S-VLAN's shared deficit is a CAS reserve with a bounded
  refund — see "Shared S-VLAN deficit must be linearizable" below.

### Work-conserving escape when a parent is not saturated
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** Without it an idle child's unused quantum is lost every round.
  The escape fires only when a parent's virtual time is a full round behind the
  wall clock, which means genuine spare capacity. §4.4.
- **Amended by Phase 2 (F5, F8):** the escape is consulted only after the
  eligibility check or reserve refuses, so deficits stay honest through idle
  periods; the comparison is written in addition form (u64 underflow at
  boot); and the default aggregate burst drops to 10 ms because burst credit
  is DRR-unarbitrated.

### Hierarchical DRR, accepting one new shared per-packet write
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** An S-VLAN's own deficit is written per packet by whichever
  worker dequeues one of its children, so it takes its own cache line
  (cacheline3) and does not disturb the layout commit `7c04b13` established.
  Flat DRR straight to the port would avoid the write but would let a customer
  with 100 subscribers out-compete one with 5, defeating the S-VLAN tier. §4.7.

### `_v2` scheduler message rather than a separate set-weight message
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** Adding `weight` to the shipped
  `osvbng_cake_sched_enable_disable` breaks its CRC and every existing control
  plane. A separate set-weight message preserves CRCs but adds a second binapi
  round trip per session bring-up, on a declared control-plane hot path. §5.2.

### Harness before the second tier
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Resolution:** Phase 2 builds `tests/drr_harness.c` before Phase 3 adds the
  S-VLAN tier. The invariants it asserts — weight accounting under churn, refund
  balance, progress — are what make the two-level path reviewable, and it also
  covers the already-merged admission race and fixed-point rate change, neither
  of which any test has ever exercised. §9.1.

## Phase 6 — post-implementation code review triage (2026-08-12)

Three passes over `ae8ed7c..HEAD` and the osvbng control-plane branch:
Claude bug hunt (`code-reviews/CLAUDE.md`), Codex spec compliance
(`code-reviews/CODEX.md`), Codex protocol conformance
(`code-reviews/CODEX-PROTOCOL.md`, the Gemini slot, run on Codex at the
user's direction). Benchmark gate results in `PHASE6_VERIFICATION.md`.
Both independent passes converged on the stale-round wedge and on the
level/rate validation pair, which is the strongest signal in the set.

### Accepted: round tags initialize to zero and wedge late-created or long-idle children
- **Source:** CODEX-PROTOCOL #1 and CLAUDE CL-1, independently
- **Severity:** CRITICAL — every deployment crosses day 25 of uptime
- **Resolution:** Past 2^31 ms of uptime, a zero or >25-day-old round tag
  sits in the *future* half-space of the F5-4 signed comparison, so the
  refill never fires and the child runs on escape debt alone — a subscriber
  provisioned on day 30 wedges after one or two packets. Fixed twice over:
  both tags are now seeded from the current round at create
  (`cake_drr_shared_init` takes `now_ns`; `cake_sched_enable_disable` seeds
  `cs->drr.round`), and `cake_drr_round_refill_due` treats a tag leading by
  more than `CAKE_DRR_ROUND_STALE_LEAD` (2^20 rounds, ~17 minutes — far
  beyond any real worker skew, far short of the 2^31 wrap artifact) as
  elapsed rather than future, so recovery does not depend on the seed. The
  F5-4 invariant is preserved: a one-round lead still neither refills nor
  rewinds. Harness: `test_stale_round_rebases`, 71 checks green.

### Accepted: v2 handlers alias unknown levels to the port operation
- **Source:** CODEX #1 and CLAUDE CL-6
- **Severity:** HIGH — a level byte of 2 deleted the port tier
- **Resolution:** All three v2 handlers (`_v2_create`, `_v2_delete`,
  `_v2_update`) accept only `PORT` and `SVLAN` and return `INVALID_VALUE`
  for anything else, before any mutation.

### Accepted: a port could be updated to a rate beneath its S-VLANs
- **Source:** CODEX #2 and CLAUDE CL-5
- **Severity:** HIGH
- **Resolution:** `cake_aggregate_update` now enforces the hierarchy
  invariant from both sides: the existing child-above-parent check, plus a
  barrier-time walk rejecting a port rate below any child's. The Go
  `ValidateAggregates` guard only covers full config commits; the CLI and
  direct API path needed the dataplane to refuse it.

### Accepted: §4.7 cache-line deviation, as documentation
- **Source:** CODEX #3
- **Severity:** MEDIUM as raised; resolved as a spec amendment
- **Resolution:** The finding is factually right that the code deviates from
  §4.7's table and that no finding recorded it. The layout itself is kept:
  `drr.effective_weight` is read on exactly one path — the refill inside the
  CAS loop — by the worker about to write the word beside it, so cacheline3
  is the right home and cacheline0 would put it on the hottest read-shared
  line. §4.7 now carries the amendment instead of the code carrying a
  regression.

### Accepted: conf-handler shape change leaked the old dataplane objects (osvbng)
- **Source:** CLAUDE CL-3
- **Severity:** MEDIUM — a narrowed tag set kept shaping forever, a widened
  one silently kept the old rate on the overlap
- **Resolution:** `AggregateHandler.Apply` tears down the old revision's
  objects before applying a new shape; same-shape revisions still update in
  place. The already-exists-is-replay tolerance stays, since checkpoint
  restore depends on it.

### Accepted: capability probe cached its own failure (osvbng)
- **Source:** CLAUDE CL-4
- **Severity:** MEDIUM — a control plane started before its dataplane
  refused S-VLAN configuration until process restart
- **Resolution:** The `sync.Once` became a mutex-guarded probe that latches
  only on an *answer* — a capabilities reply, or a dataplane that does not
  carry the message. A channel that cannot be opened is retried on the next
  use. Dataplane-restart staleness remains and is documented at the site.

### Accepted: three small hardenings
- **Source:** CLAUDE CL-8, CL-9, CL-10
- **Severity:** LOW
- **Resolution:** The two comments still describing the pre-F5-2 uncharged
  escape now state the charged semantics (`cake_dequeue.c`, the
  `cake_agg_dequeue_gate` refund). The derived `buffer_limit` saturates at
  `~0U` instead of wrapping above ~229 Gbit/s. The CLI burst multiply is
  widened to u64 so wrapped millisecond values cannot alias back into the
  valid range.

### Recorded, no code change: AQM re-entry while parent gates are closed
- **Source:** CLAUDE CL-2
- **Severity:** MEDIUM as a design inconsistency, LOW in measured effect
- **Rationale:** The parent gates sit after `cobalt_should_drop`, so a
  gate-blocked head packet re-enters the AQM per dispatch — the placement
  the subscriber-level DRR check deliberately avoids. Bounded honestly:
  COBALT's escalating actions are gated on `drop_next_us`, so state moves at
  CoDel cadence, not dispatch rate; the §9.3 contended pair shows drops
  *relocating* between enqueue overflow and AQM (236k total in both phases,
  identical goodput), not net inflation; and AQM-draining a queue the gate
  will not release is defensible queue management. What is genuinely wrong
  is ECN accounting (a held packet can be re-marked and re-counted per CoDel
  deadline) and that the af-packet rig cannot test whether unblock-time
  over-drop exists. Restructuring the dequeue path to fix a
  counter-accuracy issue, without a rig that can validate the behavioural
  half, fails "do not spec what you cannot verify". Revisit with the
  hardware/bngblaster rig; the analysis lives in CL-2.

### Rejected: fix the per-packet cost floor on this branch
- **Source:** CODEX-PROTOCOL #2, scope-corrected by CLAUDE CL-7
- **Severity:** MEDIUM at 40-100 Gbit/s with minimum frames; <1% below
  ~10 Gbit/s with real frame mixes
- **Rationale:** The arithmetic is correct as reported — `cake_cost_ns`
  floors each packet's cost and the error is systematic, 2.4% over rate at
  100 Gbit/s / 64 B. But it is v1-shipped arithmetic
  (`ae8ed7c:osvbng_qos_sched.h:709-723`), extracted unchanged into
  `cake_shaper.h`; this branch neither introduced it nor widened its blast
  radius, and the fix (per-thread fractional remainder, or folding fraction
  into the gate word) is its own design with its own verification burden.
  One feature per PR: filed as follow-up work alongside the batching debt,
  not folded into a fairness branch at review time.

## Phase 3 — Codex adversarial critique (2026-08-12)

Two findings raised, both verified against source before acceptance; each was
narrower or differently weighted than reported, and the corrections are
recorded alongside the acceptances.

### Accepted: shared S-VLAN deficit must be linearizable
- **Source:** Phase 3 (Codex), verified
- **Severity:** CRITICAL
- **Resolution:** The draft specified a read-only DRR check at one step and a
  decrement at a later step for the S-VLAN's own child versus the port — state
  written per packet by every worker owning one of its members. Check-then-
  decrement is not coherent there: lost updates with plain stores, TOCTOU
  underflow wrapping a `u64` deficit open until the next refill with atomics,
  and double refill when two workers race the round transition. Fixed by
  packing `(round u32, deficit u32)` into one word and making refill + admit +
  decrement a single CAS mirroring the shipped `cake_agg_dequeue_gate`
  (`osvbng_qos_sched.h:702`), with a bounded `fetch_add` refund on later gate
  rejection. §4.3–§4.5, §4.7.
- **Scope correction to the finding:** Codex's "invalidates the claimed
  weighted fairness" is overbroad. Every Phase 1 DRR child is a scheduler,
  whose state is owner-thread-local end to end (`cake_dequeue.c:240`,
  charge site owner-only at `cake_enqueue.c:180-193`); Phase 1 is unaffected.
  Only the Phase 3 S-VLAN→port arbitration carried the race. The contended
  per-packet write Codex flagged is real but of the same cardinality as the
  S-VLAN gate CAS every packet already pays.

### Accepted: reparenting under backlog must transfer charge lineage
- **Source:** Phase 3 (Codex), verified
- **Severity:** HIGH (CRITICAL consequence on the backfill path)
- **Resolution:** `cake_agg_discharge` resolves through the scheduler's
  *current* attachment at free time (`osvbng_qos_sched.h:691`); nothing on the
  packet records what was charged. Shipped code is safe only because it never
  reparents a scheduler holding charged packets — create does not backfill,
  delete detaches only while destroying the charge target
  (`osvbng_qos_sched.c:358-447`). The spec's backfill breaks that invariant: a
  pre-backfill packet freed post-backfill underflows the S-VLAN's `u32`
  `buffer_usage`, wraps it, and pins admission (`cake_enqueue.c:263`) shut.
  Fixed by barrier-time charge transfer: `cs->buffer_usage` moves between
  parents with the member. Exact because both counters share the raw `pkt_len`
  basis (`cake_enqueue.c:274`, `:310`), the charge site is owner-thread-only,
  handed-off packets are uncharged, and the barrier quiesces the rest. Depends
  on PR #5 (`fix/agg-buffer-accounting`), promoted to hard prerequisite. §3,
  §4.9, §9.1, §9.2.
- **Scope correction to the finding:** full S-VLAN delete is benign — the
  counter dies with the aggregate and the port stays balanced, exactly as
  shipped delete behaves today. The undischargeable-charge hazard is real for
  backfill and for per-tag detach where the aggregate survives.

### Rejected: drain-before-reparent (Codex's primary recommendation)
- **Source:** Phase 3 (Codex)
- **Severity:** MEDIUM
- **Rationale:** Refusing attachment changes while backlog is nonzero stalls
  indefinitely on busy residential sessions unless enqueue is gated during the
  drain, which adds a hot-path state check to fix a config-time problem. The
  barrier transfer achieves exactness with zero hot-path cost.

### Rejected: per-packet charge lineage / per-worker credit redesign
- **Source:** Phase 3 (Codex alternatives)
- **Severity:** MEDIUM
- **Rationale:** Recording charged aggregate indices in buffer metadata costs
  opaque space and forces aggregates to outlive their deletion until every
  charge drains. Per-worker credits with reconciliation changes the fairness
  semantics to fix a race the packed CAS removes outright, using a loop shape
  the codebase already ships.

## Phase 2 — Claude Fable 5 deep review (2026-08-12)

Ten findings, all verified against source within the review
(`spec-reviews/CLAUDE.md`), all accepted by the human, all folded in at
Phase 4. The review also confirmed clean — no spec change — the §4.9 charge
transfer equality (attack item 3) and the structural refund pairing (attack
item 2, arithmetic bound corrected by F7).

### Accepted F1: `>= adj_len` admission deadlocks a child whose head packet exceeds the deficit cap
- **Source:** CLAUDE (Phase 2)
- **Severity:** HIGH (CRITICAL consequence with jumbo/ATM or unsplit GSO)
- **Resolution:** Refills clamp at the cap and the escape never fires under a
  saturated parent, so MTU 9000 × `dsl-pppoe-atm` (`adj_len ≈ 9,976`) or an
  unsplit GSO chain wedges the whole subscriber permanently. Replaced with
  the discipline the plugin's own flow-level DRR (`cake_dequeue.c:317`) and
  sch_cake use: eligible while `deficit > 0`, subtract full `adj_len`, carry
  bounded debt. Local child becomes `i64`; shared word stores the deficit
  biased by 2^31 (`CAKE_DRR_DEFICIT_BIAS`). Activation clamps
  `min(deficit, 0)` so debt survives idle (a debt reset would let a sparse
  child overshoot by `pkt/quantum`×). `CAKE_MAX_PKT_BYTES` fixed at 2048,
  explicitly not load-bearing. §4.3–§4.5, §9.1.

### Accepted F2: quantum arithmetic still overflows u64 for permitted configurations
- **Source:** CLAUDE (Phase 2)
- **Severity:** HIGH
- **Resolution:** `round_bytes * effective_weight` wraps at e.g. 25 Gbit/s
  child × weight 1000 under a 100 Gbit/s port (3.9e19). Weight validated
  1–256 at the API and commit time; quantum computed with a 128-bit
  intermediate at refill (once per child per round). §4.3, §5.2, §5.3.

### Accepted F3: activation atomics on cacheline0 reintroduce the 7c04b13 bounce for sparse traffic
- **Source:** CLAUDE (Phase 2)
- **Severity:** MEDIUM
- **Resolution:** Activation transitions track traffic, not config — a 50 pps
  VoIP subscriber toggles per packet and a single-member S-VLAN propagates
  each toggle to the port — invalidating the line every worker reads per
  packet for gate cost and buffer limit. `active_weight` and
  `n_active_children` move to their own cacheline4. §4.7.

### Accepted F4: §10 hot-path cost claim understated by ~4 RMWs; enqueue-side RMWs run at offered rate
- **Source:** CLAUDE (Phase 2)
- **Severity:** MEDIUM
- **Resolution:** Honest count stated in §10: 3 RMWs on 2 hot lines
  (shipped) → 7 on 5 (S-VLAN tier). Read-only overload filter added in front
  of the fetch-add-verify pair at each level (stale read used only to reject
  at a full queue — pre-#5 over-admission cannot return). New §9.3 benchmark
  phase (port-only vs S-VLAN clocks) gates the Phase 3 merge. §4.10, §9.3,
  §10.

### Accepted F5: gate burst credit is DRR-unarbitrated; 25 ms default re-opens a walk-order window at congestion onset
- **Source:** CLAUDE (Phase 2)
- **Severity:** MEDIUM
- **Resolution:** Under sustained sub-saturation virtual time pins at
  `now − burst_ns`, so the escape is continuously open and the first
  `burst_ns × rate` bytes of every saturation onset are admitted in walk
  order. Aggregate `burst_ns` default drops to 10 ms (range floor; also the
  post-idle downstream burst budget); the escape moves after the
  eligibility/reserve step so children enter onset with bounded deficits;
  §9.1 gains cyclic-load and onset-convergence rows. §4.4, §8, §9.1.

### Accepted F6: DRR check placement inside `cake_dequeue_one` unpinned; gate-style placement churns COBALT per blocked retry
- **Source:** CLAUDE (Phase 2)
- **Severity:** MEDIUM
- **Resolution:** A DRR-blocked child retries every polling dispatch —
  1000× round frequency — and placed after the AQM each retry re-runs
  `cobalt_should_drop` on the same head packet, escalating `codel_count` and
  inflating `ecn_marks`. The step-2 check is specified to run before
  `cobalt_should_drop` and the ECN mark, mutating nothing when blocked;
  matches sch_cake under a closed shaper. Steps 3–5 keep AQM-before-gate so
  COBALT drains under a closed gate. §4.5.

### Accepted F9: defensive deactivation paths share the bitmap-clear mechanism with the weight-bearing site
- **Source:** CLAUDE (Phase 2)
- **Severity:** MEDIUM
- **Resolution:** `pool_is_free_index` (`cake_dequeue.c:232`) and
  owner-mismatch (`:242`) route through the same `deactivate[]` array as
  empty-detect. Weight moves only on empty-detect of a live, owned scheduler
  and at teardown; the defensive paths clear the bit only — subtracting
  there double-counts a completed teardown and collapses `W`. §9.1 gains a
  teardown-race row. §4.9.

### Accepted F7: refund-carry bound is per-worker, not per-packet
- **Source:** CLAUDE (Phase 2)
- **Severity:** LOW
- **Resolution:** One refund can be in flight per worker; transient ceiling
  restated as `BIAS + cap + n_workers × max adj_len` (still ≪ 2^32) so the
  §9.1 linearizability row asserts the correct invariant. §4.4.

### Accepted F8: escape comparison underflows u64 at boot
- **Source:** CLAUDE (Phase 2)
- **Severity:** LOW
- **Resolution:** `now_ns - PERIOD` wraps in the first round after boot and
  in the harness (stub clock starts at 0). Rewritten in addition form. §4.4.

### Accepted F10: two §4.9/§4.4 wordings invited implementation mistakes
- **Source:** CLAUDE (Phase 2)
- **Severity:** LOW
- **Resolution:** Backfill now states the port side is not decremented
  (pre-backfill packets still discharge the port; a symmetric subtract
  over-admits). `cake_drr_local_admit` relabelled "owner-local, no refund
  obligation" — the refill mutates, so "read-only" was wrong. §4.4, §4.9.

## Rejected

### Depend on the walk-rotation fix (PR #9) for fairness
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Rationale:** Rotation equalises service opportunities per dispatch, not
  bytes, so it lands on equal shares only when children have equal rates and
  equal demand. It carries no weighting and no notion of a child's rate. DRR
  makes walk order irrelevant on its own (§4.2), so this spec neither requires
  nor conflicts with #9.
- **Challenged and upheld at Phase 5 (2026-08-12):** an implementation-time
  model predicted that the §4.4 work-conserving escape, being unarbitrated,
  would hand ~14% of the parent's rate to the lowest pool index and break
  §9.1. The rejection was briefly reversed on that basis. Measurement on the
  built plugin refuted it: DRR alone gives 0.63% spread for four equal
  children and +0.57 points worst error at weights 1:2:4:8, both inside
  §9.1. Walk order does bias the escape, by 0.2 to 0.6 points, which is not
  material. The rationale above stands as originally written. Full data and
  why the model misled in PHASE5_FINDINGS.md F5-1.

### Aggregate-driven dequeue (invert the loop, DRR from a central arbiter)
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Rationale:** Textbook DRR, and it requires every child of an aggregate to
  share one owner thread. That caps an aggregate at one core and fights RSS,
  which is exactly why `hqos-qinq` pivoted away from per-S-VLAN scheduling. The
  pivot was right about pinning; it was wrong that DRR implies it.

### Virtual-time WFQ instead of DRR
- **Source:** Phase 1
- **Severity:** LOW
- **Rationale:** Less state (one `u64` per child), no quantum tuning, smoother
  than round boundaries, and a better fit for a codebase that is already all
  virtual time in nanoseconds. Rejected because the eligibility bound is subtle:
  too tight reintroduces the progress problem that wall-clock rounds exist to
  prevent, too loose allows a child to burst, and the correct bound depends on
  the active weight sum in a way that needs a proof this spec cannot supply.
  Worth revisiting if the harness shows DRR round boundaries cost measurable
  smoothness.

### Configurable DRR round period
- **Source:** Phase 1
- **Severity:** LOW
- **Rationale:** A granularity knob with no good operator answer, on an API
  surface that already grows by six messages. Compile-time constant at 1 ms;
  revisit only if measurement demands it.

### Equal-share fairness (fixed quantum for every child)
- **Source:** Phase 1
- **Severity:** LOW
- **Rationale:** Simplest possible DRR, and it gives a 100 Mbit/s subscriber the
  same congested share as a 10 Mbit/s one. Wrong for a BNG, where the shaped
  rate is what the customer bought.
