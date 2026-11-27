# SPDX-License-Identifier: MIT
# Triango GPU thunk layer — build & tests.
#
#   make -C gpu test        # generate thunks + run host unit tests
#   make -C gpu gpud        # build the triango-gpud daemon (host build)
#   make -C gpu CROSS_COMPILE=aarch64-linux-gnu- gpud   # target build

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude -Ibuild/gen

BUILD := build
GEN := $(BUILD)/gen

THUNK_JSON := thunks/egl.json thunks/gles2.json thunks/vulkan.json
GEN_SRCS := $(GEN)/thunks_gen_guest.c $(GEN)/thunks_gen_host.c
GEN_HDRS := $(GEN)/thunks_gen.h

LIB_SRCS := lib/proto.c lib/tg_runtime.c
CONV_SRCS := struct_conv/vk_structs.c

TEST_SRCS := tests/test_main.c tests/test_proto.c tests/test_roundtrip.c \
	tests/test_struct_conv.c

.PHONY: all test gpud clean

all: gpud

$(GEN_HDRS) $(GEN_SRCS) &: $(THUNK_JSON) gen_thunks.py
	python3 gen_thunks.py $(THUNK_JSON) -o $(GEN)

$(BUILD)/test_runner: $(TEST_SRCS) $(LIB_SRCS) $(CONV_SRCS) $(GEN_SRCS) $(GEN_HDRS) tests/harness.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -I. -o $@ $(TEST_SRCS) $(LIB_SRCS) $(CONV_SRCS) $(GEN_SRCS)

test: $(BUILD)/test_runner
	./$(BUILD)/test_runner

$(BUILD)/triango-gpud: gpud/main.c $(LIB_SRCS) $(GEN_SRCS) $(GEN_HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ gpud/main.c $(LIB_SRCS) $(GEN_SRCS)

gpud: $(BUILD)/triango-gpud

clean:
	rm -rf $(BUILD)
