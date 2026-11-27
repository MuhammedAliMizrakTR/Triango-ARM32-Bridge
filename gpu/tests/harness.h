/* SPDX-License-Identifier: MIT */
#ifndef GPU_TESTS_HARNESS_H
#define GPU_TESTS_HARNESS_H

/* Reuse the bridge test harness. */
#include "../../bridge/tests/harness.h"

void test_proto(void);
void test_roundtrip(void);
void test_struct_conv(void);

#endif /* GPU_TESTS_HARNESS_H */
