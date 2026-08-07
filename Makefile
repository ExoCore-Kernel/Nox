.SUFFIXES:

IMAGE := nox
LIMINE_VERSION := v12.3.2
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
TW_CC ?= $(LLVM_PREFIX)/bin/clang
TW_LD ?= $(LLVM_PREFIX)/bin/ld.lld
else
TW_CC ?= clang
TW_LD ?= ld.lld
endif

ifeq ($(strip $(TW_CC)),)
TW_CC := clang
endif
ifeq ($(strip $(TW_LD)),)
TW_LD := ld.lld
endif

.PHONY: all twilight run clean distclean check-kernel-tools check-image-tools
all: $(IMAGE).iso

check-kernel-tools:
	@command -v git >/dev/null || (echo 'error: git is required' && false)
	@test -x "$(TW_CC)" || command -v "$(TW_CC)" >/dev/null || (echo 'error: clang/LLVM not found (see docs/building.md)' && false)
	@test -x "$(TW_LD)" || command -v "$(TW_LD)" >/dev/null || (echo 'error: ld.lld not found (see docs/building.md)' && false)

check-image-tools:
	@command -v curl >/dev/null || (echo 'error: curl is required' && false)
	@command -v xorriso >/dev/null || (echo 'error: xorriso is required' && false)

twilight: check-kernel-tools
	$(MAKE) -C twilight CC="$(TW_CC)" LD="$(TW_LD)"

limine-binary/limine:
	rm -rf limine-binary
	curl -fL "https://github.com/Limine-Bootloader/Limine/releases/download/$(LIMINE_VERSION)/limine-binary.tar.gz" | gunzip | tar -xf -
	$(MAKE) -C limine-binary

$(IMAGE).iso: check-image-tools twilight limine-binary/limine limine.conf
	rm -rf iso_root
	mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
	cp twilight/bin/twilight.elf iso_root/boot/twilight.elf
	cp limine.conf iso_root/boot/limine/limine.conf
	cp limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin limine-binary/limine-uefi-cd.bin iso_root/boot/limine/
	cp limine-binary/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE).iso
	./limine-binary/limine bios-install $(IMAGE).iso
	rm -rf iso_root

run: $(IMAGE).iso
	@command -v qemu-system-x86_64 >/dev/null || (echo 'error: qemu-system-x86_64 is required to run Nox' && false)
	qemu-system-x86_64 -M q35 -m 512M -cdrom $(IMAGE).iso -boot d

clean:
	$(MAKE) -C twilight clean
	rm -rf iso_root $(IMAGE).iso

distclean: clean
	$(MAKE) -C twilight clean
	rm -rf limine-binary
