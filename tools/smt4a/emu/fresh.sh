#!/bin/bash
# fresh.sh : restart sr_tool clean
D=/tmp/claude-0/emu
pkill -x sr_tool; pkill -f '^sleep infinity$'; sleep 1
$D/drv.sh start >/dev/null; sleep 3
