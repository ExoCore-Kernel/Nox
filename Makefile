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
LINUX_COMPAT_SELF_TEST ?= 1
SCROLL_SELF_TEST ?= 0
UPSTREAM_8139 ?= 0
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
UPSTREAM_8139_C := $(GEN_DIR)/upstream/8139too.c
UPSTREAM_8139_O := $(OBJ_DIR)/generated/upstream/8139too.o
UPSTREAM_TEST_BUILD_DIR := build/upstream-8139
UPSTREAM_TEST_ISO := $(UPSTREAM_TEST_BUILD_DIR)/nox.iso

C_SOURCES := $(shell find twilight -type f -name '*.c' ! -path 'twilight/src/*' -print)
ifeq ($(UPSTREAM_8139),1)
# The strict upstream-driver test compiles Linux's untouched 8139too.c instead
# of Twilight's compatibility port, so only one driver can claim 10ec:8139.
C_SOURCES := $(filter-out twilight/drivers/linux/8139too.c,$(C_SOURCES))
endif
ASM_SOURCES := $(shell find twilight -type f -name '*.S' -print)
C_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.S,$(OBJ_DIR)/%.S.o,$(ASM_SOURCES))
OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS)
OBJECTS += $(OBJ_DIR)/generated/font_blob.o
OBJECTS += $(OBJ_DIR)/generated/version_blob.o
ifeq ($(UPSTREAM_8139),1)
OBJECTS += $(UPSTREAM_8139_O)
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
	-DTWILIGHT_LINUX_COMPAT_SELF_TEST=$(LINUX_COMPAT_SELF_TEST) \
	-DTWILIGHT_SCROLL_SELF_TEST=$(SCROLL_SELF_TEST) \
	-Itwilight/include

ASFLAGS := \
	-target x86_64-unknown-none-elf \
	-ffreestanding \
	-m64 \
	-march=x86-64 \
	-mno-red-zone

LDFLAGS := \
	-m elf_x86_64 \
	-nostdlib \
	-static \
	-z max-page-size=0x1000 \
	-T twilight/linker.ld

.PHONY: all twilight font iso run run-gui run-headless run-q35 run-driver-test run-linux-driver-test run-ethernet-test run-upstream-ethernet-test run-tpm tpm-reset limine clean check-tools new-it FORCE_VERSION

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

$(UPSTREAM_8139_C): scripts/fetch-linux-8139too.py
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/fetch-linux-8139too.py $@

$(UPSTREAM_8139_O): $(UPSTREAM_8139_C)
	@mkdir -p $(dir $@)
	@echo "Compiling exact upstream Linux v2.6.24 8139too.c (source remains unmodified)"
	$(CC) $(CFLAGS) -DKBUILD_MODNAME=\"8139too\" -c $< -o $@

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
