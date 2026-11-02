#!/bin/bash
# dtxa.sh <generator> <old harness> <new harness> <first> <last>: dtx.sh for ARM-only generators
cd "$(dirname "$0")"
W=$(mktemp -d rt/runx.XXXXXX)
pass=0; fail=0; skip=0
for seed in $(seq $4 $5); do
  python3 -I $1 $seed arm $W/p.s
  ./asm.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }
  a=$(timeout 20 ./$2 test $W/p.bin arm nosingle $DTX_OPTS 2>/dev/null); b=$(timeout 20 ./$3 test $W/p.bin arm nosingle $DTX_OPTS 2>/dev/null); c=$(timeout 20 ./$3 test $W/p.bin arm single $DTX_OPTS 2>/dev/null)
  if [ "$a" = "$b" ] && [ "$a" = "$c" ] && [ -n "$a" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1 seed $seed"; diff <(echo "$a") <(echo "$b") | head -6; cp $W/p.s rt/failx_$(basename $1 .py)_${seed}_arm.s; fi
done
rm -rf $W
echo "$1 $2 vs $3: pass $pass fail $fail skipped(asm) $skip"
