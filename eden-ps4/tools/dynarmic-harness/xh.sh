#!/bin/bash
# xh.sh <gen> <arm|thumb|a64> <old harness> <new harness> <first> <last> [options]: the same programs on two harnesses
cd "$(dirname "$0")"
W=$(mktemp -d rt/xh.XXXXXX); pass=0; fail=0; skip=0
for seed in $(seq $5 $6); do
  if [ $2 = a64 ]; then python3 -I $1 $seed $W/p.s; ./asm64.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }; m=""
  else python3 -I $1 $seed $2 $W/p.s; if [ $2 = arm ]; then ./asm.sh $W/p.s $W/p.bin 2>/dev/null; else ./asmt.sh $W/p.s $W/p.bin 2>/dev/null; fi || { skip=$((skip+1)); continue; }; m=$2; fi
  a=$(timeout 20 ./$3 test $W/p.bin $m ${@:7} 2>/dev/null); b=$(timeout 20 ./$4 test $W/p.bin $m ${@:7} 2>/dev/null)
  if [ "$a" = "$b" ] && [ -n "$a" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1 $seed $2"; fi
done
echo "$1 $2 $3 vs $4: pass $pass fail $fail skipped $skip"
