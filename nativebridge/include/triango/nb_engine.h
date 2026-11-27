/* SPDX-License-Identifier: MIT */
/*
 * Triango translation-engine plug-in ABI.
 *
 * The native bridge itself only marshals; actual arm32 code translation is
 * provided by an engine plug-in (qemu-user bridge first, Triango JIT later).
 * The bridge dlopens:
 *
 *     $TRIANGO_NB_ENGINE                       (explicit override)
 *     <private_dir>/libtriango_nb_engine.so    (module staging dir)
 *     /data/adb/triango/lib/libtriango_nb_engine.so
 *
 * A plug-in exports:
 *
 *     const triango_nb_engine_ops *triango_nb_engine_open(const char *private_dir);
 *     void triango_nb_engine_close(void);
 */
#ifndef TRIANGO_NB_ENGINE_H
#define TRIANGO_NB_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRIANGO_NB_ENGINE_ABI 1u

typedef struct triango_nb_engine_ops {
	uint32_t version; /* TRIANGO_NB_ENGINE_ABI */
	/* Load an arm32 ELF shared object under translation.  Returns an
	 * opaque handle (non-zero) or NULL with get_error() set. */
	void *(*load_library)(const char *libpath, int flag);
	/* Return a host-callable trampoline for `name` in `handle`. */
	void *(*get_trampoline)(void *handle, const char *name,
				const char *shorty, uint32_t len);
	/* Release a handle from load_library. 0 on success. */
	int (*unload_library)(void *handle);
	/* Thread-safe error string for the last failure (never NULL). */
	const char *(*get_error)(void);
} triango_nb_engine_ops;

#ifdef __cplusplus
}
#endif
#endif /* TRIANGO_NB_ENGINE_H */
