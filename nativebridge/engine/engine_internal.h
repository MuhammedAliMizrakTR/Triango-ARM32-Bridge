/* SPDX-License-Identifier: MIT */
/*
 * Triango NB engine — internal interfaces.
 *
 * The engine executes arm32 code in-process on any host:
 *   mem32    — guest (32-bit) address space: low-memory regions
 *   cpu      — ARM/Thumb interpreter core (re-entrant, one per thread)
 *   shims    — imported libc functions bind to host implementations
 *   elf32    — ELF32 EM_ARM shared-object loader (PT_LOAD + relocations)
 *   tramp    — per-arch host-callable trampolines (x86_64 + aarch64)
 *
 * Guest pointers are 32-bit; every guest-accessible byte lives in memory
 * mapped below 4 GB so an arm32 pointer can hold it.
 */
#ifndef TRIANGO_ENGINE_INTERNAL_H
#define TRIANGO_ENGINE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

/* ---------------- guest memory ---------------- */

#define T32_MAX_REGIONS 64

typedef struct {
	uint32_t base;   /* guest address */
	uint32_t size;
	uint8_t *host;   /* host pointer == base for our flat model */
} t32_region;

typedef struct {
	t32_region regions[T32_MAX_REGIONS];
	int nregions;
	uint32_t brk_hint;
} t32_space;

void t32_space_init(t32_space *sp);
/* Map `size` bytes of fresh zeroed memory (low 4 GB). Returns host ptr or
 * NULL; *gbase receives the guest base. */
void *t32_map(t32_space *sp, uint32_t size, uint32_t *gbase);
/* Map `size` bytes at a fixed guest base (ELF PT_LOAD). init may be NULL. */
void *t32_map_fixed(t32_space *sp, uint32_t base, uint32_t size,
		    const void *init, uint32_t initlen);
/* Validated lookup: NULL when [addr, addr+len) is not fully mapped. */
uint8_t *t32_ptr(t32_space *sp, uint32_t addr, uint32_t len);

/* ---------------- shims (host-backed imports) ---------------- */

typedef uint64_t (*t32_shim_fn)(uint64_t a0, uint64_t a1, uint64_t a2,
				uint64_t a3);

/* Install a host function behind a guest-callable entry.  Returns the
 * guest address of the entry (executable by the interpreter). */
uint32_t t32_shim_install(t32_space *sp, const char *name, t32_shim_fn fn);
/* Lookup by name (relocation time). 0 when unknown. */
uint32_t t32_shim_find(t32_space *sp, const char *name);
/* If `pc` is a shim entry: returns its fn, else NULL. */
t32_shim_fn t32_shim_at(t32_space *sp, uint32_t pc);
/* Builtin set: memcpy memset strlen strcmp memcmp malloc free abort. */
void t32_shim_install_builtins(t32_space *sp);

/* ---------------- CPU (interpreter) ---------------- */

#define T32_CPSR_N 0x80000000u
#define T32_CPSR_Z 0x40000000u
#define T32_CPSR_C 0x20000000u
#define T32_CPSR_V 0x10000000u
#define T32_CPSR_T 0x00000020u

#define T32_TRAP_OK 0
#define T32_TRAP_UNDEF 1   /* unimplemented/undefined instruction */
#define T32_TRAP_MEM 2     /* bad guest memory access */
#define T32_TRAP_SVC 3     /* SVC/BKPT reached */
#define T32_TRAP_LIMIT 4   /* instruction budget exceeded */

typedef struct {
	uint32_t r[16];
	uint32_t cpsr;
	t32_space *mem;
	int trap;
	uint32_t trap_pc;
	uint32_t trap_insn;
	uint64_t insn_budget;
	uint32_t s[32];    /* VFP s0..s31 (d[i] = s[2i],s[2i+1]) */
	uint32_t fpscr;
} t32_cpu;

/* VFP/NEON execution (vfp.c).  Returns 1 when `insn` was handled. */
int t32_vfp_exec(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc);

/* Call guest code `fn` with up to 4 integer/pointer args.  Returns r0.
 * cpu->trap != 0 on failure.  Re-entrant: one cpu per thread. */
uint32_t t32_call(t32_cpu *cpu, t32_space *mem, uint32_t fn,
		  const uint32_t *args, int nargs);

/* ---------------- ELF32 loader ---------------- */

typedef struct {
	uint32_t base;      /* load base */
	uint32_t entry;
	t32_space *mem;
	void *priv;         /* loader-private (symtab, path, ...) */
} t32_module;

/* Load an ELF32 EM_ARM ET_DYN/ET_EXEC into `mem`.  Undefined symbols are
 * resolved against shims + `deps`.  Returns 0 on success. */
int t32_elf_load(t32_space *mem, const char *path, t32_module *out,
		 const t32_module *deps, int ndeps, char *err, size_t errlen);
/* Find an exported function. 0 when missing. */
uint32_t t32_elf_symbol(const t32_module *m, const char *name);
/* Free loader-private state of a loaded module. */
void t32_elf_free(t32_module *m);

/* ---------------- trampolines ---------------- */

/*
 * Build a host-callable trampoline that marshals the host calling
 * convention into t32_call().  `arity` = number of integer args to pass
 * (0..4).  Returns executable host pointer (NULL unsupported arch).
 */
typedef uint32_t (*t32_stub_fn)(uint32_t a0, uint32_t a1, uint32_t a2,
				uint32_t a3);
void *t32_trampoline_build(t32_cpu *cpu, t32_space *mem, uint32_t fn);
/* Release every stub built so far (engine shutdown). */
void t32_trampoline_free_all(void);

#endif /* TRIANGO_ENGINE_INTERNAL_H */
