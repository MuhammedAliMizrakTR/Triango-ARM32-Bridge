/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_STACK_H
#define TRIANGO_STACK_H

#include <stddef.h>
#include <stdint.h>

/*
 * Guest (AArch32) process stack + auxv builder.
 *
 * Used by the future Triango JIT engine to hand a freshly "exec"ed guest
 * process its initial stack image. The qemu engine builds its own stack
 * internally; this code exists so the JIT has a tested foundation.
 *
 * Layout produced (low -> high, SP points at offset 0):
 *   [argc][argv ptrs + NULL][env ptrs + NULL][auxv pairs ... AT_NULL]
 *   [pad to 16][argv strings][env strings]["v7l"][execfn][16B AT_RANDOM]
 */

#define TB_AT_NULL 0
#define TB_AT_IGNORE 1
#define TB_AT_EXECFD 2
#define TB_AT_PHDR 3
#define TB_AT_PHENT 4
#define TB_AT_PHNUM 5
#define TB_AT_PAGESZ 6
#define TB_AT_BASE 7
#define TB_AT_FLAGS 8
#define TB_AT_ENTRY 9
#define TB_AT_UID 11
#define TB_AT_EUID 12
#define TB_AT_GID 13
#define TB_AT_EGID 14
#define TB_AT_HWCAP 16
#define TB_AT_CLKTCK 17
#define TB_AT_SECURE 23
#define TB_AT_RANDOM 25
#define TB_AT_HWCAP2 26
#define TB_AT_PLATFORM 15
#define TB_AT_EXECFN 31

/* Default fake HWCAP for an Armv7-A guest: VFPv4 + NEON. */
#define TB_HWCAP_ARMV7 0x000001c0u /* VFP | VFPv3 | VFPv4 | NEON-ish bits */

typedef struct {
	uint32_t entry;
	uint32_t phdr;
	uint32_t base;
	uint16_t phnum;
	uint16_t phentsize;
	uint32_t uid, euid, gid, egid;
	uint32_t hwcap; /* 0 -> TB_HWCAP_ARMV7 */
	uint32_t stack_vaddr; /* guest VA of the stack image's offset 0 (SP) */
} tb_stack_img;

typedef struct {
	uint8_t *data; /* full stack image; offset 0 == guest SP */
	size_t size;
} tb_stack;

/*
 * Build the stack image. argv[0..argc-1] and envp[0..envc-1] are copied into
 * the string area. @rnd is the 16-byte AT_RANDOM blob (may be NULL -> zeros).
 * Returns 0 on success, -1 on invalid input/OOM. Free with tb_stack_free().
 */
int tb_stack_build(const tb_stack_img *img, const char *const *argv, int argc,
		   const char *const *envp, int envc, const uint8_t rnd[16],
		   const char *execfn, tb_stack *out);

void tb_stack_free(tb_stack *st);

/* Read a little-endian u32 from the stack image (helper for tests/tools). */
uint32_t tb_stack_u32(const tb_stack *st, size_t off);

#endif /* TRIANGO_STACK_H */
