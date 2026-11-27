/* SPDX-License-Identifier: MIT */
#include "vk_structs.h"

#include <string.h>

#define TG_VK_APP_INFO_MAX_STR 256

/* String storage kept inside the converter result lifetime (daemon-side
 * scratch, documented limitation of the PoC). */
static char g_str0[TG_VK_APP_INFO_MAX_STR];
static char g_str1[TG_VK_APP_INFO_MAX_STR];

uint32_t tg_vk_copy_guest_string(uint32_t guest_ptr, char *out, uint32_t cap,
				 tg_guest_map_fn map, void *ctx)
{
	const char *src;
	uint32_t i;

	if (!guest_ptr) {
		if (out && cap)
			out[0] = '\0';
		return 0;
	}
	src = (const char *)map(guest_ptr, ctx);
	if (!src) {
		if (out && cap)
			out[0] = '\0';
		return 0;
	}
	for (i = 0; i + 1 < cap && src[i]; i++)
		out[i] = src[i];
	out[i] = '\0';
	return i + 1;
}

int tg_vk_conv_app_info_32to64(const tg_vk_application_info32 *in,
			       tg_vk_application_info64 *out,
			       tg_guest_map_fn map, void *ctx)
{
	if (!in || !out || !map)
		return -1;

	memset(out, 0, sizeof(*out));
	out->sType = in->sType;
	out->pNext = in->pNext ? map(in->pNext, ctx) : NULL;
	out->applicationVersion = in->applicationVersion;
	out->engineVersion = in->engineVersion;
	out->apiVersion = in->apiVersion;

	if (in->pApplicationName &&
	    !tg_vk_copy_guest_string(in->pApplicationName, g_str0,
				     sizeof(g_str0), map, ctx))
		return -1;
	out->pApplicationName = in->pApplicationName ? g_str0 : NULL;

	if (in->pEngineName &&
	    !tg_vk_copy_guest_string(in->pEngineName, g_str1,
				     sizeof(g_str1), map, ctx))
		return -1;
	out->pEngineName = in->pEngineName ? g_str1 : NULL;
	return 0;
}
