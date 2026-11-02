#!/bin/sh
# link3.sh <source.cpp> <out>: a harness against the current libdynarmic.a ($EXTRA: more flags)
D=$(dirname "$0")
clang++-21 -std=c++20 -O2 $EXTRA -isystem $HOME/eden-deps/xbyak -isystem $HOME/eden-deps/fmt/include -DFMT_HEADER_ONLY -DARCHITECTURE_x86_64 \
  -I$HOME/eden/src -I$HOME/eden/src/dynarmic/src -I$D/${BUILD:-build}/dynarmic/src \
  $D/$1 $D/${BUILD:-build}/dynarmic/src/dynarmic/libdynarmic.a $D/stubs.cpp -o $D/$2
