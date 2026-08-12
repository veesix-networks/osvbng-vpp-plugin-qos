# Phase 6 verification: hqos-svlan

Post-implementation review and the §9.3 benchmark gate. 2026-08-12.

## §9.3 Clocks/Packet phase pair

Run via `tests/perf-rig.sh` in the osvbng-vpp builder container
(release build, VPP v26.06, single main thread, Rosetta-emulated x86_64 —
same-box relative numbers only, per `perf/README.md`'s validity boundary).
The spec asked for a `tests/benchmark.py` phase pair; that harness needs
containerlab, which this workspace does not have, so the rig reuses the
§9.2 fairness topology instead: 8 subscribers on pg1.100-103 / pg1.200-203,
identical pg offered load, with and without two S-VLAN aggregates between
them and the port. 10 s windows, 1400 B frames.

| pair | ip4-cake-enqueue clk/pkt | cake-dequeue clk/pkt | dequeue vectors/call |
|---|---|---|---|
| uncontended (offered ≪ every rate) | 106 → 108 (**+2**) | 3200 → 3860 † | .01 |
| hot (offered ≈ 90% of every rate) | 64.3 → 65.3 (**+1.0, +1.6%**) | 3210 → 3380 (**+170, +5.3%**) | .04 |
| contended (offered ≫ port) | 91.1 → 97.4 (**+6.3, +6.9%**) | 3.96e5 → 4.48e5 † | 0.00 |

† `cake-dequeue` is an always-polling INPUT node; at vectors/call ≈ .01 its
clocks/pkt amortizes empty polls over few emitted packets, and the deferral
counts differ between phases (9.0M vs 14.7M uncontended), so those two
cells are not clean per-packet costs. The **hot** row, where the node is
actually busy and deferral counts are within 7% of each other (74.2M vs
79.5M), is the honest dequeue-side number.

Reading against §10's prediction (3 RMWs on 2 hot lines → 7 on 5): the
enqueue side gains the S-VLAN buffer fetch_add/discharge pair for +1 to +6
clocks/pkt depending on load; the dequeue side gains the tier reserve CAS
and port gate CAS for +170 clocks/pkt (+5.3%) at the hot point, which also
includes the extra deferred-dispatch polling the second arbitration level
causes. Single-threaded container, so none of this prices cross-core
cache-line bouncing — that is what §10 sizes the RMW count against, and it
remains hardware-session work.

Contended-pair drop accounting (identical goodput, 7157 vs 7154 pkts):
total drops 236k in both phases, split 205k overflow / 31k AQM port-only
versus 55k / 181k with the tier. The relocation is analysed in
`code-reviews/CLAUDE.md` CL-2.

Gate verdict: **pass**. The tier's per-packet cost is single-digit percent
at every load point, no memory growth observed across phases, and the
number the spec demanded exists. The §10 batching debt (Requirement #2) is
unchanged and remains its own issue.

## Review inventory

| artifact | lens | reviewer | findings |
|---|---|---|---|
| `code-reviews/CLAUDE.md` | bug hunt | Claude Opus 5 | 1 high, 4 medium, 4 low, 7 checked-clean |
| `code-reviews/CODEX.md` | spec compliance | Codex | 2 high, 1 medium |
| `code-reviews/CODEX-PROTOCOL.md` | protocol conformance (Gemini slot, run on Codex at user direction) | Codex | 1 high, 1 medium |

Cross-reviewer agreement, before triage: both independent passes found the
stale-round-tag wedge (CL-1 / CODEX-PROTOCOL #1) and the invalid-level
aliasing + port-rate-lowering pair (CL-5, CL-6 / CODEX #1, #2). Accept/
reject rationale and the fixes are recorded in `DECISIONS.md` under
"Phase 6 triage".
