#!/bin/bash

# run as root
if [[ $EUID -ne 0 ]]; then
   echo 'This script must be run as root' 1>&2
   exit 1
fi

# Process Options
VERBOSE=false
PTP_VERBOSE=''
while getopts "v" opt; do
	case ${opt} in
 	v) VERBOSE=true ;
           PTP_VERBOSE='-v' ;;
	*) echo "Non valid option. exiting." ;
           exit -1 ;;
	esac
done

IFACE=enp2s0

# Clear previous dqisc config
sudo tc qdisc delete dev $IFACE parent 100:4 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:3 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:2 2> /dev/null
sudo tc qdisc delete dev $IFACE parent 100:1 2> /dev/null
sudo tc qdisc delete dev $IFACE root 2> /dev/null

##### Compute Base time #####
# Get time of now by using date command and adding the 37s UTC-TAI offset to the timestamp
# NOW_TAI=$((`date +%s%N` + 37000000000))
NOW=`date +%s%N`
NOW_TAI=$(($NOW + 37000000000 + (10 * 1000000000)))
BASE=$(($NOW_TAI - ($NOW_TAI % 1000000000)))                      

if [ "$VERBOSE" = true ] ; then
    echo " "
    echo "NOW = $NOW"
    echo "NOW_TAI = $NOW_TAI"
    echo "BASE = $BASE"
fi

# This prevents segmentation faut!
# Seg fault occures if 'tc qdisc ... taprio' is executed to fast after clearing the previous qdisc config
# 8 sec is conservative and could be tuned down..
sleep 2

tc qdisc replace dev $IFACE parent root handle 100 mqprio         \
                 num_tc 2                                                 \
                 map 1 1 1 0 1 1 1 1 1 1 1 1 1 1 1 1                      \
                 queues 1@0 3@1                                       \
                 hw 0

# Traffic Class 0 (TC0) for TSN traffic
sudo tc qdisc replace dev $IFACE parent 100:1 etf clockid CLOCK_TAI delta 100000 offload

# Display running qdisc config
if [ "$VERBOSE" = true ] ; then
    echo " "
    echo "interface config:"
    ip addr show dev $IFACE
    echo " "
    echo "qdisc config"
    tc qdisc show dev $IFACE
fi

sudo ../../build/bin/examples/pubsub_custom_publisher -u opc.eth://01-00-5e-00-00-01 -i $IFACE -b $BASE -p 1000000 -d 600000