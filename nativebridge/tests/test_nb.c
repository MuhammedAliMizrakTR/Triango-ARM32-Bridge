/* SPDX-License-Identifier: MIT */
/*
 * NativeBridge host unit tests: ABI version negotiation, ELF gating,
 * app env, engine binding (static + dlopen plug-in), error paths.
 */
#include "triango/nb.h"
#include "triango/nb_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nb_fails, nb_checks;

#define CHECK(cond)                                                        \
	do {                                                               \
		nb_checks++;                                               \
		if (!(cond)) {                                             \
			nb_fails++;                                        \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
			       #cond);                                     \
		}                                                          \
	} while (0)

#define CHECK_EQ(a, b)                                                     \
	do {                                                               \
		long long a_ = (long long)(a), b_ = (long long)(b);        \
		nb_checks++;                                               \
		if (a_ != b_) {                                            \
			nb_fails++;                                        \
			printf("FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
			       __FILE__, __LINE__, #a, #b, a_, b_);        \
		}                                                          \
	} while (0)

static void write_file(const char *path, const unsigned char *buf,
		       size_t len)
{
	FILE *f = fopen(path, "wb");

	fwrite(buf, 1, len, f);
	fclose(f);
}

static void test_elf_classifier(void)
{
	static const unsigned char arm32[20] = {
		0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 0, 40, 0 /* e_type ET_DYN, e_machine EM_ARM */
	};
	static const unsigned char arm64[20] = {
		0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 0, 183, 0 /* EM_AARCH64 */
	};
	static const unsigned char x86_32[20] = {
		0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		3, 0, 3, 0 /* EM_386 */
	};
	static const unsigned char garbage[20] = { 'N', 'O', 'T', 'E', 'L', 'F' };

	write_file("build/t_arm32.so", arm32, sizeof(arm32));
	write_file("build/t_arm64.so", arm64, sizeof(arm64));
	write_file("build/t_x86.so", x86_32, sizeof(x86_32));
	write_file("build/t_junk.bin", garbage, sizeof(garbage));

	CHECK_EQ(triango_nb_is_arm32_elf("build/t_arm32.so"), 1);
	CHECK_EQ(triango_nb_is_arm32_elf("build/t_arm64.so"), 0);
	CHECK_EQ(triango_nb_is_arm32_elf("build/t_x86.so"), 0);
	CHECK_EQ(triango_nb_is_arm32_elf("build/t_junk.bin"), 0);
	CHECK_EQ(triango_nb_is_arm32_elf("build/does-not-exist"), 0);
	CHECK_EQ(triango_nb_is_arm32_elf(NULL), 0);
}

static void test_interface_version(void)
{
	CHECK_EQ(NativeBridgeItf.version, 6);
	CHECK(NativeBridgeItf.isCompatibleWith != NULL);
	CHECK(NativeBridgeItf.isCompatibleWith(1));
	CHECK(NativeBridgeItf.isCompatibleWith(6));
	CHECK(!NativeBridgeItf.isCompatibleWith(0));
	CHECK(!NativeBridgeItf.isCompatibleWith(7));
}

static void test_app_env(void)
{
	const NativeBridgeRuntimeValues *env;

	env = NativeBridgeItf.getAppEnv("armeabi-v7a");
	CHECK(env != NULL);
	if (env) {
		CHECK(strcmp(env->cpu_abi, "armeabi-v7a") == 0);
		CHECK_EQ(env->abi_count, 2);
	}
	CHECK(NativeBridgeItf.getAppEnv("armeabi") != NULL);
	CHECK(NativeBridgeItf.getAppEnv("x86") == NULL);
	CHECK(NativeBridgeItf.getAppEnv(NULL) == NULL);
}

static void test_gating(void)
{
	CHECK(NativeBridgeItf.isSupported("build/t_arm32.so"));
	CHECK(!NativeBridgeItf.isSupported("build/t_arm64.so"));
	CHECK(NativeBridgeItf.isPathSupported("/system/lib/libfoo.so"));
	CHECK(NativeBridgeItf.isPathSupported("/data/app/x/lib/arm/libx.so"));
	CHECK(!NativeBridgeItf.isPathSupported("/system/lib64/libfoo.so"));
	CHECK(!NativeBridgeItf.isPathSupported(NULL));
}

static void test_init_gate(void)
{
	triango_nb_reset();
	CHECK(!NativeBridgeItf.initialize(NULL, NULL, "x86"));
	CHECK(!NativeBridgeItf.initialize(NULL, NULL, NULL));
	CHECK(NativeBridgeItf.initialize(NULL, "/data/local/tmp",
					 "armeabi-v7a"));
	/* idempotent */
	CHECK(NativeBridgeItf.initialize(NULL, "/other", "armeabi"));
}

static void test_no_engine_error_path(void)
{
	triango_nb_reset();
	CHECK(NativeBridgeItf.initialize(NULL, NULL, "armeabi-v7a"));
	/* No engine present anywhere: clean NULL + error string. */
	unsetenv("TRIANGO_NB_ENGINE");
	CHECK(NativeBridgeItf.loadLibrary("build/t_arm32.so", 0) == NULL);
	CHECK(NativeBridgeItf.getError()[0] != 0);
}

/* trampoline target lives in the fake plug-in for the dlopen test; for the
 * static-registration test we use a local function. */
static int local_add(int a, int b)
{
	return a * 10 + b;
}

static void *st_load(const char *path, int flag)
{
	(void)path;
	(void)flag;
	return (void *)(uintptr_t)0xBEEFull;
}

static void *st_tramp(void *handle, const char *name, const char *shorty,
		      uint32_t len)
{
	(void)handle;
	(void)shorty;
	(void)len;
	if (name && strcmp(name, "add") == 0)
		return (void *)(uintptr_t)local_add;
	return NULL;
}

static int st_unload(void *handle)
{
	(void)handle;
	return 0;
}

static const char *st_err(void)
{
	return "static engine error";
}

static void test_engine_binding(void)
{
	static const triango_nb_engine_ops ops = {
		.version = TRIANGO_NB_ENGINE_ABI,
		.load_library = st_load,
		.get_trampoline = st_tramp,
		.unload_library = st_unload,
		.get_error = st_err,
	};
	void *h;
	int (*fn)(int, int);

	triango_nb_reset();
	triango_nb_set_engine(&ops);
	CHECK(NativeBridgeItf.initialize(NULL, NULL, "armeabi-v7a"));
	h = NativeBridgeItf.loadLibrary("libgame.so", 0);
	CHECK(h == (void *)(uintptr_t)0xBEEFull);
	fn = (int (*)(int, int))NativeBridgeItf.getTrampoline(h, "add", "ii", 3);
	CHECK(fn != NULL);
	if (fn)
		CHECK_EQ(fn(2, 3), 23);
	CHECK(NativeBridgeItf.getTrampoline(h, "missing", "v", 1) == NULL);
	CHECK_EQ(NativeBridgeItf.unloadLibrary(h), 0);
	/* loadLibraryExt routes through the same engine. */
	h = NativeBridgeItf.loadLibraryExt("libgame.so", 0, NULL);
	CHECK(h != NULL);
}

static void test_dlopen_engine(void)
{
	void *h;
	int (*fn)(int, int);

	triango_nb_reset();
	setenv("TRIANGO_NB_ENGINE", "build/libfake_engine.so", 1);
	CHECK(NativeBridgeItf.initialize(NULL, NULL, "armeabi-v7a"));
	h = NativeBridgeItf.loadLibrary("libgame.so", 0);
	CHECK(h == (void *)(uintptr_t)0xF00Dull);
	fn = (int (*)(int, int))NativeBridgeItf.getTrampoline(h, "add", "ii", 3);
	CHECK(fn != NULL);
	if (fn)
		CHECK_EQ(fn(2, 3), 5); /* real call through the plug-in */
	CHECK_EQ(NativeBridgeItf.unloadLibrary(h), 0);
	unsetenv("TRIANGO_NB_ENGINE");
}

int main(void)
{
	test_elf_classifier();
	test_interface_version();
	test_app_env();
	test_gating();
	test_init_gate();
	test_no_engine_error_path();
	test_engine_binding();
	test_dlopen_engine();

	printf("nb checks: %d, failures: %d\n", nb_checks, nb_fails);
	printf("nativebridge_test: %s\n", nb_fails ? "FAIL" : "PASS");
	return nb_fails ? 1 : 0;
}
