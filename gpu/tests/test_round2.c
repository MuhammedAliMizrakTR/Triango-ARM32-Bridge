/* SPDX-License-Identifier: MIT */
/*
 * GPU round 2 thunk coverage: string arrays (glShaderSource), computed
 * buffer lengths (glReadPixels/glTexImage2D), 0-terminated attribute lists
 * (eglChooseConfig), and string-ish out buffers (glGetShaderInfoLog).
 */
#include "harness.h"
#include "thunks_gen.h"

#include <string.h>

static tgpu_host g2_host;

/* ---- fake driver ---- */

static uint32_t cap_shader;
static int32_t cap_count;
static char cap_str[3][64];
static int32_t cap_len[3];
static int32_t cap_has_len;

static void fake_glShaderSource(uint32_t shader, int32_t count,
				const char *const *string, const int32_t *length)
{
	int32_t i;

	cap_shader = shader;
	cap_count = count;
	cap_has_len = length != NULL;
	for (i = 0; i < count && i < 3; i++) {
		uint32_t l = length ? (uint32_t)length[i]
				    : (uint32_t)strlen(string[i]);

		cap_len[i] = (int32_t)l;
		if (l >= sizeof(cap_str[i]))
			l = sizeof(cap_str[i]) - 1;
		memcpy(cap_str[i], string[i], l);
		cap_str[i][l] = 0;
	}
}

static uint8_t cap_pixels[64];
static int32_t cap_rw, cap_rh;

static void fake_glReadPixels(int32_t x, int32_t y, int32_t width,
			      int32_t height, uint32_t format, uint32_t type,
			      void *pixels)
{
	uint32_t n = (uint32_t)width * (uint32_t)height * 4u;
	uint32_t i;

	(void)x;
	(void)y;
	(void)format;
	(void)type;
	cap_rw = width;
	cap_rh = height;
	for (i = 0; pixels && i < n && i < sizeof(cap_pixels); i++)
		((uint8_t *)pixels)[i] = (uint8_t)(0xA0u + i);
}

static uint8_t cap_tex[64];
static uint32_t cap_tex_len;

static void fake_glTexImage2D(uint32_t target, int32_t level,
			      int32_t internalformat, int32_t width,
			      int32_t height, int32_t border, uint32_t format,
			      uint32_t type, const void *pixels)
{
	uint32_t n = (uint32_t)width * (uint32_t)height * 4u;

	(void)target;
	(void)level;
	(void)internalformat;
	(void)border;
	(void)format;
	(void)type;
	cap_tex_len = 0;
	if (pixels && n <= sizeof(cap_tex)) {
		memcpy(cap_tex, pixels, n);
		cap_tex_len = n;
	}
}

static int32_t cap_cfg_size;
static uint64_t cap_cfg_ret[4];
static int32_t cap_cfg_num;

static int32_t fake_eglChooseConfig(void *dpy, const void *attrib_list,
				    void *configs, int32_t config_size,
				    void *num_config)
{
	int32_t i, n = config_size < 3 ? config_size : 3;

	(void)dpy;
	(void)attrib_list;
	for (i = 0; i < n; i++)
		((uint64_t *)configs)[i] = 0xC0FFEE00ull + (uint64_t)i;
	cap_cfg_size = config_size;
	*(int32_t *)num_config = n;
	cap_cfg_num = n;
	for (i = 0; i < n; i++)
		cap_cfg_ret[i] = 0;
	return 1;
}

static void fake_glGetShaderInfoLog(uint32_t shader, int32_t bufSize,
				    void *length, void *infoLog)
{
	const char *msg = "ok";

	(void)shader;
	if (length)
		*(int32_t *)length = 2;
	if (infoLog && bufSize >= 3)
		memcpy(infoLog, msg, 3);
}

static int32_t fake_eglGetConfigAttrib(void *dpy, void *config, int32_t attr,
				       void *value)
{
	(void)dpy;
	(void)config;
	(void)attr;
	*(int32_t *)value = 0x1234;
	return 1;
}

static int32_t inline_serve2(void *region)
{
	return tgpu_host_serve(&g2_host, region, 1) >= 0 ? 0 : -1;
}

