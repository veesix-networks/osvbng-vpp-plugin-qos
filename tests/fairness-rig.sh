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
#   AGG_RATE=8000                     port kbps
#   MEASURE_SECONDS=30
#
# Two-tier mode, for the section 9.2 headline assertion - port capacity splits
# by S-VLAN rate, and within each S-VLAN by subscriber rate:
#
#   SVLANS="100-103:6000 200-203:2000" tag range and kbps per S-VLAN
#   SVLAN_SUB_RATE=5000                per-subscriber kbps, when a group does
#                                      not name its own
#
# A group may carry per-subscriber rates, one per tag, for the case that
# exercises weighted DRR at both tiers at once - unequal subscribers inside
# unequal S-VLANs under a contended port:
#
#   SVLANS="100-103:6000:1000,2000,4000,8000 200-203:3000:500,1000,1000,2000"
#
# with AGG_RATE=8000 the S-VLANs ask for 9000 between them, so the port
# divides 2:1 by their rates, and each S-VLAN then divides its own share by
# its subscribers' rates. No single S-VLAN may exceed the port rate; the
# dataplane refuses it at create.
#
# One sub-interface is created per tag in a range, because a plain VLAN
# sub-interface carries exactly one outer tag. In a real deployment the
# subscribers are session interfaces beneath one S-VLAN sub-interface, which
# needs the session-parentage fixes this spec lists as outstanding.
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
# Packets per second offered per subscriber. Explicit rather than flat out:
# with unpaced streams the generator services them in creation order and
# starves the tail, which reads as dataplane unfairness. At 1400 bytes this is
# several times the whole port rate per stream, so every subscriber is still
# offered far more than its share.
PG_RATE=${PG_RATE:-3000}
# Frame sizes offered, as a comma list cycled across subscribers, e.g.
# "256,640,1400". Not a range: VPP's pg builds one fixed template from the
# data spec and rejects any size range whose maximum exceeds it
# ("min-size < total header size"), so a single stream cannot vary its frame
# length at all. Cycling sizes across children tests the property that
# matters anyway - byte-based DRR must give a child sending 256-byte frames
# the same share of bytes as one sending 1400-byte frames, where an
# opportunity-based round robin would hand the large-frame child several times
# its due.
read -r -a PKT_SIZES <<< "$(echo "${PKT_SIZE:-1400}" | tr ',' ' ')"

# Operator weight multipliers, cycled across subscribers. A child's share is
# its rate times its weight, so this is the knob that lets two subscribers on
# the same tariff be given different congested shares.
read -r -a WEIGHTS <<< "$(echo "${SUB_WEIGHTS:-1}" | tr ',' ' ')"
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

# Configuration that must succeed. vppctl exits 0 even when the CLI handler
# returns an error, so a rejected aggregate would otherwise leave the rig
# measuring a different topology than the one it printed - which is exactly
# how an S-VLAN configured above its port rate once produced a page of
# plausible, entirely meaningless numbers.
cfg() {
  local out
  out=$(cli "$@" 2>&1)
  # Not "any output": several of these print the object they created. These are
  # the shapes a clib_error_return reaches vppctl in.
  if printf '%s' "$out" | grep -qiE 'returned -?[0-9]+|unknown input|required|must be|invalid|no such|failed|in use'; then
    echo "rig: configuration failed: '$*' -> $out" >&2
    exit 1
  fi
}

cfg create packet-generator interface pg0
cfg set interface state pg0 up
cfg set interface ip address pg0 10.0.0.1/24
MAC=$(cli show hardware-interfaces pg0 | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | head -1)

cfg create packet-generator interface pg1
cfg set interface state pg1 up

