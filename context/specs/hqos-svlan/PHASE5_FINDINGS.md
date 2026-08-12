# Phase 5 findings: hqos-svlan

Findings raised while implementing, that change what the spec says rather
than how it is coded.

## F5-1: DRR alone does not meet the section 9.1 fairness criterion — RAISED AND REFUTED

- **Raised:** 2026-08-12, during section 7 Phase 1 implementation
- **Refuted:** 2026-08-12, by measurement on the built plugin
- **Status:** closed, no spec change. The spec's original rejection of PR #9
  stands as written.

### What was claimed

That DRR makes walk order irrelevant on the *arbitrated* path only, that the
§4.4 work-conserving escape is unarbitrated and carries ~14% of the parent's
rate under saturation, and that walk order therefore hands all of it to the
lowest pool index — 36.8 / 21.2 / 21.0 / 21.0 % for four equal children
against §9.1's ±2%. The conclusion drawn was that PR #9's rotation is
required rather than redundant, and on that basis the rejection in
`DECISIONS.md` was reversed and #9's commit cherry-picked onto this branch.

### What the measurement shows

The claim came from a standalone C model of the arbitration arithmetic, run
because the workspace was believed to have no way to build the plugin. Once
the VPP tree and container builder were available (`../vpp` v26.06,
`../osvbng-vpp` `make vpp-dev`), the same scenarios ran against the real
plugin: VPP packet generator, four sub-interfaces under one port aggregate,
single thread, 1400-byte frames, offered far above capacity, 30 s windows.

Four equal 5 Mbit/s children under an 8 Mbit/s aggregate:

| Build | Shares | Spread |
|---|---|---|
| DRR alone | 7609680 / 7568964 / 7566156 / 7561944 | **0.63%** |
| DRR + rotation | 7580196 / 7559136 / 7575984 / 7563348 | **0.28%** |

Weights 1:2:4:8 (1000 / 2000 / 4000 / 8000 kbps), worst error against the
configured ratio:

| Build | Worst error |
|---|---|
| DRR alone | **+0.57 pts** |
| DRR + rotation | **+0.41 pts** |

Both criteria — §9.1's ±2% equal-weight row and ±5% weighted row — are met by
DRR alone. The model over-predicted the effect by more than an order of
magnitude.

A residual walk-order bias does exist and is measurable. Running the weighted
case with the rate order reversed, so the largest child sits at the lowest
pool index, moves the over-service with the index rather than the weight:
index 0 gains +0.57 pts when it is the smallest child and +0.22 pts when it
is the largest, and the last index is under-served in both. So the escape
does favour whoever the walk reaches first, exactly as argued — the error is
just 0.2 to 0.6 points, not 11 to 15, and nothing in §9.1 is threatened by
it.

### Why the model was wrong

It polled every child in index order on a fixed 2 µs tick. The real dequeue
node dispatches from the VPP main loop far more often, re-samples the clock
each dispatch, breaks out of a scheduler the moment its gate or quantum
refuses, and carries a frame budget. Those dynamics spread the escape
admissions across children in a way the fixed-tick model did not reproduce.
The model was right that the escape is unarbitrated and that walk order
decides who gets it; it was wrong about how much capacity that is worth.

### Consequences

- `DECISIONS.md` keeps its original rejection of #9; the reversal is undone.
- The cherry-picked rotation commit no longer has a justification tied to
  this spec. It is harmless and marginally beneficial (0.63% → 0.28%), but
  the one-feature-per-PR exception taken to land it was bought with evidence
  that did not hold.
- **Lesson for §9.1:** the standalone harness cannot stand in for the
  container rig on anything involving dispatch timing. The Phase 2 harness
  should assert the arithmetic — refill, debt, weight accounting invariants,
  linearizability — and leave share measurements to §9.2, which is now
  runnable here.

## Verification status of §7 Phase 1

Now that the plugin builds, the earlier "cannot be compiled or run in this
workspace" constraint is obsolete. As of 2026-08-12 Phase 1 has:

- Compiled clean against VPP v26.06, zero warnings, including the
  `MULTIARCH_SOURCES` x86_64_v3 and x86_64_v4 variants of `cake_dequeue.c`
  and `cake_enqueue.c`.
- Run under VPP with the §9.2-style rig above: fairness within criteria at
  both equal and unequal rates, `drr_blocked` and `parent_blocked` moving
  independently, `AGG_SHAPED` wired, and `active_weight` / `n_active_children`
  returning to 0 after the offered load stops — the §4.9 weight accounting
  invariant, observed rather than argued.

Not covered: multiple worker threads (the rig is main-thread only, as
af-packet's single rx queue forced in the original issue #8 measurement),
latency under load, and the S-VLAN tier, which does not exist yet.
