# Decisions: hqos-svlan

Phase 2 (Gemini refinement) has not run. Phase 3 (Codex adversarial critique)
ran 2026-08-12; its findings were verified against source before acceptance and
are recorded under "Phase 3". Phase 1 entries are decisions taken during
drafting, recorded here so the review agents attack the reasoning rather than
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

## Rejected

### Depend on the walk-rotation fix (PR #9) for fairness
- **Source:** Phase 1
- **Severity:** MEDIUM
- **Rationale:** Rotation equalises service opportunities per dispatch, not
  bytes, so it lands on equal shares only when children have equal rates and
  equal demand. It carries no weighting and no notion of a child's rate. DRR
  makes walk order irrelevant on its own (§4.2), so this spec neither requires
  nor conflicts with #9.

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
