#!/bin/bash

# run as root
if [[ $EUID -ne 0 ]]; then
   echo 'This script must be run as root' 1>&2
   exit 1
fi

IFACE=$1

if [ -z $IFACE ]; then
    echo "You must provide the network interface as first argument"
    exit -1
fi

# Now plus 1 minute
PLUS_1MIN=$((`date +%s%N` + (1 * 1000000000)))

# Base will the next "round" timestamp ~1 min from now, plus 250us
BASE=$(($PLUS_1MIN - ( $PLUS_1MIN % 1000000000 ) + 250000))

sudo ../../build/bin/examples/pubsub_custom_publisher -u opc.eth://01-00-5e-00-00-01:3000.7 -i $IFACE -b $BASE -p 500000 -d 400000 -E -m 200000 -w "$2" -c 1

# sudo ./l2_tai -i $IFACE -b $BASE -P 1000000 -t 3 -p 90 -d 600000 -m 01:00:5e:00:00:01
