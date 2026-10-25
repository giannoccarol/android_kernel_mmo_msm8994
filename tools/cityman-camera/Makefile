# SPDX-License-Identifier: GPL-2.0-only
# Freestanding static aarch64 binary: no libc, raw syscalls, kernel UAPI
# headers from this tree.
CROSS_COMPILE ?= aarch64-linux-android-
CC := $(CROSS_COMPILE)gcc
KINC ?= $(abspath ../../include)

CFLAGS := -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-asynchronous-unwind-tables -O2 -std=gnu89 -Wall -Wextra \
	-I$(KINC) -Wl,-e,_start

.PHONY: all clean
all: cci_probe

cci_probe: cci_probe.c start.S
	$(CC) $(CFLAGS) -o $@ cci_probe.c start.S

clean:
	rm -f cci_probe
