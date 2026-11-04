#!/bin/bash
# steady64.sh <harness> [env...]: callgrind instructions for 20 steady frames of game64 (30 minus 10)
cd "$(dirname "$0")"
b=$1; shift
a=$(env "$@" valgrind --tool=callgrind --callgrind-out-file=/dev/null ./$b image programs/c/game64.bin 30 fastmem2 2>&1 | grep Collected | awk '{print $4}')
c=$(env "$@" valgrind --tool=callgrind --callgrind-out-file=/dev/null ./$b image programs/c/game64.bin 10 fastmem2 2>&1 | grep Collected | awk '{print $4}')
echo "$b $* steady=$((a-c))"
