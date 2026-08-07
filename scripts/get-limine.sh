#!/bin/sh
set -eu

LIMINE_DIR="limine-binary"

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

rm -rf "$LIMINE_DIR"
mkdir -p "$LIMINE_DIR"

if command -v brew >/dev/null 2>&1 && brew --prefix limine >/dev/null 2>&1; then
    LIMINE_PREFIX="$(brew --prefix limine)"
else
    LIMINE_PREFIX="$(dirname "$(dirname "$(command -v limine)")")"
fi

find_boot_file() {
    name="$1"
    for path in \
        "$LIMINE_PREFIX/share/$name" \
        "$LIMINE_PREFIX/share/limine/$name"; do
        if [ -f "$path" ]; then
            printf '%s\n' "$path"
            return 0
        fi
    done

    found="$(find "$LIMINE_PREFIX/share" -type f -name "$name" -print -quit 2>/dev/null || true)"
    if [ -n "$found" ]; then
        printf '%s\n' "$found"
        return 0
    fi

    return 1
}

for f in limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI; do
    src="$(find_boot_file "$f" || true)"
    if [ -z "$src" ]; then
        echo "error: could not locate $f under $LIMINE_PREFIX/share" >&2
        exit 1
    fi
    cp "$src" "$LIMINE_DIR/$f"
done

echo "Limine host utility + boot files ready from $LIMINE_PREFIX."
