#!/bin/sh
set -eu

MODE="${1:-auto}"
MACHINE="${2:-pc}"
ISO="${3:-build/nox.iso}"
QEMU_BIN="${QEMU:-qemu-system-x86_64}"
SWTPM_BIN="${SWTPM:-swtpm}"
TPM_STATE_DIR="${TPM_STATE_DIR:-.nox-tpm-state}"
TMP_BASE="${TMPDIR:-/tmp}"
TPM_SOCKET="${TPM_SOCKET:-${TMP_BASE%/}/nox-swtpm-${UID:-user}.sock}"
TPM_LOG="${TPM_LOG:-${TPM_STATE_DIR}/swtpm.log}"

if ! command -v "$SWTPM_BIN" >/dev/null 2>&1; then
    echo "error: missing swtpm" >&2
    echo "Install it first (macOS/Homebrew: brew install swtpm)." >&2
    exit 1
fi

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "error: missing $QEMU_BIN" >&2
    exit 1
fi

if [ ! -f "$ISO" ]; then
    echo "error: ISO not found: $ISO" >&2
    exit 1
fi

mkdir -p "$TPM_STATE_DIR"
rm -f "$TPM_SOCKET"

TPM_PID=""
cleanup() {
    if [ -n "$TPM_PID" ]; then
        kill "$TPM_PID" >/dev/null 2>&1 || true
        wait "$TPM_PID" >/dev/null 2>&1 || true
    fi
    rm -f "$TPM_SOCKET"
}
trap cleanup EXIT HUP INT TERM

echo "Starting persistent software TPM 2.0"
echo "TPM state: $TPM_STATE_DIR"
echo "TPM frontend: QEMU CRB"

"$SWTPM_BIN" socket \
    --tpm2 \
    --tpmstate dir="$TPM_STATE_DIR" \
    --ctrl type=unixio,path="$TPM_SOCKET" \
    --log file="$TPM_LOG",level=5 &
TPM_PID=$!

# Wait for swtpm to create its control socket before QEMU connects.
ready=0
i=0
while [ "$i" -lt 100 ]; do
    if [ -S "$TPM_SOCKET" ]; then
        ready=1
        break
    fi
    if ! kill -0 "$TPM_PID" >/dev/null 2>&1; then
        echo "error: swtpm exited before creating its control socket" >&2
        echo "See: $TPM_LOG" >&2
        exit 1
    fi
    sleep 0.05
    i=$((i + 1))
done

if [ "$ready" -ne 1 ]; then
    echo "error: timed out waiting for swtpm socket: $TPM_SOCKET" >&2
    echo "See: $TPM_LOG" >&2
    exit 1
fi

# Fail early with a useful message if this QEMU build has no TPM emulator
# backend or CRB frontend. Older/minimal host packages can omit TPM support.
if ! "$QEMU_BIN" -tpmdev help 2>&1 | grep -q 'emulator'; then
    echo "error: this QEMU build does not provide the TPM emulator backend" >&2
    exit 1
fi
if ! "$QEMU_BIN" -device help 2>&1 | grep -q 'tpm-crb'; then
    echo "error: this QEMU build does not provide the tpm-crb frontend" >&2
    exit 1
fi

TPM_QEMU_ARGS="-chardev socket,id=chrtpm,path=$TPM_SOCKET -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-crb,tpmdev=tpm0"

echo "Launching Twilight with emulated TPM 2.0..."
QEMU="$QEMU_BIN" QEMU_EXTRA_ARGS="$TPM_QEMU_ARGS" \
    sh scripts/run-qemu.sh "$MODE" "$MACHINE" "$ISO"
