/* SPDX-License-Identifier: MIT */
/*
 * End-to-end thunk roundtrip: generated guest stubs marshal real API calls
 * through the shared channel; the inline host server dispatches them to fake
 * driver functions; results travel back. This validates the exact machinery
 * the arm32 shims and triango-gpud will use.
 */
#include "harness.h"
#include "thunks_gen.h"

#include <string.h>

static tgpu_host g_host;

/* ---- fake driver (stands in for the arm64 Adreno/Turnip stack) ---- */

static float fr, fg, fb, fa;
static uint8_t last_data[256];
static uint32_t last_data_len;
static int32_t last_size;

static void *fake_eglGetDisplay(void *id)
{
	(void)id;
	return (void *)(uintptr_t)0xE61D15ull;
}

static int32_t fake_eglInitialize(void *dpy, void *major, void *minor)
{
	(void)dpy;
	if (major)
		*(int32_t *)major = 1;
	if (minor)
		*(int32_t *)minor = 4;
	return 1;
}

static int32_t fake_eglSwapBuffers(void *dpy, void *surface)
{
	(void)dpy;
	(void)surface;
	return 1;
}

static void fake_glClearColor(float r, float g, float b, float a)
{
	fr = r;
	fg = g;
	fb = b;
	fa = a;
}

static void fake_glBufferData(uint32_t target, int64_t size, const void *data,
			      uint32_t usage)
{
	(void)target;
	(void)usage;
	last_size = (int32_t)size;
	last_data_len = 0;
	if (data && size > 0 && (uint64_t)size <= sizeof(last_data)) {
		memcpy(last_data, data, (size_t)size);
		last_data_len = (uint32_t)size;
	}
}

static void fake_glGenBuffers(int32_t n, void *buffers)
{
	int32_t i;
	uint32_t *b = (uint32_t *)buffers;

	for (i = 0; b && i < n; i++)
		b[i] = 0xB00Fu + (uint32_t)i;
}

static const char *fake_glGetString(uint32_t name)
{
	(void)name;
	return "Adreno (TM) 840";
}

static int32_t fake_vkEnumerateInstanceVersion(void *pApiVersion)
{
	if (pApiVersion)
		*(uint32_t *)pApiVersion = 0x00403000u;
	return 0;
}

static int32_t fake_vkEnumeratePhysicalDevices(void *instance, void *pCount,
					       void *pDevs)
{
	uint64_t *devs = (uint64_t *)pDevs;

	(void)instance;
	if (pCount)
		*(uint32_t *)pCount = 2;
	if (devs) {
		devs[0] = 0x9D000001ull;
		devs[1] = 0x9D000002ull;
	}
	return 0;
}

static int inline_serve(void *region)
{
	return tgpu_host_serve(&g_host, region, 1) >= 0 ? 0 : -1;
}

