/* SPDX-License-Identifier: MIT */
#include "triango/elf32.h"
#include "triango/stack.h"

#include <stdlib.h>
#include <string.h>

#define TB_STACK_MAX (1u << 20) /* 1 MiB sanity cap */

static uint32_t align_up32(uint32_t v, uint32_t a)
{
	return (v + a - 1u) & ~(a - 1u);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

uint32_t tb_stack_u32(const tb_stack *st, size_t off)
{
	const uint8_t *p;

	if (!st || !st->data || off + 4 > st->size)
		return 0;
	p = st->data + off;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* auxv entries emitted (order matters only for AT_NULL termination). */
static const uint16_t aux_keys[] = {
	TB_AT_PHDR,    TB_AT_PHENT,   TB_AT_PHNUM,  TB_AT_PAGESZ, TB_AT_BASE,
	TB_AT_FLAGS,   TB_AT_ENTRY,   TB_AT_UID,    TB_AT_EUID,   TB_AT_GID,
	TB_AT_EGID,    TB_AT_HWCAP,   TB_AT_CLKTCK, TB_AT_SECURE, TB_AT_RANDOM,
	TB_AT_PLATFORM, TB_AT_EXECFN,
};
#define TB_NAUX (sizeof(aux_keys) / sizeof(aux_keys[0]))

int tb_stack_build(const tb_stack_img *img, const char *const *argv, int argc,
		   const char *const *envp, int envc, const uint8_t rnd[16],
		   const char *execfn, tb_stack *out)
{
	size_t str_bytes = 0;
	uint32_t ptr_bytes, pad, str_off, total;
	uint32_t base;
	uint8_t *buf;
	uint32_t off;
	int i;

	if (!img || !out || argc < 1 || !argv)
		return -1;
	if (envc < 0 || (envc > 0 && !envp))
		return -1;
	if (!execfn)
		execfn = argv[0];

	for (i = 0; i < argc; i++) {
		if (!argv[i])
			return -1;
		str_bytes += strlen(argv[i]) + 1;
	}
	for (i = 0; i < envc; i++) {
		if (!envp[i])
			return -1;
		str_bytes += strlen(envp[i]) + 1;
	}
	str_bytes += sizeof("v7l"); /* AT_PLATFORM string incl. NUL */
	str_bytes += strlen(execfn) + 1;
	str_bytes += 16; /* AT_RANDOM */

	ptr_bytes = 4u /* argc */ + 4u * (uint32_t)(argc + 1) +
		    4u * (uint32_t)(envc + 1) +
		    8u * (uint32_t)(TB_NAUX + 1) /* + AT_NULL pair */;
	pad = align_up32(ptr_bytes, 16) - ptr_bytes;
	total = ptr_bytes + pad + (uint32_t)str_bytes;
	if (total > TB_STACK_MAX)
		return -1;

	buf = calloc(1, total);
	if (!buf)
		return -1;

	base = img->stack_vaddr;
	wr32(buf, (uint32_t)argc);

	/* --- string area --- */
	str_off = ptr_bytes + pad;

	off = str_off;
	uint32_t *argv_vas = calloc((size_t)argc, sizeof(uint32_t));
	uint32_t *env_vas = calloc((size_t)(envc > 0 ? envc : 1), sizeof(uint32_t));
	uint32_t platform_va, execfn_va, random_va;
	if (!argv_vas || !env_vas) {
		free(argv_vas);
		free(env_vas);
		free(buf);
		return -1;
	}

	for (i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;

		memcpy(buf + off, argv[i], n);
		argv_vas[i] = base + off;
		off += (uint32_t)n;
	}
	for (i = 0; i < envc; i++) {
		size_t n = strlen(envp[i]) + 1;

		memcpy(buf + off, envp[i], n);
		env_vas[i] = base + off;
		off += (uint32_t)n;
	}
	memcpy(buf + off, "v7l", sizeof("v7l"));
	platform_va = base + off;
	off += (uint32_t)sizeof("v7l");
	{
		size_t n = strlen(execfn) + 1;

		memcpy(buf + off, execfn, n);
		execfn_va = base + off;
		off += (uint32_t)n;
	}
	random_va = base + off;
	if (rnd)
		memcpy(buf + off, rnd, 16);
	off += 16;

	/* --- pointer block --- */
	off = 4; /* after argc */
	for (i = 0; i < argc; i++, off += 4)
		wr32(buf + off, argv_vas[i]);
	wr32(buf + off, 0);
	off += 4;
	for (i = 0; i < envc; i++, off += 4)
		wr32(buf + off, env_vas[i]);
	wr32(buf + off, 0);
	off += 4;

	for (i = 0; i < (int)TB_NAUX; i++) {
		uint32_t key = aux_keys[i];
		uint32_t val = 0;

		switch (key) {
		case TB_AT_PHDR:
			val = img->phdr;
			break;
		case TB_AT_PHENT:
			val = img->phentsize ? img->phentsize : TB_PHDR_SIZE;
			break;
		case TB_AT_PHNUM:
			val = img->phnum;
			break;
		case TB_AT_PAGESZ:
			val = 4096;
			break;
		case TB_AT_BASE:
			val = img->base;
			break;
		case TB_AT_FLAGS:
			val = 0;
			break;
		case TB_AT_ENTRY:
			val = img->entry;
			break;
		case TB_AT_UID:
			val = img->uid;
			break;
		case TB_AT_EUID:
			val = img->euid;
			break;
		case TB_AT_GID:
			val = img->gid;
			break;
		case TB_AT_EGID:
			val = img->egid;
			break;
		case TB_AT_HWCAP:
			val = img->hwcap ? img->hwcap : TB_HWCAP_ARMV7;
			break;
		case TB_AT_CLKTCK:
			val = 100;
			break;
		case TB_AT_SECURE:
			val = 0;
			break;
		case TB_AT_RANDOM:
			val = random_va;
			break;
		case TB_AT_PLATFORM:
			val = platform_va;
			break;
		case TB_AT_EXECFN:
			val = execfn_va;
			break;
		default:
			break;
		}
		wr32(buf + off, key);
		wr32(buf + off + 4, val);
		off += 8;
	}
	wr32(buf + off, TB_AT_NULL);
	wr32(buf + off + 4, 0);

	free(argv_vas);
	free(env_vas);

	out->data = buf;
	out->size = total;
	return 0;
}

void tb_stack_free(tb_stack *st)
{
	if (!st)
		return;
	free(st->data);
	st->data = NULL;
	st->size = 0;
}
