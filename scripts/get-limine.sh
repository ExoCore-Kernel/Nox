#!/bin/sh
set -eu

LIMINE_DIR="limine-binary"
LIMINE_REPO="https://github.com/Limine-Bootloader/Limine.git"
LIMINE_BRANCH="v12.x"

if [ ! -d "$LIMINE_DIR" ]; then
    echo "Fetching Limine boot files ($LIMINE_BRANCH)..."
    git clone --depth=1 --branch="$LIMINE_BRANCH" "$LIMINE_REPO" "$LIMINE_DIR"
fi

for f in limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI; do
    if [ ! -f "$LIMINE_DIR/$f" ]; then
        echo "error: missing Limine boot file: $LIMINE_DIR/$f" >&2
        exit 1
    fi
done

if ! command -v limine >/dev/null 2>&1; then
    if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
        echo "Installing Limine host utility with Homebrew..."
        brew install limine
    else
        echo "error: host 'limine' utility not found in PATH" >&2
        echo "Install Limine 12.x, then rerun make run." >&2
        exit 1
    fi
fi

echo "Limine boot files + host utility ready."
