#!/usr/bin/env bash
# A FreeBSD 14.3 VM as a stand-in for the PS4's system: the console's kernel and libkernel come from
# FreeBSD, and its threading library's signal wrapper (which reads fs:[0x10]) is FreeBSD's libthr. In
# this VM the fault path behaves as on the console: with libthr's sigaction the app dies silently at
# the first fault in guest code (XB_SIGNAL_ROUTE=libthr), the kernel's own works. The CPU is QEMU's
# Haswell without AVX2, BMI2 and FMA: the Jaguar's instruction set (the build uses -march=btver2).
# No KVM is needed (QEMU's TCG): slow (a full build ~10 min), but faithful.
#
#   run.sh boot                       download the image (once) and start the VM; ssh on port 2222
#   run.sh sync                       copy src/ in and build it there (~/xc-bsd/xbox-core in the VM)
#   run.sh test                       the regression tests in the VM (kernel, SEH, disc, triangle)
#   run.sh run <file> [args]          copy a file in and run xbox-core on it there
#   run.sh ssh [command]              a shell or command in the VM (root)
#   run.sh stop
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$(cd "$HERE/../.." && pwd)"
VM="${XB_VM_DIR:-$HOME/vm}"
IMAGE_URL=https://github.com/cross-platform-actions/freebsd-builder/releases/download/v0.13.0/freebsd-14.3-x86-64.qcow2
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
vssh() { sshpass -p runner ssh "${SSH_OPTS[@]}" -p 2222 root@127.0.0.1 "$@"; }
vcp() { sshpass -p runner scp "${SSH_OPTS[@]}" -P 2222 "$@"; }
need() { command -v "$1" >/dev/null || sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$2" >/dev/null; }

case "${1:-}" in
boot)
    need qemu-system-x86_64 qemu-system-x86
    need qemu-img qemu-utils
    need sshpass sshpass
    mkdir -p "$VM"
    if [ ! -f "$VM/freebsd.qcow2" ]; then
        curl -sSL -o "$VM/freebsd.qcow2" "$IMAGE_URL"
        qemu-img resize "$VM/freebsd.qcow2" 20G >/dev/null
    fi
    pgrep -f "qemu-system-x86_64.*freebsd.qcow2" >/dev/null && { echo "already running"; exit 0; }
    : > "$VM/serial.log"
    nohup qemu-system-x86_64 -machine q35 -accel tcg,thread=multi \
        -cpu Haswell-v4,-avx2,-bmi2,-fma,-hle,-rtm,-invpcid,-erms,+movbe,+f16c,+fsgsbase \
        -smp "$(nproc)" -m 6G -drive "file=$VM/freebsd.qcow2,if=virtio" \
        -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22 \
        -display none -serial "file:$VM/serial.log" > "$VM/qemu.out" 2>&1 &
    timeout 600 bash -c "until grep -q 'login:' '$VM/serial.log'; do sleep 5; done"
    vssh 'uname -rm'
    ;;
sync)
    tar czf "$VM/src.tgz" -C "$CORE/.." xbox-core/src xbox-core/VERSION xbox-core/tests/freebsd-vm
    vcp "$VM/src.tgz" root@127.0.0.1:/root/
    vssh 'rm -rf /root/xc/xbox-core/src && mkdir -p /root/xc && tar xzf /root/src.tgz -C /root/xc && /root/xc/xbox-core/tests/freebsd-vm/build.sh /root/xc-bsd'
    ;;
test)
    NXDK_DIR="${NXDK_DIR:-$HOME/emu/nxdk}"
    tar czf "$VM/t.tgz" -C "$CORE/tests" kernel-test/bin seh-test/bin bigimage-test/bin disc-test/disc-test.iso -C "$NXDK_DIR/samples" triangle/bin
    vcp "$VM/t.tgz" root@127.0.0.1:/root/
    vssh 'rm -rf /root/t && mkdir -p /root/t /root/run && tar xzf /root/t.tgz -C /root/t && cd /root/run && fail=0
for x in /root/t/kernel-test/bin/default.xbe /root/t/seh-test/bin/default.xbe /root/t/disc-test/disc-test.iso /root/t/triangle/bin/default.xbe ${XB_VM_BIG:+/root/t/bigimage-test/bin/default.xbe}; do
    XB_STRICT_SIGNAL=1 timeout 600 /root/xc-bsd/xbox-core "$x" --seconds 8 > out.txt 2>&1; rc=$?
    r=$(grep -E "test: [0-9]+ failures" out.txt | tail -1)
    p=$(grep -oE "backpatched hardware accesses so far: [0-9]+ at [0-9]+ sites" out.txt | tail -1)
    echo "$x: exit $rc ${r:-} ${p:-}"; [ "$rc" = 0 ] || { fail=1; tail -5 out.txt; }
    case "$r" in *" 0 failures"*|"") ;; *) fail=1 ;; esac
done; exit $fail'
    ;;
run)
    f="$2"; shift 2
    vcp "$f" root@127.0.0.1:/root/run/
    vssh "cd /root/run && timeout 900 /root/xc-bsd/xbox-core /root/run/$(basename "$f") $*"
    ;;
ssh)
    shift
    vssh "$@"
    ;;
stop)
    vssh 'shutdown -p now' || true
    ;;
*)
    sed -n 2,16p "$0"
    exit 1
    ;;
esac
