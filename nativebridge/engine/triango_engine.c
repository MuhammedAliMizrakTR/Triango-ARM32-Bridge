/* SPDX-License-Identifier: MIT */
/*
 * libtriango_nb_engine.so — interpreter engine plug-in.
 *
 * Implements the triango_nb_engine_ops ABI from nb_engine.h:
 *   load_library()     — ELF32 EM_ARM .so into one shared guest space
 *   get_trampoline()   — host-callable stub per exported symbol
 *   unload_library()   — free loader state (stubs stay; see docs)
 *   get_error()        — per-thread error string
 *
 * Guest code executes in the ARM/Thumb interpreter; libc imports run as
 * host shims at full native speed.  One interpreter cpu per host thread.
 */
#include "triango/nb_engine.h"
#include "engine_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HANDLES 128
#define MAX_STUBS 256

typedef struct {
	t32_module mod;
	int used;
	struct {
		char name[64];
		void *stub;
	} stubs[MAX_STUBS];
	int nstubs;
} engine_handle;

static t32_space g_space;
static int g_open;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static engine_handle g_handles[MAX_HANDLES];
static __thread char g_err[256];

static void set_error(const char *msg)
{
	snprintf(g_err, sizeof(g_err), "%s", msg ? msg : "unknown error");
}

static void *el_load_library(const char *libpath, int flag)
{
	char err[256];
	int i, slot = -1;

	(void)flag;
	if (!g_open) {
		set_error("engine not opened");
		return NULL;
	}
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < MAX_HANDLES; i++) {
		if (!g_handles[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_lock);
		set_error("too many loaded libraries");
		return NULL;
	}
	memset(&g_handles[slot], 0, sizeof(g_handles[slot]));
	if (t32_elf_load(&g_space, libpath, &g_handles[slot].mod, NULL, 0,
			 err, sizeof(err)) != 0) {
		pthread_mutex_unlock(&g_lock);
		set_error(err);
		return NULL;
	}
	g_handles[slot].used = 1;
	pthread_mutex_unlock(&g_lock);
	return &g_handles[slot];
}

static void *el_get_trampoline(void *handle, const char *name,
			       const char *shorty, uint32_t len)
{
	engine_handle *h = handle;
	uint32_t fn;
	void *stub;
	int i;

	(void)shorty;
	(void)len; /* V1.1(a): all entry points marshal as (u32 x 4) */
	if (!h || !h->used) {
		set_error("bad handle");
		return NULL;
	}
	pthread_mutex_lock(&g_lock);
	for (i = 0; i < h->nstubs; i++) {
		if (!strcmp(h->stubs[i].name, name)) {
			pthread_mutex_unlock(&g_lock);
			return h->stubs[i].stub;
		}
	}
	fn = t32_elf_symbol(&h->mod, name);
	if (!fn) {
		pthread_mutex_unlock(&g_lock);
		set_error("symbol not found");
		return NULL;
	}
	stub = t32_trampoline_build(NULL, &g_space, fn);
	if (!stub) {
		pthread_mutex_unlock(&g_lock);
		set_error("trampoline unsupported on this host arch");
		return NULL;
	}
	if (h->nstubs < MAX_STUBS) {
		snprintf(h->stubs[h->nstubs].name,
			 sizeof(h->stubs[0].name), "%s", name);
		h->stubs[h->nstubs].stub = stub;
		h->nstubs++;
	}
	pthread_mutex_unlock(&g_lock);
	return stub;
}

static int el_unload_library(void *handle)
{
	engine_handle *h = handle;

	if (!h || !h->used)
		return -1;
	pthread_mutex_lock(&g_lock);
	t32_elf_free(&h->mod);
	/* stubs intentionally survive: a stub called after unload traps at
	 * the interpreter level instead of crashing the host (documented) */
	h->used = 0;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

static const char *el_get_error(void)
{
	return g_err[0] ? g_err : "no error";
}

static const triango_nb_engine_ops g_ops = {
	.version = TRIANGO_NB_ENGINE_ABI,
	.load_library = el_load_library,
	.get_trampoline = el_get_trampoline,
	.unload_library = el_unload_library,
	.get_error = el_get_error,
};

const triango_nb_engine_ops *triango_nb_engine_open(const char *private_dir)
{
	(void)private_dir;
	pthread_mutex_lock(&g_lock);
	if (!g_open) {
		t32_space_init(&g_space);
		t32_shim_install_builtins(&g_space);
		g_open = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return &g_ops;
}

void triango_nb_engine_close(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_open) {
		t32_trampoline_free_all();
		g_open = 0;
	}
	pthread_mutex_unlock(&g_lock);
}
