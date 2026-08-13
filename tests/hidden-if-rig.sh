#!/bin/bash
# Copyright 2026 Veesix Networks Ltd
# Licensed under the GNU General Public License v3.0 or later.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression check: cake scheduler disable must succeed on a hidden
# (parked) PPPoE session interface. The osvbng_pppoe plugin deletes a
# session by hiding and recycling its interface, and the control plane's
# RemoveScheduler races that hiding; a disable rejected with -2 leaks the
# scheduler onto the interface's next subscriber. The CLI cannot name a
# hidden interface at all, so the disable goes through the binary API by
# sw_if_index, exactly as the control plane does.
#
# Runs INSIDE the osvbng-vpp builder container with the tree in /work and
# the osvbng_pppoe and osvbng_qos_sched plugins built (make vpp-dev):
#
#   docker run --rm --privileged -v osvbng-vpp-work:/work \
#     -v "$PWD/../osvbng-vpp-plugin-qos/tests/hidden-if-rig.sh":/rig.sh:ro \
#     osvbng-vpp-builder:v26.06 bash /rig.sh
set -uo pipefail

B=/work/vpp/build-root/build-vpp-native/vpp
export LD_LIBRARY_PATH=$B/lib/x86_64-linux-gnu
mkdir -p /run/vpp

cat > /tmp/hidden-startup.conf <<EOF
unix { nodaemon log /tmp/vpp-hidden.log cli-listen /run/vpp/cli.sock gid 0 }
socksvr { socket-name /run/vpp/api.sock }
cpu { main-core 0 }
plugins {
  path $B/lib/x86_64-linux-gnu/vpp_plugins
  plugin dpdk_plugin.so { disable }
  plugin osvbng_pppoe_plugin.so { enable }
  plugin osvbng_qos_sched_plugin.so { enable }
}
EOF

$B/bin/vpp -c /tmp/hidden-startup.conf &
VPP_PID=$!
until $B/bin/vppctl -s /run/vpp/cli.sock show version >/dev/null 2>&1; do sleep 1; done
cli() { $B/bin/vppctl -s /run/vpp/cli.sock "$@"; }

cli create packet-generator interface pg0
cli set interface state pg0 up

SESS="client-ip 10.0.0.2 session-id 1 client-mac 02:00:00:00:00:02 local-mac 02:00:00:00:00:01 encap-if pg0"

echo "== create session"
cli create osvbng pppoe session $SESS
cli show interface | grep pppoe

echo "== enable scheduler on the session interface"
cli set cake scheduler pppoe_session0 rate 4000 besteffort ethernet
cli show cake scheduler | head -4

echo "== delete session (interface hidden + parked, not deleted)"
cli create osvbng pppoe session $SESS del

# The CLI cannot name a hidden interface (unformat_vnet_sw_interface skips
# them), so disable goes through the binary API by sw_if_index - the same
# path the control plane's RemoveScheduler takes.
echo "== disable via binary API on the hidden interface (the regression):"
PYTHONPATH=/work/vpp/src/vpp-api/python python3 - <<'EOF'
import glob
from vpp_papi import VPPApiClient
apifiles = glob.glob('/work/vpp/build-root/install-vpp-native/vpp/share/vpp/api/**/*.api.json', recursive=True)
vpp = VPPApiClient(apifiles=apifiles, server_address='/run/vpp/api.sock')
vpp.connect('hidden-rig')
r = vpp.api.osvbng_cake_sched_enable_disable(sw_if_index=2, is_enable=False)
print(f"   disable retval: {r.retval}")
vpp.disconnect()
raise SystemExit(0 if r.retval == 0 else 1)
EOF
API_RC=$?

echo "== remaining schedulers (expect none):"
cli show cake scheduler

if cli show cake scheduler | grep -q pppoe_session0; then
  echo "RESULT: FAIL - scheduler survived on hidden interface"
  RC=1
elif [ $API_RC -ne 0 ]; then
  echo "RESULT: FAIL - disable returned nonzero retval"
  RC=1
else
  echo "RESULT: PASS - scheduler removed from hidden interface"
  RC=0
fi
kill $VPP_PID 2>/dev/null
exit $RC
