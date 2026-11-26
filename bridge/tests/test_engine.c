/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "triango/engine.h"

#include <stdlib.h>
#include <string.h>

void test_engine(void)
{
	const char *av[TB_QEMU_MAX_ARGS];
	tb_launch_spec spec;
	char err[128];
	int n;
	const char *args[] = { "a", "b" };

	memset(&spec, 0, sizeof(spec));
	spec.target = "/x/foo";
	spec.orig_argv0 = "foo";
	spec.args = args;
	spec.nargs = 2;
	spec.sysroot = "/sysroot32";

	/* Full form: engine -L sysroot -0 argv0 target args... */
	n = tb_qemu_build_argv(&spec, "/eng/qemu-arm", av, TB_QEMU_MAX_ARGS,
			       err, sizeof(err));
	CHECK_EQ_INT(n, 8);
	if (n == 8) {
		CHECK_STR(av[0], "/eng/qemu-arm");
		CHECK_STR(av[1], "-L");
		CHECK_STR(av[2], "/sysroot32");
		CHECK_STR(av[3], "-0");
		CHECK_STR(av[4], "foo");
		CHECK_STR(av[5], "/x/foo");
		CHECK_STR(av[6], "a");
		CHECK_STR(av[7], "b");
		CHECK(av[8] == NULL);
	}

	/* Without sysroot: no -L pair. */
	spec.sysroot = NULL;
	n = tb_qemu_build_argv(&spec, "/eng/qemu-arm", av, TB_QEMU_MAX_ARGS,
			       err, sizeof(err));
	CHECK_EQ_INT(n, 6);
	if (n == 6) {
		CHECK_STR(av[1], "-0");
		CHECK_STR(av[2], "foo");
		CHECK_STR(av[3], "/x/foo");
	}

	/* Without orig_argv0: -0 falls back to the target path. */
	spec.orig_argv0 = NULL;
	spec.nargs = 0;
	n = tb_qemu_build_argv(&spec, "/eng/qemu-arm", av, TB_QEMU_MAX_ARGS,
			       err, sizeof(err));
	CHECK_EQ_INT(n, 4);
	if (n == 4)
		CHECK_STR(av[2], "/x/foo");

	/* NULL engine path falls back to the resolver. */
	setenv("TRIANGO_ENGINE", "/custom/qemu", 1);
	n = tb_qemu_build_argv(&spec, NULL, av, TB_QEMU_MAX_ARGS, err,
			       sizeof(err));
	CHECK_EQ_INT(n, 4);
	if (n == 4)
		CHECK_STR(av[0], "/custom/qemu");
	unsetenv("TRIANGO_ENGINE");

	/* spec->engine_path wins over the environment. */
	setenv("TRIANGO_ENGINE", "/custom/qemu", 1);
	spec.engine_path = "/explicit/qemu";
	n = tb_qemu_build_argv(&spec, NULL, av, TB_QEMU_MAX_ARGS, err,
			       sizeof(err));
	CHECK_EQ_INT(n, 4);
	if (n == 4)
		CHECK_STR(av[0], "/explicit/qemu");
	CHECK_STR(tb_qemu_resolve_path(&spec), "/explicit/qemu");
	unsetenv("TRIANGO_ENGINE");

	/* Capacity overflow reports an error. */
	spec.nargs = 2;
	n = tb_qemu_build_argv(&spec, "/eng/qemu-arm", av, 5, err, sizeof(err));
	CHECK_EQ_INT(n, -1);

	/* Bad input. */
	n = tb_qemu_build_argv(NULL, "/eng/qemu-arm", av, TB_QEMU_MAX_ARGS,
			       err, sizeof(err));
	CHECK_EQ_INT(n, -1);

	/* Engine registry. */
	CHECK(tb_engine_get("qemu") != NULL);
	CHECK(tb_engine_get("jit") != NULL);
	CHECK(tb_engine_get("auto") != NULL);
	CHECK(tb_engine_get("nope") == NULL);
	CHECK(tb_engine_get("qemu")->probe(NULL, err, sizeof(err)) != 0); /* engine binary absent on host */
	CHECK(tb_engine_get("jit")->probe(NULL, err, sizeof(err)) != 0);
}
