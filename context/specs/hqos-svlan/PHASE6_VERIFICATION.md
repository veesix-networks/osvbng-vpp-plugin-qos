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

## §9.4 containerlab (2026-08-13, external lab host)

Suites 18 and 19 run green on a fresh x86_64 containerlab host with the
image built from `feat/hqos-svlan-control-plane`: the refreshed qos, ipoe
and pppoe plugins (the latter two from their `fix/session-sup-sw-if-index`
branches) plus a patched `af_packet_plugin.so`. Suite 19 passing is the
first live validation of the whole new chain: a QinQ 802.1ad IPoE session,
AAA service-group resolution, and a CAKE scheduler applied to a *session*
interface through the `_v2` weighted message, resolving its attachment
through the new session parentage. The `osvbng_cake_capabilities`
handshake was also observed live (`svlan_tier=true weighted_drr=true`).

Getting there surfaced four defects, none in this spec's code:

1. **bngtester vs suite configs** — current `bngtester:alpine-latest`
   builds its QinQ stack with an 802.1ad outer tag while all 85 suite
   configs pinned `vlan-tpid: dot1q`; CI stays green only because its
   runners cache a stale image. Suites 18/19 flipped to dot1ad on the
   branch.
2. **VPP af_packet RX discards the kernel-reported VLAN TPID** and
   reconstructs stripped tags as 0x8100, so dot1ad sub-interfaces can
   never match behind af_packet (on veth the kernel strips the outer tag
   into metadata unconditionally). Fixed by
   `osvbng-vpp/patches/0001-af-packet-honour-rx-vlan-tpid.patch`,
   upstream-first; the patched plugin ships via `test-infra/`.
3. **Suite 19 bound its service group to a dead config key** (`service-group`
   on the aaa policy, which `AAAPolicy` has no field for), so sessions had
   no service group and the scheduler block was never reached. Rebound via
   `default-service-group` on the subscriber-group; suites 25/30 carry the
   same dead key upstream.
4. The osvbng image lacks `ethtool`/`tcpdump`, which made the wire-level
   debugging above needlessly indirect.

**Aggregates under sessions (2026-08-13): suite 51-ipoe-hqos-svlan, 7/7.**
Six bngblaster IPoE sessions across three S-VLANs under a shaped port,
aggregates programmed from the `qos-aggregates` conf schema at startup,
downstream streams saturating the hierarchy. Every arbitration
relationship measured at once — oversubscribed port (6000+3000+3000
asking 8000), unequal and equal S-VLAN pairs, unequal (1:4) and equal
subscribers:

| tier | worst error |
|---|---|
| S-VLAN split (50/25/25) | 0.05 pts |
| subscriber split (10/40/12.5×4) | 0.20 pts |
| port rate | 8003 of 8000 kbps |

Same accuracy class as the §9.2 pg rig, now through real sessions. Its
first deploys also caught two control-plane defects in this branch's conf
handler (nil dataplane-state deref at startup load; a per-name dependency
declaration the resolver cannot express), both fixed on the branch.

Still unrun: everything multi-worker.

### Open, not ours: VPP crashes under PPPoE session churn

