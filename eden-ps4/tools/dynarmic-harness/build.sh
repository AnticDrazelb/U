#!/bin/sh
# build.sh <name>: dynarmic from ~/eden/src/dynarmic (wrap/) into build/, then the A32 harness
# (dynbench.cpp) linked against it as ./<name>; DYNBENCH=dynbench64.cpp for the A64 one.
# (04.17) EDEN_SRC=<tree> builds another Eden tree's dynarmic (a git worktree), BUILD=<dir> into
# another build directory (so two trees' builds don't mix).
set -e
D=$(cd "$(dirname "$0")" && pwd)
# (Eden's own fmt and xbyak, from fetch-deps.sh: the system's fmt is too old for Eden's logging)
DEPS=${EDEN_DEPS:-$HOME/eden-deps}
B="$D/${BUILD:-build}"
[ -f "$B/build.ninja" ] || ${CMAKE:-cmake} -S "$D/wrap" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
    -DCMAKE_CXX_FLAGS="-isystem $DEPS/xbyak -isystem $DEPS/fmt/include -DFMT_HEADER_ONLY -DARCHITECTURE_x86_64"
ninja -C "$B" dynarmic
mkdir -p "$D/rt"
"$D/link.sh" "${DYNBENCH:-dynbench.cpp}" "$1"
