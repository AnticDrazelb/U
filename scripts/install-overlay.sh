#!/usr/bin/env bash
# Install toolchain/libc-overlay (FreeBSD-layout socket/netinet/poll/fcntl headers from PSChrome)
# over the sysroot's libc overlay (DolphinPS4's: pthread sizes, clocks, mmap flags, sysconf, ...).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../toolchain/env.sh"
cp -rv "$HERE/../toolchain/libc-overlay/." "$PS4_SYSROOT/libc-overlay/"
