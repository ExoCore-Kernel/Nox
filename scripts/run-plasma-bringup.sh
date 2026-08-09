#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build/plasma-bringup}"
ISO_ROOT="$BUILD_DIR/iso_root"
ISO="$BUILD_DIR/nox-plasma.iso"
ROOTFS="$BUILD_DIR/plasma-rootfs.cpio"
ROOTFS_KIND="${PLASMA_ROOTFS:-plasma-x11}"
PYTHON="${PYTHON:-python3}"
LIMINE="${LIMINE:-limine}"
QEMU="${QEMU:-qemu-system-x86_64}"
MODE="${1:-auto}"

if ! command -v xorriso >/dev/null 2>&1; then
    echo "error: missing xorriso" >&2
    exit 1
fi

make BUILD_DIR="$BUILD_DIR" \
    LINUX_USER_SELF_TEST=0 \
    BUSYBOX_SELF_TEST=1 \
    BASH_SHELL=1 \
    twilight limine

# GUI-only generated Linux ABI: read-only Alpine CPIO + getdents, writable
# /tmp and /run, pipes/AF_UNIX, direct fbdev mmap, dynamic ELF/musl, and
# cooperative multi-process scheduling. Normal Nox builds remain unchanged.
BASH_COMPAT_C="$BUILD_DIR/generated/linux/bash-shell-compat.c"
BASH_COMPAT_O="$BUILD_DIR/obj/generated/linux/bash-shell-compat.o"
"$PYTHON" scripts/add-rootfs-to-bash-compat.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-bash-compat.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-getdents.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-file-mmap.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-runtime-ipc.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-fbdev-v2.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-execve.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-execve.py "$BASH_COMPAT_C"
"$PYTHON" scripts/normalize-plasma-process-input.py "$BASH_COMPAT_C"
"$PYTHON" scripts/add-plasma-processes.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-scheduler.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-process-stack.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-runtime.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-low-fds.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-cooperative-io.py "$BASH_COMPAT_C"
"$PYTHON" scripts/finalize-plasma-generated-c.py "$BASH_COMPAT_C"
rm -f "$BASH_COMPAT_O" "$BUILD_DIR/twilight.elf"
make BUILD_DIR="$BUILD_DIR" \
    LINUX_USER_SELF_TEST=0 \
    BUSYBOX_SELF_TEST=1 \
    BASH_SHELL=1 \
    twilight

case "$ROOTFS_KIND" in
    tiny)
        echo "Plasma rootfs mode: tiny CPIO protocol sanity test"
        "$PYTHON" scripts/make-plasma-rootfs.py "$ROOTFS"
        ;;
    alpine)
        echo "Plasma rootfs mode: Alpine 3.24.1 x86_64 diagnostic minirootfs"
        "$PYTHON" scripts/fetch-alpine-plasma-base.py "$ROOTFS"
        ;;
    plasma-x11)
        echo "Plasma rootfs mode: Alpine 3.21.7 + Plasma 6.2 + Xorg fbdev"
        if [ "$(uname -s)" = "Darwin" ]; then
            echo "macOS rootfs backend: native apko (no Docker daemon required)"
            "$PYTHON" scripts/fetch-alpine-plasma-x11-apko.py "$ROOTFS"
        else
            "$PYTHON" scripts/fetch-alpine-plasma-x11.py "$ROOTFS"
        fi
        ;;
    *)
        echo "error: PLASMA_ROOTFS must be 'tiny', 'alpine', or 'plasma-x11'" >&2
        exit 2
        ;;
esac

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"
cp "$BUILD_DIR/twilight.elf" "$ISO_ROOT/boot/twilight.elf"
cp "$ROOTFS" "$ISO_ROOT/boot/plasma-rootfs.cpio"
cp limine-plasma.conf "$ISO_ROOT/boot/limine/limine.conf"
cp limine-binary/limine-bios.sys "$ISO_ROOT/boot/limine/"
cp limine-binary/limine-bios-cd.bin "$ISO_ROOT/boot/limine/"
cp limine-binary/limine-uefi-cd.bin "$ISO_ROOT/boot/limine/"
cp limine-binary/BOOTX64.EFI "$ISO_ROOT/EFI/BOOT/"

xorriso -as mkisofs \
    -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$ISO"

"$LIMINE" bios-install "$ISO"

echo ""
echo "Plasma bring-up ISO: $ISO"
echo "Rootfs mode: $ROOTFS_KIND"
echo "Expected early proof:"
echo "  [linux] Plasma rootfs mounted from Limine module: ..."
echo "  [linux] Plasma ELF gate PASS: loader=..."
if [ "$ROOTFS_KIND" = "plasma-x11" ]; then
    echo "GUI stack included: Alpine 3.21.7, Plasma 6.2, Xorg fbdev, D-Bus"
    echo "At nox#, enter the real Alpine shell:"
    echo "  exec /bin/busybox sh -i"
    echo "Then run:"
    echo "  ls /usr/bin"
    echo "  /usr/bin/Xorg -version"
    echo "  /usr/bin/Xorg :0 -retro -nolisten tcp -novtswitch -sharevts -logfile /dev/null"
    echo "Expected framebuffer proof when Xorg maps video memory:"
    echo "  [linux:fbdev] mapped /dev/fb0 into userspace"
    echo "After Xorg itself runs, the actual Plasma session target is:"
    echo "  DISPLAY=:0 XDG_RUNTIME_DIR=/tmp/runtime-root dbus-run-session startplasma-x11"
    echo "Use 'gui' mode so the QEMU display is visible."
fi
echo ""

QEMU="$QEMU" QEMU_EXTRA_ARGS="-m 3072M ${QEMU_EXTRA_ARGS:-}" \
    sh scripts/run-qemu.sh "$MODE" pc "$ISO"
