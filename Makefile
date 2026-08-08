SHELL := /bin/sh

CC := clang
LD := ld.lld
PYTHON := python3
QEMU := qemu-system-x86_64
LIMINE := limine
PANIC_SELF_TEST ?= 0
PMM_SELF_TEST ?= 1
VMM_SELF_TEST ?= 1
HEAP_SELF_TEST ?= 1
USERMODE_SELF_TEST ?= 1
LINUX_USER_SELF_TEST ?= 1
BUSYBOX_SELF_TEST ?= 0
BASH_SHELL ?= 0
LINUX_COMPAT_SELF_TEST ?= 1
SCROLL_SELF_TEST ?= 0
STORAGE_SELF_TEST ?= 0
UPSTREAM_8139 ?= 0
UPSTREAM_AHCI ?= 0
TPM_STATE_DIR ?= .nox-tpm-state
TPM_MODE ?= auto

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/generated
ISO_ROOT := $(BUILD_DIR)/iso_root
KERNEL := $(BUILD_DIR)/twilight.elf
ISO := $(BUILD_DIR)/nox.iso
LIMINE_DIR := limine-binary
FONT_C := $(GEN_DIR)/font_blob.c
VERSION_C := $(GEN_DIR)/version_blob.c
LINUX_HELLO_O := $(GEN_DIR)/linux/hello-linux.o
LINUX_HELLO_ELF := $(GEN_DIR)/linux/hello-linux.elf
LINUX_HELLO_C := $(GEN_DIR)/linux/hello-linux-blob.c
LINUX_HELLO_BLOB_O := $(OBJ_DIR)/generated/linux/hello-linux-blob.o
BUSYBOX_BIN := $(GEN_DIR)/linux/busybox-1.35.0-x86_64-linux-musl
BUSYBOX_C := $(GEN_DIR)/linux/busybox-blob.c
BUSYBOX_BLOB_O := $(OBJ_DIR)/generated/linux/busybox-blob.o
BUSYBOX_TEST_BUILD_DIR := build/busybox
BUSYBOX_TEST_ISO := $(BUSYBOX_TEST_BUILD_DIR)/nox.iso
BASH_BIN := $(GEN_DIR)/linux/bash-static-5.3-amd64
BASH_C := $(GEN_DIR)/linux/bash-blob.c
BASH_BLOB_O := $(OBJ_DIR)/generated/linux/bash-blob.o
BASH_SHELL_C := $(GEN_DIR)/linux/bash-shell-compat.c
BASH_SHELL_O := $(OBJ_DIR)/generated/linux/bash-shell-compat.o
BASH_TEST_BUILD_DIR := build/bash
BASH_TEST_ISO := $(BASH_TEST_BUILD_DIR)/nox.iso
UPSTREAM_8139_C := $(GEN_DIR)/upstream/8139too.c
UPSTREAM_8139_O := $(OBJ_DIR)/generated/upstream/8139too.o
UPSTREAM_TEST_BUILD_DIR := build/upstream-8139
UPSTREAM_TEST_ISO := $(UPSTREAM_TEST_BUILD_DIR)/nox.iso
UPSTREAM_AHCI_C := $(GEN_DIR)/upstream/ahci.c
UPSTREAM_AHCI_O := $(OBJ_DIR)/generated/upstream/ahci.o
UPSTREAM_AHCI_TEST_BUILD_DIR := build/upstream-ahci
UPSTREAM_AHCI_TEST_ISO := $(UPSTREAM_AHCI_TEST_BUILD_DIR)/nox.iso
UPSTREAM_AHCI_TEST_DISK := $(UPSTREAM_AHCI_TEST_BUILD_DIR)/ahci-test.img