if [ -n "${SVLANS:-}" ]; then
  SVLAN_SUB_RATE=${SVLAN_SUB_RATE:-5000}
  TAGS=()
  TAG_RATES=()
  for group in $SVLANS; do
    range=${group%%:*}
    rest=${group#*:}
    kbps=${rest%%:*}
    subs=""
    [ "$rest" != "$kbps" ] && subs=${rest#*:}
    first=${range%%-*}
    last=${range##*-}
    i=0
    for t in $(seq "$first" "$last"); do
      TAGS+=("$t")
      if [ -n "$subs" ]; then
        n_subs=$(echo "$subs" | tr ',' '\n' | wc -l)
        TAG_RATES+=("$(echo "$subs" | cut -d, -f$(((i % n_subs) + 1)))")
      else
        TAG_RATES+=("$SVLAN_SUB_RATE")
      fi
      i=$((i + 1))
    done
  done
else
  TAGS=()
  for i in "${!RATES[@]}"; do TAGS+=("$((100 + i))"); done
fi

idx=0
for vlan in "${TAGS[@]}"; do
  cfg create sub-interfaces pg1 "$vlan"
  cfg set interface state pg1."$vlan" up
  cfg set interface ip address pg1."$vlan" 10."$vlan".0.1/24
  # Static neighbour with a MAC nothing answers for: the rewrite succeeds and
  # the packet is shaped, without ARP resolution entering the measurement.
  cfg set ip neighbor pg1."$vlan" 10."$vlan".0.2 02:00:00:00:00:$(printf %02x $((idx + 16))) static
  idx=$((idx + 1))
done

cfg set cake aggregate pg1 rate "$AGG_RATE"

if [ -n "${SVLANS:-}" ]; then
  for group in $SVLANS; do
    range=${group%%:*}
    rest=${group#*:}
    kbps=${rest%%:*}
    first=${range%%-*}
    last=${range##*-}
    cfg set cake aggregate pg1 svlan "${first}-${last}" rate "$kbps"
  done
  i=0
  for vlan in "${TAGS[@]}"; do
    cfg set cake scheduler pg1."$vlan" rate "${TAG_RATES[$i]}" besteffort ethernet \
      weight "${WEIGHTS[$((i % ${#WEIGHTS[@]}))]}"
    i=$((i + 1))
  done
else
  for i in "${!RATES[@]}"; do
    cfg set cake scheduler pg1.$((100 + i)) rate "${RATES[$i]}" besteffort ethernet \
      weight "${WEIGHTS[$((i % ${#WEIGHTS[@]}))]}"
  done
fi

echo "== topology: port ${AGG_RATE} kbps, svlans '${SVLANS:-none}', frames ${PKT_SIZES[*]}"
cli show cake aggregate

idx=0
for vlan in "${TAGS[@]}"; do
  sz=${PKT_SIZES[$((idx % ${#PKT_SIZES[@]}))]}
  # 42 = 14 ethernet + 20 IPv4 + 8 UDP
  cfg packet-generator new "name s$idx limit 0 rate $PG_RATE node ethernet-input source pg0 \
size $sz-$sz data { IP4: 02:00:00:00:00:02 -> $MAC \
UDP: 10.0.0.2 -> 10.$vlan.0.2 UDP: 1000 -> 2000 incrementing $((sz - 42)) }"
  idx=$((idx + 1))
done

cfg packet-generator enable

# Sample mid-run: active_weight and n_active_children are the inputs every
# quantum is derived from, and after the load stops they have all drained to
# zero, which tells you nothing about what they were while it mattered.
sleep $((MEASURE_SECONDS / 2))
echo
echo "== mid-run state"
cli show cake aggregate | grep -E 'rate|active|svlan'

sleep $((MEASURE_SECONDS - MEASURE_SECONDS / 2))
cfg packet-generator disable

# A run that offered nothing must not report shares: every artifact this rig
# has produced so far looked plausible until the offered counts were checked.
if ! cli show cake scheduler | grep -qE 'enqueued: [1-9]'; then
  echo "rig: no traffic reached any scheduler - check the pg stream definition" >&2
  exit 1
fi

echo
echo "== after ${MEASURE_SECONDS}s of offered overload"
cli show cake aggregate
echo
cli show errors | grep -iE 'cake' || true

echo
if [ -n "${DUMP_SCHED:-}" ]; then
  echo
  echo "== per-scheduler detail"
  cli show cake scheduler
fi

echo "== shares against configured rate"

# Expected share of the port for each subscriber. With an S-VLAN tier that is
# two nested splits, not one: the port divides between S-VLANs by their rate
# (capped at each S-VLAN's own rate when the port is not oversubscribed), and
# each S-VLAN then divides its share between its own subscribers. Comparing
# against subscriber rate alone would read a correct 75/25 tier as a 6-point
# error on every child.
EXPECT=""
if [ -n "${SVLANS:-}" ]; then
  sum_svlan=0
  for group in $SVLANS; do
    rest=${group#*:}
    sum_svlan=$((sum_svlan + ${rest%%:*}))
  done
  idx=0
  for group in $SVLANS; do
    range=${group%%:*}
    rest=${group#*:}
    kbps=${rest%%:*}
    first=${range%%-*}
    last=${range##*-}
    n=$((last - first + 1))
    # The port divides by S-VLAN rate, capped at what each S-VLAN asked for.
    weighted=$((AGG_RATE * kbps / sum_svlan))
    svlan_share=$kbps
    [ "$weighted" -lt "$svlan_share" ] && svlan_share=$weighted
    # The S-VLAN then divides its share by its subscribers' rates.
    sum_sub=0
    for j in $(seq 0 $((n - 1))); do
      sum_sub=$((sum_sub + TAG_RATES[idx + j] * WEIGHTS[(idx + j) % ${#WEIGHTS[@]}]))
    done
    for j in $(seq 0 $((n - 1))); do
      ew=$((TAG_RATES[idx + j] * WEIGHTS[(idx + j) % ${#WEIGHTS[@]}]))
      EXPECT="$EXPECT $(awk -v s="$svlan_share" -v r="$ew" \
        -v t="$sum_sub" -v p="$AGG_RATE" 'BEGIN { printf "%.6f", 100 * (s * r / t) / p }')"
    done
    idx=$((idx + n))
  done
else
  sum_sub=0
  for i in "${!RATES[@]}"; do
    sum_sub=$((sum_sub + RATES[i] * WEIGHTS[i % ${#WEIGHTS[@]}]))
  done
  for i in "${!RATES[@]}"; do
    ew=$((RATES[i] * WEIGHTS[i % ${#WEIGHTS[@]}]))
    EXPECT="$EXPECT $(awk -v r="$ew" -v t="$sum_sub" 'BEGIN { printf "%.6f", 100 * r / t }')"
  done
fi

cli show cake scheduler | awk -v want="$EXPECT" '
  BEGIN { split(want, w, " ") }
  /^  pg1\./  { split($1, a, ":"); name[++k] = a[1] }
  /dequeued:/ { for (i = 1; i <= NF; i++)
                  if ($i == "dequeued:") { b[k] = $(i + 3); total_bytes += $(i + 3) } }
  END {
    worst = 0
    for (i = 1; i <= k; i++) {
      got = 100 * b[i] / total_bytes
      err = got - w[i]
      if (err < 0) e = -err; else e = err
      if (e > worst) worst = e
      printf "  %-10s %14d bytes  got %6.2f%%  want %6.2f%%  err %+5.2f pts\n",
             name[i], b[i], got, w[i], err
    }
    printf "  worst error %.2f points\n", worst
  }
'
echo "== done"
