#!/bin/bash
# Copyright 2026 Veesix Networks Ltd
# Licensed under the GNU General Public License v3.0 or later.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Clocks/Packet phase pair (IMPLEMENTATION_SPEC.md section 9.3): identical
# offered load through a port-only aggregate and through port + S-VLAN tier,
# per-node cycle readout for the CAKE enqueue and dequeue nodes. This turns
# section 10's RMW pricing (3 on 2 hot lines -> 7 on 5) into cycles.
#
# Runs INSIDE the osvbng-vpp builder container, against the tree in the /work
# volume. From ../osvbng-vpp with the plugin already built by `make vpp-dev`:
#
#   docker run --rm --privileged -v osvbng-vpp-work:/work \
#     -v "$PWD/../osvbng-vpp-plugin-qos/tests/perf-rig.sh":/rig.sh:ro \
#     osvbng-vpp-builder:v26.06 bash /rig.sh
#
# Validity boundary is perf/README.md's in osvbng-vpp: same-box, relative,
# containerized, main thread only. cake-dequeue is an always-polling INPUT
# node whose clocks/packet amortizes empty polls over emitted packets, so
# compare it only within the same load profile, never across profiles.

set -euo pipefail

B=/work/vpp/build-root/build-vpp-native/vpp
export LD_LIBRARY_PATH=$B/lib/x86_64-linux-gnu
mkdir -p /run/vpp

MEASURE_SECONDS=${MEASURE_SECONDS:-10}
N_SUBS=8
NODES="ip4-cake-enqueue cake-dequeue ip4-input ip4-lookup ip4-rewrite"

start_vpp() {
  cat > /tmp/perf-startup.conf <<EOF
unix { nodaemon log /tmp/vpp-perf.log cli-listen /run/vpp/cli.sock gid 0 }
cpu { main-core 0 }
memory { main-heap-size 1G }
buffers { buffers-per-numa 131072 }
plugins {
  path $B/lib/x86_64-linux-gnu/vpp_plugins
  plugin dpdk_plugin.so { disable }
  plugin osvbng_qos_sched_plugin.so { enable }
}
EOF
  $B/bin/vpp -c /tmp/perf-startup.conf &
  VPP_PID=$!
  until $B/bin/vppctl -s /run/vpp/cli.sock show version >/dev/null 2>&1; do sleep 1; done
}

stop_vpp() {
  kill "$VPP_PID" 2>/dev/null || true
  wait "$VPP_PID" 2>/dev/null || true
}

cli() { $B/bin/vppctl -s /run/vpp/cli.sock "$@"; }

# Same must-succeed wrapper as fairness-rig.sh: vppctl exits 0 on CLI errors,
# and a phase must never measure a different topology than it printed.
cfg() {
  local out
  out=$(cli "$@" 2>&1)
  if printf '%s' "$out" | grep -qiE 'returned -?[0-9]+|unknown input|required|must be|invalid|no such|failed|in use'; then
    echo "rig: configuration failed: '$*' -> $out" >&2
    exit 1
  fi
}

# topology <port-kbps> <sub-kbps> <svlan-kbps|0>: pg0 feeds 8 subscribers on
# pg1.100-103 and pg1.200-203; svlan-kbps=0 skips the S-VLAN tier.
topology() {
  local port_kbps=$1 sub_kbps=$2 svlan_kbps=$3

  cfg create packet-generator interface pg0
  cfg set interface state pg0 up
  cfg set interface ip address pg0 10.0.0.1/24
  MAC=$(cli show hardware-interfaces pg0 | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | head -1)

  cfg create packet-generator interface pg1
  cfg set interface state pg1 up

  TAGS="100 101 102 103 200 201 202 203"
  local idx=0
  for vlan in $TAGS; do
    cfg create sub-interfaces pg1 "$vlan"
    cfg set interface state pg1."$vlan" up
    cfg set interface ip address pg1."$vlan" 10."$vlan".0.1/24
    cfg set ip neighbor pg1."$vlan" 10."$vlan".0.2 02:00:00:00:00:$(printf %02x $((idx + 16))) static
    idx=$((idx + 1))
  done

  cfg set cake aggregate pg1 rate "$port_kbps"
  if [ "$svlan_kbps" != 0 ]; then
    cfg set cake aggregate pg1 svlan 100-103 rate "$svlan_kbps"
    cfg set cake aggregate pg1 svlan 200-203 rate "$svlan_kbps"
  fi
  for vlan in $TAGS; do
    cfg set cake scheduler pg1."$vlan" rate "$sub_kbps" besteffort ethernet
  done
}

offer() { # <pps-per-stream>
  local idx=0
  for vlan in $TAGS; do
    cfg packet-generator new "name s$idx limit 0 rate $1 node ethernet-input source pg0 \
size 1400-1400 data { IP4: 02:00:00:00:00:02 -> $MAC \
UDP: 10.0.0.2 -> 10.$vlan.0.2 UDP: 1000 -> 2000 incrementing 1358 }"
    idx=$((idx + 1))
  done
  cfg packet-generator enable
}

stop_offer() {
  cfg packet-generator disable
  local idx=0
  for vlan in $TAGS; do
    cfg packet-generator delete "s$idx"
    idx=$((idx + 1))
  done
}

sample() { # <label>
  # Warm up before clearing so pool allocation, neighbour resolution and the
  # first-packet flow setup are not in the window.
  sleep 2
  cli clear runtime
  cli clear errors
  sleep "$MEASURE_SECONDS"
  echo "--- $1"
  printf '%-18s %14s %14s %12s\n' node clocks/pkt vectors/call pkts
  for node in $NODES; do
    cli show runtime | awk -v n="$node" \
      '$1==n {printf "%-18s %14s %14s %12s\n", $1, $6, $7, $4}'
  done
  cli show errors | grep -iE 'cake' | sed 's/^/    /' || true
}

phase() { # <name> <port-kbps> <sub-kbps> <svlan-kbps|0> <pps>
  echo
  echo "== phase: $1"
  start_vpp
  topology "$2" "$3" "$4"
  cli show cake aggregate | sed 's/^/    /'

  offer "$5"
  sample "${5}pps x $N_SUBS streams, 1400B"
  stop_offer

  stop_vpp
  # The cli socket lingers briefly; make sure the next phase gets a fresh one.
  rm -f /run/vpp/cli.sock
  sleep 1
}

echo "== hqos-svlan section 9.3 Clocks/Packet phase pair"
start_vpp
echo "== $(cli show version | head -1)"
stop_vpp
rm -f /run/vpp/cli.sock
sleep 1
echo "== window ${MEASURE_SECONDS}s per sample, containerized main thread:"
echo "== same-box relative numbers only (see perf/README.md boundary)."

# Uncontended pair: 268 Mbit offered against a 500 Mbit port, 50 Mbit
# subscribers, 250 Mbit S-VLANs - every packet admitted on its first gate
# attempt, so the delta is the pure admission cost of the extra tier.
phase "port-only, uncontended"    500000 50000 0      3000
phase "port + svlan, uncontended" 500000 50000 250000 3000

# Hot pair: offered just under every rate (717 Mbit against an 800 Mbit
# port). The dequeue node is busy enough for vectors/call to mean something,
# so the dequeue-side delta is read here, not from the paced pair.
phase "port-only, hot"           800000 100000 0      8000
phase "port + svlan, hot"        800000 100000 400000 8000

# Contended pair: fairness-rig rates, identical offered load, everything
# overloaded. Prices the gate-refused retry and overflow paths too.
phase "port-only, contended"     8000 4000 0    3000
phase "port + svlan, contended"  8000 4000 6000 3000

echo "== done"
