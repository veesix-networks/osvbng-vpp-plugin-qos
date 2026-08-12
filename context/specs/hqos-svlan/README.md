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
| Phase 5 | Implementation | **§7 Phase 1 done and measured** (81be05a, 2026-08-12) — DRR on the existing port tier. Builds clean against VPP v26.06 with zero warnings incl. SIMD variants; fairness verified on a running VPP within §9.1 criteria at equal and unequal rates. **§7 Phases 2-4 done** — harness (65 checks), the S-VLAN tier, and the `_v2` binary API. All v1 message CRCs verified unchanged. Next: §7 Phase 5, the osvbng control plane |
| Phase 6 | Code Review | Not started |

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
IMPLEMENTATION_SPEC.md, DECISIONS.md and PHASE5_FINDINGS.md.

Spec phases 1-4 complete. Phase 5 implementation on `feat/hqos-svlan-drr`:

- §7 Phase 1 (DRR on the port tier)   done, measured
- §7 Phase 2 (tests/drr_harness.c)    done, 65 checks
- §7 Phase 3 (the S-VLAN tier)        done, measured
- §7 Phase 4 (binary API)             done, exercised via vat2
- §7 Phase 5 (osvbng control plane)   NEXT. Nothing of it exists.

The API is frozen at Phase 4's exit, which is what unblocks the control
plane. All twelve v1 message CRCs are verified unchanged against `main`;
twelve new messages are added. `osvbng_cake_capabilities` reports
version 3, max_levels 2, weight 1-256, and a feature bitmap.

This workspace builds and runs the plugin. Use it:

  ../vpp                  VPP v26.06, the pinned DATAPLANE_VERSION
  ../osvbng-vpp           copy src/* into plugins/osvbng_qos_sched/ then
                          DOCKER_DEFAULT_PLATFORM=linux/amd64 make vpp-dev
                          (omit VPP_DEV_TARGET to rebuild the vat2 plugin too)
  tests/fairness-rig.sh   VPP under pg load, per-subscriber shares
  tests/CMakeLists.txt    cmake -S tests -B build/tests && ctest --test-dir build/tests

Read PHASE5_FINDINGS.md first. F5-1 is a worked example of a standalone
model predicting a fairness failure the dataplane does not have. F5-2 to
F5-4 are three real defects the rig and harness found in the spec's design.
F5-5 bounds what the rig can honestly measure - roughly 20 children per
tier, because the main-thread packet generator cannot offer uniform load
beyond that.

Phase 5 work, from §6:
- pkg/vpp/binapi/osvbng_qos_sched/ regenerated from the new API JSON
- pkg/config/qos_aggregate.go: the `qos-aggregates` schema of §5.3, with
  commit-time validation - interface exists, S-VLAN sets disjoint per port,
  child rate not above parent, weight 1-256
- pkg/conf/handlers/qos_aggregate.go: conf.Handler with Dependencies() on
  the interface path
- pkg/southbound/vpp/qos.go: ApplyAggregate/RemoveAggregate/UpdateAggregate,
  ApplyScheduler moved to _v2 carrying weight, re-assert on
  ENTRY_ALREADY_EXISTS
- pkg/handlers/show/qos/aggregate.go: CLI plus
  telemetry.RegisterMetric[southbound.AggregateState]
- pkg/svcgroup/apply.go: pass the service-group weight to ApplyScheduler

Note osvbng's own conventions differ from this repo's: Conventional Commits
with release-please, no blocking I/O under locks, O(1) lookups on the
session path. Read osvbng/docs/contributing/guidelines.md before starting.
```
