# Building Nox

Nox currently targets x86_64 and uses the Twilight kernel with the Limine bootloader.

## Linux

Install the build tools:

```sh
sudo apt install clang lld make git curl xorriso qemu-system-x86
```

Build only Twilight:

```sh
make twilight
```

Build a bootable Nox ISO:

```sh
make
```

Run it in QEMU:

```sh
make run
```

## macOS

Install dependencies with Homebrew:

```sh
brew install llvm make xorriso qemu
```

The top-level Makefile automatically uses Homebrew's LLVM toolchain so the host Apple linker is never used for the x86_64 ELF kernel.

Build only Twilight:

```sh
make twilight
```

Build the ISO:

```sh
make
```

Run it:

```sh
make run
```

## Current bring-up target

A successful boot should open a graphics framebuffer, clear it to a near-black background, draw `Hello, World!` in a framebuffer-rendered console-style bitmap font, and halt.

Twilight does not use VGA text mode.
