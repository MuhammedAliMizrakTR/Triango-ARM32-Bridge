/* SPDX-License-Identifier: MIT */
/*
 * Host-callable trampolines.
 *
 * The bridge asks for a host function pointer per exported guest symbol.
 * The emitted stub takes the host calling convention (up to 4 integer
 * args), packs them and calls t32_call() — so from the host's point of
 * view the guest function is just another function.  Stubs are the only
 * native code the engine ever executes (guest code stays pure data for
 * the interpreter, W^X friendly).
 *
 * Supported hosts: x86_64 (SysV) and aarch64 (AAPCS64).  Any other arch
 * returns NULL and the plugin reports it.
 */
#include "engine_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* C entry the emitted stub calls (fixed signature).  Uses one interpreter
 * cpu per host thread so concurrent callers never share state. */
static uint32_t t32_stub_entry(t32_cpu *cpu, t32_space *mem, uint32_t fn,
			       uint32_t *args, int nargs)
{
	static __thread t32_cpu tls_cpu;

	(void)cpu;
	return t32_call(&tls_cpu, mem, fn, args, nargs);
}

typedef struct stub_slot {
	void *code;
	size_t size;
	struct stub_slot *next;
} stub_slot;

static stub_slot *g_slots;

static void *stub_alloc(size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return NULL;
	return p;
}

static void stub_seal(void *p, size_t size)
{
	/* W^X: drop write once emitted */
	mprotect(p, size, PROT_READ | PROT_EXEC);
}

static void stub_record(void *code, size_t size)
{
	stub_slot *s = malloc(sizeof(*s));

	if (!s)
		return;
	s->code = code;
	s->size = size;
	s->next = g_slots;
	g_slots = s;
}

void t32_trampoline_free_all(void)
{
	stub_slot *s = g_slots;

	while (s) {
		stub_slot *n = s->next;

		munmap(s->code, s->size);
		free(s);
		s = n;
	}
	g_slots = NULL;
}

#if defined(__x86_64__)
static void emit_u32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static void emit_u64(uint8_t *p, uint64_t v)
{
	memcpy(p, &v, 8);
}