C_SOURCES := $(shell find twilight -type f -name '*.c' ! -path 'twilight/src/*' -print)
ifeq ($(UPSTREAM_8139),1)
# The strict upstream-driver test compiles Linux's untouched 8139too.c instead
# of Twilight's compatibility port, so only one driver can claim 10ec:8139.
C_SOURCES := $(filter-out twilight/drivers/linux/8139too.c,$(C_SOURCES))
endif
ifeq ($(BASH_SHELL),1)
# Reuse the proven Linux ABI shell implementation through a generated Bash-
# identity compatibility unit, so only one router implementation is linked.
C_SOURCES := $(filter-out twilight/kernel/busybox_shell.c,$(C_SOURCES))
endif
ASM_SOURCES := $(shell find twilight -type f -name '*.S' -print)
C_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.S,$(OBJ_DIR)/%.S.o,$(ASM_SOURCES))
OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS)
OBJECTS += $(OBJ_DIR)/generated/font_blob.o
OBJECTS += $(OBJ_DIR)/generated/version_blob.o
ifeq ($(LINUX_USER_SELF_TEST),1)
OBJECTS += $(LINUX_HELLO_BLOB_O)
endif
ifeq ($(BUSYBOX_SELF_TEST),1)
ifeq ($(BASH_SHELL),1)
OBJECTS += $(BASH_BLOB_O) $(BASH_SHELL_O)
else
OBJECTS += $(BUSYBOX_BLOB_O)
endif
endif
ifeq ($(UPSTREAM_8139),1)
OBJECTS += $(UPSTREAM_8139_O)
endif
ifeq ($(UPSTREAM_AHCI),1)
OBJECTS += $(UPSTREAM_AHCI_O)
endif

CFLAGS := \
	-target x86_64-unknown-none-elf \
	-std=gnu11 \
	-O2 \
	-Wall -Wextra -Wpedantic \
	-ffreestanding \
	-fno-builtin \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-lto \
	-fno-pic \
	-fno-pie \
	-m64 \
	-march=x86-64 \
	-mno-red-zone \
	-mno-mmx \
	-mno-sse \
	-mno-sse2 \
	-mcmodel=kernel \
	-DTWILIGHT_PANIC_SELF_TEST=$(PANIC_SELF_TEST) \
	-DTWILIGHT_PMM_SELF_TEST=$(PMM_SELF_TEST) \
	-DTWILIGHT_VMM_SELF_TEST=$(VMM_SELF_TEST) \
	-DTWILIGHT_HEAP_SELF_TEST=$(HEAP_SELF_TEST) \
	-DTWILIGHT_USERMODE_SELF_TEST=$(USERMODE_SELF_TEST) \
	-DTWILIGHT_LINUX_USER_SELF_TEST=$(LINUX_USER_SELF_TEST) \
	-DTWILIGHT_BUSYBOX_SELF_TEST=$(BUSYBOX_SELF_TEST) \
	-DTWILIGHT_LINUX_COMPAT_SELF_TEST=$(LINUX_COMPAT_SELF_TEST) \
	-DTWILIGHT_SCROLL_SELF_TEST=$(SCROLL_SELF_TEST) \
	-DTWILIGHT_STORAGE_SELF_TEST=$(STORAGE_SELF_TEST) \
	-Itwilight/include

ASFLAGS := \
	-target x86_64-unknown-none-elf \
	-ffreestanding \
	-m64 \
	-march=x86-64 \
	-mno-red-zone \
	-DTWILIGHT_BUSYBOX_SELF_TEST=$(BUSYBOX_SELF_TEST)

LDFLAGS := \
	-m elf_x86_64 \
	-nostdlib \
	-static \
	-z max-page-size=0x1000 \
	-T twilight/linker.ld

.PHONY: all twilight font iso run run-gui run-headless run-q35 run-busybox-test run-bash-test run-driver-test run-linux-driver-test run-ethernet-test run-upstream-ethernet-test run-upstream-ahci-test run-tpm tpm-reset limine clean check-tools new-it FORCE_VERSION

all: twilight

twilight: check-tools $(KERNEL)

font: $(FONT_C)

new-it:
	$(PYTHON) scripts/bump-build.py
	@rm -f $(VERSION_C) $(OBJ_DIR)/generated/version_blob.o $(KERNEL) $(ISO)

$(FONT_C): scripts/embed-font.py
	@mkdir -p $(GEN_DIR)
	$(PYTHON) scripts/embed-font.py $@

FORCE_VERSION:

$(VERSION_C): FORCE_VERSION scripts/gen-version.py twilight/build-number.txt
	@mkdir -p $(GEN_DIR)
	$(PYTHON) scripts/gen-version.py $@

# Build a genuine freestanding x86-64 Linux ELF. The finished ELF is then
# embedded only as transport for the early loader test; Twilight still parses
# and maps it as an ELF executable at runtime.
$(LINUX_HELLO_O): tests/linux/hello.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-linux-gnu -c $< -o $@

$(LINUX_HELLO_ELF): $(LINUX_HELLO_O) tests/linux/hello.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -static -T tests/linux/hello.ld -o $@ $(LINUX_HELLO_O)
	@echo "Built Linux userspace test ELF: $@"

