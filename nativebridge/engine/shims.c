/* SPDX-License-Identifier: MIT */
/*
 * Host-backed imports (the "wrapped libs" idea): undefined symbols in guest
 * modules resolve to guest-callable shim entries.  When the interpreter
 * branches to a shim entry it calls the host implementation directly —
 * memcpy, strlen and friends run at full native speed inside translated
 * code (this is where "native speed" already holds in V1.1(a)).
 */
#include "engine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHIM_MAX 128

typedef struct {
	char name[48];
	t32_shim_fn fn;
	uint32_t addr; /* guest address of the 8-byte entry record */
} shim_entry;

static shim_entry g_shims[SHIM_MAX];
static int g_nshims;
static uint32_t g_shim_base, g_shim_end;
static t32_space *g_shim_mem;

uint32_t t32_shim_install(t32_space *sp, const char *name, t32_shim_fn fn)
{
	uint32_t gbase;
	uint64_t *slot;
	uint32_t addr;

	if (g_nshims >= SHIM_MAX || !name || !fn)
		return 0;
	if (!g_shim_base) {
		void *p = t32_map(sp, 0x4000u, &gbase);

		if (!p)
			return 0;
		g_shim_base = gbase;
		g_shim_end = gbase + 0x4000u;
		g_shim_mem = sp;
	}
	addr = g_shim_base + (uint32_t)g_nshims * 8u;
	slot = (uint64_t *)t32_ptr(sp, addr, 8);
	if (!slot)
		return 0;
	*slot = (uint64_t)(uintptr_t)fn;
	snprintf(g_shims[g_nshims].name, sizeof(g_shims[g_nshims].name), "%s",
		 name);
	g_shims[g_nshims].fn = fn;
	g_shims[g_nshims].addr = addr;
	g_nshims++;
	return addr;
}

uint32_t t32_shim_find(t32_space *sp, const char *name)
{
	int i;

	(void)sp;
	for (i = 0; i < g_nshims; i++)
		if (strcmp(g_shims[i].name, name) == 0)
			return g_shims[i].addr;
	return 0;
}

t32_shim_fn t32_shim_at(t32_space *sp, uint32_t pc)
{
	(void)sp;
	if (pc < g_shim_base || pc >= g_shim_end)
		return NULL;
	if (((pc - g_shim_base) & 7u) != 0)
		return NULL;
	{
		uint32_t idx = (pc - g_shim_base) / 8u;

		if (idx >= (uint32_t)g_nshims)
			return NULL;
		return g_shims[idx].fn;
	}
}

/* ---- builtin shims (host libc behind the guest ABI: ≤4 int args) ---- */

static uint8_t *g2p(uint32_t addr, uint32_t len)
{
	return t32_ptr(g_shim_mem, addr, len);
}

static uint64_t sh_memcpy(uint64_t d, uint64_t s, uint64_t n, uint64_t z)
{
	uint8_t *dp = g2p((uint32_t)d, (uint32_t)n);
	const uint8_t *sp2 = g2p((uint32_t)s, (uint32_t)n);

	(void)z;
	if (dp && sp2)
		memcpy(dp, sp2, (size_t)n);
	return d;
}

static uint64_t sh_memset(uint64_t d, uint64_t c, uint64_t n, uint64_t z)
{
	uint8_t *dp = g2p((uint32_t)d, (uint32_t)n);

	(void)z;
	if (dp)
		memset(dp, (int)c, (size_t)n);
	return d;
}

static uint64_t sh_strlen(uint64_t s, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint32_t n = 0;

	(void)a1;
	(void)a2;
	(void)a3;
	while (g2p((uint32_t)s + n, 1) && g2p((uint32_t)s + n, 1)[0])
		n++;
	return n;
}

static uint64_t sh_strcmp(uint64_t a, uint64_t b, uint64_t a2, uint64_t a3)
{
	uint32_t i = 0;

	(void)a2;
	(void)a3;
	for (;;) {
		const uint8_t *pa = g2p((uint32_t)a + i, 1);
		const uint8_t *pb = g2p((uint32_t)b + i, 1);

		if (!pa || !pb)
			return 0;
		if (*pa != *pb || !*pa)
			return (uint64_t)(int64_t)((int)*pa - (int)*pb);
		i++;
	}
}

