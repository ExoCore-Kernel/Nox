.SUFFIXES:

OUTPUT := twilight.elf
CC = clang
LD = ld.lld

CFLAGS := \
    -target x86_64-unknown-none-elf \
    -std=gnu11 \
    -O2 \
    -g \
    -Wall \
    -Wextra \
    -Werror \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-pic \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mabi=sysv \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=kernel

CPPFLAGS := -Iinclude -MMD -MP

LDFLAGS := \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker.ld

SOURCES := $(shell find src -name '*.c' -type f | LC_ALL=C sort)
OBJECTS := $(patsubst src/%.c,obj/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean
all: bin/$(OUTPUT)

bin/$(OUTPUT): $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -rf obj bin
