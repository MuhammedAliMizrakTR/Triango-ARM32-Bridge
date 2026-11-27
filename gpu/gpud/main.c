/* SPDX-License-Identifier: MIT */
/*
 * triango-gpud — GPU thunk daemon (skeleton).
 *
 * Loads the arm64 driver stack (Adreno blobs or Mesa Turnip via Zink) and
 * serves GL/Vulkan thunk requests from arm32 guests over the shared-memory
 * channel. This main loop is the deployable daemon; the dispatch machinery
 * itself is generated (gen_thunks.py) and covered by host unit tests.
 *
 * Usage:
 *   triango-gpud [--region /triango-gpu0] [--size 65536] [--serve-count N]
 *
 * v0 scaffold notes (docs/gpu-thunking.md):
 *  - channel transport is POSIX shared memory; production Android build will
 *    use ashmem + binder handoff
 *  - driver libraries are looked up via TRIANGO_EGL_LIB / TRIANGO_GLES_LIB /
 *    TRIANGO_VULKAN_LIB (defaults: the Adreno stack; Zink+Turnip optional)
 */
#include "triango/tg_runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __ANDROID__
/* POSIX shm for the host build / integration tests. */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

static void load_drivers(tgpu_host *h)
{
#ifdef __ANDROID__
	const char *egl = getenv("TRIANGO_EGL_LIB");
	const char *gles = getenv("TRIANGO_GLES_LIB");
	const char *vk = getenv("TRIANGO_VULKAN_LIB");

	if (!egl)
		egl = "/vendor/lib64/libEGL_adreno.so";
	if (!gles)
		gles = "/vendor/lib64/libGLESv2_adreno.so";
	if (!vk)
		vk = "/vendor/lib64/hw/vulkan.adreno.so";

	tgpu_host_set_lib(h, 0, dlopen(egl, RTLD_NOW | RTLD_LOCAL));
	tgpu_host_set_lib(h, 1, dlopen(gles, RTLD_NOW | RTLD_LOCAL));
	tgpu_host_set_lib(h, 2, dlopen(vk, RTLD_NOW | RTLD_LOCAL));
#else
	(void)h;
#endif
}

int main(int argc, char **argv)
{
	const char *region_name = "triango-gpu0";
	size_t size = 65536;
	long serve_count = -1; /* -1 = forever */
	void *region = NULL;
	tgpu_host host;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--region") == 0 && i + 1 < argc)
			region_name = argv[++i];
		else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
			size = (size_t)strtoul(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--serve-count") == 0 && i + 1 < argc)
			serve_count = strtol(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--help") == 0) {
			printf("usage: %s [--region NAME] [--size N] "
			       "[--serve-count N]\n", argv[0]);
			return 0;
		}
	}

#ifndef __ANDROID__
	{
		int fd = shm_open(region_name, O_CREAT | O_RDWR, 0600);

		if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
			perror("triango-gpud: shm");
			return 1;
		}
		region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			      fd, 0);
		if (region == MAP_FAILED) {
			perror("triango-gpud: mmap");
			return 1;
		}
	}
#else
	/* Android: region is created by the client bridge and handed over
	 * via binder (roadmap); v0 falls back to an anonymous mapping. */
	region = calloc(1, size);
	if (!region)
		return 1;
#endif

	if (tgpu_region_init(region, size) != 0) {
		fprintf(stderr, "triango-gpud: bad region size %zu\n", size);
		return 1;
	}

	tgpu_host_init(&host, tg_thunk_table, tg_thunk_table_len);
	load_drivers(&host);
	fprintf(stderr, "triango-gpud: serving %u thunks on '%s' (%zu bytes)\n",
		tg_thunk_table_len, region_name, size);

	/* Adaptive poll: 1 ms while requests flow, exponential backoff to
	 * 32 ms when the channel is idle (battery friendly — ~31 wakeups/s
	 * at idle instead of 1000).  TRIANGO_GPUD_IDLE_US overrides the cap. */
	{
		unsigned idle = 0;
		long idle_cap = 32000;
		const char *cap = getenv("TRIANGO_GPUD_IDLE_US");

		if (cap && cap[0])
			idle_cap = strtol(cap, NULL, 0);

		while (serve_count != 0) {
			int n = tgpu_host_serve(&host, region, 0);
			unsigned us;

			if (n < 0)
				break;
			if (serve_count > 0)
				serve_count--;
			if (n > 0) {
				idle = 0;
			} else if (idle < 6u) {
				idle++;
			}
			us = 1000u << idle;
			if (idle_cap >= 0 && us > (unsigned)idle_cap)
				us = (unsigned)idle_cap;
			if (us == 0)
				continue;
			usleep(us);
		}
	}
	return 0;
}
