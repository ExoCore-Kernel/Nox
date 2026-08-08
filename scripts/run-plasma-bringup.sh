#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build/plasma-bringup}"
ISO_ROOT="$BUILD_DIR/iso_root"
ISO="$BUILD_DIR/nox-plasma.iso"
ROOTFS="$BUILD_DIR/plasma-rootfs.cpio"
PYTHON="${PYTHON:-python3}"
LIMINE="${LIMINE:-limine}"
QEMU="${QEMU:-qemu-system-x86_64}"
MODE="${1:-auto}"

if ! command -v xorriso >/dev/null 2>&1; then
    echo "error: missing xorriso" >&2
    exit 1
fi

# Keep the already-proven static Bash Linux ABI as our diagnostic console while
# the real filesystem/dynamic-loader/process pieces needed by Plasma are added.
make BUILD_DIR="$BUILD_DIR" \
    LINUX_USER_SELF_TEST=0 \
    BUSYBOX_SELF_TEST=1 \
    BASH_SHELL=1 \
    twilight limine

"$PYTHON" scripts/make-plasma-rootfs.py "$ROOTFS"

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
echo "Expected early proof:"
echo "  [linux] Plasma rootfs mounted from Limine module: ..."
echo "  [linux] Plasma rootfs probe PASS: /etc/nox-release is readable ..."
echo ""

# Plasma itself will need substantially more than 512 MiB. Supplying a second
# -m option here intentionally overrides run-qemu.sh's conservative default.
QEMU="$QEMU" QEMU_EXTRA_ARGS="-m 2048M ${QEMU_EXTRA_ARGS:-}" \
    sh scripts/run-qemu.sh "$MODE" pc "$ISO"
