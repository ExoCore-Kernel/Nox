# Nox

**Nox** is a Unix-like operating system built around the independently structured **Twilight** kernel.

The project is currently at the x86_64 bring-up stage. Twilight boots through Limine, requests a 32-bit linear framebuffer, loads a generated Terminus-family PSF console font, draws `Hello, World!` directly into the framebuffer, and halts.

## Repository split

- `twilight/` — the Twilight kernel
- `scripts/` — host-side build helpers
- `docs/` — Nox/Twilight documentation
- `limine.conf` — Nox boot configuration

Twilight is deliberately kept separate from future Nox userspace so the kernel can remain independently buildable.

## Quick start

```sh
make twilight   # build build/twilight.elf
make iso        # build build/nox.iso
make run        # boot Nox in QEMU
```

See `docs/building.md` for macOS and Linux dependencies and setup.
