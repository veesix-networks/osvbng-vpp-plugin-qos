# QoS: S-VLAN HQoS with Fair Queueing

**What:** A per-S-VLAN aggregate tier between the subscriber CAKE scheduler and
the per-port aggregate, plus weighted deficit round robin so any aggregate
shares its rate fairly between its children instead of serving them in
pool-index order.

**Issue:** [#8](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/issues/8)
(fairness) — the S-VLAN tier itself extends
[#1](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/issues/1).

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Spec Draft | **Complete** |
| Phase 2 | Spec Refinement (Claude Fable 5, substituting Gemini) | **Complete** (2026-08-12; see [spec-reviews/CLAUDE.md](spec-reviews/CLAUDE.md) — 10 findings, all accepted) |
| Phase 3 | Spec Critique (Codex) | **Complete** (2026-08-12, adversarial; see DECISIONS.md "Phase 3") |
| Phase 4 | Spec Finalization | **Complete** (2026-08-12; all Phase 2 + Phase 3 findings folded in, resolutions in DECISIONS.md) |
| Phase 5 | Implementation | **§7 Phase 1 done and measured** (81be05a, 2026-08-12) — DRR on the existing port tier. Builds clean against VPP v26.06 with zero warnings incl. SIMD variants; fairness verified on a running VPP within §9.1 criteria at equal and unequal rates. **All five §7 phases done.** DRR, harness, the S-VLAN tier, the `_v2` API (v1 CRCs verified unchanged), and the osvbng control plane on `feat/hqos-svlan-control-plane` in that repo. Open items in [PHASE5_FINDINGS.md](PHASE5_FINDINGS.md) |
| Phase 6 | Code Review | **Complete** (2026-08-12) — Claude bug hunt + two Codex adversarial passes (spec compliance; protocol conformance in the Gemini slot, at user direction), artifacts in [code-reviews/](code-reviews/). §9.3 benchmark gate **passed** ([PHASE6_VERIFICATION.md](PHASE6_VERIFICATION.md), `tests/perf-rig.sh`): tier costs +1.0 clk/pkt enqueue / +5.3% dequeue at the hot point. Triage in DECISIONS.md "Phase 6": 7 findings accepted and fixed (incl. the CRITICAL stale-round-tag wedge both reviewers found independently), 1 recorded, 1 rejected as pre-existing v1 arithmetic. Harness 71 checks green; two-tier fairness re-verified post-fix (worst 0.30 pts) |

## Blocking prerequisites

**Decision (2026-08-12, human):** proceed on the assumption that qos #4, #5,
#6 and #7 will merge. `feat/hqos-svlan-drr` therefore carries
`fix/aggregate-shaper-correctness` — the branch that stacks all four — as its
implementation baseline. If any of them changes materially before merging,
rebase this branch rather than reconciling by hand.

- [#5](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/5)
  (buffer accounting) is the **hard prerequisite**: the §4.9 charge transfer
  and the §9.1 accounting invariants are exact only under #5's
  `cs->buffer_usage`-equals-outstanding-charge invariant (spec §3).
- The aggregate path cannot execute correctly without
  [#4](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/4) (gate
  livelock),
  [#6](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/6) (rate
  precision) and
  [#7](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/7)
  (interface teardown).

Still outstanding, and **not** assumed away — session interfaces are
registered standalone, so `sup_sw_if_index == sw_if_index` and the attachment
walk terminates at the session, meaning no scheduler under a real IPoE/PPPoE
session reaches an S-VLAN or a port:

- [`osvbng-vpp-plugin-ipoe` #7](https://github.com/veesix-networks/osvbng-vpp-plugin-ipoe/pull/7)
- [`osvbng-vpp-plugin-pppoe-control` #3](https://github.com/veesix-networks/osvbng-vpp-plugin-pppoe-control/pull/3)

These gate *session-based* validation only. The walk resolves normally for
plain VLAN sub-interfaces, which is how issue #8 was measured (spec §3), so
the §9.2 container rig exercises both tiers without them.

This spec does **not** depend on
[#9](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/9) (walk
rotation). DRR makes walk order irrelevant on its own; if #9 merges the two are
compatible and #9 becomes redundant.

> **Challenged and upheld at Phase 5 (2026-08-12).** A model built during
> implementation predicted the unarbitrated §4.4 escape would break §9.1;
> measurement on the built plugin refuted it. DRR alone: 0.63% spread for
> four equal children, +0.57 points worst error at weights 1:2:4:8 — both
> inside criteria. #9's commit was briefly cherry-picked here and is now
> reverted (521dbd4); it improves those to 0.28% / +0.41 points but is not
> required. See [PHASE5_FINDINGS.md](PHASE5_FINDINGS.md) F5-1.

## Build reality in this workspace

**Superseded 2026-08-12 — the plugin builds and runs here.** Two siblings
make it possible, and Phase 1 was verified with both:

- `../vpp` — VPP v26.06 source, the pinned `DATAPLANE_VERSION`.
- `../osvbng-vpp` — containerised builder. Copy the plugin sources into
  `plugins/osvbng_qos_sched/`, then
  `DOCKER_DEFAULT_PLATFORM=linux/amd64 VPP_DEV_TARGET=osvbng_qos_sched_plugin make vpp-dev`.
  Incremental, about a minute, and it builds the `MULTIARCH_SOURCES` SIMD
  variants too. amd64 under Rosetta on an arm64 host.

VPP itself can then be run in that container against the built `.so`, which
is how the §9.2 fairness rig ran (packet generator into sub-interfaces under
one port aggregate, `show cake scheduler` for per-subscriber bytes). That is
a real measurement of the real code, not a model.

Keeping the DRR core (`cake_drr_*` inlines, quantum arithmetic, and later the
packed-word reserve) free of `vlib`/`vnet` dependencies beyond fixed-width
types is still worth doing — `tests/drr_harness.c` gets to link the shipped
arithmetic and run it under pthreads without a VPP environment. But it is no
longer the *only* verification available, and F5-1 is a worked example of a
standalone model confidently predicting behaviour the real dataplane does not
show. Anything involving dispatch timing or share measurement belongs in the
rig, not the harness.

## Key Context Files

- [IMPLEMENTATION_SPEC.md](IMPLEMENTATION_SPEC.md) — technical specification
- [DECISIONS.md](DECISIONS.md) — Phase 1 decisions, recorded so review agents
  attack the reasoning rather than rediscover it
- [spec-reviews/CLAUDE.md](spec-reviews/CLAUDE.md) — Phase 2 deep review
  (design vs VPP/DPDK practice, fairness under congestion, bufferbloat);
  answers the four attack items below
- [../hqos-qinq/IMPLEMENTATION_SPEC.md](../hqos-qinq/IMPLEMENTATION_SPEC.md) —
  the pre-pivot per-S-VLAN design; §4.7 gate and §4.9 lifecycle are inherited
- [../hqos-qinq/DECISIONS.md](../hqos-qinq/DECISIONS.md) — records "Weighted DRR
  in Phase 1" as rejected; this spec reverses it, see §4.2
- [../cake-scheduler/IMPLEMENTATION_SPEC.md](../cake-scheduler/IMPLEMENTATION_SPEC.md) —
  the leaf level, unchanged here

## Codebase Entry Points

Line numbers are on `feat/hqos-svlan-drr` as of ba1e335 (§7 Phases 1-2 done).

| Path | Relevance |
|---|---|
| `src/cake_drr.h` | the DRR core, dependency-free by contract; gains `cake_drr_shared_child_t`, `cake_drr_shared_reserve()`, `cake_drr_shared_refund()` |
| `src/cake_shaper.h` | rate/cost, the gate CAS, buffer admission — same contract; `cake_shaper_gate_take` is the loop shape the shared reserve mirrors |
| `src/osvbng_qos_sched.h:167` | `cake_aggregate_t` — cache-line layout to preserve; gains `level`/`parent_index`/`svlan_id`/`weight` (cl0) and `drr.round_deficit` on its own line, beside the existing activation pair |
| `src/osvbng_qos_sched.h:199` | `cake_sched_t` — has `drr`, `aggregate_index`; gains `agg_svlan_index` and `weight` |
| `src/osvbng_qos_sched.h:729` | `cake_agg_discharge` — resolves via *current* attachment; why reparenting must transfer charges |
| `src/osvbng_qos_sched.h:743` | `cake_flow_discard` — the wholesale-release path the parent-chain walk must generalise |
| `src/osvbng_qos_sched.h:765` | `cake_sched_drr_admit` — binds the core to a scheduler; gains the S-VLAN hop |
| `src/osvbng_qos_sched.h:824` | `cake_agg_dequeue_gate` — becomes two-level with a refund |
| `src/cake_dequeue.c:284` | the bitmap walk; order left alone deliberately, see the comment there |
| `src/cake_dequeue.c:291,299` | **defensive** deactivations (pool-free, owner-mismatch) — clear the bit, never move weight (§4.9) |
| `src/cake_dequeue.c:330,339` | **empty-detect** deactivations — these are the weight-bearing ones |
| `src/cake_dequeue.c:485` | the shared bitmap-clear loop both kinds funnel through — do not attach weight accounting here |
| `src/cake_enqueue.c:263` | aggregate buffer admission — gains the read-only overload filter and the second level |
| `src/cake_enqueue.c:351` | activation site — weight accounting |
| `src/osvbng_qos_sched.c:247` | `sup_sw_if_index` attachment walk — extended for the S-VLAN hop |
| `src/osvbng_qos_sched.c:314` | teardown deactivation — weight accounting |
| `tests/drr_harness.c` | §9.1 arithmetic and concurrency; gains the shared-reserve and refund rows |
| `tests/fairness-rig.sh` | §9.2 shares against a running VPP; gains a two-tier topology |

## Prompt to Resume

```
Read context/PROCESS.md, then context/specs/hqos-svlan/README.md,
IMPLEMENTATION_SPEC.md, DECISIONS.md, PHASE5_FINDINGS.md and
PHASE6_VERIFICATION.md.

Phases 5 AND 6 are COMPLETE across two repos:

  osvbng-vpp-plugin-qos  feat/hqos-svlan-drr
    §7 P1-P4 DRR, harness, S-VLAN tier, _v2 API   done, measured
    §9.3 benchmark gate (tests/perf-rig.sh)       passed
    Phase 6 review + triage + fixes               done (71 harness checks)

  osvbng                 feat/hqos-svlan-control-plane
    §7 P5 control plane + Phase 6 fixes           done, builds/tests on linux

Phase 6 artifacts: code-reviews/{CLAUDE,CODEX,CODEX-PROTOCOL}.md, triage in
DECISIONS.md "Phase 6", benchmark in PHASE6_VERIFICATION.md. Seven findings
were accepted and fixed — the CRITICAL one was round tags initializing to
zero, which wedged any child created past day 25 of uptime, found
independently by both reviewers. One finding (per-packet cost floor at
40-100G) was rejected as pre-existing v1 arithmetic and needs its own issue.
CL-2 (AQM re-entry under closed gates) is recorded, unfixed, and needs a
bottlenecked rig to evaluate.

What remains before merge:
- §9.4 containerlab suites 18/19 — needs a dataplane image and clab, not
  runnable in this workspace.
- qos #4/#5/#6/#7 merging in the stacked order
  (fix/aggregate-shaper-correctness); this branch rebases if they change.
- Session parentage: osvbng-vpp-plugin-ipoe #7, pppoe-control #3. All
  measurement used plain VLAN sub-interfaces.
- osvbng control plane has never met a live dataplane.
- Follow-up issues to file: cost-floor precision (CODEX-PROTOCOL #2),
  hot-path batching (two specs deep against Requirement #2).

Standing measurement bounds (PHASE5_FINDINGS.md): F5-5 caps honest fairness
claims at ~20 children per tier on this rig; intra-subscriber frame-size
quantisation unmeasured; everything main-thread only.

Build and measure with:
  ../vpp                  VPP v26.06, the pinned DATAPLANE_VERSION
  ../osvbng-vpp           copy src/* into plugins/osvbng_qos_sched/ then
                          DOCKER_DEFAULT_PLATFORM=linux/amd64 make vpp-dev
  tests/fairness-rig.sh   VPP under pg load, per-subscriber shares
  tests/perf-rig.sh       §9.3 clocks/packet phase pairs
  tests/CMakeLists.txt    cmake -S tests -B build/tests && ctest --test-dir build/tests
  osvbng                  golang:1.24 container; it does not build on darwin
```
