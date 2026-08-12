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
| Phase 5 | Implementation | Not started |
| Phase 6 | Code Review | Not started |

## Blocking prerequisites

Implementation cannot start until both merge. Session interfaces are registered
standalone, so `sup_sw_if_index == sw_if_index` and the attachment walk
terminates at the session — no scheduler reaches an S-VLAN or a port.

- [`osvbng-vpp-plugin-ipoe` #7](https://github.com/veesix-networks/osvbng-vpp-plugin-ipoe/pull/7)
- [`osvbng-vpp-plugin-pppoe-control` #3](https://github.com/veesix-networks/osvbng-vpp-plugin-pppoe-control/pull/3)

[#5](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/5)
(buffer accounting) is a **hard prerequisite**: the §4.9 charge transfer and
the §9.1 accounting invariants are exact only under #5's
`cs->buffer_usage`-equals-outstanding-charge invariant (see spec §3).
Strongly preferred first, since the aggregate path cannot execute correctly
without them: qos
[#4](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/4) (gate
livelock), [#6](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/6)
(rate precision), [#7](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/7)
(interface teardown).

This spec does **not** depend on
[#9](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/9) (walk
rotation). DRR makes walk order irrelevant on its own; if #9 merges the two are
compatible and #9 becomes redundant.

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

| Path | Relevance |
|---|---|
| `src/osvbng_qos_sched.h:155` | `cake_aggregate_t` — cache-line layout to preserve |
| `src/osvbng_qos_sched.h:175` | `cake_sched_t` — gains one `cake_drr_child_t` |
| `src/osvbng_qos_sched.h:218` | `cake_per_thread_t` |
| `src/cake_dequeue.c:227` | the bitmap walk whose stable order causes #8 |
| `src/cake_dequeue.c:240` | owner-thread check — why scheduler-side DRR state is uncontended |
| `src/cake_dequeue.c:267,393` | deactivation sites (empty detect, bitmap clear) — weight accounting |
| `src/osvbng_qos_sched.h:691` | `cake_agg_discharge` — resolves via *current* attachment; why reparenting must transfer charges |
| `src/cake_enqueue.c:347` | activation site — weight accounting |
| `src/osvbng_qos_sched.c:237` | `sup_sw_if_index` attachment walk |
| `src/osvbng_qos_sched.c:305` | teardown deactivation — weight accounting |

## Prompt to Resume

```
Read context/PROCESS.md, then context/specs/hqos-svlan/README.md,
IMPLEMENTATION_SPEC.md and DECISIONS.md.

Phases 1-4 are complete (Codex adversarial critique and Claude Fable 5 deep
review both 2026-08-12; finalization 2026-08-12 — all findings from both
reviews accepted and folded in, every resolution recorded in DECISIONS.md).

Next step is Phase 5 implementation, in the spec's §7 order (its Phase 1: DRR
on the existing port tier, which closes issue #8 on its own). Blocked until
the prerequisites merge: osvbng-vpp-plugin-ipoe #7 and
osvbng-vpp-plugin-pppoe-control #3 (session parentage), qos #5 (buffer
accounting — hard prerequisite), with qos #4, #6 and #7 strongly preferred
first. Verify prerequisite status before starting.

Review-derived details that are easy to miss when skimming the spec:

1. DRR eligibility is deficit > 0 with the full adj_len subtracted and
   bounded debt carried (biased packed word for the shared child); activation
   clamps min(deficit, 0). §4.3-4.4.
2. The step-2 DRR check runs before cobalt_should_drop and the ECN mark in
   cake_dequeue_one; the escape is consulted only after the eligibility
   check or reserve refuses, in addition form. §4.4-4.5.
3. Weight moves only on empty-detect and teardown deactivations — never on
   the defensive bitmap-clear paths (pool_is_free, owner-mismatch). §4.9.
4. active_weight/n_active_children live on their own cache line (cacheline4);
   weight is validated 1-256 and quantum uses a 128-bit intermediate; the
   aggregate burst default is 10 ms; the enqueue admission has a read-only
   overload filter in front of fetch-add-verify. §4.3, §4.7, §4.10, §8.
```
