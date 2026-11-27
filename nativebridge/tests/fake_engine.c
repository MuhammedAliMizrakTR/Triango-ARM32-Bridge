/* SPDX-License-Identifier: MIT */
/*
 * Fake Triango NB engine plug-in (host test): pretend to translate arm32
 * libraries and hand out a real callable trampoline.
 */
#include "triango/nb_engine.h"

#include <string.h>

static int fake_add(int a, int b)
{
	return a + b;
}

static void *fe_load(const char *path, int flag)
{
	(void)path;
	(void)flag;
	return (void *)(uintptr_t)0xF00Dull;
}

static void *fe_tramp(void *handle, const char *name, const char *shorty,
		      uint32_t len)
{
	(void)handle;
	(void)shorty;
	(void)len;
	if (name && strcmp(name, "add") == 0)
		return (void *)(uintptr_t)fake_add;
	return NULL;
}

static int fe_unload(void *handle)
{
	(void)handle;
	return 0;
}

static const char *fe_error(void)
{
	return "fake engine: synthetic failure";
}

static const triango_nb_engine_ops OPS = {
	.version = TRIANGO_NB_ENGINE_ABI,
	.load_library = fe_load,
	.get_trampoline = fe_tramp,
	.unload_library = fe_unload,
	.get_error = fe_error,
};

const triango_nb_engine_ops *triango_nb_engine_open(const char *private_dir)
{
	(void)private_dir;
	return &OPS;
}

void triango_nb_engine_close(void)
{
}