void *t32_trampoline_build(t32_cpu *cpu, t32_space *mem, uint32_t fn)
{
	/*
	 * sub  rsp, 0x28
	 * mov  [rsp+0x00], edi        ; a0 (u32[4] layout!)
	 * mov  [rsp+0x04], esi        ; a1
	 * mov  [rsp+0x08], edx        ; a2
	 * mov  [rsp+0x0c], ecx        ; a3
	 * movabs rdi, cpu
	 * movabs rsi, mem
	 * movabs rdx, fn
	 * lea  rcx, [rsp]
	 * mov  r8d, 4
	 * movabs rax, t32_stub_entry
	 * call rax
	 * add  rsp, 0x28
	 * ret
	 */
	enum { SIZE = 128 };
	uint8_t *p = stub_alloc(SIZE);
	uint8_t *q;

	if (!p)
		return NULL;
	q = p;
	memcpy(q, "\x48\x83\xEC\x28", 4); q += 4;
	memcpy(q, "\x89\x3C\x24", 3); q += 3;
	memcpy(q, "\x89\x74\x24\x04", 4); q += 4;
	memcpy(q, "\x89\x54\x24\x08", 4); q += 4;
	memcpy(q, "\x89\x4C\x24\x0C", 4); q += 4;
	*q++ = 0x48; *q++ = 0xBF; emit_u64(q, (uint64_t)(uintptr_t)cpu); q += 8;
	*q++ = 0x48; *q++ = 0xBE; emit_u64(q, (uint64_t)(uintptr_t)mem); q += 8;
	*q++ = 0x48; *q++ = 0xBA; emit_u64(q, (uint64_t)fn); q += 8;
	memcpy(q, "\x48\x8D\x0C\x24", 4); q += 4;
	memcpy(q, "\x41\xB8\x04\x00\x00\x00", 6); q += 6;
	*q++ = 0x48; *q++ = 0xB8;
	emit_u64(q, (uint64_t)(uintptr_t)t32_stub_entry); q += 8;
	memcpy(q, "\xFF\xD0", 2); q += 2;
	memcpy(q, "\x48\x83\xC4\x28", 4); q += 4;
	*q++ = 0xC3;
	(void)emit_u32;

	stub_seal(p, SIZE);
	stub_record(p, SIZE);
	return p;
}
#elif defined(__aarch64__)
void *t32_trampoline_build(t32_cpu *cpu, t32_space *mem, uint32_t fn)
{
	/*
	 *  0: sub  sp, sp, #0x30
	 *  4: str  w0, [sp, #0]        ; args u32[4]
	 *  8: str  w1, [sp, #4]
	 * 12: str  w2, [sp, #8]
	 * 16: str  w3, [sp, #12]
	 * 20: ldr  x0, lit_cpu        ; +48
	 * 24: ldr  x1, lit_mem        ; +56
	 * 28: ldr  x2, lit_fn         ; +64
	 * 32: mov  x3, sp
	 * 36: mov  w4, #4
	 * 40: ldr  x5, lit_entry      ; +72
	 * 44: blr  x5
	 * 48: add  sp, sp, #0x30
	 * 52: ret
	 * 56: .quad cpu, mem, fn, t32_stub_entry
	 */
	enum { SIZE = 128 };
	uint32_t *w = stub_alloc(SIZE);

	if (!w)
		return NULL;
	w[0] = 0xD100C3FFu;            /* sub sp, sp, #0x30 */
	w[1] = 0xB90003E0u;            /* str w0, [sp, #0] */
	w[2] = 0xB90007E1u;            /* str w1, [sp, #4] */
	w[3] = 0xB9000BE2u;            /* str w2, [sp, #8] */
	w[4] = 0xB9000FE3u;            /* str w3, [sp, #12] */
	w[5] = 0x58000000u | (8u << 5) | 0u;  /* ldr x0, +56 */
	w[6] = 0x58000000u | (8u << 5) | 1u;  /* ldr x1, +64 */
	w[7] = 0x58000000u | (9u << 5) | 2u;  /* ldr x2, +72 */
	w[8] = 0x910003E3u;            /* mov x3, sp */
	w[9] = 0x52800084u;            /* mov w4, #4 */
	w[10] = 0x58000000u | (4u << 5) | 5u; /* ldr x5, +72 -> fixed below */
	w[11] = 0xD63F00A0u;           /* blr x5 */
	w[12] = 0x9100C3FFu;           /* add sp, sp, #0x30 */
	w[13] = 0xD65F03C0u;           /* ret */
	{
		uint64_t lits[4] = {
			(uint64_t)(uintptr_t)cpu,
			(uint64_t)(uintptr_t)mem,
			(uint64_t)fn,
			(uint64_t)(uintptr_t)t32_stub_entry,
		};

		memcpy(&w[14], lits, sizeof(lits));
	}
	/* literal offsets: lits start at byte 56.
	 * w[5] pc=20 -> (56-20)/4 = 9
	 * w[6] pc=24 -> (64-24)/4 = 10
	 * w[7] pc=28 -> (72-28)/4 = 11
	 * w[10] pc=40 -> (80-40)/4 = 10 */
	w[5] = 0x58000000u | (9u << 5) | 0u;
	w[6] = 0x58000000u | (10u << 5) | 1u;
	w[7] = 0x58000000u | (11u << 5) | 2u;
	w[10] = 0x58000000u | (10u << 5) | 5u;

	stub_seal(w, SIZE);
	stub_record(w, SIZE);
	return w;
}
#else
void *t32_trampoline_build(t32_cpu *cpu, t32_space *mem, uint32_t fn)
{
	(void)cpu;
	(void)mem;
	(void)fn;
	return NULL;
}
#endif
