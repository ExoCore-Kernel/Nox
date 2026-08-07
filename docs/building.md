# Building Nox / Twilight

Twilight is a freestanding x86_64 kernel. The host build is designed to work on macOS and Linux using Clang/LLVM.

## Dependencies

Required for `make twilight`:

- `clang`
- `ld.lld`
- `python3`
- `git`
- internet access on the first build, to fetch the console font

Additional tools for `make iso` and `make run`:

- `xorriso`
- `qemu-system-x86_64`
- a normal host C toolchain and `make` so the Limine host utility can be built

### macOS

With Homebrew:

```sh
brew install llvm lld xorriso qemu
```

If Homebrew's LLVM binaries are not on your PATH, add its LLVM `bin` directory before building.

### Ubuntu / Debian

```sh
sudo apt update
sudo apt install clang lld python3 git make build-essential xorriso qemu-system-x86
```

## Build targets

```sh
make twilight
```

Builds `build/twilight.elf`.

```sh
make iso
```

Fetches/builds Limine if necessary and creates `build/nox.iso`.

```sh
make run
```

Builds the ISO and boots it with QEMU's `q35` machine.

```sh
make clean
```

Removes generated build output. The next clean build will fetch the console font again.

## Current boot result

At this stage Twilight:

1. enters through the Limine protocol,
2. checks the Limine base revision,
3. obtains the first 32-bit linear framebuffer,
4. clears it to a dark background,
5. initializes a PSF console font,
6. renders `Hello, World!` into framebuffer pixels,
7. halts the CPU.

There is no VGA text-mode dependency.
