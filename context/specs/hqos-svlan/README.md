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
| Phase 5 | Implementation | **§7 Phase 1 committed** (81be05a, 2026-08-12) — DRR on the existing port tier. Not compiled: no VPP tree in this workspace. One open finding blocks the phase's own acceptance criterion — see [PHASE5_FINDINGS.md](PHASE5_FINDINGS.md) |
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

> **Contradicted at Phase 5 (2026-08-12).** Measured: DRR makes walk order
> irrelevant on the *arbitrated* path only. The §4.4 work-conserving escape
> is unarbitrated by design and carries ~14% of the parent's capacity under
> saturation, all of it to the lowest pool index — 36.8% vs 21.0% for four
> equal children, against a §9.1 criterion of ±2%. #9 is complementary, not
> redundant. See [PHASE5_FINDINGS.md](PHASE5_FINDINGS.md) F5-1; awaiting a
> human call.

## Build reality in this workspace

The workspace has no VPP tree, so plugin C **cannot be compiled or run
locally** — it builds only inside VPP via `-DVPP_EXTRA_PLUGINS` or a symlink
into `vpp/src/plugins/`. Consequence for Phase 5 sequencing: keep the DRR
core (`cake_drr_*` inlines, quantum arithmetic, the packed-word reserve) free
of `vlib`/`vnet` dependencies beyond fixed-width types, so
`tests/drr_harness.c` compiles and runs standalone against a stub clock on a
plain host toolchain. That is the only local verification available; the
containerlab suites and the benchmark need a VPP environment.

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
IMPLEMENTATION_SPEC.md and DECISIONS.md.

Spec phases 1-4 are complete (Codex adversarial critique and Claude Fable 5
deep review, both 2026-08-12; finalization 2026-08-12 — all findings from
both reviews accepted and folded in, every resolution in DECISIONS.md). We
are starting Phase 5 implementation. No implementation code has been written
yet.

Git state — check it before assuming:
- `spec/hqos-svlan`: the finalized docs (8c9366f review, 28ef09c
  finalization).
- `feat/hqos-svlan-drr`: branched from it, with
  `fix/aggregate-shaper-correctness` merged as the implementation baseline
  (ae8ed7c). That branch stacks qos #4/#5/#6/#7; the human decided on
  2026-08-12 to proceed assuming they merge. Work here.
- Confirm the four PRs are still unmerged/unchanged; if any changed
  materially, rebase rather than reconciling by hand.

Start with the spec's §7 Phase 1 — DRR on the existing port tier
(cake_drr_child_t, cake_drr_local_admit, weight accounting at the three
sites, CAKE_DEQ_DRR_BLOCKED, counters, CLI weight/share display). Weights
derive from configured rate only; the explicit multiplier needs the _v2
message and lands in §7 Phase 4. This closes issue #8 on its own and is the
phase that proves the mechanism. Then §7 Phase 2, the harness, before the
S-VLAN tier.

Build constraint: this workspace has no VPP tree, so plugin C cannot be
compiled or run here. Keep the DRR core free of vlib/vnet dependencies
beyond fixed-width types so tests/drr_harness.c compiles and runs standalone
against a stub clock — that is the only local verification available. Do not
claim the plugin builds without a VPP environment to build it in.

Review-derived details that are easy to miss when skimming the spec:

1. DRR eligibility is deficit > 0 with the full adj_len subtracted and
   bounded debt carried (biased packed word for the shared child); activation
   clamps min(deficit, 0), which forgives no debt. The refill cap and addend
   must be signed — a u64 operand promotes a negative deficit and silently
   forgives the debt. §4.3-4.4.
2. The step-2 DRR check runs before cobalt_should_drop and the ECN mark in
   cake_dequeue_one; the escape is consulted only after the eligibility
   check or reserve refuses, and is written in addition form. §4.4-4.5.
3. Weight moves only on empty-detect (cake_dequeue.c:282,290) and teardown
   deactivations — never on the defensive bitmap-clear paths (pool-free
   :244, owner-mismatch :252), and never in the shared clear loop at :415
   that all of them funnel through. §4.9.
4. active_weight/n_active_children live on their own cache line (cacheline4,
   not cacheline0 — activation frequency tracks sparse traffic, not config);
   weight is validated 1-256 and quantum uses a 128-bit intermediate; the
   aggregate burst default is 10 ms; the enqueue admission gets a read-only
   overload filter in front of fetch-add-verify. §4.3, §4.7, §4.10, §8.
```
