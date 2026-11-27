/* SPDX-License-Identifier: MIT */
/*
 * libtriango_native_bridge.so — Android NativeBridge (v6) backed by the
 * Triango translation engine.
 *
 * Honesty note (v1.0): the interface, gating, engine binding and lifetime
 * management are real and unit-tested; the engine plug-in that actually
 * translates arm32 code (qemu-user bridge / Triango JIT) is a separate
 * module (docs/nativebridge.md).  Until it is present, loadLibrary() fails
 * cleanly with a descriptive error — nothing crashes and the bridge stays
 * load-safe inside zygote.
 */
#define _GNU_SOURCE
#include "triango/nb.h"
#include "triango/nb_engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* from nb_engine.c */
const triango_nb_engine_ops *triango_nb_ensure_engine(void);
void triango_nb_engine_reset(void);

#define NB_LOG_PATH "/data/adb/triango/log/nativebridge.log"

static const NativeBridgeRuntimeCallbacks *g_rt;
static char g_private_dir[256];
static char g_instr_set[32];
static char g_error[256];
static int g_initialized;

static void nb_log(const char *fmt, ...)
{
	va_list ap;
	FILE *f = fopen(NB_LOG_PATH, "a");

	if (!f)
		f = stderr;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	if (f != stderr)
		fclose(f);
}

static void set_error(const char *msg)
{
	snprintf(g_error, sizeof(g_error), "%s", msg ? msg : "");
}

/* ---- v1 ---- */

static bool nb_initialize(const NativeBridgeRuntimeCallbacks *runtime_cbs,
			  const char *private_dir,
			  const char *instruction_set)
{
	if (!instruction_set || !instruction_set[0]) {
		set_error("triango: initialize: no instruction set");
		return false;
	}
	if (strcmp(instruction_set, "armeabi-v7a") &&
	    strcmp(instruction_set, "armeabi") && strcmp(instruction_set, "arm")) {
		set_error("triango: unsupported instruction set");
		return false;
	}
	if (!g_initialized) {
		g_rt = runtime_cbs;
		snprintf(g_private_dir, sizeof(g_private_dir), "%s",
			 private_dir ? private_dir : "");
		snprintf(g_instr_set, sizeof(g_instr_set), "%s",
			 instruction_set);
		g_initialized = 1;
		nb_log("triango: initialize ok (dir=%s isa=%s)\n",
		       g_private_dir, g_instr_set);
	}
	return true;
}

static void *nb_loadLibrary(const char *libpath, int flag)
{
	const triango_nb_engine_ops *eng;
	void *h;

	if (!libpath) {
		set_error("triango: loadLibrary: NULL path");
		return NULL;
	}
	eng = triango_nb_ensure_engine();
	if (!eng) {
		set_error("triango: engine not present "
			  "(libtriango_nb_engine.so)");
		nb_log("triango: loadLibrary(%s): no engine\n", libpath);
		return NULL;
	}
	h = eng->load_library(libpath, flag);
	if (!h) {
		const char *e = eng->get_error ? eng->get_error() : NULL;

		set_error(e ? e : "triango: engine load failed");
		nb_log("triango: loadLibrary(%s): failed: %s\n", libpath,
		       g_error);
		return NULL;
	}
	nb_log("triango: loadLibrary(%s) -> %p\n", libpath, h);
	return h;
}

static void *nb_getTrampoline(void *handle, const char *name,
			      const char *shorty, uint32_t len)
{
	const triango_nb_engine_ops *eng = triango_nb_ensure_engine();

	if (!eng || !handle) {
		set_error("triango: getTrampoline: no engine/handle");
		return NULL;
	}
	return eng->get_trampoline(handle, name, shorty, len);
}

static bool nb_isSupported(const char *libpath)
{
	return triango_nb_is_arm32_elf(libpath) != 0;
}

static const NativeBridgeRuntimeValues *nb_getAppEnv(
	const char *instruction_set)
{
	static const char *abis[] = { "armeabi-v7a", "armeabi" };
	static const NativeBridgeRuntimeValues env = {
		.os_arch = "armv7l",
		.cpu_abi = "armeabi-v7a",
		.cpu_abi2 = "armeabi",
		.supported_abis = abis,
		.abi_count = 2,
	};

	if (!instruction_set)
		return NULL;
	if (strcmp(instruction_set, "armeabi-v7a") &&
	    strcmp(instruction_set, "armeabi") && strcmp(instruction_set, "arm"))
		return NULL;
	return &env;
}

