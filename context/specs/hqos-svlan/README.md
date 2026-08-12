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
| Phase 5 | Implementation | **§7 Phase 1 done and measured** (81be05a, 2026-08-12) — DRR on the existing port tier. Builds clean against VPP v26.06 with zero warnings incl. SIMD variants; fairness verified on a running VPP within §9.1 criteria at equal and unequal rates. **§7 Phase 2 done** — `tests/drr_harness.c`, 53 checks green. Next: §7 Phase 3, the S-VLAN tier |
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

Line numbers are on the `feat/hqos-svlan-drr` baseline (= `main` +
`fix/aggregate-shaper-correctness`), not on `main`.

| Path | Relevance |
|---|---|
| `src/osvbng_qos_sched.h:159` | `cake_aggregate_t` — cache-line layout to preserve; gains `level`/`parent_index`/`svlan_id`/`weight` (cl0), activation pair (cl4), `drr.round_deficit` (cl3) |
| `src/osvbng_qos_sched.h:179` | `cake_sched_t` — gains one `cake_drr_child_t`, `weight`, `agg_svlan_index` |
| `src/osvbng_qos_sched.h:220` | `cake_per_thread_t` |
| `src/osvbng_qos_sched.h:696` | `cake_agg_discharge` — resolves via *current* attachment; why reparenting must transfer charges |
| `src/osvbng_qos_sched.h:729` | `cake_flow_discard` (from #5) — the wholesale-release path the parent-chain walk must generalise |
| `src/osvbng_qos_sched.h:748` | `cake_agg_dequeue_gate` — the CAS loop shape `cake_drr_shared_reserve` mirrors |
| `src/cake_dequeue.c:239` | the bitmap walk whose stable order causes #8 |
| `src/cake_dequeue.c:244,252` | **defensive** deactivations (pool-free, owner-mismatch) — clear the bit, never move weight (§4.9) |
| `src/cake_dequeue.c:282,290` | **empty-detect** deactivations — these are the weight-bearing ones |
| `src/cake_dequeue.c:415` | the shared bitmap-clear loop both kinds funnel through — do not attach weight accounting here |
| `src/cake_enqueue.c:258` | aggregate buffer admission — gains the read-only overload filter and the second level |
| `src/cake_enqueue.c:353` | activation site — weight accounting |
| `src/osvbng_qos_sched.c:239` | `sup_sw_if_index` attachment walk — extended for the S-VLAN hop |
| `src/osvbng_qos_sched.c:305` | teardown deactivation — weight accounting |

## Prompt to Resume

```
Read context/PROCESS.md, then context/specs/hqos-svlan/README.md,
IMPLEMENTATION_SPEC.md, DECISIONS.md and PHASE5_FINDINGS.md.

Spec phases 1-4 are complete. Phase 5 implementation is under way on
`feat/hqos-svlan-drr`:

- §7 Phase 1 (DRR on the port tier) is done and measured.
- §7 Phase 2 (the harness) is done: tests/drr_harness.c, 53 checks.
- §7 Phase 3 (the S-VLAN tier) is next. Nothing of it exists yet.

This workspace CAN build and run the plugin - the README's old "no VPP
tree" constraint is gone. Use it, and do not claim anything is verified
that you have not run:

  ../vpp                  VPP v26.06 source, the pinned DATAPLANE_VERSION
  ../osvbng-vpp           containerised builder; copy src/* into
                          plugins/osvbng_qos_sched/ then
                          DOCKER_DEFAULT_PLATFORM=linux/amd64 \
                          VPP_DEV_TARGET=osvbng_qos_sched_plugin make vpp-dev
  tests/fairness-rig.sh   runs VPP under packet-generator load and prints
                          per-subscriber shares against configured rate
  tests/CMakeLists.txt    cmake -S tests -B build/tests && ctest --test-dir build/tests

Read PHASE5_FINDINGS.md F5-1 before trusting any model: a standalone
simulation predicted an 11-point fairness failure that the real dataplane
does not have, and it was wrong enough to reverse a spec decision before
measurement put it back. Arithmetic and concurrency invariants go in the
harness; anything about shares or dispatch timing goes in the rig.

Phase 3 work, from §4.7-§4.9:
- cake_aggregate_t gains level/parent_index/svlan_id, the packed
  cake_drr_shared_child_t round_deficit on its own cache line, and the
  4096-entry per-port S-VLAN map.
- cake_drr_shared_reserve()/refund() in cake_drr.h - one CAS over
  (round u32 << 32 | biased deficit u32), biased by 2^31 so bounded debt
  packs unsigned. Mirror cake_shaper_gate_take()'s loop shape. A failed
  admission in an unchanged round must fail read-only.
- The §4.5 five-step dequeue path with the two refund obligations, and the
  local flag recording whether the reserve actually happened (escape
  admissions are never refunded).
- Attachment walk extension, backfill and per-tag detach, including the
  buffer-charge transfer that stops a u32 underflow pinning admission shut.
- Two-level enqueue admission with the read-only overload filter.
- §9.3 benchmark phase pair gates this merge.

Extend the harness with the §9.1 rows that only become testable now:
shared-reserve linearizability, two-level refund pairing, and the reparent
charge transfer. Extend the rig to a two-tier topology for the shares.
```
