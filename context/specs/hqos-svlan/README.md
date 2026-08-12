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
| Phase 2 | Spec Refinement (Gemini) | Not started |
| Phase 3 | Spec Critique (Codex) | **Complete** (2026-08-12, adversarial; see DECISIONS.md "Phase 3") |
| Phase 4 | Spec Finalization | Not started |
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

We are at Phase 3 complete (Codex adversarial critique, 2026-08-12). Two
verified findings were folded in: the S-VLAN's shared deficit is now a packed
(round, deficit) word reserved in one CAS with a bounded refund (§4.3-§4.5,
§4.7), and backfill/detach transfer each moved member's outstanding buffer
charge under the barrier (§4.9), with PR #5 promoted to hard prerequisite
(§3). DECISIONS.md "Phase 3" records acceptances, scope corrections, and
rejected alternatives.

Next step is Phase 2 (Gemini) if run at all, else Phase 4 finalization. The
things now most worth attacking:

1. The progress argument in section 4.4. Wall-clock rounds exist to prevent a
   deadlock where every child is blocked, nothing sends, and nothing unblocks.
   Is that argument complete, and does the work-conserving escape interact
   safely with the reserve CAS — in particular, can an escape admission ever
   be refunded?
2. The refund pairing in sections 4.4-4.5. A reserve may be refunded at most
   once, only if it happened, and a refund may cross a round boundary. Is the
   bounded-inaccuracy argument airtight under all three failure orderings?
3. The charge transfer in section 4.9. It relies on cs->buffer_usage equalling
   the member's outstanding parent charge at barrier time, which in turn
   relies on PR #5 and on handed-off packets being uncharged. Does any path
   violate that equality?
4. The weight accounting in section 4.9. active_weight and n_active_children
   are maintained at three sites with refcount propagation upward. Are the
   transition points exhaustive, and is the backfill-on-create weight move
   correct under concurrent activation?
```
