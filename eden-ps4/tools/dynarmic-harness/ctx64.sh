#!/bin/bash
# ctx64.sh <generator> <harness> <first> <last>: (04.18) as ctx.sh for A64: random programs run fresh
# (test) and from a saved code cache (cachetest), outputs compared. DTX_OPTS: harness options.
cd "$(dirname "$0")"
export DYNARMIC_CODE_ADDRESS=${DYNARMIC_CODE_ADDRESS:-0x3e0000000000}
W=$(mktemp -d rt/runc64.XXXXXX)
pass=0; fail=0; skip=0; used=0
for seed in $(seq $3 $4); do
  python3 -I $1 $seed $W/p.s; ./asm64.sh $W/p.s $W/p.bin 2>/dev/null || { skip=$((skip+1)); continue; }
  a=$(timeout 20 ./$2 test $W/p.bin $DTX_OPTS 2>/dev/null); b=$(timeout 20 ./$2 cachetest $W/p.bin $DTX_OPTS 2>$W/err)
  if [ "$a" = "$b" ] && [ -n "$a" ]; then pass=$((pass+1)); u=$(grep -o "[0-9]* used" $W/err | awk '{s+=$1} END{print s+0}'); used=$((used+u)); else fail=$((fail+1)); echo "FAIL $1 seed $seed: $(tail -1 $W/err)"; diff <(echo "$a") <(echo "$b") | head -4; cp $W/p.s rt/failc64_$(basename $1 .py)_$seed.s; fi
done
rm -rf $W
echo "$1 $2 [$DTX_OPTS]: pass $pass fail $fail skipped(asm) $skip; blocks used from the cache $used"
