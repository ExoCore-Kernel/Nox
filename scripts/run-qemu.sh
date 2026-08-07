#!/bin/sh
set -eu

MODE="${1:-auto}"
MACHINE="${2:-pc}"
ISO="${3:-build/nox.iso}"
QEMU_BIN="${QEMU:-qemu-system-x86_64}"

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "error: missing $QEMU_BIN" >&2
    exit 1
fi

if [ ! -f "$ISO" ]; then
    echo "error: ISO not found: $ISO" >&2
    exit 1
fi

has_graphical_session() {
    case "$(uname -s)" in
        Darwin)
            # macOS terminals normally have no DISPLAY variable, so inspect the
            # active console session instead. loginwindow/root means no logged-in
            # graphical desktop is available.
            console_user="$(stat -f '%Su' /dev/console 2>/dev/null || true)"
            if [ -n "$console_user" ] && \
               [ "$console_user" != "root" ] && \
               [ "$console_user" != "loginwindow" ]; then
                return 0
            fi
            return 1
            ;;
        Linux)
            # X11 and Wayland both advertise a display through these variables.
            [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]
            ;;
        *)
            [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]
            ;;
    esac
}

case "$MODE" in
    auto)
        if has_graphical_session; then
            MODE="gui"
        else
            MODE="headless"
        fi
        ;;
    gui|headless)
        ;;
    *)
        echo "error: unknown QEMU display mode '$MODE' (expected auto, gui, or headless)" >&2
        exit 1
        ;;
esac

# Explicitly expose x2APIC in the development VM. Twilight still starts with
# the legacy 8259 PIC during bring-up, but can use x2APIC virtual-wire ExtINT
# as a compatibility bridge when direct PIC->CPU routing is unavailable.
COMMON_ARGS="-M $MACHINE -cpu qemu64,+x2apic -m 512M -cdrom $ISO -serial stdio -monitor none -no-reboot -no-shutdown"

if [ "$MODE" = "gui" ]; then
    echo "QEMU display: graphical session detected; opening display window"
    # Intentional word splitting: COMMON_ARGS contains QEMU's individual argv.
    # shellcheck disable=SC2086
    exec "$QEMU_BIN" $COMMON_ARGS
else
    echo "QEMU display: no graphical session detected; serial-only headless mode"
    echo "(Override with 'make run-gui' if auto-detection is wrong.)"
    # -display none leaves the emulated video hardware available to Twilight,
    # but creates no host-side graphical window. Serial remains on this terminal.
    # shellcheck disable=SC2086
    exec "$QEMU_BIN" $COMMON_ARGS -display none
fi
