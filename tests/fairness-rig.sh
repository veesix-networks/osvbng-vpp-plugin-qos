#!/bin/bash
# Copyright 2026 Veesix Networks Ltd
# Licensed under the GNU General Public License v3.0 or later.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Aggregate fairness rig (IMPLEMENTATION_SPEC.md section 9.2). Reproduces the
# issue #8 topology on a running VPP: N schedulers under one oversubscribed
# port aggregate, every one offered far above its own rate, and reads the
# per-subscriber bytes that actually left.
#
# Runs INSIDE the osvbng-vpp builder container, against the tree in the /work
# volume. From ../osvbng-vpp with the plugin already built by `make vpp-dev`:
#
#   docker run --rm --privileged -v osvbng-vpp-work:/work \
#     -v "$PWD/../osvbng-vpp-plugin-qos/tests/fairness-rig.sh":/rig.sh:ro \
#     osvbng-vpp-builder:v26.06 bash /rig.sh
#
#   SUB_RATES="1000 2000 4000 8000"   per-subscriber kbps, one per child
#   AGG_RATE=8000                     parent kbps
#   MEASURE_SECONDS=30
#
# The packet generator is the load source rather than veth plus an external
# sender: it saturates from inside the graph, so nothing upstream rate-limits
# the offered load and no host networking is involved.
#
# Main thread only, which is not a limitation of the rig but the condition
# issue #8 was measured under - af-packet presents a single rx queue, so every
# scheduler lands on one owner thread and the aggregate gate is contended by
# exactly the walk this arbitrates.

set -euo pipefail

B=/work/vpp/build-root/build-vpp-native/vpp
export LD_LIBRARY_PATH=$B/lib/x86_64-linux-gnu
mkdir -p /run/vpp

MEASURE_SECONDS=${MEASURE_SECONDS:-30}
AGG_RATE=${AGG_RATE:-8000}
read -r -a RATES <<< "${SUB_RATES:-5000 5000 5000 5000}"

cat > /tmp/fair-startup.conf <<EOF
unix { nodaemon log /tmp/vpp-fair.log cli-listen /run/vpp/cli.sock gid 0 }
cpu { main-core 0 }
memory { main-heap-size 1G }
buffers { buffers-per-numa 131072 }
plugins {
  path $B/lib/x86_64-linux-gnu/vpp_plugins
  plugin dpdk_plugin.so { disable }
  plugin osvbng_qos_sched_plugin.so { enable }
}
EOF

$B/bin/vpp -c /tmp/fair-startup.conf &
until $B/bin/vppctl -s /run/vpp/cli.sock show version >/dev/null 2>&1; do sleep 1; done
cli() { $B/bin/vppctl -s /run/vpp/cli.sock "$@"; }

cli create packet-generator interface pg0
cli set interface state pg0 up
cli set interface ip address pg0 10.0.0.1/24
MAC=$(cli show hardware-interfaces pg0 | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | head -1)

cli create packet-generator interface pg1
cli set interface state pg1 up

for i in "${!RATES[@]}"; do
  vlan=$((100 + i))
  cli create sub-interfaces pg1 $vlan
  cli set interface state pg1.$vlan up
  cli set interface ip address pg1.$vlan 10.$vlan.0.1/24
  # Static neighbour with a MAC nothing answers for: the rewrite succeeds and
  # the packet is shaped, without ARP resolution entering the measurement.
  cli set ip neighbor pg1.$vlan 10.$vlan.0.2 02:00:00:00:0$i:00 static
done

cli set cake aggregate pg1 rate "$AGG_RATE"
for i in "${!RATES[@]}"; do
  cli set cake scheduler pg1.$((100 + i)) rate "${RATES[$i]}" besteffort ethernet
done

echo "== topology: parent ${AGG_RATE} kbps, children ${RATES[*]} kbps"
cli show cake aggregate

for i in "${!RATES[@]}"; do
  vlan=$((100 + i))
  cli packet-generator new "name s$i limit 0 node ethernet-input source pg0 \
size 1400-1400 data { IP4: 02:00:00:00:00:02 -> $MAC \
UDP: 10.0.0.2 -> 10.$vlan.0.2 UDP: 1000 -> 2000 incrementing 1300 }"
done

cli packet-generator enable
sleep "$MEASURE_SECONDS"
cli packet-generator disable

echo
echo "== after ${MEASURE_SECONDS}s of offered overload"
cli show cake aggregate
echo
cli show errors | grep -iE 'cake' || true

echo
echo "== shares against configured rate"
cli show cake scheduler | awk -v rates="${RATES[*]}" '
  BEGIN { n = split(rates, r, " "); for (i = 1; i <= n; i++) total_rate += r[i] }
  /^  pg1\./  { split($1, a, ":"); name[++k] = a[1] }
  /dequeued:/ { for (i = 1; i <= NF; i++)
                  if ($i == "dequeued:") { b[k] = $(i + 3); total_bytes += $(i + 3) } }
  END {
    for (i = 1; i <= k; i++)
      printf "  %-10s %14d bytes  got %6.2f%%  want %6.2f%%  err %+5.2f pts\n",
             name[i], b[i], 100 * b[i] / total_bytes, 100 * r[i] / total_rate,
             100 * b[i] / total_bytes - 100 * r[i] / total_rate
  }
'
echo "== done"
