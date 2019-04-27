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

sleep 4

i=$((`date +%s%N` + 37000000000 + (10 * 1000000000)))

BASE_TIME=$(($i - ($i % 1000000000)))

# num_tc: defines the total number of traffic classes. Here TC0 = TSN and TC1 = BE
# map: maps the SO_PRIORITY (defined by the position in the list) to a traffic class (TC) defined by number in the list
#      here only SO_PRIORITY=5 is TC0 (TSN). All other SO_PRIORITY values are TC1 (BE).
# queues: maps the traffic classes (tc) defined by position in the list to the physical queues.
#         here 1st queue is mapped to TC0 and remaining 3 queues are mapped to TC1.
#tc qdisc replace dev $IFACE parent root handle 100 taprio \
#      num_tc 2 \
#      map 1 1 1 1 1 0 1 1 1 1 1 1 1 1 1 1 \
#      queues 1@0 3@1 \
#      base-time $TIME_BASE \
#      sched-entry S 02 $SLOT_BE_PRE \
#      sched-entry S 01 $SLOT_TSN \
#      sched-entry S 02 $SLOT_BE_POST \
#      clockid CLOCK_TAI

BATCH_FILE=taprio.batch
cat > $BATCH_FILE <<EOF
qdisc replace dev $IFACE parent root handle 100 taprio \\
      num_tc 2 \\
      map 1 1 1 0 1 1 1 1 1 1 1 1 1 1 1 1 \\
      queues 1@0 3@1 \\
      base-time $BASE_TIME \\
      sched-entry S 01 300000 \\
      sched-entry S 02 200000 \\
      clockid CLOCK_TAI
qdisc replace dev $IFACE parent 100:1 etf \\
      offload delta 150000 clockid CLOCK_TAI
EOF

tc -batch $BATCH_FILE

echo "Base time: $BASE_TIME"
echo "Configuration saved to: $BATCH_FILE"

sleep 2
sudo ./setup_clock_sync.sh -i $IFACE -v "-$PTP_MODE"
