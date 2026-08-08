#!/bin/sh
set -eu

QEMU_BIN="${QEMU:-qemu-system-x86_64}"
BUILD_DIR="${BUILD_DIR:-build/upstream-all-matrix}"
ISO="$BUILD_DIR/nox.iso"
LOG_DIR="${LOG_DIR:-build/upstream-all-matrix-logs}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-60}"
POLL_INTERVAL="${POLL_INTERVAL:-1}"

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "error: missing $QEMU_BIN" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" "$LOG_DIR"

# Build every upstream driver currently supported by the Twilight Linux
# compatibility layer into one kernel. This intentionally exercises driver
# coexistence instead of giving each upstream driver a private kernel image.
make BUILD_DIR="$BUILD_DIR" \
    LINUX_USER_SELF_TEST=0 \
    BUSYBOX_SELF_TEST=1 \
    BASH_SHELL=1 \
    UPSTREAM_8139=1 \
    UPSTREAM_AHCI=1 \
    STORAGE_SELF_TEST=0 \
    iso

DEVICE_HELP="$($QEMU_BIN -device help 2>/dev/null || true)"

device_available() {
    case "$DEVICE_HELP" in
        *"name \"$1\""*|*"$1"*) return 0 ;;
        *) return 1 ;;
    esac
}

stop_qemu() {
    pid="$1"
    kill -TERM "$pid" 2>/dev/null || true
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 3 ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

binding_present() {
    log="$1"
    pci_id="$2"
    driver="$3"
    tr -d '\r' <"$log" | grep -Eq "PCI BIND .*id=${pci_id} .*driver=${driver}($| )"
}

run_case() {
    name="$1"
    expectations="$2"
    shift 2

    log="$LOG_DIR/$name.log"
    qemu_log="$LOG_DIR/$name.qemu.log"

    echo ""
    echo "===== $name ====="
    echo "QEMU args: $*"

    : >"$log"
    : >"$qemu_log"

    "$QEMU_BIN" \
        -M q35 \
        -cpu qemu64 \
        -m 512M \
        -cdrom "$ISO" \
        -boot d \
        -nic none \
        -serial "file:$log" \
        -monitor none \
        -display none \
        -no-reboot \
        -no-shutdown \
        "$@" >"$qemu_log" 2>&1 &
    qemu_pid=$!

    elapsed=0
    outcome=""

    while [ "$elapsed" -lt "$TIMEOUT_SECONDS" ]; do
        if grep -q '\[panic\]' "$log" 2>/dev/null || \
           grep -q '\[serial\] FATAL:' "$log" 2>/dev/null || \
           grep -q '\[linux:error\] PCI shell preflight:.*failed' "$log" 2>/dev/null || \
           grep -q '\[linux:error\] Linux module initcall failed:' "$log" 2>/dev/null; then
            outcome="fail"
            break
        fi

        if grep -q 'nox#' "$log" 2>/dev/null || \
           grep -q '\[serial\] bash-shell: entering GNU Bash /bin/bash -i at CPL3' "$log" 2>/dev/null; then
            outcome="pass"
            break
        fi

        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            outcome="exited"
            break
        fi

        sleep "$POLL_INTERVAL"
        elapsed=$((elapsed + POLL_INTERVAL))
    done

    stop_qemu "$qemu_pid"

    case "$outcome" in
        pass)
            echo "BOOT: PASS (GNU Bash userspace reached after ~${elapsed}s)"
            ;;
        fail)
            echo "BOOT: FAIL (driver init or kernel failure after ~${elapsed}s)"
            ;;
        exited)
            echo "BOOT: FAIL (QEMU exited before Bash after ~${elapsed}s)"
            ;;
        *)
            echo "BOOT: FAIL (timeout after ${TIMEOUT_SECONDS}s)"
            ;;
    esac

    driver_failures=0
    old_ifs="$IFS"
    IFS=','
    for expectation in $expectations; do
        [ -n "$expectation" ] || continue
        pci_id=${expectation%%=*}
        driver=${expectation#*=}
        if binding_present "$log" "$pci_id" "$driver"; then
            echo "DRIVER: PASS id=$pci_id driver=$driver"
        else
            echo "DRIVER: FAIL id=$pci_id expected_driver=$driver"
            driver_failures=$((driver_failures + 1))
        fi
    done
    IFS="$old_ifs"

    grep -E 'Linux module initcall|PCI shell preflight|PCI BIND|Ethernet|AHCI|ata|pvpanic|bash-shell|nox#|\[panic\]|\[linux:error\]' "$log" | tail -n 140 || true

    if [ "$outcome" != "pass" ] || [ "$driver_failures" -ne 0 ]; then
        if [ -s "$qemu_log" ]; then
            echo "--- QEMU diagnostics ---"
            tail -n 40 "$qemu_log" || true
        fi
        return 1
    fi

    return 0
}

failures=0

# q35 contains an ICH9 AHCI function (8086:2922) even with -nic none, so this
# isolated case proves the exact upstream ahci.c can bind without another PCI
# function being added.
run_case ahci "8086:2922=ahci" || failures=$((failures + 1))

if device_available rtl8139; then
    run_case rtl8139 "8086:2922=ahci,10ec:8139=8139too" \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:61 || failures=$((failures + 1))
else
    echo "SKIP rtl8139: QEMU device unavailable"
fi

if device_available pvpanic-pci; then
    run_case pvpanic "8086:2922=ahci,1b36:0011=pvpanic-pci" \
        -device pvpanic-pci || failures=$((failures + 1))
else
    echo "SKIP pvpanic-pci: QEMU device unavailable"
fi

if device_available rtl8139 && device_available pvpanic-pci; then
    run_case all-upstream "8086:2922=ahci,10ec:8139=8139too,1b36:0011=pvpanic-pci" \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0,mac=52:54:00:12:34:62 \
        -device pvpanic-pci || failures=$((failures + 1))
else
    echo "SKIP all-upstream combined case: rtl8139 or pvpanic-pci unavailable"
fi

echo ""
echo "Upstream driver matrix logs: $LOG_DIR"
if [ "$failures" -ne 0 ]; then
    echo "$failures upstream matrix case(s) failed."
    exit 1
fi

echo "All currently integrated upstream drivers bound successfully in one kernel and reached Bash."