/* ---- v2 ---- */

static bool nb_isCompatibleWith(uint32_t bridge_version)
{
	return bridge_version >= 1 && bridge_version <= 6;
}

static NativeBridgeSignalHandlerFn nb_getSignalHandler(int signal)
{
	/* Engine-owned translation faults will be handled by the engine's
	 * own SIGSEGV machinery (roadmap); we claim no signals yet. */
	(void)signal;
	return NULL;
}

/* ---- v3 ---- */

static int nb_unloadLibrary(void *handle)
{
	const triango_nb_engine_ops *eng = triango_nb_ensure_engine();

	if (!eng || !eng->unload_library) {
		set_error("triango: unloadLibrary: no engine");
		return 1;
	}
	return eng->unload_library(handle);
}

static const char *nb_getError(void)
{
	return g_error;
}

static bool nb_isPathSupported(const char *library_path)
{
	static const char *const ok[] = {
		"/system/lib", "/vendor/lib", "/data/app", "/data/adb",
		NULL,
	};
	size_t i;

	if (!library_path)
		return false;
	for (i = 0; ok[i]; i++) {
		size_t n = strlen(ok[i]);

		if (!strncmp(library_path, ok[i], n) &&
		    (library_path[n] == '/' || library_path[n] == '\0'))
			return true;
	}
	return false;
}

static bool nb_unused_initAnonymousNamespace(const char *a, const char *b)
{
	(void)a;
	(void)b;
	return true;
}

static native_bridge_namespace_t *nb_createNamespace(
	const char *name, const char *ld_library_path,
	const char *default_library_path, uint64_t type,
	const char *permitted_when_isolated_path,
	native_bridge_namespace_t *parent_ns)
{
	/* Single flat namespace: engines load libraries themselves. */
	(void)name;
	(void)ld_library_path;
	(void)default_library_path;
	(void)type;
	(void)permitted_when_isolated_path;
	(void)parent_ns;
	return (native_bridge_namespace_t *)(void *)&g_initialized;
}

static bool nb_linkNamespaces(native_bridge_namespace_t *from,
			      native_bridge_namespace_t *to,
			      const char *shared_libs_sonames)
{
	(void)from;
	(void)to;
	(void)shared_libs_sonames;
	return true;
}

static void *nb_loadLibraryExt(const char *libpath, int flag,
			       native_bridge_namespace_t *ns)
{
	(void)ns;
	return nb_loadLibrary(libpath, flag);
}

/* ---- v4 / v5 / v6 ---- */

static native_bridge_namespace_t *nb_getVendorNamespace(void)
{
	return NULL;
}

static native_bridge_namespace_t *nb_getExportedNamespace(const char *name)
{
	(void)name;
	return NULL;
}

static void nb_preZygoteFork(void)
{
	/* b/146904103: close emulated fds before app-zygote fork. */
	nb_log("triango: preZygoteFork\n");
}

NativeBridgeCallbacks NativeBridgeItf = {
	.version = 6,
	/* v1 */
	.initialize = nb_initialize,
	.loadLibrary = nb_loadLibrary,
	.getTrampoline = nb_getTrampoline,
	.isSupported = nb_isSupported,
	.getAppEnv = nb_getAppEnv,
	/* v2 */
	.isCompatibleWith = nb_isCompatibleWith,
	.getSignalHandler = nb_getSignalHandler,
	/* v3 */
	.unloadLibrary = nb_unloadLibrary,
	.getError = nb_getError,
	.isPathSupported = nb_isPathSupported,
	.unused_initAnonymousNamespace = nb_unused_initAnonymousNamespace,
	.createNamespace = nb_createNamespace,
	.linkNamespaces = nb_linkNamespaces,
	.loadLibraryExt = nb_loadLibraryExt,
	/* v4 */
	.getVendorNamespace = nb_getVendorNamespace,
	/* v5 */
	.getExportedNamespace = nb_getExportedNamespace,
	/* v6 */
	.preZygoteFork = nb_preZygoteFork,
};

void triango_nb_reset(void)
{
	triango_nb_engine_reset();
	g_rt = NULL;
	g_private_dir[0] = 0;
	g_instr_set[0] = 0;
	g_error[0] = 0;
	g_initialized = 0;
}
