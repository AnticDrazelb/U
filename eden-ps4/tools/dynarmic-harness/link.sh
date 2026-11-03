#!/bin/sh
# link3.sh <source.cpp> <out>: a harness against the current libdynarmic.a ($EXTRA: more flags)
D=$(dirname "$0")
EDEN=${EDEN_SRC:-$HOME/eden}  # (04.17) another Eden tree, e.g. a git worktree
# (the options Eden turns on compiled in, as the tree has them all; DYN_* in the environment
# switches each on at run time)
OPTS=${OPTS:--DHAVE_NEW_OPTS -DHAVE_INLINE_DISPATCH -DHAVE_INLINE_COND -DHAVE_DECLINED_WALK -DHAVE_INLINE_EXCLUSIVES -DHAVE_SINGLE_WALK}
clang++-21 -std=c++20 -O2 $OPTS $EXTRA -isystem $HOME/eden-deps/xbyak -isystem $HOME/eden-deps/fmt/include -DFMT_HEADER_ONLY -DARCHITECTURE_x86_64 \
  -I$EDEN/src -I$EDEN/src/dynarmic/src -I$D/${BUILD:-build}/dynarmic/src \
  $D/$1 $D/${BUILD:-build}/dynarmic/src/dynarmic/libdynarmic.a $D/stubs.cpp -o $D/$2
