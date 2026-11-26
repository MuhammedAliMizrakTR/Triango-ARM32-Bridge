# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build for triango_bridge.ko
#
# Usage:
#   make KERNEL_SRC=/path/to/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# or:
#   ./build.sh /path/to/kernel

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
PWD := $(shell pwd)

.PHONY: all modules clean help

all: modules

modules:
	@test -d "$(KERNEL_SRC)" || { echo "error: KERNEL_SRC='$(KERNEL_SRC)' not found"; exit 1; }
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	@if [ -d "$(KERNEL_SRC)" ]; then \
		$(MAKE) -C $(KERNEL_SRC) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean; \
	fi
	rm -rf .tmp_versions *.o *.ko *.mod *.mod.c *.cmd Module.symvers modules.order

help:
	@echo "Targets: all (modules), clean"
	@echo "Vars: KERNEL_SRC (kernel tree), ARCH, CROSS_COMPILE"