$(LINUX_HELLO_C): $(LINUX_HELLO_ELF) scripts/embed-linux-elf.py
	$(PYTHON) scripts/embed-linux-elf.py $(LINUX_HELLO_ELF) $@

$(LINUX_HELLO_BLOB_O): $(LINUX_HELLO_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# BusyBox is not rebuilt or patched: fetch BusyBox.org's official static
# x86_64-musl executable and embed those exact ELF bytes as temporary transport.
$(BUSYBOX_BIN): scripts/fetch-busybox.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/fetch-busybox.py $@

$(BUSYBOX_C): $(BUSYBOX_BIN) scripts/embed-linux-elf.py
	$(PYTHON) scripts/embed-linux-elf.py $(BUSYBOX_BIN) $@ twilight_busybox_elf

$(BUSYBOX_BLOB_O): $(BUSYBOX_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# GNU Bash is a separate Linux executable. Debian's bash-static package gives
# us an x86_64 ET_EXEC with no dynamic interpreter, which fits the current
# Twilight ELF loader while keeping the Bash executable bytes unchanged.
$(BASH_BIN): scripts/fetch-bash-static.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/fetch-bash-static.py $@

$(BASH_C): $(BASH_BIN) scripts/embed-linux-elf.py
	$(PYTHON) scripts/embed-linux-elf.py $(BASH_BIN) $@ twilight_busybox_elf

$(BASH_BLOB_O): $(BASH_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BASH_SHELL_C): twilight/kernel/busybox_shell.c scripts/make-bash-shell-compat.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/make-bash-shell-compat.py twilight/kernel/busybox_shell.c $@

$(BASH_SHELL_O): $(BASH_SHELL_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(UPSTREAM_8139_C): scripts/fetch-linux-8139too.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/fetch-linux-8139too.py $@

$(UPSTREAM_8139_O): $(UPSTREAM_8139_C)
	@mkdir -p $(dir $@)
	@echo "Compiling exact upstream Linux v2.6.24 8139too.c (source remains unmodified)"
	$(CC) $(CFLAGS) -DKBUILD_MODNAME=\"8139too\" -c $< -o $@

$(UPSTREAM_AHCI_C): scripts/fetch-linux-ahci.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/fetch-linux-ahci.py $@

$(UPSTREAM_AHCI_O): $(UPSTREAM_AHCI_C)
	@mkdir -p $(dir $@)
	@echo "Compiling exact upstream Linux v2.6.24 ahci.c (source remains unmodified)"
	$(CC) $(CFLAGS) -DKBUILD_MODNAME=\"ahci\" -c $< -o $@

$(KERNEL): $(OBJECTS) twilight/linker.ld | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@
	@echo "Built $(KERNEL)"

$(OBJ_DIR)/generated/font_blob.o: $(FONT_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/generated/version_blob.o: $(VERSION_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $@

limine:
	sh scripts/get-limine.sh

iso: check-tools $(KERNEL) limine
	@command -v xorriso >/dev/null || { echo "Missing xorriso"; exit 1; }
	@command -v $(LIMINE) >/dev/null || { echo "Missing limine host utility"; exit 1; }
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/twilight.elf
	cp limine.conf $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs \
		-R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $(ISO)
	$(LIMINE) bios-install $(ISO)
	@echo "Built $(ISO)"

run: iso
	@echo "Running Twilight on QEMU pc/i440FX (auto display detection)"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh auto pc $(ISO)

run-gui: iso
	@echo "Running Twilight on QEMU pc/i440FX (forced graphical display)"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh gui pc $(ISO)

run-headless: iso
	@echo "Running Twilight on QEMU pc/i440FX (forced serial-only mode)"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh headless pc $(ISO)

run-q35: iso
	@echo "Running Twilight on QEMU q35 (auto display detection; APIC/IOAPIC support still recommended)"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh auto q35 $(ISO)

# Official, unmodified BusyBox.org x86_64-musl binary. This isolated build
# routes native SYSCALL into the broader BusyBox ABI dispatcher rather than the
# tiny hello-world dispatcher.
run-busybox-test:
	@$(MAKE) BUILD_DIR=$(BUSYBOX_TEST_BUILD_DIR) LINUX_USER_SELF_TEST=0 BUSYBOX_SELF_TEST=1 iso
	@echo "Running Twilight with official UNMODIFIED BusyBox 1.35.0 x86_64-musl"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh auto pc $(BUSYBOX_TEST_ISO)

# Boot a real statically linked GNU Bash Linux executable through the same
# native x86_64 Linux ABI and serial/PS2 TTY that already runs BusyBox ash.
run-bash-test:
	@$(MAKE) BUILD_DIR=$(BASH_TEST_BUILD_DIR) LINUX_USER_SELF_TEST=0 BUSYBOX_SELF_TEST=1 BASH_SHELL=1 iso
	@echo "Running Twilight with GNU Bash 5.3 static x86_64 Linux ELF as /bin/bash -i"
	@QEMU="$(QEMU)" sh scripts/run-qemu.sh auto pc $(BASH_TEST_ISO)

run-driver-test: iso
	@echo "Running Twilight with QEMU pci-testdev for Linux-driver compatibility testing"
	@QEMU="$(QEMU)" QEMU_EXTRA_ARGS="-device pci-testdev" sh scripts/run-qemu.sh auto pc $(ISO)

run-linux-driver-test: iso
	@echo "Running Twilight with QEMU pvpanic-pci and the upstream Linux pvpanic PCI driver"
	@QEMU="$(QEMU)" QEMU_EXTRA_ARGS="-device pvpanic-pci" sh scripts/run-qemu.sh auto pc $(ISO)

run-ethernet-test: iso
	@echo "Running Twilight with QEMU RTL8139 + user-mode Ethernet for Linux 8139too bring-up"
	@QEMU="$(QEMU)" QEMU_EXTRA_ARGS="-netdev user,id=noxnet -device rtl8139,netdev=noxnet,mac=52:54:00:12:34:56" sh scripts/run-qemu.sh auto pc $(ISO)

# Strict compatibility test: build into an isolated tree so normal and
# upstream-driver kernels can never be mistaken for each other by Make.
run-upstream-ethernet-test:
	@$(MAKE) BUILD_DIR=$(UPSTREAM_TEST_BUILD_DIR) UPSTREAM_8139=1 iso
	@echo "Running Twilight with UNMODIFIED upstream Linux v2.6.24 8139too.c"
	@QEMU="$(QEMU)" QEMU_EXTRA_ARGS="-netdev user,id=noxnet -device rtl8139,netdev=noxnet,mac=52:54:00:12:34:56" sh scripts/run-qemu.sh auto pc $(UPSTREAM_TEST_ISO)

# Strict AHCI compatibility test. The Linux source is hash-pinned and compiled
# unchanged; Twilight supplies the libata compatibility boundary around it.
# STORAGE_SELF_TEST=1 enables a destructive write/read check only on this
# generated disposable disk; normal builds leave automatic writes disabled.
run-upstream-ahci-test:
	@$(MAKE) BUILD_DIR=$(UPSTREAM_AHCI_TEST_BUILD_DIR) UPSTREAM_AHCI=1 STORAGE_SELF_TEST=1 iso
	@$(PYTHON) scripts/make-ahci-test-disk.py $(UPSTREAM_AHCI_TEST_DISK)
	@echo "Running Twilight with UNMODIFIED upstream Linux v2.6.24 ahci.c + QEMU ICH9 AHCI"
	@QEMU="$(QEMU)" QEMU_EXTRA_ARGS="-boot d -device ich9-ahci,id=ahci -drive if=none,id=ahcidisk,format=raw,file=$(UPSTREAM_AHCI_TEST_DISK) -device ide-hd,drive=ahcidisk,bus=ahci.0" sh scripts/run-qemu.sh auto pc $(UPSTREAM_AHCI_TEST_ISO)

run-tpm: iso
	@echo "Running Twilight with persistent emulated TPM 2.0 (CRB frontend)"
	@QEMU="$(QEMU)" TPM_STATE_DIR="$(TPM_STATE_DIR)" sh scripts/run-qemu-tpm.sh "$(TPM_MODE)" pc $(ISO)

tpm-reset:
	@echo "Resetting emulated TPM state: $(TPM_STATE_DIR)"
	rm -rf "$(TPM_STATE_DIR)"

check-tools:
	@command -v $(CC) >/dev/null || { echo "Missing clang"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "Missing ld.lld"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "Missing python3"; exit 1; }
	@echo "Toolchain ready: clang + ld.lld + python3"

clean:
	rm -rf build