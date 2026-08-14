> **Moved.** This plugin now lives in the [osvbng-vpp](https://github.com/veesix-networks/osvbng-vpp) monorepo at `plugins/osvbng_qos_sched`, where all osvbng VPP plugins are developed and versioned together. This repository is archived read-only; its history, pull requests and reviews remain as the development record. Report issues and send changes to osvbng-vpp.

# osvbng-vpp-plugin-qos

<p align="center">
  <a href="https://github.com/veesix-networks/osvbng-vpp-plugin-qos/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg?style=for-the-badge" alt="GPL-3.0 License"></a>
  <a href="https://dsc.gg/osvbng"><img src="https://img.shields.io/discord/1483536004337107017?label=Discord&logo=discord&logoColor=white&color=5865F2&style=for-the-badge" alt="Discord"></a>
</p>

CAKE-equivalent per-subscriber traffic scheduler for [VPP](https://fd.io/vpp). Part of the [osvbng](https://github.com/veesix-networks/osvbng) open-source BNG platform.

> **This project is fully LLM-driven.** All code, specs, and documentation are produced by AI agents following a [structured workflow](context/PROCESS.md). Humans file issues and approve results — agents do the rest. See [Contributing](#contributing) to get involved.

> **While this plugin is currently developed in the context of osvbng, it is designed as a generic VPP plugin with no osvbng-specific dependencies in the dataplane.** Any VPP deployment can use it. We are completely open to restructuring this as a standalone, vendor-neutral VPP CAKE implementation and separately building the osvbng-specific integration (Go bindings, subscriber lifecycle, config schema) as a thin layer on top. If you're interested in using this outside of osvbng — whether in your own BNG, a router, or any VPP-based platform — please open an issue and we'll work with you to make that happen.

## What is this?

A VPP plugin that replaces per-subscriber egress policers with intelligent traffic scheduling — eliminating bufferbloat while maintaining per-flow fairness within each subscriber's rate allocation. Dual-stack (IPv4 + IPv6). Operates in the **egress direction only** (traffic from the internet toward the subscriber), which is where the BNG controls the bottleneck. Ingress (subscriber upload) continues to use standard policers.

Traditional BNG QoS uses policers (drop excess packets at the metering point). This works for rate enforcement but causes 200-500ms of added latency under load because downstream buffers (DSLAM, OLT, CPE) absorb permitted bursts. A household streaming 4K Netflix while someone games experiences terrible interactive performance.

This plugin implements CAKE's algorithms inside VPP's vector processing pipeline:

- **Per-flow queuing** — each TCP/UDP flow gets its own queue. One greedy flow can't starve others.
- **COBALT AQM** — CoDel + BLUE active queue management. Signals congestion early via ECN marks or targeted drops, keeping queue delay near 5ms instead of 500ms.
- **DiffServ tins** — priority traffic classes (Voice > Video > Best Effort > Bulk). VoIP stays at <1ms latency even under full load.
- **Token-bucket shaping** — smooth pacing at the exact provisioned rate, preventing downstream buffer buildup.
- **Overhead compensation** — accurate shaping for DSL (ATM cell rounding), VDSL2 (PTM), GPON (GEM), and DOCSIS subscribers.
- **Triple isolation** — per-flow AND per-host fairness. Multiple devices behind CPE NAT each get their fair share.

The BNG is the single most impactful place to deploy this in an ISP network. It's the last device before the subscriber's constrained access link, it knows the exact provisioned rate and access technology, and it works for every subscriber regardless of CPE capability.

## Features

| Feature | Status |
|---------|--------|
| Token-bucket shaper (egress pacing) | Skeleton |
| Per-flow queuing (set-associative hashing) | Skeleton |
| COBALT AQM (CoDel + BLUE) | Skeleton |
| DiffServ tins (besteffort, diffserv3/4/8) | Skeleton |
| Overhead compensation (ATM/PTM/GPON) | Skeleton |
| Dual-stack (IPv4 + IPv6) | Skeleton |
| Triple isolation (per-host fairness) | Not started |
| HQoS aggregate shaping (QinQ S-VLAN) | Spec draft |
| ACK filtering | Not started |
| GSO segment splitting | Not started |
| **Per-subscriber operational metrics** | **Day 1 priority** |

## Operational Visibility

A common problem with vendor BNG platforms is poor operational metrics — operators have limited insight into what the QoS system is actually doing per subscriber. This plugin treats observability as a first-class requirement, not an afterthought. The following metrics are available from day 1 via both VPP CLI (`show cake scheduler`) and binary API (`osvbng_cake_sched_dump`):

**Per-subscriber:**
- Current buffer usage vs limit (bytes queued)
- Configured rate and active tin mode

**Per-tin (traffic class) per-subscriber:**
- Packets and bytes forwarded
- Packets dropped by AQM (COBALT) and by buffer overflow
- Packets ECN CE marked (congestion signals sent instead of drops)
- Peak and average queue delay (microseconds) — the most important metric for diagnosing bufferbloat
- Active flow counts by state (sparse vs bulk) — shows whether per-flow fairness is working

These metrics feed directly into the osvbng Go control plane for Prometheus export, `show` commands, and operator dashboards. An operator should be able to answer "why is subscriber X's latency high?" by querying scheduler stats — not by guessing.

## Architecture

```
  ip4-lookup → ip4-output feature arc
                    │
            ┌───────▼──────────────┐
            │  ip4-cake-enqueue    │  ip4-output / ip6-output feature node
            │  (ip6-cake-enqueue)  │  classifies → tins → flows → queues
            └───────┬──────────────┘
                    │
            ┌───────▼──────────────┐
            │    cake-dequeue      │  INPUT polling node
            │                      │  shaper → DRR → COBALT → output
            └───────┬──────────────┘
                    │
            ┌───────▼──────────────┐
            │  ip4/ip6-rewrite     │  resume normal output path
            │  → tunnel-output     │  (midchain/tunnel interfaces)
            │  → interface-output  │  (regular interfaces)
            └───────┬──────────────┘
                    │
                 TX driver
```

**ip4/ip6-cake-enqueue** hooks into VPP's `ip4-output` and `ip6-output` feature arcs. These arcs run before `ip4-rewrite`/`ip4-midchain`, keyed on `tx_sw_if_index` — so they work on every interface type: physical interfaces, VLAN sub-interfaces, and midchain/tunnel interfaces (IPoE sessions, PPPoE sessions). Non-scheduled interfaces pass through untouched.

**cake-dequeue** is a `VLIB_NODE_TYPE_INPUT` polling node that runs every main loop iteration. It checks active schedulers, drains queues at the shaped rate using deficit round robin, applies COBALT AQM decisions, and re-injects packets into the correct ip4/ip6-output feature arc to resume the normal output path.

## Building

The plugin builds as part of VPP's external plugin system:

```bash
# From VPP build directory
cmake -DVPP_EXTRA_PLUGINS="path/to/osvbng-vpp-plugin-qos" ..
make -j$(nproc)
```

Or symlink into VPP's plugin directory:

```bash
ln -s /path/to/osvbng-vpp-plugin-qos /path/to/vpp/src/plugins/osvbng_qos_sched
cd /path/to/vpp && make build-release
```

## Usage

### CLI

```
# Enable scheduler on a subscriber interface
set cake scheduler ipoe_session42 rate 100000 tin-mode diffserv4 overhead 27 ptm

# Show scheduler state
show cake scheduler
show cake scheduler ipoe_session42

# Disable
set cake scheduler ipoe_session42 disable
```

### Binary API

```
osvbng_cake_sched_enable_disable  — enable/disable per-subscriber scheduler
osvbng_cake_sched_dump            — query scheduler state and statistics
osvbng_cake_sched_reset_stats     — reset per-subscriber counters
```

The osvbng Go control plane calls these APIs during subscriber session activation/release.

## Hierarchical QoS (HQoS) for QinQ Deployments

In QinQ (802.1ad) access networks, the S-VLAN (outer VLAN) represents an aggregate link to downstream equipment (OLT, DSLAM, aggregation switch) with a finite physical capacity, while C-VLANs (inner VLANs) represent individual subscribers. Per-subscriber CAKE shaping alone is not sufficient in these deployments: without an aggregate shaper on the S-VLAN, multiple subscribers can collectively exceed the aggregate link capacity, causing uncontrolled tail-drop at the downstream equipment and negating the AQM benefits of per-subscriber scheduling.

This plugin supports two-level hierarchical scheduling:

1. **Leaf level:** per-subscriber CAKE scheduler (C-VLAN shaping, per-flow fairness, COBALT AQM)
2. **Parent level:** per-S-VLAN aggregate shaper with DRR across child schedulers for fair bandwidth distribution

When the aggregate link is not congested, per-subscriber shaping works as normal with minimal overhead. When the aggregate is saturated, DRR ensures each subscriber gets a fair share of the available capacity rather than relying on random tail-drop at the downstream device.

See [`context/specs/hqos-qinq/`](context/specs/hqos-qinq/) for the full technical specification.

### Deployment Considerations

**Multiple S-VLANs on a single physical interface:** Each S-VLAN gets its own independent aggregate shaper. If a 10G physical port carries S-VLAN 100 (aggregate 2G) and S-VLAN 200 (aggregate 2G), these are two separate aggregates with independent token buckets and DRR child lists. The operator is responsible for ensuring the sum of S-VLAN aggregate rates does not exceed the physical port capacity. The plugin does not enforce port-level aggregate shaping -- this is a deliberate design choice to keep the hierarchy at two levels. In practice, S-VLAN aggregate rates are derived from the known capacity of the downstream equipment (e.g. a 1G GPON OLT port, a 10G XGS-PON port) and the operator provisions them accordingly.

**Bond/LAG interfaces:** When S-VLANs sit on a bond interface (e.g. 2x10G LACP), VPP's bond driver presents a single `sw_if_index` and handles member selection internally. The aggregate rate for S-VLANs on a bond should reflect the bond's total capacity (20G for 2x10G), not a single member. Note that LACP hashing distributes flows across member links, so individual flow throughput may be bounded by a single member's capacity even when the bond has spare aggregate bandwidth. This is a property of the bond, not the scheduler.

**Single-VLAN and non-QinQ deployments:** HQoS is entirely optional. Subscribers without an aggregate parent behave identically to the flat per-subscriber scheduling model. There is zero overhead for deployments that do not use aggregates -- the aggregate dequeue loop does not execute when no aggregates are configured.

## Specification

The full technical specification lives in this repo's `context/` directory:

- [`context/specs/full-qos/`](context/specs/full-qos/) — Full QoS pipeline spec (policers, DSCP marking, dynamic rates, scheduling)
- [`context/specs/cake-scheduler/`](context/specs/cake-scheduler/) — CAKE algorithm deep dive (VPP plugin architecture, data structures, implementation phases)
- [`context/specs/hqos-qinq/`](context/specs/hqos-qinq/) — Hierarchical QoS for QinQ deployments (per-S-VLAN aggregate shaping)

## Status

Early development. The plugin skeleton compiles and registers with VPP. The enqueue and dequeue nodes are scaffolded with the core pipeline flow but have TODO markers for production-ready implementations of:

- Dual-loop vectorized enqueue (currently scalar)
- Full COBALT AQM state machine
- Proper DRR list management and flow state transitions
- Buffer reference counting for held packets
- Cross-thread frame queue handoff
- SIMD flow hashing (AVX2/AVX-512 8-way tag comparison)

See the implementation spec for the 8-phase development plan.

## Acknowledgments

The algorithms in this plugin are derived from the Linux CAKE qdisc ([`sch_cake.c`](https://github.com/dtaht/sch_cake)). The following people are the original authors of the CAKE qdisc — not this plugin, which is a separate clean-room reimplementation:

- **Dave Taht** — Co-creator of CAKE, co-founder of the Bufferbloat Project. Dave spent over a decade pushing the networking world to take latency seriously, driving FQ-CoDel and CAKE into the Linux kernel. He passed away in 2024. It is our duty to carry Dave's name forward — every packet this scheduler paces, every millisecond of latency it eliminates, is built on the foundation he laid.
- **Jonathan Morton** — Primary author of the CAKE algorithm. Designed set-associative hashing, DiffServ tins, COBALT AQM, triple isolation, and overhead compensation.
- **Toke Hoiland-Jorgensen** — Key contributor to CAKE and FQ-CoDel. Instrumental in getting CAKE merged into Linux mainline.
- **Sebastian Moeller** — Contributor. Deep work on overhead compensation and DSL framing models.
- **Kevin Darbyshire-Bryant** — Contributor to the CAKE implementation and testing.
- **Ryan Mounce** — Contributor to CAKE development and testing.

This is a clean-room reimplementation adapted for VPP's vector processing architecture. No code is copied from `sch_cake.c`.

> **Note:** This project is primarily LLM-driven. We make every effort to ensure all references and attributions are correct and complete, but if we have missed or misattributed anyone's contribution, please [open an issue](https://github.com/veesix-networks/osvbng-vpp-plugin-qos/issues) or submit a pull request and we will correct it immediately.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Copyright 2026 Veesix Networks Ltd.
