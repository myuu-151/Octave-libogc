# Build the minimal GameCube apploader with devkitPPC.
#
#   make            -> ../gcn_apploader.img   (consumed by Package Project)
#   make clean
#
# Requires DEVKITPPC (set by the devkitPro environment). Run from the
# devkitPro MSYS2 shell on Windows, or any shell with devkitPPC on PATH.

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set. Install devkitPPC and run from the devkitPro environment.)
endif

PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

# -G0 / -msdata=none: no small-data addressing (the IPL does not set r2/r13).
# freestanding, no libc/crt: the IPL calls app_entry directly.
CFLAGS  := -mcpu=750 -O2 -G0 -msdata=none -fno-builtin -ffreestanding \
           -fno-pic -fno-stack-protector -Wall -Wextra
LDFLAGS := -nostdlib -Wl,-T,apploader.ld -Wl,-static -Wl,-Map,apploader.map

OUT := ../gcn_apploader.img

all: $(OUT)

apploader.elf: apploader.c apploader.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ apploader.c

apploader.bin: apploader.elf
	$(OBJCOPY) -O binary $< $@

$(OUT): apploader.bin apploader.elf mkimg.py
	python mkimg.py apploader.bin $(OUT) $$($(SIZE) apploader.elf | awk 'NR==2{print $$3}')
	@echo "Apploader image ready: $(OUT)"

clean:
	rm -f apploader.elf apploader.bin apploader.map $(OUT)

.PHONY: all clean
