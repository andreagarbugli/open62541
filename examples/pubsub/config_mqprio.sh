#!/bin/bash
#
# Copyright (c) 2018, Intel Corporation
#
# SPDX-License-Identifier: BSD-3-Clause
#

# run as root
if [[ $EUID -ne 0 ]]; then
   echo 'This script must be run as root' 1>&2
   exit 1
fi

IFACE=$1
PTP_MODE=$2

if [ -z $IFACE ]; then
    echo "You must provide the network interface as first argument"
    exit -1
fi

# Clear previous dqisc config
sudo tc qdisc delete dev $IFACE parent 100:4 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:3 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:2 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:1 2> /dev/null
sudo tc qdisc delete dev $IFACE root 2> /dev/null

sudo ip link add link $IFACE name "$IFACE.3000" type vlan id 3000 egress 3:7
sudo ip link set dev "$IFACE.3000" up

sleep 8

BATCH_FILE=taprio.batch
cat > $BATCH_FILE <<EOF
qdisc add dev $IFACE handle 100: parent root mqprio \\
      num_tc 2 \\
      map 1 1 1 0 1 1 1 1 1 1 1 1 1 1 1 1 \\
      queues 1@0 3@1 \\
      hw 0
qdisc replace dev $IFACE parent 100:1 etf \\
      offload delta 200000 clockid CLOCK_TAI deadline_mode
EOF

tc -batch $BATCH_FILE

echo "Configuration saved to: $BATCH_FILE"

sleep 2
sudo ./setup_clock_sync.sh -i $IFACE -v "-$PTP_MODE"
