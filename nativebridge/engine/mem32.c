/* SPDX-License-Identifier: MIT */
/*
 * Guest (32-bit) address space.  All guest-accessible memory is mapped
 * below 4 GB so arm32 pointers can represent it.  W^X friendly: guest
 * segments are pure data for the interpreter (no execute permission
 * needed); only trampoline stubs ever execute natively.
 *
 * Fresh allocations come from one or more low-4GB arena chunks (bump
 * allocated); fixed mappings (ELF images) are exact mmap()s at the
 * requested guest base.
 */
#include "engine_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_CHUNK (256u * 1024u * 1024u)

void t32_space_init(t32_space *sp)
{
	memset(sp, 0, sizeof(*sp));
	sp->brk_hint = 0x20000000u;
}

static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

static int in_low4g(const void *p, uint32_t size)
{
	uintptr_t a = (uintptr_t)p;

	return a + size <= 0x100000000ull;
}

static void *map_low_exact(size_t size, uint32_t hint, int fixed,
			   uint32_t *gbase)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (fixed)
		flags |= MAP_FIXED_NOREPLACE;
	{
		void *p = mmap(fixed ? (void *)(uintptr_t)hint : NULL, size,
			       PROT_READ | PROT_WRITE, flags, -1, 0);

		if (p == MAP_FAILED)
			return NULL;
		if (!fixed && hint && (uintptr_t)p != (uintptr_t)hint) {
			/* hint ignored — accept only if still low-4G */
		}
		if (!in_low4g(p, (uint32_t)size)) {
			munmap(p, size);
			return NULL;
		}
		*gbase = (uint32_t)(uintptr_t)p;
		return p;
	}
}

/* arena bump allocator */
static uint8_t *g_arena;
static uint32_t g_arena_base, g_arena_size, g_arena_used;
static const uint32_t g_hints[] = {
	0x40000000u, 0x50000000u, 0x30000000u, 0x60000000u,
	0x20000000u, 0x70000000u, 0x10000000u, 0x08000000u,
};

static void *arena_alloc(uint32_t size, uint32_t *gbase)
{
	size_t i;

	if (g_arena && g_arena_used + size <= g_arena_size) {
		uint32_t off = g_arena_used;

		g_arena_used += size;
		*gbase = g_arena_base + off;
		return g_arena + off;
	}
	for (i = 0; i < sizeof(g_hints) / sizeof(g_hints[0]); i++) {
		void *p = mmap((void *)(uintptr_t)g_hints[i], ARENA_CHUNK,
			       PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED)
			continue;
		if (!in_low4g(p, ARENA_CHUNK)) {
			munmap(p, ARENA_CHUNK);
			continue;
		}
		g_arena = p;
		g_arena_base = (uint32_t)(uintptr_t)p;
		g_arena_size = ARENA_CHUNK;
		g_arena_used = size;
		*gbase = g_arena_base;
		return g_arena;
	}
	return NULL;
}

static int add_region(t32_space *sp, uint32_t base, uint32_t size,
		      uint8_t *host)
{
	if (sp->nregions >= T32_MAX_REGIONS)
		return -1;
	sp->regions[sp->nregions].base = base;
	sp->regions[sp->nregions].size = size;
	sp->regions[sp->nregions].host = host;
	sp->nregions++;
	return 0;
}

void *t32_map(t32_space *sp, uint32_t size, uint32_t *gbase)
{
	void *p;
	uint32_t base;

	if (!size)
		return NULL;
	size = (size + 0xfffu) & ~0xfffu;
	pthread_mutex_lock(&g_map_lock);
	p = arena_alloc(size, &base);
	if (!p) {
		pthread_mutex_unlock(&g_map_lock);
		return NULL;
	}
	if (add_region(sp, base, size, p) != 0) {
		/* region table full: keep the memory reserved (bump already
		 * advanced) — caller sees failure */
		pthread_mutex_unlock(&g_map_lock);
		return NULL;
	}
	pthread_mutex_unlock(&g_map_lock);
	if (gbase)
		*gbase = base;
	return p;
}

void *t32_map_fixed(t32_space *sp, uint32_t base, uint32_t size,
		    const void *init, uint32_t initlen)
{
	void *p;

	if (!size)
		return NULL;
	size = (size + 0xfffu) & ~0xfffu;
	pthread_mutex_lock(&g_map_lock);
	if (t32_ptr(sp, base, 1)) { /* already mapped */
		pthread_mutex_unlock(&g_map_lock);
		return NULL;
	}
	p = map_low_exact(size, base, 1, &base);
	if (!p) {
		pthread_mutex_unlock(&g_map_lock);
		return NULL;
	}
	if (add_region(sp, base, size, p) != 0) {
		munmap(p, size);
		pthread_mutex_unlock(&g_map_lock);
		return NULL;
	}
	pthread_mutex_unlock(&g_map_lock);
	if (init && initlen)
		memcpy(p, init, initlen > size ? size : initlen);
	return p;
}

uint8_t *t32_ptr(t32_space *sp, uint32_t addr, uint32_t len)
{
	int i;

	if (!len)
		len = 1;
	for (i = 0; i < sp->nregions; i++) {
		t32_region *r = &sp->regions[i];

		if (addr >= r->base && addr + len > addr &&
		    addr + len <= r->base + r->size)
			return r->host + (addr - r->base);
	}
	return NULL;
}
