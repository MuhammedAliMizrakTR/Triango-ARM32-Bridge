/* SPDX-License-Identifier: MIT */
#include "triango/engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const tb_engine tb_engine_jit;

const char *tb_qemu_resolve_path(const tb_launch_spec *spec)
{
	const char *env;

	if (spec && spec->engine_path && spec->engine_path[0])
		return spec->engine_path;
	env = getenv("TRIANGO_ENGINE");
	if (env && env[0])
		return env;
	return TB_QEMU_DEFAULT_PATH;
}

int tb_qemu_build_argv(const tb_launch_spec *spec, const char *engine_path,
		       const char *out[], int out_cap, char *err, size_t errlen)
{
	int n = 0;
	int i;

	if (!spec || !spec->target || !spec->target[0] || !out || out_cap < 8) {
		if (err && errlen)
			snprintf(err, errlen, "invalid qemu launch spec");
		return -1;
	}
	if (!engine_path || !engine_path[0])
		engine_path = tb_qemu_resolve_path(spec);

	out[n++] = engine_path;

	if (spec->sysroot && spec->sysroot[0]) {
		if (n + 2 >= out_cap)
			goto too_many;
		out[n++] = "-L";
		out[n++] = spec->sysroot;
	}

	if (n + 3 >= out_cap)
		goto too_many;
	out[n++] = "-0";
	out[n++] = (spec->orig_argv0 && spec->orig_argv0[0]) ? spec->orig_argv0 :
							       spec->target;
	out[n++] = spec->target;

	for (i = 0; i < spec->nargs; i++) {
		if (!spec->args[i]) {
			if (err && errlen)
				snprintf(err, errlen, "args[%d] is NULL", i);
			return -1;
		}
		if (n + 1 >= out_cap)
			goto too_many;
		out[n++] = spec->args[i];
	}
	out[n] = NULL;
	return n;

too_many:
	if (err && errlen)
		snprintf(err, errlen, "qemu argv exceeds capacity (%d)",
			 out_cap);
	return -1;
}

static int qemu_probe(const tb_launch_spec *spec, char *why, size_t whylen)
{
	const char *p = tb_qemu_resolve_path(spec);
	struct stat st;

	if (stat(p, &st) != 0) {
		if (why)
			snprintf(why, whylen,
				 "engine not found: %s (build it with tools/build-qemu-engine.sh)",
				 p);
		return -1;
	}
	if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
		if (why)
			snprintf(why, whylen, "engine not executable: %s", p);
		return -1;
	}
	return 0;
}

static int qemu_launch(const tb_launch_spec *spec)
{
	const char *argv[TB_QEMU_MAX_ARGS];
	char err[160];
	const char *engine_path = tb_qemu_resolve_path(spec);
	int n;

	n = tb_qemu_build_argv(spec, engine_path, argv, TB_QEMU_MAX_ARGS, err,
			       sizeof(err));
	if (n < 0) {
		errno = EINVAL;
		return -1;
	}
	execv(engine_path, (char *const *)argv);
	/* Only reached on failure; errno is set by execv(). */
	return -1;
}

const tb_engine tb_engine_qemu = {
	.name = "qemu",
	.probe = qemu_probe,
	.launch = qemu_launch,
};

const tb_engine *tb_engine_get(const char *name)
{
	if (!name || !name[0] || strcmp(name, "auto") == 0) {
		/* Prefer the JIT when it becomes available; qemu for now. */
		if (tb_engine_jit.probe(NULL, NULL, 0) == 0)
			return &tb_engine_jit;
		return &tb_engine_qemu;
	}
	if (strcmp(name, "qemu") == 0)
		return &tb_engine_qemu;
	if (strcmp(name, "jit") == 0)
		return &tb_engine_jit;
	return NULL;
}
