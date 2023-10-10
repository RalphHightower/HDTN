#!/bin/sh

# path variables
config_files=$HDTN_SOURCE_ROOT/config_files
hdtn_config=$config_files/hdtn/hdtn_ingress1tcpclv4_port4556_egress1tcpclv4_port4558flowid2.json
bpsec_config=$config_files/bpsec/BPSec3.json
sink_config=$config_files/inducts/bpsink_one_tcpclv4_port4558.json
gen_config=$config_files/outducts/bpgen_one_tcpclv4_port4556.json


cd $HDTN_SOURCE_ROOT

# bpsink
./build/common/bpcodec/apps/bpsink-async --my-uri-eid=ipn:2.1 --inducts-config-file=$sink_config &
bpsink_PID=$!
sleep 3

# HDTN one process
# use the option --use-unix-timestamp when using a contact plan with unix timestamp
# use the option --use-mgr to use Multigraph Routing Algorithm (the default routing Algorithm is CGR Dijkstra) 
#./build/module/hdtn_one_process/hdtn-one-process --hdtn-config-file=$hdtn_config --bpsec-config-file=$bpsec_config --contact-plan-file=contactPlanCutThroughMode_unlimitedRate.json &
#oneprocess_PID=$!
#sleep 10
./build/module/hdtn_one_process/hdtn-one-process --hdtn-config-file=$hdtn_config --bpsec-config-file=$bpsec_config --contact-plan-file=contactPlanCutThroughMode_unlimitedRate.json &
oneprocess_PID=$!
sleep 10



#bpgen (configure bundle-rate=0 to send bundles at high rate)
./build/common/bpcodec/apps/bpgen-async --use-bp-version-7 --bundle-rate=100 --my-uri-eid=ipn:1.1 --dest-uri-eid=ipn:2.1 --duration=40 --outducts-config-file=$gen_config &
bpgen_PID=$!
sleep 8

# cleanup
sleep 30
echo "\nkilling bpgen1..." && kill -2 $bpgen_PID
sleep 2
echo "\nkilling egress..." && kill -2 $oneprocess_PID
sleep 2
echo "\nkilling bpsink2..." && kill -2 $bpsink_PID

