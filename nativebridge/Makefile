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
ENGINE_SRCS := engine/mem32.c engine/shims.c engine/arm32_cpu.c \
	       engine/arm32_elf.c engine/trampoline.c engine/triango_engine.c

.PHONY: all lib test engine engine-test clean

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

# ---- interpreter engine (libtriango_nb_engine.so) ----

$(BUILD)/libtriango_nb_engine.so: $(ENGINE_SRCS) engine/engine_internal.h \
		include/triango/nb_engine.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -shared -fPIC -o $@ $(ENGINE_SRCS) -lpthread

engine: $(BUILD)/libtriango_nb_engine.so

$(BUILD)/test_engine: engine/tests/test_engine.c engine/tests/blobs.h \
		$(filter-out engine/triango_engine.c,$(ENGINE_SRCS)) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ engine/tests/test_engine.c \
		$(filter-out engine/triango_engine.c,$(ENGINE_SRCS)) -lpthread

# cross-built arm32 guest .so for the ELF integration test (optional)
ARM_CC := $(shell command -v arm-linux-gnueabi-gcc 2>/dev/null)
$(BUILD)/guest_fns.so: engine/tests/guest_fns.c | $(BUILD)
ifeq ($(ARM_CC),)
	@echo "skip guest_fns.so (no arm-linux-gnueabi-gcc)"
	@touch $@
else
	$(ARM_CC) -shared -fPIC -O2 -fno-stack-protector -fno-builtin \
		-Wl,--hash-style=sysv -o $@ engine/tests/guest_fns.c
endif

engine-test: $(BUILD)/test_engine $(BUILD)/guest_fns.so
	@if [ -n "$(ARM_CC)" ] && [ -s $(BUILD)/guest_fns.so ]; then \
		TRIANGO_TEST_SO=$(BUILD)/guest_fns.so ./$(BUILD)/test_engine; \
	else \
		./$(BUILD)/test_engine; \
	fi

clean:
	rm -rf $(BUILD)
