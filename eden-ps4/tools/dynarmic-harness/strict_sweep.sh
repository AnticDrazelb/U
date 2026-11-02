#!/bin/bash
# strict_sweep.sh <first> <last>: every generator's programs on the strict-xbyak harness (every
# option, PS4 CPU features): any xbyak exception or abort is printed
cd "$(dirname "$0")"
export DYNARMIC_HOST_FEATURES_MASK=16b00f DYN_NOFRAME=1 DYN_FWD=1 DYN_RSB=1 DYN_COND=1 DYN_DISP=1
W=$(mktemp -d rt/strict.XXXXXX); n=0; bad=0
for s in $(seq $1 $2); do
  for g in gen7.py gen.py gen2.py gen3.py gen4.py; do for m in arm thumb; do
    python3 -I $g $s $m $W/p.s 2>/dev/null || continue
    if [ $m = arm ]; then ./asm.sh $W/p.s $W/p.bin 2>/dev/null || continue; else ./asmt.sh $W/p.s $W/p.bin 2>/dev/null || continue; fi
    for o in "nosingle" "nosingle fastmem" "single" "nosingle noexcl"; do
      n=$((n+1)); out=$(timeout 20 ./dynbench_strict test $W/p.bin $m $o 2>&1 | grep -m1 -E "what\(\)|terminate|Abort|exception")
      [ -n "$out" ] && { bad=$((bad+1)); echo "A32 $g $s $m $o: $out"; cp $W/p.s rt/strict_${g%.py}_${s}_$m.s; }
    done
  done; done
  for g in gen5.py gen6.py gen8.py; do
    python3 -I $g $s $W/q.s 2>/dev/null || continue; ./asm64.sh $W/q.s $W/q.bin 2>/dev/null || continue
    for o in "" fastmem fastmem2 noexcl nowalk; do
      n=$((n+1)); out=$(timeout 20 ./dynbench64_strict test $W/q.bin $o 2>&1 | grep -m1 -E "what\(\)|terminate|Abort|exception")
      [ -n "$out" ] && { bad=$((bad+1)); echo "A64 $g $s $o: $out"; cp $W/q.s rt/strict_${g%.py}_$s.s; }
    done
  done
done
rm -rf $W; echo "strict sweep $1-$2: $n runs, $bad with exceptions"
