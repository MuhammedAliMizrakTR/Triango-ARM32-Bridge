/* SPDX-License-Identifier: MIT */
/*
 * Triango NativeBridge — Android libnativebridge ABI (versions 1..6).
 *
 * The struct layouts below are the wire contract with ART's libnativebridge
 * (see AOSP libnativebridge/include/nativebridge/native_bridge.h and the
 * test bridge in test/115-native-bridge/nativebridge.cc).  We intentionally
 * avoid jni.h here so the library core builds and unit-tests on any host;
 * the pointer types are opaque and identical on every ABI.
 *
 * The bridge library must export a DATA symbol:
 *
 *     android::NativeBridgeCallbacks NativeBridgeItf;
 *
 * (dlsym("NativeBridgeItf") is cast to the callbacks struct pointer —
 * confirmed against AOSP native_bridge.cc).
 */
#ifndef TRIANGO_NB_H
#define TRIANGO_NB_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque JNI stand-ins (layout-compatible: they are only ever pointers). */
typedef struct triango_nbJNIEnv JNIEnv;
typedef struct triango_nbJMethodID *jmethodID;
typedef struct triango_nbJClass *jclass;
typedef struct triango_nbNamespace native_bridge_namespace_t;

typedef struct {
	const char *name;
	const char *signature;
	void *fnPtr;
} JNINativeMethod;

typedef bool (*NativeBridgeSignalHandlerFn)(int, siginfo_t *, void *);

enum JNICallType {
	kJNICallTypeRegular = 1,
	kJNICallTypeCriticalNative = 2,
};

/* Environment values the bridge sets for apps after fork.  Layout matches
 * AOSP's test bridge exactly. */
typedef struct NativeBridgeRuntimeValues {
	const char *os_arch;
	const char *cpu_abi;
	const char *cpu_abi2;
	const char **supported_abis;
	int32_t abi_count;
} NativeBridgeRuntimeValues;

/* Runtime -> bridge callbacks (stored by initialize()). */
typedef struct NativeBridgeRuntimeCallbacks {
	const char *(*getMethodShorty)(JNIEnv *env, jmethodID mid);
	uint32_t (*getNativeMethodCount)(JNIEnv *env, jclass clazz);
	uint32_t (*getNativeMethods)(JNIEnv *env, jclass clazz,
				     JNINativeMethod *methods,
				     uint32_t method_count);
} NativeBridgeRuntimeCallbacks;

/*
 * Bridge -> runtime callbacks.  Field order is ABI; versions:
 *   v1: initialize, loadLibrary, getTrampoline, isSupported, getAppEnv
 *   v2: isCompatibleWith, getSignalHandler
 *   v3: unloadLibrary, getError, isPathSupported,
 *       unused_initAnonymousNamespace, createNamespace, linkNamespaces,
 *       loadLibraryExt
 *   v4: getVendorNamespace   v5: getExportedNamespace   v6: preZygoteFork
 * (v7 getTrampolineWithJNICallType / v8 isNativeBridgeFunctionPointer are
 * intentionally not provided: we declare .version = 6.)
 */
typedef struct NativeBridgeCallbacks {
	uint32_t version;

	/* v1 */
	bool (*initialize)(const NativeBridgeRuntimeCallbacks *runtime_cbs,
			   const char *private_dir, const char *instruction_set);
	void *(*loadLibrary)(const char *libpath, int flag);
	void *(*getTrampoline)(void *handle, const char *name,
			       const char *shorty, uint32_t len);
	bool (*isSupported)(const char *libpath);
	const NativeBridgeRuntimeValues *(*getAppEnv)(
		const char *instruction_set);

	/* v2 */
	bool (*isCompatibleWith)(uint32_t bridge_version);
	NativeBridgeSignalHandlerFn (*getSignalHandler)(int signal);

	/* v3 */
	int (*unloadLibrary)(void *handle);
	const char *(*getError)();
	bool (*isPathSupported)(const char *library_path);
	bool (*unused_initAnonymousNamespace)(const char *, const char *);
	native_bridge_namespace_t *(*createNamespace)(
		const char *name, const char *ld_library_path,
		const char *default_library_path, uint64_t type,
		const char *permitted_when_isolated_path,
		native_bridge_namespace_t *parent_ns);
	bool (*linkNamespaces)(native_bridge_namespace_t *from,
			       native_bridge_namespace_t *to,
			       const char *shared_libs_sonames);
	void *(*loadLibraryExt)(const char *libpath, int flag,
				native_bridge_namespace_t *ns);

	/* v4 */
	native_bridge_namespace_t *(*getVendorNamespace)();

	/* v5 */
	native_bridge_namespace_t *(*getExportedNamespace)(const char *name);

	/* v6 */
	void (*preZygoteFork)();
} NativeBridgeCallbacks;

/* The exported interface object (data symbol, see file comment). */
extern NativeBridgeCallbacks NativeBridgeItf;

/* ---- Triango extensions (test hooks / engine control) ---------------- */

/* Register the translation engine directly (bypasses dlopen).  Pass NULL to
 * force a fresh engine search on the next loadLibrary(). */
struct triango_nb_engine_ops;
void triango_nb_set_engine(const struct triango_nb_engine_ops *ops);

/* Reset all bridge state (tests). */
void triango_nb_reset(void);

/* ELF classification helper (also used by tests).  Returns 1 when `path`
 * names a 32-bit ARM ELF (class ELF32, e_machine EM_ARM). */
int triango_nb_is_arm32_elf(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* TRIANGO_NB_H */
