#!/bin/sh
set -eu

LIMINE_DIR="limine-binary"
LIMINE_REPO="https://github.com/Limine-Bootloader/Limine.git"
LIMINE_BRANCH="v12.x"

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

# Limine's v12.x branch is already a binary distribution.
# There is no Makefile to run here; the required boot files and
# host-side limine utility are shipped directly in the branch.

if [ ! -f "$LIMINE_DIR/limine" ]; then
    echo "error: Limine utility not found in $LIMINE_DIR" >&2
    exit 1
fi

chmod +x "$LIMINE_DIR/limine" 2>/dev/null || true