void test_round2(void)
{
	static uint8_t region[65536];

	CHECK_EQ_INT(tgpu_region_init(region, sizeof(region)), 0);
	tgpu_host_init(&g2_host, tg_thunk_table, tg_thunk_table_len);
	CHECK_EQ_INT(tg_guest_attach(region), 0);
	tg_set_inline_server(inline_serve2);

	tgpu_host_set_fn(&g2_host, TGPU_OP_GLES2_GLSHADERSOURCE,
			 (void *)fake_glShaderSource);
	tgpu_host_set_fn(&g2_host, TGPU_OP_GLES2_GLREADPIXELS,
			 (void *)fake_glReadPixels);
	tgpu_host_set_fn(&g2_host, TGPU_OP_GLES2_GLTEXIMAGE2D,
			 (void *)fake_glTexImage2D);
	tgpu_host_set_fn(&g2_host, TGPU_OP_EGL_EGLCHOOSECONFIG,
			 (void *)fake_eglChooseConfig);
	tgpu_host_set_fn(&g2_host, TGPU_OP_GLES2_GLGETSHADERINFOLOG,
			 (void *)fake_glGetShaderInfoLog);
	tgpu_host_set_fn(&g2_host, TGPU_OP_EGL_EGLGETCONFIGATTRIB,
			 (void *)fake_eglGetConfigAttrib);

	/* string_array: count + blobs (+ optional length array). */
	{
		const char *src[2] = {
			"#version 300 es\n", "void main(){ gl_Position = vec4(0); }"
		};

		glShaderSource(42u, 2, src, NULL);
		CHECK_EQ_INT(cap_shader, 42u);
		CHECK_EQ_INT(cap_count, 2);
		CHECK_EQ_INT(cap_has_len, 0);
		CHECK_STR(cap_str[0], "#version 300 es\n");
		CHECK_STR(cap_str[1], "void main(){ gl_Position = vec4(0); }");
		CHECK_EQ_INT(cap_len[1], (int32_t)strlen(src[1]));
	}
	{
		/* Explicit length array: strings need not be NUL-terminated
		 * in the caller's memory; the channel copy appends NULs. */
		const char *src[1] = { "ABCD-not-terminated" };
		int32_t lens[1] = { 4 };

		glShaderSource(7u, 1, src, lens);
		CHECK_EQ_INT(cap_shader, 7u);
		CHECK_EQ_INT(cap_count, 1);
		CHECK_EQ_INT(cap_has_len, 1);
		CHECK_EQ_INT(cap_len[0], 4);
		CHECK_STR(cap_str[0], "ABCD");
	}

	/* len_expr out buffer: width*height*4. */
	{
		uint8_t px[64];

		memset(px, 0, sizeof(px));
		glReadPixels(0, 0, 4, 2, 0x1908u, 0x1401u, px);
		CHECK_EQ_INT(cap_rw, 4);
		CHECK_EQ_INT(cap_rh, 2);
		CHECK_EQ_INT(px[0], 0xA0);
		CHECK_EQ_INT(px[31], 0xA0 + 31);
		CHECK_EQ_INT(px[32], 0); /* untouched past width*height*4 */
	}

	/* len_expr in buffer. */
	{
		uint8_t tex[32];

		memset(tex, 0x5C, sizeof(tex));
		glTexImage2D(0x0DE1u, 0, 0x1908u, 2, 4, 0, 0x1908u, 0x1401u,
			     tex);
		CHECK_EQ_INT(cap_tex_len, 32);
		CHECK_EQ_INT(memcmp(cap_tex, tex, 32), 0);
	}

	/* 0-terminated attrib list + count_from SCALAR capacity. */
	{
		int32_t attrs[4] = { 0x3032, 5, 0x3033, 0 }; /* terminator */
		void *cfgs[4];
		int32_t num = -1;

		memset(cfgs, 0, sizeof(cfgs));
		CHECK_EQ_INT(eglChooseConfig(NULL, attrs, cfgs, 4, &num), 1);
		CHECK_EQ_INT(cap_cfg_size, 4);
		CHECK_EQ_INT(num, 3);
		CHECK(cfgs[0] != NULL); /* token ids */
		CHECK(cfgs[2] != NULL);
	}

	/* buf_out with len_arg (info log) + short fixed out. */
	{
		int32_t len = -1;
		char log[8];

		memset(log, 0, sizeof(log));
		glGetShaderInfoLog(3u, 8, &len, log);
		CHECK_EQ_INT(len, 2);
		CHECK_STR(log, "ok");
	}
	{
		int32_t val = 0;

		CHECK_EQ_INT(eglGetConfigAttrib(NULL, NULL, 0x3024, &val), 1);
		CHECK_EQ_INT(val, 0x1234);
	}

	tg_set_inline_server(NULL);
}
