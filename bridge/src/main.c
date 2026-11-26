/* SPDX-License-Identifier: MIT */
/*
 * triango-bridge — AArch64 userspace bridge for Arm32 binaries.
 *
 * Invoked either by the triango_bridge.ko kernel module (through the
 * triango-bridge-lkm wrapper) or the binfmt_misc fallback (same wrapper)
 * when an ELF32/EM_ARM binary is exec'ed, or manually:
 *
 *   triango-bridge [--engine qemu|jit] [--sysroot DIR]
 *                  [--conv lkm|manual] [--verbose]
 *                  [--probe|--selftest|--version] <arm32-elf> [args...]
 *
 * argv conventions (docs/architecture.md §2.1):
 *   lkm    : argv = [bridge, target, orig_argv0, orig_args...]
 *            (kernel LKM and binfmt_misc 'P' fallback, via the
 *             triango-bridge-lkm wrapper that passes --conv=lkm)
 *   manual : argv = [bridge, target, guest_args...]  (default; guest
 *            argv[0] falls back to the target path)
 *
 * The shipped wrapper always passes --conv explicitly, so the default
 * ("manual") is unambiguous for direct CLI use.
 */

#include "triango/bridge.h"
#include "triango/elf32.h"
#include "triango/engine.h"
#include "triango/stack.h"
#include "triango/syscalls.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int tb_verbose = 0;

static const char *progname = "triango-bridge";

static void usage(FILE *f)
{
	fprintf(f,
		"usage: %s [options] <arm32-elf> [args...]\n"
		"\n"
		"options:\n"
		"  --engine qemu|jit        execution engine (default: auto)\n"
		"  --sysroot DIR            32-bit sysroot for the engine\n"
		"  --conv lkm|manual        argv convention (default: manual)\n"
		"  --verbose, -v            verbose logging\n"
		"  --probe                  report environment status\n"
		"  --selftest               run built-in sanity checks\n"
		"  --version                print version\n"
		"  --help, -h               this help\n"
		"\n"
		"env: TRIANGO_ENGINE, TRIANGO_SYSROOT, TRIANGO_ARGV_CONV\n",
		progname);
}

