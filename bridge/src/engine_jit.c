/* SPDX-License-Identifier: MIT */
#include "triango/engine.h"

#include <errno.h>
#include <stdio.h>

/*
 * Triango JIT engine skeleton (arm32 -> arm64 block recompiler).
 * Roadmap: docs/roadmap.md v0.4. The syscall contract it must implement
 * lives in src/syscalls.c; the guest stack builder lives in src/stack.c.
 */

static int jit_probe(const tb_launch_spec *spec, char *why, size_t whylen)
{
	(void)spec;
	if (why)
		snprintf(why, whylen,
			 "triango-jit is not implemented yet (roadmap v0.4); use the qemu engine");
	return -1;
}

static int jit_launch(const tb_launch_spec *spec)
{
	(void)spec;
	errno = ENOSYS;
	return -1;
}

const tb_engine tb_engine_jit = {
	.name = "jit",
	.probe = jit_probe,
	.launch = jit_launch,
};
