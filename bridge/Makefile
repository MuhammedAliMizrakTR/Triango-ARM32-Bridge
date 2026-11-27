# SPDX-License-Identifier: MIT
# Triango bridge — userspace build.
#
#   make            # host build (tests engine code paths)
#   make test       # host build + run unit tests
#   make CROSS_COMPILE=aarch64-linux-gnu-   # target build
#
# The produced binary is build/triango-bridge.

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude

BUILD := build

SRCS := src/elf32.c src/syscalls.c src/stack.c src/engine_qemu.c src/engine_jit.c
MAIN := src/main.c
TESTS := tests/test_main.c tests/test_elf32.c tests/test_syscalls.c \
	tests/test_stack.c tests/test_engine.c

HDRS := $(wildcard include/triango/*.h) tests/harness.h

.PHONY: all test clean help

all: $(BUILD)/triango-bridge

$(BUILD)/triango-bridge: $(SRCS) $(MAIN) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(MAIN)

$(BUILD)/test_runner: $(SRCS) $(TESTS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(TESTS)

test: $(BUILD)/test_runner
	./$(BUILD)/test_runner

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

help:
	@echo "targets: all, test, clean"
	@echo "vars: CC, CROSS_COMPILE, CFLAGS, CPPFLAGS"
