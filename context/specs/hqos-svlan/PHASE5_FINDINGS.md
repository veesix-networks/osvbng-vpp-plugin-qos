# Phase 5 findings: hqos-svlan

Findings raised while implementing, that change what the spec says rather
than how it is coded. Recorded here rather than folded into
`IMPLEMENTATION_SPEC.md` because each reverses or qualifies a decision the
human already ruled on in `DECISIONS.md`.

## F5-1: DRR alone does not meet the section 9.1 fairness criterion — the walk rotation is complementary, not redundant

- **Raised:** 2026-08-12, during section 7 Phase 1 implementation
- **Severity:** HIGH — the phase's stated acceptance criterion is not met
- **Status:** **resolved 2026-08-12** by option 1 below, varied: the human
  chose to fold the rotation into this branch now rather than reopen #9, so
  #9's commit is cherry-picked as c2e41a8 and the one-feature-per-PR rule is
  knowingly relaxed for this branch. `DECISIONS.md` records the reversal. The
  weighted-share residual under "Also observed" stays open for Phase 2.
- **Reverses:** `DECISIONS.md` → Rejected → "Depend on the walk-rotation fix
  (PR #9) for fairness", and the §3 / §4.2 claim that "DRR makes walk order
  irrelevant on its own ... if #9 merges the two are compatible and #9
  becomes redundant".

### What the spec claims

§4.2: "Walk order stops mattering. Whichever scheduler `clib_bitmap_foreach`
reaches first can only take its quantum, not all the credit. This is what
closes issue #8 without the rotation fix."

§9.1, first row: equal weights, 4 children, saturated → shares within ±2%.

### What is actually true

Walk order stops mattering **on the arbitrated path only**. The
work-conserving escape (§4.4) is by design unarbitrated — it admits without
consulting or decrementing the deficit — and under saturation with
MTU-sized packets it carries a large fraction of the parent's capacity.
Walk order decides who receives all of it.

The escape is not a corner case under saturation. Every child refills on
the same global round boundary (`round = now_ns / ROUND_PERIOD`), so the
children move in lockstep: all become eligible together, all exhaust
together, and the parent then has no eligible child for several rounds. The
escape exists to reclaim exactly those gaps, and it hands each one to the
lowest pool index.

Measured against the spec's own headline scenario — four equal 5 Mbit/s
children under one 8 Mbit/s aggregate, all saturated, 1514-byte packets:

| Configuration | Utilisation | Shares | Worst error |
|---|---|---|---|
| Spec as written | 100.0% | 36.8 / 21.2 / 21.0 / 21.0 % | **11.8 pts** |
| Escape disabled | 85.9% | 25.0 / 25.0 / 25.0 / 25.0 % | 0.0 pts |
| Spec + walk rotation | 100.0% | 25.1 / 24.8 / 25.4 / 24.7 % | **0.4 pts** |

The rotation row uses PR #9's actual policy — resume the walk after whichever
scheduler last got service — not a simplified rotate-by-one. Eight equal
children under a 16 Mbit/s aggregate land at 0.25 points by the same policy.

Disabling the escape is not an option: the 14.1% of capacity it recovers is
discarded by the aggregate gate's burst clamp, confirmed directly
(698 ms of virtual time clamped away over a 5 s run — exactly the missing
utilisation). With two busy and two active-but-trickling children, removing
the escape drops utilisation to 44.9%.

Eight children under a 16 Mbit/s aggregate behaves the same way: 21.9% for
the first child against 10.7% for the tail, 11.2 points of error, going to
0.4 points with rotation.

### Two mechanisms that were each rejected for being the other's job

PR #9's own description says rotation "is not fairness ... Real fairness
needs the aggregate to know its children and do DRR across them". That is
correct: rotation equalises service *opportunities* per dispatch, not bytes,
and carries no weighting. This spec then rejected #9 on the grounds that DRR
subsumes it. Both are half right, and the gap between them is the escape:

- **DRR** bounds each child's share of the *arbitrated* capacity, with
  weights. It has no opinion about the unarbitrated remainder.
- **Rotation** distributes the *unarbitrated* remainder evenly. It has no
  opinion about weights.

Neither is sufficient. Together they measure at 0.3 points of error at full
utilisation.

### Also observed, not yet resolved

With weights 1:2:4:8 the error is 4.6 points without rotation and 3.4 points
with it — better, but still outside a strict reading of §9.1's ±5% row
(3.4 points against a 26.7% target is 12.7% relative). The residual is the
escape again: distributed evenly by rotation, it over-serves low-weight
children, which is structurally the wrong shape for a weighted scheme.
Whether that matters is a judgement call, and pinning it down is what the
Phase 2 harness is for.

With 590-byte packets every configuration is exactly fair, with or without
rotation. The error scales with packet size relative to quantum, which is
consistent with the lockstep-gap explanation.

### Options

1. **Fold the rotation into this branch** and amend `DECISIONS.md`. PR #9 is
   closed but the branch (`fix/agg-walk-rotation`, 2aaddad) applies to the
   same file; it is two static helpers plus one `u32` in
   `cake_per_thread_t`, with no shared state. Cheapest path to the spec's
   own criterion, at the cost of the "one feature per PR" rule.
2. **Reopen #9 and merge it first**, then rebase this branch on it. Keeps
   the scope rule intact; costs a round trip.
3. **Amend §9.1 instead** and accept that Phase 1 removes structural
   starvation without reaching proportional fairness. Defensible only if the
   escape's allocation is documented as best-effort.

Option 2 was the recommendation; the human chose option 1 on 2026-08-12 to
avoid the round trip, accepting the scope-rule exception. The PR description
for this branch must call out that it carries #9's commit.

### How this was measured, and what it does not prove

A single-threaded C model in a scratchpad, linking the real `cake_drr.h`
inlines unmodified against a faithful reimplementation of
`cake_agg_dequeue_gate` (virtual time, burst clamp) and the subscriber
shaper, polled every 2 µs over 5 simulated seconds.

It exercises the arbitration arithmetic and the interaction between the
deficit, the escape and the gate. It is **not** the plugin: no VPP, no
multiple workers, no COBALT, no real packet arrival, no handoff. The
workspace has no VPP tree, so nothing stronger is available here. Treat the
direction as solid and the exact percentages as indicative. The Phase 2
harness is where these become real assertions — it should carry the rows in
the table above, including the escape-disabled control.
