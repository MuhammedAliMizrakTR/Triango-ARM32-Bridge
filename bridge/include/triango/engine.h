/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_ENGINE_H
#define TRIANGO_ENGINE_H

#include <stddef.h>

/*
 * Execution engine interface.
 *
 * v0.1 ships the "qemu" engine (qemu-arm user-mode, external binary).
 * The "jit" engine (Triango arm32->arm64 recompiler) is a roadmap item and
 * currently only probes as unavailable.
 */

typedef struct {
	const char *target;      /* arm32 binary path */
	const char *orig_argv0;  /* guest argv[0]; NULL -> target */
	const char *const *args; /* guest args (argv[1..]); may be NULL */
	int nargs;
	const char *sysroot;     /* 32-bit sysroot for the engine; may be NULL */
	const char *engine_path; /* explicit engine binary; NULL -> default */
} tb_launch_spec;

typedef struct tb_engine {
	const char *name;
	/* Return 0 when usable; else -1 with a reason in @why. */
	int (*probe)(const tb_launch_spec *spec, char *why, size_t whylen);
	/* Replace the current process with the guest (exec semantics).
	 * Returns only on failure (-1, errno set). */
	int (*launch)(const tb_launch_spec *spec);
} tb_engine;

/* name: "qemu" | "jit"; NULL selects the first available engine. */
const tb_engine *tb_engine_get(const char *name);

/* Default engine binary locations (env TRIANGO_ENGINE overrides). */
#define TB_QEMU_DEFAULT_PATH "/data/adb/triango/engines/qemu-arm"
#define TB_JIT_DEFAULT_PATH "/data/adb/triango/engines/triango-jit"
#define TB_SYSROOT_DEFAULT "/data/adb/triango/sysroot32"

/* ---- qemu engine (engine_qemu.c) ---- */

#define TB_QEMU_MAX_ARGS 96

/*
 * Build the qemu-arm argv:
 *   [engine, "-L", sysroot, "-0", argv0, target, args..., NULL]
 * ("-L" pair omitted without a sysroot; "-0" always present.)
 * Pointers reference @spec/@engine_path storage or string literals — the
 * caller must keep them alive until launch. Returns the argc (without the
 * terminating NULL) or -1 on error.
 */
int tb_qemu_build_argv(const tb_launch_spec *spec, const char *engine_path,
		       const char *out[], int out_cap, char *err, size_t errlen);

/* Resolve the engine binary path (spec->engine_path, env, then default). */
const char *tb_qemu_resolve_path(const tb_launch_spec *spec);

#endif /* TRIANGO_ENGINE_H */
