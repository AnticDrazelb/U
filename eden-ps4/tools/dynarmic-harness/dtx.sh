#!/bin/bash
# dtx.sh <generator> <old harness> <new harness> <first> <last>: random A32 programs (arm and thumb),
# the old build's normal run vs the new build's (with the env options), and the new single-step run
cd "$(dirname "$0")"
W=$(mktemp -d rt/runx.XXXXXX)
pass=0; fail=0; skip=0
for seed in $(seq $4 $5); do for mode in arm thumb; do
  python3 -I $1 $seed $mode $W/p.s
  if [ $mode = arm ]; then ./asm.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }; else ./asmt.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }; fi
  a=$(timeout 20 ./$2 test $W/p.bin $mode nosingle $DTX_OPTS 2>/dev/null); b=$(timeout 20 ./$3 test $W/p.bin $mode nosingle $DTX_OPTS 2>/dev/null); c=$(timeout 20 ./$3 test $W/p.bin $mode single $DTX_OPTS 2>/dev/null)
  if [ "$a" = "$b" ] && [ "$a" = "$c" ] && [ -n "$a" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1 seed $seed $mode"; diff <(echo "$a") <(echo "$b") | head -6; cp $W/p.s rt/failx_$(basename $1 .py)_${seed}_$mode.s; fi
done; done
rm -rf $W
echo "$1 $2 vs $3: pass $pass fail $fail skipped(asm) $skip"