Suites 51 and 52 gained a churn phase (bngblaster's monkey) to exercise
scheduler create/teardown repeatedly. Suite 52 crashes the dataplane:

```
received signal SIGSEGV, PC ..., faulting address ...
#0  interface_drop_fn_x86_64_v3 + 0xa18   from libvnet.so.26.06
```

Reproducible - three of three PPPoE churn runs. What the evidence rules
in and out:

| | result |
|---|---|
| PPPoE churn, full HQoS config | crashed 3/3 |
| **PPPoE churn, all QoS stripped** (`config/bng1/osvbng-noqos.yaml`) | **crashed** - no aggregates, no schedulers, port shaped 0 bytes |
| **PPPoE churn, no QoS and no 802.1ad** (`osvbng-noqos-dot1q.yaml` + `config-dot1q.json`) | **crashed** |
| IPoE churn (suite 51) | no crash in 3 runs |
| Sub-interface churn under saturation, no control plane (`tests/churn-rig.sh`) | no crash in 150 deletions |

Isolated to an unmerged dependency. The no-QoS run removes this spec's
code - with nothing programmed the enqueue feature is not enabled on any
interface, so the plugin never sees a packet - and the dot1q run removes
802.1ad and with it any effect of the af_packet TPID patch. What remained
was the PPPoE plugin itself, and swapping only that `.so` settles it:

| PPPoE plugin | runs | result |
|---|---|---|
| `fix/session-sup-sw-if-index` (pppoe-control #3) | 4 | SIGSEGV every time |
| shipped | 1 | no crash, churn completed, teardown clean |

**The crash belongs to the session parentage PR this spec depends on**, not
to shipped osvbng and not to the QoS plugin. That branch points a session
interface's `sup_sw_if_index` at its encap sub-interface, on an interface
created by `vnet_register_interface()` and therefore of type
`VNET_SW_INTERFACE_TYPE_HARDWARE`, where VPP's convention is
`sup_sw_if_index == sw_if_index`; `vnet_get_sup_sw_interface()` only
follows the field for SUB, P2P and PIPE. The PPPoE plugin also recycles
session interfaces through a free list rather than deleting them, so a
reused interface is re-parented over whatever the previous parenting left.
The IPoE equivalent (ipoe #7) does not crash under the same churn and does
not recycle interfaces the same way.

Consequence for this spec: attaching a scheduler to an S-VLAN through a
*session* interface depends on those PRs, so pppoe-control #3 has to be
fixed before HQoS over PPPoE sessions can ship. Nothing here needs to
change. Filed upstream as veesix-networks/osvbng#419.

**Secondary observation for whoever fixes it.** `osvbng_pppoe.c` creates each session's
midchain adjacency (`adj_nbr_midchain_update_rewrite`,
`adj_nbr_midchain_stack`, around :234-265) but the delete path (:490-520)
never unstacks or releases it: the session interface is not deleted at all,
it is set down, hidden, unparented and parked on
`free_pppoe_session_hw_if_indices` for reuse, then the FIB path is removed
and the pool entry freed. An adjacency left stacked on a DPO that is being
torn down, with traffic still following it, is the standard way a buffer
acquires garbage metadata - and `interface_drop_fn` crashes indexing a
per-interface counter by the buffer's sw_if_index, which is what a garbage
buffer produces. A debug VPP build with assertions would confirm it at the
point of corruption rather than three nodes later.

That the interface is recycled rather than deleted also explains why the
dataplane's interface-delete hook never cleaned up PPPoE schedulers, which
is what the teardown check caught before the IfIndex fix.

Consequence for this spec: suite 52 cannot pass until that is fixed.

### Open, not ours: IPoE sessions do not always come back from an ungraceful kill

Suite 51 does not crash, but it fails about half its churn runs with three
to five of six sessions re-established, at random. The QoS side stays
correct throughout - the surviving subscribers' shares are right for the
set of sessions that exist - so this is session re-establishment, not
shaping.

`max_concurrent_sessions` is enforced per (MAC, S-VLAN, C-VLAN) against a
cached counter (`internal/ipoe/session.go:172`). The monkey's "restart
without termination" method kills a DHCP session with no RELEASE, so
nothing decrements that counter, and the reconnect from the same MAC on the
same VLAN is refused until the old session expires - which at a 3600 s
lease is well past the end of the run. Sessions killed gracefully come
back; ungraceful ones do not, which is exactly the random split observed.
Confirm on a failing run with:

    docker logs <bng> 2>&1 | grep -ai "session limit"

Both churn suites now leave the limit unset, because a suite that
reconnects the same MAC as fast as a monkey can kill it is testing the AAA
limiter rather than HQoS. The product question it raises is worth its own
issue and is not this spec's to answer: an ungraceful CPE reboot - a power
cut, a crash, no RELEASE sent - is the ordinary case in the field, and a
BNG that refuses the returning subscriber until lease expiry locks them out
for up to an hour.

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
