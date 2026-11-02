#!/bin/bash
# dtx64.sh <generator> <old harness> <new harness> <first> <last>: random A64 programs, the old
# build vs the new (env options), each with $DTX_OPTS
cd "$(dirname "$0")"
W=$(mktemp -d rt/runx64.XXXXXX); pass=0; fail=0; skip=0
for seed in $(seq $4 $5); do
  python3 -I $1 $seed $W/p.s; ./asm64.sh $W/p.s $W/p.bin 2>$W/err || { skip=$((skip+1)); [ $skip -le 2 ] && head -3 $W/err; continue; }
  a=$(timeout 20 ./$2 test $W/p.bin $DTX_OPTS 2>/dev/null); b=$(timeout 20 ./$3 test $W/p.bin $DTX_OPTS 2>/dev/null)
  if [ "$a" = "$b" ] && [ -n "$a" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1 $seed"; diff <(echo "$a") <(echo "$b") | head -6; cp $W/p.s rt/failx64_$(basename $1 .py)_$seed.s; fi
done
rm -rf $W
echo "$1 $2 vs $3 [$DTX_OPTS]: pass $pass fail $fail skipped $skip"
