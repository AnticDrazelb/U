#!/bin/bash
# ctx.sh <generator> <harness> <first> <last> [arm|thumb|both]: (04.17) random programs run fresh (test)
# and from a saved code cache (cachetest: the first run's objects unmapped, the cache loaded in a
# second run elsewhere), outputs compared. DTX_OPTS: harness options; DYN_*: the translator's.
cd "$(dirname "$0")"
export DYNARMIC_CODE_ADDRESS=${DYNARMIC_CODE_ADDRESS:-0x3e0000000000}
W=$(mktemp -d rt/runc.XXXXXX)
pass=0; fail=0; skip=0; used=0
modes=${5:-both}; [ "$modes" = both ] && modes="arm thumb"
for seed in $(seq $3 $4); do for mode in $modes; do
  python3 -I $1 $seed $mode $W/p.s
  if [ $mode = arm ]; then ./asm.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }; else ./asmt.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }; fi
  t=; [ $mode = thumb ] && t=thumb
  a=$(timeout 20 ./$2 test $W/p.bin $t nosingle $DTX_OPTS 2>/dev/null); b=$(timeout 20 ./$2 cachetest $W/p.bin $t nosingle $DTX_OPTS 2>$W/err)
  if [ "$a" = "$b" ] && [ -n "$a" ]; then pass=$((pass+1)); u=$(grep -o "[0-9]* used" $W/err | awk '{s+=$1} END{print s+0}'); used=$((used+u)); else fail=$((fail+1)); echo "FAIL $1 seed $seed $mode: $(tail -1 $W/err)"; diff <(echo "$a") <(echo "$b") | head -4; cp $W/p.s rt/failc_$(basename $1 .py)_${seed}_$mode.s; fi
done; done
rm -rf $W
echo "$1 $2: pass $pass fail $fail skipped(asm) $skip; blocks used from the cache $used"