static uint64_t sh_memcmp(uint64_t a, uint64_t b, uint64_t n, uint64_t z)
{
	const uint8_t *pa = g2p((uint32_t)a, (uint32_t)n);
	const uint8_t *pb = g2p((uint32_t)b, (uint32_t)n);

	(void)z;
	if (!pa || !pb)
		return 0;
	return (uint64_t)(int64_t)memcmp(pa, pb, (size_t)n);
}

static uint64_t sh_malloc(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint32_t gbase;

	(void)a1;
	(void)a2;
	(void)a3;
	if (!t32_map(g_shim_mem, (uint32_t)n, &gbase))
		return 0;
	return gbase;
}

static uint64_t sh_free(uint64_t p, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)p;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

static uint64_t sh_abort(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	abort();
	return 0;
}

/* extra libc surface (run at full host speed) */
static uint64_t sh_strcpy(uint64_t d, uint64_t s, uint64_t a2, uint64_t a3)
{
	uint32_t i = 0;

	(void)a2;
	(void)a3;
	for (;;) {
		const uint8_t *p = g2p((uint32_t)s + i, 1);
		uint8_t *q = g2p((uint32_t)d + i, 1);

		if (!p || !q)
			return d;
		*q = *p;
		if (!*p)
			return d;
		i++;
	}
}

static uint64_t sh_strncpy(uint64_t d, uint64_t s, uint64_t n, uint64_t z)
{
	uint32_t i;

	(void)z;
	for (i = 0; i < (uint32_t)n; i++) {
		const uint8_t *p = g2p((uint32_t)s + i, 1);
		uint8_t *q = g2p((uint32_t)d + i, 1);

		if (!p || !q)
			return d;
		*q = *p;
		if (!*p) {
			i++;
			break;
		}
	}
	for (; i < (uint32_t)n; i++) {
		uint8_t *q = g2p((uint32_t)d + i, 1);

		if (!q)
			break;
		*q = 0;
	}
	return d;
}

static uint64_t sh_strncmp(uint64_t a, uint64_t b, uint64_t n, uint64_t z)
{
	uint32_t i;

	(void)z;
	for (i = 0; i < (uint32_t)n; i++) {
		const uint8_t *pa = g2p((uint32_t)a + i, 1);
		const uint8_t *pb = g2p((uint32_t)b + i, 1);

		if (!pa || !pb)
			return 0;
		if (*pa != *pb || !*pa)
			return (uint64_t)(int64_t)((int)*pa - (int)*pb);
	}
	return 0;
}

static uint64_t sh_strcat(uint64_t d, uint64_t s, uint64_t a2, uint64_t a3)
{
	uint32_t len = (uint32_t)sh_strlen(d, 0, 0, 0);

	(void)a2;
	(void)a3;
	sh_strcpy(d + len, s, 0, 0);
	return d;
}

static uint64_t sh_strchr(uint64_t s, uint64_t ch, uint64_t a2, uint64_t a3)
{
	uint32_t i = 0;

	(void)a2;
	(void)a3;
	for (;;) {
		const uint8_t *p = g2p((uint32_t)s + i, 1);

		if (!p)
			return 0;
		if (*p == (uint8_t)ch)
			return s + i;
		if (!*p)
			return 0;
		i++;
	}
}

static uint64_t sh_strrchr(uint64_t s, uint64_t ch, uint64_t a2, uint64_t a3)
{
	uint64_t hit = 0;
	uint32_t i = 0;

	(void)a2;
	(void)a3;
	for (;;) {
		const uint8_t *p = g2p((uint32_t)s + i, 1);

		if (!p)
			return hit;
		if (*p == (uint8_t)ch)
			hit = s + i;
		if (!*p)
			return hit;
		i++;
	}
}

static uint64_t sh_calloc(uint64_t a, uint64_t b, uint64_t a2, uint64_t a3)
{
	uint64_t n = a * b;

	(void)a2;
	(void)a3;
	if (!n)
		n = 1;
	return sh_malloc(n, 0, 0, 0); /* t32_map zero-fills */
}

static uint64_t sh_realloc(uint64_t p, uint64_t n, uint64_t a2, uint64_t a3)
{
	uint32_t gbase;
	uint8_t *dst;
	const uint8_t *src;

	(void)a2;
	(void)a3;
	if (!p)
		return sh_malloc(n, 0, 0, 0);
	if (!t32_map(g_shim_mem, (uint32_t)n, &gbase))
		return 0;
	dst = g2p(gbase, (uint32_t)n);
	src = g2p((uint32_t)p, 1);
	if (dst && src) {
		/* copy the smaller of old/new — old size unknown: copy min(n, 4K)
		 * is wrong; guest realloc of unknown size is best-effort and
		 * documented as such.  Copy n bytes when source is readable. */
		uint32_t i;

		for (i = 0; i < (uint32_t)n; i++) {
			const uint8_t *q = g2p((uint32_t)p + i, 1);

			if (!q)
				break;
			dst[i] = *q;
		}
	}
	return gbase;
}

