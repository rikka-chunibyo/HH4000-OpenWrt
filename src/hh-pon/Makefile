# SPDX-License-Identifier: GPL-2.0-only
# Host build of the hardware-independent core + unit tests, and the arm64 module.
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined
CORE    := core/hh_ploam.c core/hh_gpon_fsm.c
KDIR    ?= ../../ref/linux/linux-6.18.60
REPLAY  ?= ../../work/pon/gpon/replay/replay_init.txt

all: test

build/test_main: tests/test_main.c $(CORE) core/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_main.c $(CORE)

test: build/test_main
	./build/test_main

# Generated: register replay tables (from the emulation output of docs/13).
kmod/gen/hh_pon_replay.h: tools/replay2c.awk $(REPLAY)
	@mkdir -p kmod/gen
	gawk -f tools/replay2c.awk $(REPLAY) > $@

kmod: kmod/gen/hh_pon_replay.h
	cp core/hh_ploam.c core/hh_gpon_fsm.c kmod/
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kmod ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_MODPOST_WARN=1 modules

clean:
	rm -rf build kmod/*.o kmod/*.ko kmod/*.mod* kmod/.*.cmd kmod/modules.order kmod/Module.symvers \
	       kmod/hh_ploam.c kmod/hh_gpon_fsm.c

.PHONY: all test kmod clean
