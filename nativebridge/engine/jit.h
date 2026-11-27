/* Triango ARM32 Bridge — block JIT public interface.
 *
 * Design (locked): pc-keyed hash cache of translated basic blocks
 * (<= 32 insns), exits NEXT / INDIRECT / SHIM / INTERP / TRAP.  Guest
 * state stays memory-resident in t32_cpu; the host context pointer lives
 * in a dedicated host register.  VFP/NEON semantics are shared with the
 * interpreter through t32_vfp_exec().  All guest memory access goes
 * through the validated helpers (crash-safe).
 *
 * Two backends: jit_x64.c (host tests / CI x86) and jit_a64.c (device +
 * CI ubuntu-24.04-arm).  Both consume the same block-IR from jit.c.
 */
#ifndef TRIANGO_JIT_H
#define TRIANGO_JIT_H

#include "engine_internal.h"

/* exit reasons returned by a translated block */
enum {
	T32_JIT_NEXT = 0,   /* fall-through: npc = block end pc          */
	T32_JIT_IND,        /* indirect/conditional branch: npc = r[15]   */
	T32_JIT_SHIM,       /* shim call site: npc = shim pc              */
	T32_JIT_INTERP,     /* complex insn: interpreter step at npc      */
	T32_JIT_TRAP,       /* fault inside the block                     */
};

typedef struct t32_jit t32_jit;

/* create / destroy the translator (host code arena + cache) */
t32_jit *t32_jit_create(void);
void t32_jit_destroy(t32_jit *j);

/* run from pc (thumb, bit0 clear) until an exit the dispatcher cannot
 * chain; returns the exit reason and stores the next pc in *npc. */
int t32_jit_run(t32_jit *j, t32_cpu *c, uint32_t pc, uint32_t *npc);

/* statistics (tests / diagnostics) */
void t32_jit_stats(t32_jit *j, uint64_t *blocks, uint64_t *hits,
		   uint64_t *exits);

#endif /* TRIANGO_JIT_H */
