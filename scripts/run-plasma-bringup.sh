#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build/plasma-bringup}"
ISO_ROOT="$BUILD_DIR/iso_root"
ISO="$BUILD_DIR/nox-plasma.iso"
ROOTFS="$BUILD_DIR/plasma-rootfs.cpio"
ROOTFS_KIND="${PLASMA_ROOTFS:-tiny}"
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

# Plasma bring-up extends only the generated Bash ABI unit for now: real
# read-only rootfs open/read/stat/lseek/close calls. Normal Bash/driver builds
# remain untouched. Rebuild just that object and relink the kernel afterward.
BASH_COMPAT_C="$BUILD_DIR/generated/linux/bash-shell-compat.c"
BASH_COMPAT_O="$BUILD_DIR/obj/generated/linux/bash-shell-compat.o"
"$PYTHON" scripts/add-rootfs-to-bash-compat.py "$BASH_COMPAT_C"
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
        echo "Plasma rootfs mode: real Alpine 3.24.1 x86_64 minirootfs"
        "$PYTHON" scripts/fetch-alpine-plasma-base.py "$ROOTFS"
        ;;
    *)
        echo "error: PLASMA_ROOTFS must be 'tiny' or 'alpine'" >&2
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
echo "  [linux] Plasma rootfs probe PASS: /etc/nox-release is readable ..."
echo "After Bash starts, test userspace rootfs I/O with:"
echo "  source /etc/nox-release; echo \"rootfs=$NAME kernel=$KERNEL stage=$USERSPACE_STAGE\""
echo ""

# Plasma itself will need substantially more than 512 MiB. Supplying a second
# -m option here intentionally overrides run-qemu.sh's conservative default.
QEMU="$QEMU" QEMU_EXTRA_ARGS="-m 2048M ${QEMU_EXTRA_ARGS:-}" \
    sh scripts/run-qemu.sh "$MODE" pc "$ISO"
