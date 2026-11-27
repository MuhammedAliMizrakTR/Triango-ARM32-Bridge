# SPDX-License-Identifier: MIT
# Triango NativeBridge — build & tests.
#
#   make -C nativebridge test    # host unit tests (any Linux)
#   make -C nativebridge lib     # host build of libtriango_native_bridge.so
#   ./tools/build-nativebridge.sh <ndk>   # real Android aarch64 .so (bionic)

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude

BUILD := build
LIB_SRCS := lib/nb_triango.c lib/nb_engine.c lib/nb_elf.c

.PHONY: all lib test clean

all: lib

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libtriango_native_bridge.so: $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -shared -fPIC -o $@ $(LIB_SRCS) -ldl

lib: $(BUILD)/libtriango_native_bridge.so

$(BUILD)/libfake_engine.so: tests/fake_engine.c include/triango/nb_engine.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -shared -fPIC -o $@ tests/fake_engine.c

$(BUILD)/test_nb: tests/test_nb.c $(LIB_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_nb.c $(LIB_SRCS) -ldl

test: $(BUILD)/test_nb $(BUILD)/libfake_engine.so
	./$(BUILD)/test_nb

clean:
	rm -rf $(BUILD)
