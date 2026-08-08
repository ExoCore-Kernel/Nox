#!/bin/sh
set -eu

QEMU_BIN="${QEMU:-qemu-system-x86_64}"
BUILD_DIR="${BUILD_DIR:-build/bash-hw-matrix}"
ISO="$BUILD_DIR/nox.iso"
LOG_DIR="${LOG_DIR:-build/hardware-matrix-logs}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-12}"

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "error: missing $QEMU_BIN" >&2
    exit 1
fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "error: GNU timeout is required for the matrix harness" >&2
    exit 1
fi

mkdir -p "$LOG_DIR" "$BUILD_DIR"

# One kernel image is deliberately reused for every hardware permutation. The
# point of this matrix is to prove that driver binding is generic and determined
# by Linux driver ID tables/capabilities, not by rebuilding Twilight for each
# emulated device.
make BUILD_DIR="$BUILD_DIR" \
    LINUX_USER_SELF_TEST=0 \
    BUSYBOX_SELF_TEST=1 \
    BASH_SHELL=1 \
    iso

DISK="$LOG_DIR/test-disk.raw"
if [ ! -f "$DISK" ]; then
    truncate -s 64M "$DISK"
fi

DEVICE_HELP="$($QEMU_BIN -device help 2>/dev/null || true)"

device_available() {
    case "$DEVICE_HELP" in
        *"name \"$1\""*|*"$1"*) return 0 ;;
        *) return 1 ;;
    esac
}

run_case() {
    name="$1"
    shift
    log="$LOG_DIR/$name.log"

    echo ""
    echo "===== $name ====="
    echo "QEMU args: $*"

    set +e
    timeout --signal=TERM "$TIMEOUT_SECONDS" \
        "$QEMU_BIN" \
        -M q35 \
        -cpu qemu64 \
        -m 512M \
        -cdrom "$ISO" \
        -boot d \
        -serial stdio \
        -monitor none \
        -display none \
        -no-reboot \
        -no-shutdown \
        "$@" >"$log" 2>&1
    rc=$?
    set -e

    if grep -q '\[panic\]' "$log" || grep -q '\[serial\] FATAL:' "$log"; then
        echo "RESULT: BOOT FAIL"
        grep -E '\[panic\]|\[serial\] FATAL:|unsupported syscall|IOAPIC|APIC|PCI|driver' "$log" | tail -n 30 || true
        return 1
    fi

    if grep -q 'nox#' "$log"; then
        echo "RESULT: BOOT PASS (interactive Bash reached)"
        grep -E 'IOAPIC|Local APIC|PCI|driver|Ethernet|AHCI|NVMe|virtio|USB|HDA|nox#' "$log" | tail -n 40 || true
        return 0
    fi

    # timeout(1) normally returns 124 because a successful interactive kernel
    # intentionally stays alive. If the prompt was not observed this is still a
    # failure/incomplete boot, regardless of timeout status.
    echo "RESULT: INCOMPLETE (qemu status $rc; Bash prompt not observed)"
    tail -n 40 "$log" || true
    return 1
}

failures=0

run_case baseline || failures=$((failures + 1))

if device_available rtl8139; then
    run_case rtl8139 \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:51 || failures=$((failures + 1))
else
    echo "SKIP rtl8139: QEMU device unavailable"
fi

if device_available e1000; then
    run_case e1000 \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:52 || failures=$((failures + 1))
else
    echo "SKIP e1000: QEMU device unavailable"
fi

if device_available e1000e; then
    run_case e1000e \
        -netdev user,id=net0 \
        -device e1000e,netdev=net0,mac=52:54:00:12:34:53 || failures=$((failures + 1))
else
    echo "SKIP e1000e: QEMU device unavailable"
fi

if device_available virtio-net-pci; then
    run_case virtio-net \
        -netdev user,id=net0 \
        -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:54 || failures=$((failures + 1))
else
    echo "SKIP virtio-net-pci: QEMU device unavailable"
fi

if device_available nvme; then
    run_case nvme \
        -drive if=none,id=nvme0,format=raw,file="$DISK" \
        -device nvme,drive=nvme0,serial=NOXNVME0001 || failures=$((failures + 1))
else
    echo "SKIP nvme: QEMU device unavailable"
fi

if device_available virtio-blk-pci; then
    run_case virtio-blk \
        -drive if=none,id=vblk0,format=raw,file="$DISK" \
        -device virtio-blk-pci,drive=vblk0 || failures=$((failures + 1))
else
    echo "SKIP virtio-blk-pci: QEMU device unavailable"
fi

if device_available qemu-xhci; then
    if device_available usb-kbd; then
        run_case xhci-usb-hid \
            -device qemu-xhci,id=xhci \
            -device usb-kbd,bus=xhci.0 || failures=$((failures + 1))
    else
        run_case xhci \
            -device qemu-xhci,id=xhci || failures=$((failures + 1))
    fi
else
    echo "SKIP qemu-xhci: QEMU device unavailable"
fi

if device_available ich9-intel-hda; then
    if device_available hda-duplex; then
        run_case hda \
            -device ich9-intel-hda \
            -device hda-duplex || failures=$((failures + 1))
    else
        run_case hda-controller -device ich9-intel-hda || failures=$((failures + 1))
    fi
else
    echo "SKIP ich9-intel-hda: QEMU device unavailable"
fi

if device_available virtio-gpu-pci; then
    run_case virtio-gpu -device virtio-gpu-pci || failures=$((failures + 1))
else
    echo "SKIP virtio-gpu-pci: QEMU device unavailable"
fi

echo ""
echo "Hardware matrix logs: $LOG_DIR"
if [ "$failures" -ne 0 ]; then
    echo "$failures matrix case(s) failed or did not reach Bash."
    exit 1
fi

echo "All available matrix cases reached Bash without a kernel panic."
