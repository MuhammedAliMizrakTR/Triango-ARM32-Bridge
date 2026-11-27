/* SPDX-License-Identifier: MIT */
/*
 * Translation-engine plug-in loader.  LateLoad-friendly: the search is
 * retried on every loadLibrary() until an engine appears, so the module may
 * stage the plug-in after zygote already holds the bridge.
 */
#define _GNU_SOURCE
#include "triango/nb_engine.h"
#include "triango/nb.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static const triango_nb_engine_ops *g_ops; /* registered or from dlopen */
static void *g_dl;
static int g_engine_opened;

typedef const triango_nb_engine_ops *(*engine_open_fn)(const char *);
typedef void (*engine_close_fn)(void);

void triango_nb_set_engine(const struct triango_nb_engine_ops *ops)
{
	g_ops = ops;
}

static void try_open(const char *path)
{
	engine_open_fn open_fn;
	const triango_nb_engine_ops *ops;

	if (!path || !path[0])
		return;
	g_dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!g_dl)
		return;
	open_fn = (engine_open_fn)dlsym(g_dl, "triango_nb_engine_open");
	if (!open_fn) {
		dlclose(g_dl);
		g_dl = NULL;
		return;
	}
	ops = open_fn("/data/adb/triango");
	if (!ops || ops->version != TRIANGO_NB_ENGINE_ABI ||
	    !ops->load_library || !ops->get_trampoline) {
		dlclose(g_dl);
		g_dl = NULL;
		return;
	}
	g_ops = ops;
}

const triango_nb_engine_ops *triango_nb_ensure_engine(void)
{
	if (g_ops)
		return g_ops;
	if (g_engine_opened)
		return NULL; /* already searched this round */
	g_engine_opened = 1;

	try_open(getenv("TRIANGO_NB_ENGINE"));
	if (!g_ops)
		try_open("/data/adb/triango/lib/libtriango_nb_engine.so");
	if (!g_ops)
		try_open("/data/adb/modules/triango-arm32-bridge/"
			 "system/lib64/libtriango_nb_engine.so");
	return g_ops;
}

void triango_nb_engine_reset(void)
{
	engine_close_fn close_fn;

	if (g_dl) {
		close_fn = (engine_close_fn)dlsym(g_dl,
						  "triango_nb_engine_close");
		if (close_fn)
			close_fn();
		dlclose(g_dl);
		g_dl = NULL;
	}
	g_ops = NULL;
	g_engine_opened = 0;
}
