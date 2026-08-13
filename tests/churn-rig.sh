#!/bin/bash
# Copyright 2026 Veesix Networks Ltd
# Licensed under the GNU General Public License v3.0 or later.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Interface churn under load, to reproduce the SIGSEGV the containerlab HQoS
# suites hit during session churn (interface_drop_fn, VPP v26.06).
#
# The lab churns session interfaces created by the ipoe/pppoe plugins; this
# churns VLAN sub-interfaces instead, which is the same ingredient list
# without a control plane: a scheduler holding queued buffers for an
# interface that is then deleted, while every other subscriber keeps
# saturating the hierarchy.
#
#   docker run --rm --privileged -v osvbng-vpp-work:/work \
#     -v "$PWD/../osvbng-vpp-plugin-qos/tests/churn-rig.sh":/rig.sh:ro \
#     osvbng-vpp-builder:v26.06 bash /rig.sh
#
#   CHURN_SECONDS=120   how long to churn
#   CHURN_INTERVAL=2    seconds between kills
#   DELETE_IFACE=1      delete and recreate the sub-interface (1) or only
#                       disable and re-enable the scheduler (0)

set -uo pipefail

B=/work/vpp/build-root/build-vpp-native/vpp
export LD_LIBRARY_PATH=$B/lib/x86_64-linux-gnu
mkdir -p /run/vpp

CHURN_SECONDS=${CHURN_SECONDS:-120}
CHURN_INTERVAL=${CHURN_INTERVAL:-2}
DELETE_IFACE=${DELETE_IFACE:-1}
PG_RATE=${PG_RATE:-3000}

cat > /tmp/churn-startup.conf <<EOF
unix { nodaemon log /tmp/vpp-churn.log cli-listen /run/vpp/cli.sock gid 0 }
cpu { main-core 0 workers 1 }
memory { main-heap-size 1G }
buffers { buffers-per-numa 131072 }
plugins {
  path $B/lib/x86_64-linux-gnu/vpp_plugins
  plugin dpdk_plugin.so { disable }
  plugin osvbng_qos_sched_plugin.so { enable }
}
EOF

$B/bin/vpp -c /tmp/churn-startup.conf &
VPP_PID=$!
until $B/bin/vppctl -s /run/vpp/cli.sock show version >/dev/null 2>&1; do sleep 1; done
cli() { $B/bin/vppctl -s /run/vpp/cli.sock "$@"; }

# svlan:subscriber-rate, mirroring the suites' plan
TAGS="100 101 200 201 300 301"
declare -A SVLAN=([100]=100 [101]=100 [200]=200 [201]=200 [300]=300 [301]=300)
declare -A RATE=([100]=2000 [101]=8000 [200]=4000 [201]=4000 [300]=4000 [301]=4000)

cli create packet-generator interface pg0
cli set interface state pg0 up
cli set interface ip address pg0 10.0.0.1/24
MAC=$(cli show hardware-interfaces pg0 | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | head -1)

cli create packet-generator interface pg1
cli set interface state pg1 up

cli set cake aggregate pg1 rate 8000
cli set cake aggregate pg1 svlan 100-101 rate 6000
cli set cake aggregate pg1 svlan 200-201 rate 3000
cli set cake aggregate pg1 svlan 300-301 rate 3000

make_iface() { # tag
  local t=$1 idx=$2
  cli create sub-interfaces pg1 "$t"
  cli set interface state pg1."$t" up
  cli set interface ip address pg1."$t" 10."$t".0.1/24
  cli set ip neighbor pg1."$t" 10."$t".0.2 02:00:00:00:00:$(printf %02x $((idx + 16))) static
  cli set cake scheduler pg1."$t" rate "${RATE[$t]}" besteffort ethernet
}

idx=0
for t in $TAGS; do make_iface "$t" "$idx"; idx=$((idx + 1)); done

idx=0
for t in $TAGS; do
  cli packet-generator new "name s$idx limit 0 rate $PG_RATE node ethernet-input source pg0 \
size 1400-1400 data { IP4: 02:00:00:00:00:02 -> $MAC \
UDP: 10.0.0.2 -> 10.$t.0.2 UDP: 1000 -> 2000 incrementing 1358 }"
  idx=$((idx + 1))
done
cli packet-generator enable

echo "== churning for ${CHURN_SECONDS}s (delete_iface=$DELETE_IFACE)"
sleep 5

end=$((SECONDS + CHURN_SECONDS))
kills=0
while [ $SECONDS -lt $end ]; do
  set -- $TAGS
  n=$(( (RANDOM % 6) + 1 ))
  t=$(eval echo \${$n})
  idx=$((n - 1))

  if ! kill -0 "$VPP_PID" 2>/dev/null; then
    echo "== VPP DIED after $kills kills"
    break
  fi

  if [ "$DELETE_IFACE" = 1 ]; then
    # Same order the dataplane sees on a session teardown: the interface goes
    # away under the scheduler, and its buffers with it.
    cli create sub-interfaces pg1 "$t" del >/dev/null 2>&1
    sleep 0.3
    make_iface "$t" "$idx" >/dev/null 2>&1
  else
    cli set cake scheduler pg1."$t" disable >/dev/null 2>&1
    sleep 0.3
    cli set cake scheduler pg1."$t" rate "${RATE[$t]}" besteffort ethernet >/dev/null 2>&1
  fi

  kills=$((kills + 1))
  sleep "$CHURN_INTERVAL"
done

echo "== $kills kills issued"
if kill -0 "$VPP_PID" 2>/dev/null; then
  echo "== VPP SURVIVED"
  cli show cake aggregate | head -20
  cli show errors | grep -iE "cake|drop" | head -10
  cli show buffers
  kill "$VPP_PID" 2>/dev/null
else
  echo "== VPP CRASHED - log tail:"
  tail -40 /tmp/vpp-churn.log
fi
