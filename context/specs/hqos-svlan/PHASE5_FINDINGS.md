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
- The cherry-picked rotation commit is reverted (521dbd4). It is harmless and
  marginally beneficial (0.63% → 0.28% spread, +0.57 → +0.41 points weighted),
  but the one-feature-per-PR exception taken to land it was bought with
  evidence that did not hold. #9 stands or falls on its own merits; the
  measured numbers are recorded in the walk comment in `cake_dequeue.c` so
  that case can be made without redoing the work.
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

## F5-2: the work-conserving escape must be charged

- **Raised and fixed:** 2026-08-12, implementing section 7 Phase 3
- **Severity:** CRITICAL for the S-VLAN tier
- **Amends:** section 4.4, and `DECISIONS.md` "Work-conserving escape when a
  parent is not saturated"

Section 4.4 admits an escape without charging the deficit, reasoning that
capacity taken while the parent is idle should not count against a child's
congested share. That reasoning holds only for the root.

A non-root tier's virtual time also lags while the tier itself is blocked
against the tier above - it is not sending, so its clock is not advancing -
and every child reads that lag as spare capacity. Because nothing is charged,
the intra-tier deficits stop moving entirely and walk order decides
everything. Measured on the built plugin, four subscribers under one
throttled S-VLAN: 24.86 / 0.14 / 0.00 / 0.00 % of the tier.

Two changes, both needed:

- The escape is charged like any other admission, and refused once the child
  is one packet into debt. `CAKE_DRR_ESCAPE` is gone; the outcome is
  `CAKE_DRR_ADMIT`, and `CAKE_DRR_UNARBITRATED` covers only the genuinely
  parentless case, which is never charged and never refunded.
- The same rule applies to `cake_drr_shared_reserve`, inside its CAS, so an
  escape at the S-VLAN tier stays refundable.

Nothing is lost. Utilisation with a deliberately under-using sibling measures
100.6% of port rate, because the activation refcount already does the
work-conserving job: a child that stops using its share deactivates, leaves
W, and its siblings' quanta grow to fill the gap.

## F5-3: a child's quantum must come from what its parent can pass, not what it is configured for

- **Raised and fixed:** 2026-08-12, implementing section 7 Phase 3
- **Severity:** CRITICAL for the S-VLAN tier
- **Amends:** section 4.3's quantum formula

Section 4.3 derives `quantum_i` from the parent's configured
`rate_bytes_per_sec`. Correct for the port; wrong for any tier under an
oversubscribed parent, which is the ordinary HQoS case.

An S-VLAN provisioned at port rate but winning half the port hands each child
a quantum sized for the whole S-VLAN rate. No child's deficit ever runs out,
every child stays permanently eligible, and the tier stops arbitrating.
Measured: two subscribers under one such S-VLAN took 49.9% and 0.12% of the
port.

`cake_agg_effective_round_bytes` folds in the parent's own share -
`min(own round bytes, parent round bytes x own weight / parent W)` - so the
quanta beneath a tier sum to what the tier really gets. It is evaluated once
per child per round on the refill path, never per packet.

## F5-4: the round tag must test advancement, not inequality

- **Raised and fixed:** 2026-08-12, caught by the harness
- **Severity:** HIGH
- **Amends:** section 4.4's refill condition

Workers sample the wall clock independently, so two can straddle a round
boundary and see different rounds for the same instant. `cur_round != round`
lets the lagging worker treat the leader's round as new and refill again; the
two then ping-pong, issuing a quantum per crossing instead of per round.

The harness caught it immediately: eight threads over the same 20000 rounds
drew 26409 packets of credit where 3304 had been issued. Fixed by testing
`(i32) (round - current) > 0`, which is also what keeps the u32 tag wrapping
correctly. The signed form means a lagging worker neither refills nor rewinds
the published round.

## F5-5: rig capacity, and fairness at scale - OPEN

- **Raised:** 2026-08-12
- **Status:** open, not a known defect

`tests/fairness-rig.sh` drives load from VPP's packet generator on the main
thread. It cannot offer uniform load to many streams: measured offered-packet
counts across subscribers spread 7.7x at 20 subscribers and 23x at 40, even
with every stream configured at the same explicit rate. At 40 streams with no
rate set, 28 of them were never offered a single packet, which reads as
dataplane starvation and is not.

Consequences, honestly bounded:

- **8 subscribers, two tiers, unequal rates at both, contended port: worst
  error 0.06 points.** This is the configuration the tier is verified at.
- **20 subscribers:** the S-VLAN split holds (67.45 / 32.55 against 66.67 /
  33.33), and within each S-VLAN every child is accurate except the
  last-indexed one, which is 1.1 points under in one group and 5.1 in the
  other. Both are the highest pool index in their group, so this looks like
  walk-order bias at the tail rather than a weighting error - the same effect
  PR #9's rotation addresses, which F5-1 found immaterial at one tier with
  four children.
- **40 subscribers:** the rig cannot drive it. No conclusion either way.

What would settle it: a load source that is not the main-thread packet
generator, and a rig that fails when its offered-load spread exceeds a
threshold rather than reporting shares regardless. Until then this spec should
not claim a fairness figure above roughly 20 children per tier.
