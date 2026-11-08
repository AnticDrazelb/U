#!/usr/bin/env bash
# Builds the disc test's files beside its default.xbe (bin/), and a disc image of them (disc-test.iso)
# with nxdk's extract-xiso. Run after make.
set -euo pipefail
cd "$(dirname "$0")"
NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
mkdir -p bin/data/sub
python3 - <<'PY'
data = bytes(((i * 7 + i // 251) & 255) for i in range(300000))
open("bin/data/big.bin", "wb").write(data)
open("bin/data/sub/hello.txt", "w").write("hello, disc\n")
for i in range(1, 21):
    open(f"bin/data/a{i}.txt", "w").write(str(i))
PY
rm -f disc-test.iso
"$NXDK_DIR/tools/extract-xiso/build/extract-xiso" -c bin disc-test.iso > /dev/null
echo "bin/ and disc-test.iso ready"
