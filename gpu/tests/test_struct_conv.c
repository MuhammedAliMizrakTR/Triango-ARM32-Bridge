/* SPDX-License-Identifier: MIT */
/* 32<->64 struct conversion proof of concept (VkApplicationInfo). */
#include "harness.h"
#include "struct_conv/vk_structs.h"

#include <stddef.h>
#include <string.h>

/* Fake guest address space: a flat buffer; guest pointers are offsets+0x1000. */
static uint8_t g_guest_mem[512];
#define GUEST_BASE 0x1000u

static const void *fake_map(uint32_t guest_ptr, void *ctx)
{
	(void)ctx;
	if (guest_ptr < GUEST_BASE || guest_ptr >= GUEST_BASE + sizeof(g_guest_mem))
		return NULL;
	return g_guest_mem + (guest_ptr - GUEST_BASE);
}

static uint32_t guest_place(const char *s)
{
	static uint32_t off = 32; /* leave room for the struct at 0 */
	uint32_t at = off;
	uint32_t n = (uint32_t)strlen(s) + 1;

	memcpy(g_guest_mem + off, s, n);
	off += n;
	return GUEST_BASE + at;
}

void test_struct_conv(void)
{
	tg_vk_application_info32 in32;
	tg_vk_application_info64 out64;

	/* Layout sanity: the32-bit struct must be 28 bytes (EABI), the
	 * LP64 struct must match real VkApplicationInfo (48 bytes). */
	CHECK_EQ_INT(sizeof(tg_vk_application_info32), 28);
	CHECK_EQ_INT(sizeof(tg_vk_application_info64), 48);
	CHECK_EQ_INT(offsetof(tg_vk_application_info64, pApplicationName), 16);
	CHECK_EQ_INT(offsetof(tg_vk_application_info64, applicationVersion), 24);
	CHECK_EQ_INT(offsetof(tg_vk_application_info64, pEngineName), 32);
	CHECK_EQ_INT(offsetof(tg_vk_application_info64, engineVersion), 40);
	CHECK_EQ_INT(offsetof(tg_vk_application_info64, apiVersion), 44);

	memset(g_guest_mem, 0, sizeof(g_guest_mem));
	memset(&in32, 0, sizeof(in32));
	in32.sType = 0; /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
	in32.pNext = 0;
	in32.pApplicationName = guest_place("Triango Demo");
	in32.applicationVersion = 0x00010000u;
	in32.pEngineName = guest_place("TriangoJIT");
	in32.engineVersion = 0x00020003u;
	in32.apiVersion = 0x00403000u;

	CHECK_EQ_INT(tg_vk_conv_app_info_32to64(&in32, &out64, fake_map, NULL),
		     0);
	CHECK_EQ_INT(out64.sType, 0);
	CHECK(out64.pNext == NULL);
	CHECK_STR(out64.pApplicationName, "Triango Demo");
	CHECK_STR(out64.pEngineName, "TriangoJIT");
	CHECK_EQ_INT(out64.applicationVersion, 0x00010000u);
	CHECK_EQ_INT(out64.engineVersion, 0x00020003u);
	CHECK_EQ_INT(out64.apiVersion, 0x00403000u);

	/* Unmappable string pointer fails cleanly. */
	{
		tg_vk_application_info32 bad = in32;
		tg_vk_application_info64 o;

		bad.pApplicationName = 0xDEAD0000u;
		CHECK_EQ_INT(tg_vk_conv_app_info_32to64(&bad, &o, fake_map, NULL),
			     -1);
	}

	/* NULL inputs rejected. */
	CHECK_EQ_INT(tg_vk_conv_app_info_32to64(NULL, &out64, fake_map, NULL),
		     -1);
	CHECK_EQ_INT(tg_vk_conv_app_info_32to64(&in32, NULL, fake_map, NULL),
		     -1);
}
