/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_BRIDGE_H
#define TRIANGO_BRIDGE_H

#include <stdio.h>

#define TB_VERSION "0.1.0"

/*
 * argv conventions (see docs/architecture.md §2.1):
 *
 *   TB_CONV_LKM:    argv = [bridge, target, orig_argv0, orig_args...]
 *                   (kernel triango_bridge.ko AND the binfmt_misc fallback
 *                    with the 'P' flag — both use this exact shape; the
 *                    triango-bridge-lkm wrapper passes --conv=lkm)
 *   TB_CONV_MANUAL: argv = [bridge, target, guest_args...]   (default for
 *                   direct CLI use; guest argv[0] falls back to target)
 */
typedef enum {
	TB_CONV_LKM = 0,
	TB_CONV_MANUAL = 1,
} tb_conv;

extern int tb_verbose;

#define tb_log(fmt, ...)                                             \
	do {                                                         \
		fprintf(stderr, "triango-bridge: " fmt, ##__VA_ARGS__); \
	} while (0)

#define tb_vlog(fmt, ...)                       \
	do {                                    \
		if (tb_verbose)                 \
			tb_log(fmt, ##__VA_ARGS__); \
	} while (0)

#define tb_err(fmt, ...) tb_log("error: " fmt, ##__VA_ARGS__)

#endif /* TRIANGO_BRIDGE_H */
