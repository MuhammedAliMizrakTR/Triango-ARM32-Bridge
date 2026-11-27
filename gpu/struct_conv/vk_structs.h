/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_VK_STRUCTS_H
#define TRIANGO_VK_STRUCTS_H

#include <stdint.h>

/*
 * 32<->64 bit Vulkan struct conversion — proof of concept.
 *
 * The guest (arm32) and host (arm64) ABI layouts of Vulkan structs differ:
 * every pointer field grows from 4 to 8 bytes and padding shifts. Real
 * VkApplicationInfo layouts:
 *   arm32: 28 bytes (all fields 4 bytes, no padding)
 *   LP64 : 48 bytes (sType, pad, pNext, pApplicationName, applicationVersion,
 *                     pad, pEngineName, engineVersion, apiVersion)
 *
 * Guest pointers cannot be dereferenced directly by the daemon, so the
 * converter receives a mapping callback that translates guest addresses into
 * host-readable memory.
 */

typedef struct {
	uint32_t sType;
	uint32_t pNext;
	uint32_t pApplicationName;
	uint32_t applicationVersion;
	uint32_t pEngineName;
	uint32_t engineVersion;
	uint32_t apiVersion;
} tg_vk_application_info32;

/* Natural layout — matches VkApplicationInfo on LP64. */
typedef struct {
	uint32_t sType;
	const void *pNext;
	const char *pApplicationName;
	uint32_t applicationVersion;
	const char *pEngineName;
	uint32_t engineVersion;
	uint32_t apiVersion;
} tg_vk_application_info64;

typedef const void *(*tg_guest_map_fn)(uint32_t guest_ptr, void *ctx);

/* Convert a guest VkApplicationInfo to host layout. 0 = ok, -1 = error. */
int tg_vk_conv_app_info_32to64(const tg_vk_application_info32 *in,
			       tg_vk_application_info64 *out,
			       tg_guest_map_fn map, void *ctx);

/* String copy helper shared by future converters (out must have room). */
uint32_t tg_vk_copy_guest_string(uint32_t guest_ptr, char *out,
				 uint32_t cap, tg_guest_map_fn map,
				 void *ctx);

#endif /* TRIANGO_VK_STRUCTS_H */
