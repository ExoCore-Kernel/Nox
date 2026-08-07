#!/bin/sh
set -eu

LIMINE_DIR="limine-binary"
LIMINE_REPO="https://github.com/Limine-Bootloader/Limine.git"
LIMINE_BRANCH="v12.x-binary"

if [ -f "$LIMINE_DIR/limine" ] && [ -f "$LIMINE_DIR/limine-bios-cd.bin" ]; then
    exit 0
fi

if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required to fetch Limine" >&2
    exit 1
fi

rm -rf "$LIMINE_DIR"
echo "Fetching Limine ($LIMINE_BRANCH)..."
git clone --depth=1 --branch="$LIMINE_BRANCH" "$LIMINE_REPO" "$LIMINE_DIR"

# The binary branch contains the boot files, but the host-side `limine`
# utility still needs to be built for the machine doing the build.
make -C "$LIMINE_DIR"