static const char *basename_of(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

static int file_exists(const char *p)
{
	struct stat st;

	return p && stat(p, &st) == 0;
}

static int parse_conv(const char *s, tb_conv *out)
{
	if (strcmp(s, "lkm") == 0)
		*out = TB_CONV_LKM;
	else if (strcmp(s, "manual") == 0)
		*out = TB_CONV_MANUAL;
	else
		return -1;
	return 0;
}

static const char *conv_name(tb_conv c)
{
	return (c == TB_CONV_LKM) ? "lkm" : "manual";
}

static int cmd_probe(int argc, char **argv)
{
	const tb_engine *eng;
	char why[192];
	const char *sysroot;
	const char *target = (argc > 0) ? argv[0] : NULL;

	printf("triango-bridge %s\n", TB_VERSION);
	printf("  host arch      : %s\n",
#if defined(__aarch64__)
	       "aarch64"
#else
	       "non-aarch64 (dev/host build)"
#endif
	);
	printf("  conventions    : lkm | manual (wrapper passes --conv=lkm)\n");

	eng = tb_engine_get("qemu");
	if (eng->probe(NULL, why, sizeof(why)) == 0)
		printf("  engine qemu    : OK (%s)\n",
		       tb_qemu_resolve_path(NULL));
	else
		printf("  engine qemu    : MISSING — %s\n", why);

	eng = tb_engine_get("jit");
	if (eng->probe(NULL, why, sizeof(why)) == 0)
		printf("  engine jit     : OK\n");
	else
		printf("  engine jit     : unavailable — %s\n", why);

	sysroot = getenv("TRIANGO_SYSROOT");
	if (!sysroot || !sysroot[0])
		sysroot = TB_SYSROOT_DEFAULT;
	printf("  sysroot32      : %s (%s)\n", sysroot,
	       file_exists(sysroot) ? "present" : "MISSING");

	printf("  kernel module  : ");
	{
		FILE *f = fopen("/sys/module/triango_bridge/parameters/enabled",
				"r");

		if (f) {
			char buf[8] = { 0 };

			if (fgets(buf, sizeof(buf), f))
				printf("loaded (enabled=%s)", buf);
			fclose(f);
		} else {
			printf("not loaded (binfmt_misc fallback may be active)");
		}
		printf("\n");
	}

	printf("  binfmt misc    : %s\n",
	       file_exists("/proc/sys/fs/binfmt_misc/triango-arm32") ?
		       "registered" :
		       "not registered");

	if (target) {
		tb_image img;
		char err[192];

		if (tb_elf32_parse_file(target, &img, err, sizeof(err)) == 0)
			printf("  target         : %s (arm32 %s%s)\n", target,
			       img.is_dynamic ? "ET_DYN" : "ET_EXEC",
			       img.is_android ? ", android" : "");
		else
			printf("  target         : %s — %s\n", target, err);
	}
	return 0;
}

static int cmd_selftest(void)
{
	int fails = 0;
	const tb_syscall_map *m;
	tb_stack_img img;
	tb_stack st;
	const char *av[] = { "selftest", "a" };
	const char *ev[] = { "X=1" };
	const char *qargv[TB_QEMU_MAX_ARGS];
	tb_launch_spec spec;
	char err[128];
	uint8_t rnd[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
			    16 };
	int n;

	m = tb_syscall_lookup(3);
	if (!m || m->a64_nr != 63) {
		tb_err("selftest: syscall lookup(read) failed\n");
		fails++;
	}
	m = tb_syscall_lookup(192);
	if (!m || m->a64_nr != 222 || !(m->flags & TB_FIX_MMAP2)) {
		tb_err("selftest: syscall lookup(mmap2) failed\n");
		fails++;
	}

	memset(&img, 0, sizeof(img));
	img.entry = 0x10000;
	img.phdr = 0x8000;
	img.phnum = 2;
	img.phentsize = 32;
	img.stack_vaddr = 0x7f000000;
	if (tb_stack_build(&img, av, 2, ev, 1, rnd, "selftest", &st) != 0 ||
	    tb_stack_u32(&st, 0) != 2) {
		tb_err("selftest: stack build failed\n");
		fails++;
	}
	tb_stack_free(&st);

	memset(&spec, 0, sizeof(spec));
	spec.target = "/tmp/x/arm32bin";
	spec.orig_argv0 = "arm32bin";
	spec.args = NULL;
	spec.nargs = 0;
	spec.sysroot = "/sysroot32";
	n = tb_qemu_build_argv(&spec, "/eng/qemu-arm", qargv, TB_QEMU_MAX_ARGS,
			       err, sizeof(err));
	if (n != 6 || strcmp(qargv[0], "/eng/qemu-arm") != 0 ||
	    strcmp(qargv[1], "-L") != 0 || strcmp(qargv[3], "-0") != 0 ||
	    strcmp(qargv[4], "arm32bin") != 0 ||
	    strcmp(qargv[5], "/tmp/x/arm32bin") != 0) {
		tb_err("selftest: qemu argv build failed (%d)\n", n);
		fails++;
	}

	printf("triango-bridge selftest: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *engine_name = NULL;
	const char *sysroot = NULL;
	const char *conv_env;
	tb_conv conv = TB_CONV_MANUAL;
	int conv_forced = 0;
	int i;
	int target_idx = -1;
	tb_image img;
	char err[192];
	tb_launch_spec spec;
	const tb_engine *eng;
	char why[192];

	if (argc > 0 && argv[0] && argv[0][0])
		progname = basename_of(argv[0]);

	conv_env = getenv("TRIANGO_ARGV_CONV");
	if (conv_env && conv_env[0] && parse_conv(conv_env, &conv) == 0)
		conv_forced = 1;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(a, "--version") == 0) {
			printf("triango-bridge %s\n", TB_VERSION);
			return 0;
		}
		if (strcmp(a, "--probe") == 0)
			return cmd_probe(argc - i - 1, argv + i + 1);
		if (strcmp(a, "--selftest") == 0)
			return cmd_selftest();
		if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
			tb_verbose = 1;
			continue;
		}
		if (strcmp(a, "--engine") == 0 && i + 1 < argc) {
			engine_name = argv[++i];
			continue;
		}
		if (strcmp(a, "--sysroot") == 0 && i + 1 < argc) {
			sysroot = argv[++i];
			continue;
		}
		if (strcmp(a, "--conv") == 0 && i + 1 < argc) {
			if (parse_conv(argv[++i], &conv) != 0) {
				tb_err("unknown --conv '%s'\n", argv[i]);
				return 2;
			}
			conv_forced = 1;
			continue;
		}
		if (a[0] == '-' && a[1] != '\0') {
			tb_err("unknown option '%s'\n", a);
			usage(stderr);
			return 2;
		}
		target_idx = i;
		break;
	}

	if (target_idx < 0) {
		usage(stderr);
		return 2;
	}
	(void)conv_forced;

	if (!sysroot) {
		sysroot = getenv("TRIANGO_SYSROOT");
		if (!sysroot || !sysroot[0])
			sysroot = TB_SYSROOT_DEFAULT;
	}

	/*
	 * Split the incoming argv according to the convention.
	 * lkm    : [bridge, target, orig_argv0, orig_args...]  (wrapper --conv=lkm)
	 * manual : [bridge, target, guest_args...]             (guest argv0 = target)
	 */
	memset(&spec, 0, sizeof(spec));
	switch (conv) {
	case TB_CONV_LKM:
		spec.target = argv[target_idx];
		if (target_idx + 1 < argc) {
			spec.orig_argv0 = argv[target_idx + 1];
			spec.args = (const char *const *)&argv[target_idx + 2];
			spec.nargs = argc - (target_idx + 2);
		}
		break;
	default: /* TB_CONV_MANUAL */
		spec.orig_argv0 = NULL;
		spec.target = argv[target_idx];
		spec.args = (const char *const *)&argv[target_idx + 1];
		spec.nargs = argc - (target_idx + 1);
		break;
	}
	if (spec.nargs < 0)
		spec.nargs = 0;

	/* Validate the target is an Arm32 ELF before launching anything. */
	if (tb_elf32_parse_file(spec.target, &img, err, sizeof(err)) != 0) {
		tb_err("%s\n", err);
		return 127;
	}
	if (!img.is_android)
		tb_vlog("target has no Android linker interp ('%s')%s\n",
			img.has_interp ? img.interp : "<none>",
			img.has_interp ? "" : " — static binary?");

	spec.sysroot = sysroot;
	spec.engine_path = NULL;

	eng = tb_engine_get(engine_name);
	if (!eng) {
		tb_err("unknown engine '%s'\n", engine_name ? engine_name : "");
		return 2;
	}
	if (eng->probe(&spec, why, sizeof(why)) != 0) {
		tb_err("%s\n", why);
		return 127;
	}

	tb_vlog("launching engine '%s' for %s (conv=%s, sysroot=%s)\n",
		eng->name, spec.target, conv_name(conv), sysroot);

	if (eng->launch(&spec) != 0) {
		tb_err("engine launch failed: %s\n", strerror(errno));
		return 126;
	}
	return 0; /* not reached */
}
