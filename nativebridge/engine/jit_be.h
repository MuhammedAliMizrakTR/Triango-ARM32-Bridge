/* Triango ARM32 Bridge — JIT backend emission API.
 *
 * A translated block is a host function:
 *     int block(t32_cpu *c, uint32_t *next);
 * returning the exit kind (T32_JIT_*) with the exit pc in *next.
 * Guest registers stay in c->r[] / c->cpsr; backends use a dedicated
 * host register as the context pointer (x19 on A64, rbx on x86-64).
 */
#ifndef TRIANGO_JIT_BE_H
#define TRIANGO_JIT_BE_H

#include <stddef.h>
#include <stdint.h>

typedef struct t32_jb {
	uint8_t *start, *p, *end;
	int err;
} t32_jb;

void t32_jb_init(t32_jb *b, uint8_t *code, size_t cap);

/* emit the block prologue / a full exit (kind + next pc) */
void t32_jb_prologue(t32_jb *b);
void t32_jb_exit(t32_jb *b, int kind, uint32_t pc);
void t32_jb_exit_ind(t32_jb *b, int greg);
void t32_jb_exit_cc(t32_jb *b, int cond, uint32_t tpc, uint32_t fpc);
void t32_jb_exit_cbr(t32_jb *b, int greg, int cond, uint32_t tpc,
		     uint32_t fpc);

/* guest-register ALU (op = ALU_* from jit.c: ADD SUB AND ORR EOR MOV MVN;
 * flags = F_NONE / F_SET / F_ONLY) */
void t32_jb_alu_rr(t32_jb *b, int op, int rd, int rn, int rm, int flags);
void t32_jb_alu_ri(t32_jb *b, int op, int rd, int rn, uint32_t imm,
		   int flags);
void t32_jb_movw(t32_jb *b, int rd, uint32_t imm16, int is_movt);
void t32_jb_shift_imm(t32_jb *b, int op, int rd, int rm, int sh, int flags);
void t32_jb_mul(t32_jb *b, int rd, int rn, int rm);
void t32_jb_setlr(t32_jb *b, uint32_t pc);

/* single-step helper call: t32_fallback_step(c, pc, &tmp); trap ->
 * exit T32_JIT_TRAP.  The block continues afterwards. */
void t32_jb_step(t32_jb *b, uint32_t pc);

/* call a finished block */
int t32_jb_run(uint8_t *code, void *cpu, uint32_t *next);

/* flag modes for ALU emission */
enum { JB_F_NONE = 0, JB_F_SET = 1, JB_F_ONLY = 2 };
#define F_NONE JB_F_NONE
#define F_SET  JB_F_SET
#define F_ONLY JB_F_ONLY

/* implementation note: jit.c supplies these ALU op ids */
enum {
	JB_ADD, JB_SUB, JB_AND, JB_ORR, JB_EOR, JB_MOV, JB_MVN, JB_RSB
};

#endif /* TRIANGO_JIT_BE_H */