void test_roundtrip(void)
{
	static uint8_t region[65536];
	void *dpy;
	int32_t major = 0, minor = 0, rv;
	uint32_t ids[4];
	uint8_t data[16];
	uint32_t ver = 0;

	CHECK_EQ_INT(tgpu_region_init(region, sizeof(region)), 0);
	tgpu_host_init(&g_host, tg_thunk_table, tg_thunk_table_len);
	CHECK_EQ_INT(tg_guest_attach(region), 0);
	tg_set_inline_server(inline_serve);

	/* Register fakes for the ops used below. */
	tgpu_host_set_fn(&g_host, TGPU_OP_EGL_EGLGETDISPLAY,
			 (void *)fake_eglGetDisplay);
	tgpu_host_set_fn(&g_host, TGPU_OP_EGL_EGLINITIALIZE,
			 (void *)fake_eglInitialize);
	tgpu_host_set_fn(&g_host, TGPU_OP_EGL_EGLSWAPBUFFERS,
			 (void *)fake_eglSwapBuffers);
	tgpu_host_set_fn(&g_host, TGPU_OP_GLES2_GLCLEARCOLOR,
			 (void *)fake_glClearColor);
	tgpu_host_set_fn(&g_host, TGPU_OP_GLES2_GLBUFFERDATA,
			 (void *)fake_glBufferData);
	tgpu_host_set_fn(&g_host, TGPU_OP_GLES2_GLGENBUFFERS,
			 (void *)fake_glGenBuffers);
	tgpu_host_set_fn(&g_host, TGPU_OP_GLES2_GLGETSTRING,
			 (void *)fake_glGetString);
	tgpu_host_set_fn(&g_host, TGPU_OP_VULKAN_VKENUMERATEINSTANCEVERSION,
			 (void *)fake_vkEnumerateInstanceVersion);
	tgpu_host_set_fn(&g_host,
			 TGPU_OP_VULKAN_VKENUMERATEPHYSICALDEVICES,
			 (void *)fake_vkEnumeratePhysicalDevices);

	/* handle in/out + scalar out buffers (EGL). */
	dpy = eglGetDisplay(NULL);
	CHECK_EQ_INT((uintptr_t)dpy, 0xE61D15ull);
	rv = eglInitialize(dpy, &major, &minor);
	CHECK_EQ_INT(rv, 1);
	CHECK_EQ_INT(major, 1);
	CHECK_EQ_INT(minor, 4);
	CHECK_EQ_INT(eglSwapBuffers(dpy, (void *)(uintptr_t)0x5u), 1);

	/* float slots (GLES2). */
	glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
	CHECK(fr == 0.25f && fg == 0.5f && fb == 0.75f && fa == 1.0f);

	/* buffer-in marshalling: bytes survive the channel. */
	memset(data, 0, sizeof(data));
	memcpy(data, "TRIANGO-GPU!!", 13);
	glBufferData(0x8892u, 13, data, 0x88E4u);
	CHECK_EQ_INT(last_size, 13);
	CHECK_EQ_INT(last_data_len, 13);
	CHECK_EQ_INT(memcmp(last_data, "TRIANGO-GPU!!", 13), 0);

	/* buffer-out marshalling + per-element write-back. */
	memset(ids, 0, sizeof(ids));
	glGenBuffers(4, ids);
	CHECK_EQ_INT(ids[0], 0xB00F);
	CHECK_EQ_INT(ids[3], 0xB012);

	/* string return (dynamic length). */
	{
		const char *s = glGetString(0x1F01u);

		CHECK_STR(s, "Adreno (TM) 840");
	}

	/* Vulkan scalar-out + handle array out (8-byte host elems -> guests). */
	rv = vkEnumerateInstanceVersion(&ver);
	CHECK_EQ_INT(rv, 0);
	CHECK_EQ_INT(ver, 0x00403000u);
	{
		/* handle_array_out copy-back writes pointer-sized elements;
		 * the in/out count parameter caps the capacity (4 here). */
		void *arr[4];
		uint32_t cnt = 4;

		memset(arr, 0, sizeof(arr));
		rv = vkEnumeratePhysicalDevices((void *)(uintptr_t)0x77u, &cnt,
						arr);
		CHECK_EQ_INT(rv, 0);
		CHECK_EQ_INT(cnt, 2);
		CHECK_EQ_INT((uintptr_t)arr[0], 0x9D000001ull);
		CHECK_EQ_INT((uintptr_t)arr[1], 0x9D000002ull);
	}

	/* struct_conv functions answer ENOTSUP until converters land. */
	{
		uint8_t ci[128];
		void *inst = NULL;

		memset(ci, 0, sizeof(ci));
		rv = vkCreateInstance(ci, NULL, &inst);
		CHECK_EQ_INT(rv, TGPU_ST_ENOTSUP);
	}

	/* Unregistered op -> the impl cannot resolve the driver symbol and
	 * integer returns propagate the negative status code. */
	{
		int64_t r = (int64_t)eglGetError();

		CHECK_EQ_INT(r, TGPU_ST_ENOSYM);
	}

	/* Transport failure path: no server -> EAGAIN -> -1. */
	tg_set_inline_server(NULL);
	tg_guest_attach(region);
	{
		CHECK_EQ_INT(eglSwapBuffers(dpy, dpy), -1);
	}
}
