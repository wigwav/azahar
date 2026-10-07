#!/bin/bash
# start: drv.sh start ; send: drv.sh cmd "..." (waits for ok)
D=/tmp/claude-0/emu
case "$1" in
start)
  rm -f $D/in $D/out; mkfifo $D/in
  cd /home/claude/smt4a/emu
  (sleep infinity > $D/in &) 
  CITRA_LOG_ROMFS=1 SR_LOG="${SR_LOG:-*:Info HW.Memory:Critical}" SR_GAME_FILE=/home/claude/smt4a/emu/smt4a.cxi CITRA_ALLOW_STATE_BUILD_MISMATCH=1 SR_ROMFS_SUB=317000:54b000 nohup /home/claude/build-lin/bin/Release/sr_tool /home/claude/smt4a/emu/user /home/claude/smt4a/emu/smt4a.cxi < $D/in > $D/out 2>$D/err &
  echo started;;
cmd)
  shift
  n=$(cat $D/out 2>/dev/null | grep -c "^ok ")
  for c in "$@"; do echo "$c" > $D/in; done
  want=$((n+$#))
  for i in $(seq 1 36000); do k=$(grep -c '^ok ' $D/out); [ "$k" -ge "$want" ] && break; pgrep -x sr_tool >/dev/null || { echo "DIED"; break; }; sleep 0.1; done
  tail -n $(( $# + 2 )) $D/out | grep -v '^ok ';;
esac
