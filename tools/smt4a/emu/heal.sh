#!/bin/bash
D=/tmp/claude-0/emu; F=$D/film; T=/home/claude/smt4a/emu/user/load/screen_regions/trace.on
pkill -x sr_tool; pkill -f '^sleep infinity$'; sleep 1; rm -f $D/err
$D/drv.sh start >/dev/null; sleep 4
echo on > $T
$D/drv.sh cmd "load 4" "run 10" "hold b 6" "run 20" "w32 0x85f9360 4000" "w32 0x85f9768 3000" "tap right 25" "run 30" "film $F/h0_ 1 1" "hold a 8" "run 40" "film $F/h1_ 1 1" "tap right 25" "run 20" "film $F/h2_ 1 1" "hold a 8" "film $F/H_ 200 1" >/dev/null
echo off > $T
echo ALLDONE