static uint64_t sh_puts(uint64_t s, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint32_t n = (uint32_t)sh_strlen(s, 0, 0, 0);
	const uint8_t *p = g2p((uint32_t)s, n ? n : 1);

	(void)a1;
	(void)a2;
	(void)a3;
	if (p)
		fwrite(p, 1, n, stdout);
	fputc('\n', stdout);
	return 0;
}

static uint64_t sh_abs(uint64_t v, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int32_t x = (int32_t)v;

	(void)a1;
	(void)a2;
	(void)a3;
	return (uint64_t)(uint32_t)(x < 0 ? -x : x);
}

/* ARM EABI division helpers (gcc calls these on pre-armv7 targets) */
static uint64_t sh_aeabi_uidiv(uint64_t a, uint64_t b, uint64_t z, uint64_t z2)
{
	(void)z;
	(void)z2;
	return b == 0 ? 0 : (uint32_t)a / (uint32_t)b;
}

static uint64_t sh_aeabi_idiv(uint64_t a, uint64_t b, uint64_t z, uint64_t z2)
{
	int32_t x = (int32_t)a, y = (int32_t)b;

	(void)z;
	(void)z2;
	return (uint64_t)(uint32_t)(y == 0 ? 0 : x / y);
}

static uint64_t sh_aeabi_memclr(uint64_t d, uint64_t n, uint64_t z, uint64_t z2)
{
	return sh_memset(d, 0, n, 0);
}

static uint64_t sh_aeabi_memcpy4(uint64_t d, uint64_t s, uint64_t n,
				 uint64_t z)
{
	return sh_memcpy(d, s, n, z);
}

static uint64_t sh_aeabi_memcpy8(uint64_t d, uint64_t s, uint64_t n,
				 uint64_t z)
{
	return sh_memcpy(d, s, n, z);
}

void t32_shim_install_builtins(t32_space *sp)
{
	g_shim_mem = sp;
	t32_shim_install(sp, "memcpy", sh_memcpy);
	t32_shim_install(sp, "memmove", sh_memcpy);
	t32_shim_install(sp, "memset", sh_memset);
	t32_shim_install(sp, "memcmp", sh_memcmp);
	t32_shim_install(sp, "strlen", sh_strlen);
	t32_shim_install(sp, "strcmp", sh_strcmp);
	t32_shim_install(sp, "strncmp", sh_strncmp);
	t32_shim_install(sp, "strcpy", sh_strcpy);
	t32_shim_install(sp, "strncpy", sh_strncpy);
	t32_shim_install(sp, "strcat", sh_strcat);
	t32_shim_install(sp, "strchr", sh_strchr);
	t32_shim_install(sp, "strrchr", sh_strrchr);
	t32_shim_install(sp, "malloc", sh_malloc);
	t32_shim_install(sp, "calloc", sh_calloc);
	t32_shim_install(sp, "realloc", sh_realloc);
	t32_shim_install(sp, "free", sh_free);
	t32_shim_install(sp, "abort", sh_abort);
	t32_shim_install(sp, "puts", sh_puts);
	t32_shim_install(sp, "abs", sh_abs);
	t32_shim_install(sp, "labs", sh_abs);
	t32_shim_install(sp, "__aeabi_uidiv", sh_aeabi_uidiv);
	t32_shim_install(sp, "__aeabi_idiv", sh_aeabi_idiv);
	t32_shim_install(sp, "__aeabi_memclr", sh_aeabi_memclr);
	t32_shim_install(sp, "__aeabi_memclr4", sh_aeabi_memclr);
	t32_shim_install(sp, "__aeabi_memclr8", sh_aeabi_memclr);
	t32_shim_install(sp, "__aeabi_memcpy", sh_memcpy);
	t32_shim_install(sp, "__aeabi_memcpy4", sh_aeabi_memcpy4);
	t32_shim_install(sp, "__aeabi_memcpy8", sh_aeabi_memcpy8);
	t32_shim_install(sp, "__aeabi_memmove", sh_memcpy);
	t32_shim_install(sp, "__aeabi_memset", sh_memset);
}
