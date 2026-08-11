# Decisions: hqos-svlan

Phases 2 and 3 (Gemini refinement, Codex critique) have not run. The entries
below are decisions taken during Phase 1 drafting, recorded here so the review
agents attack the reasoning rather than rediscover it.

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
