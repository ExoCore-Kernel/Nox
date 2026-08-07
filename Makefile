SHELL := /bin/sh

CC := clang
LD := ld.lld
PYTHON := python3
QEMU := qemu-system-x86_64

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/generated
ISO_ROOT := $(BUILD_DIR)/iso_root
KERNEL := $(BUILD_DIR)/twilight.elf
ISO := $(BUILD_DIR)/nox.iso
LIMINE_DIR := limine-binary
FONT_C := $(GEN_DIR)/font_blob.c

SOURCES := $(shell find twilight -type f -name '*.c' -print)
OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES))
OBJECTS += $(OBJ_DIR)/generated/font_blob.o

CFLAGS := \
	-target x86_64-unknown-none-elf \
	-std=gnu11 \
	-O2 \
	-Wall -Wextra -Wpedantic \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-lto \
	-fno-pic \
	-fno-pie \
	-m64 \
	-march=x86-64 \
	-mno-red-zone \
	-mcmodel=kernel \
	-Itwilight/include

LDFLAGS := \
	-m elf_x86_64 \
	-nostdlib \
	-static \
	-z max-page-size=0x1000 \
	-T twilight/linker.ld

.PHONY: all twilight font iso run limine clean check-tools

all: twilight

twilight: check-tools $(KERNEL)

font: $(FONT_C)

$(FONT_C): scripts/embed-font.py
	@mkdir -p $(GEN_DIR)
	$(PYTHON) scripts/embed-font.py $@

$(KERNEL): $(OBJECTS) twilight/linker.ld | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@
	@echo "Built $(KERNEL)"

$(OBJ_DIR)/generated/font_blob.o: $(FONT_C)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $@

limine:
	sh scripts/get-limine.sh

iso: check-tools $(KERNEL) limine
	@command -v xorriso >/dev/null || { echo "Missing xorriso"; exit 1; }
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
	$(LIMINE_DIR)/limine bios-install $(ISO)
	@echo "Built $(ISO)"

run: iso
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU)"; exit 1; }
	$(QEMU) -M q35 -m 512M -cdrom $(ISO) -serial stdio

check-tools:
	@command -v $(CC) >/dev/null || { echo "Missing clang"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "Missing ld.lld"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "Missing python3"; exit 1; }
	@echo "Toolchain ready: clang + ld.lld + python3"

clean:
	rm -rf $(BUILD_DIR)
