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
| Phase 3 | Spec Critique (Codex) | Not started |
| Phase 4 | Spec Finalization | Not started |
| Phase 5 | Implementation | Not started |
| Phase 6 | Code Review | Not started |

## Blocking prerequisites

Implementation cannot start until both merge. Session interfaces are registered
standalone, so `sup_sw_if_index == sw_if_index` and the attachment walk
terminates at the session — no scheduler reaches an S-VLAN or a port.

- [`osvbng-vpp-plugin-ipoe` #7](https://github.com/veesix-networks/osvbng-vpp-plugin-ipoe/pull/7)
- [`osvbng-vpp-plugin-pppoe-control` #3](https://github.com/veesix-networks/osvbng-vpp-plugin-pppoe-control/pull/3)

Strongly preferred first, since the aggregate path cannot execute correctly
without them: qos
[#4](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/4) (gate
livelock), [#5](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/5)
(buffer accounting), [#6](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/pull/6)
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
| `src/cake_dequeue.c:239` | the bitmap walk whose stable order causes #8 |
| `src/cake_dequeue.c:287` | owner-thread check — why per-child DRR state is uncontended |
| `src/cake_dequeue.c:444` | deactivation site — weight accounting |
| `src/cake_enqueue.c:347` | activation site — weight accounting |
| `src/osvbng_qos_sched.c:237` | `sup_sw_if_index` attachment walk |
| `src/osvbng_qos_sched.c:305` | teardown deactivation — weight accounting |

## Prompt to Resume

```
Read context/PROCESS.md, then context/specs/hqos-svlan/README.md,
IMPLEMENTATION_SPEC.md and DECISIONS.md.

We are at Phase 1 complete. The spec adds an S-VLAN aggregate tier and weighted
DRR across the children of any aggregate, reversing the "Weighted DRR in Phase 1"
rejection recorded in ../hqos-qinq/DECISIONS.md on the strength of measured
starvation (issue #8).

Next step is Phase 2 or 3 review. The three things most worth attacking:

1. The progress argument in section 4.4. Wall-clock rounds exist to prevent a
   deadlock where every child is blocked, nothing sends, and nothing unblocks.
   Is that argument complete, and does the work-conserving escape interact with
   it safely?
2. The weight accounting in section 4.9. active_weight and n_active_children are
   maintained at three sites with refcount propagation upward. A leak in either
   direction is silent. Are the transition points exhaustive, and is the
   backfill-on-create weight move correct under concurrent activation?
3. The cache-line placement in section 4.7. An S-VLAN's own deficit is a new
   shared per-packet write. Does cacheline3 hold, and is the read-mostly group
   still read-mostly?
```
