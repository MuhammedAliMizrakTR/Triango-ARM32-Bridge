/* Triango ARM32 Bridge — block JIT: cache, dispatcher, block builder.
 *
 * Blocks chain inline-friendly insns; exotic insns end the block with an
 * INTERP exit and the dispatcher runs one interpreter step (semantic
 * single source of truth stays in the interpreter).  Memory-operand
 * loads/stores (never pc-writing) execute via the trap-checked helper
 * and the block continues.  Branches exit with their resolved target.
 *
 * Two backends implement jit_be.h: jit_x64.c and jit_a64.c.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "engine_internal.h"
#include "jit.h"
#include "jit_be.h"

#define JIT_BLOCK_MAX   32u
#define JIT_CACHE_BITS  12u
#define JIT_CACHE_SIZE  (1u << JIT_CACHE_BITS)

typedef struct {
	uint32_t pc;
	uint8_t *code;
	uint16_t ninsns;
} jit_slot;

struct t32_jit {
	jit_slot cache[JIT_CACHE_SIZE];
	uint8_t *arena;
	size_t arena_cap, arena_used;
	uint64_t blocks, hits;
};

/* ---------------- code arena ---------------- */

/* W^X: every block owns whole pages; after emission the pages flip
 * from RW to RX and are never written again. */
static uint8_t *arena_alloc(t32_jit *j, size_t n)
{
	uint8_t *p;
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);

	n = (n + pg - 1u) & ~(pg - 1u);
	if (j->arena_used + n > j->arena_cap)
		return NULL;
	p = j->arena + j->arena_used;
	j->arena_used += n;
	return p;
}

/* ---------------- decode helpers ---------------- */

static uint32_t jrd16(t32_cpu *c, uint32_t a)
{
	uint8_t *p = t32_ptr(c->mem, a, 2);

	if (!p) {
		c->trap = T32_TRAP_MEM;
		return 0;
	}
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

enum { COND_EQ, COND_NE, COND_CS, COND_CC, COND_MI, COND_PL, COND_VS,
       COND_VC, COND_HI, COND_LS, COND_GE, COND_LT, COND_GT, COND_LE,
       COND_AL };

enum { SH_LSL, SH_LSR, SH_ASR };

typedef struct {
	uint32_t size;
	int kind;
	int op, rd, rn, rm, flags, sh, cond;
	uint32_t imm;
} jop;

enum {
	K_ALU_RR, K_ALU_RI, K_MOVW, K_SHIFT_IMM, K_MEMOP,
	K_BRANCH, K_BRANCH_R, K_BRANCH_C, K_INTERP
};

static uint32_t thumb_expand_imm(uint32_t imm12)
{
	uint32_t rot = (imm12 >> 7) & 0x1Fu;

	if ((imm12 & 0xC00u) == 0) {
		switch (imm12 >> 8) {
		case 0: return imm12 & 0xFFu;
		case 1: return ((imm12 & 0xFFu) << 16) | (imm12 & 0xFFu);
		case 2: return ((imm12 & 0xFFu) << 24) |
			       ((imm12 & 0xFFu) << 8);
		default: return (imm12 & 0xFFu) * 0x01010101u;
		}
	}
	{
		uint32_t v = 0x80u | (imm12 & 0x7Fu);

		return rot ? ((v >> rot) | (v << (32u - rot))) : v;
	}
}

/* Classify one thumb insn for the JIT.  Anything not inline-safe
 * becomes K_INTERP (block end) or K_MEMOP (helper + continue). */
static void classify(t32_cpu *c, uint32_t pc, jop *o)
{
	uint32_t h = jrd16(c, pc);

	if (c->trap) {
		o->kind = K_INTERP;
		o->size = 2;
		return;
	}
	memset(o, 0, sizeof(*o));
	o->size = 2;

	/* ---- thumb16 ---- */
	if ((h & 0xF800u) == 0x0000u) {           /* lsl/lsr/asr rd,rm,#imm */
		int kind = (int)((h >> 11) & 3u);
		int sh = (int)((h >> 6) & 0x1Fu);

		if (kind < 3 && sh != 0) {
			o->kind = K_SHIFT_IMM;
			o->op = kind;
			o->rd = (int)(h & 7u);
			o->rm = (int)((h >> 3) & 7u);
			o->sh = sh;
			o->flags = F_SET;
			return;
		}
		o->kind = K_INTERP;               /* mov rd,rm (sh==0 form) */
		return;
	}
	if ((h & 0xF800u) == 0x1800u) {           /* add/sub rd,rn,rm/#imm3 */
		int imm = (h & 0x400u) != 0;

		o->kind = imm ? K_ALU_RI : K_ALU_RR;
		o->op = (h & 0x200u) ? JB_SUB : JB_ADD;
		o->rd = (int)(h & 7u);
		o->rn = (int)((h >> 3) & 7u);
		if (imm)
			o->imm = (h >> 6) & 7u;
		else
			o->rm = (int)((h >> 6) & 7u);
		o->flags = F_SET;
		return;
	}
	if ((h & 0xE000u) == 0x2000u) {           /* mov/cmp/add/sub rd,#8 */
		int top = (int)((h >> 11) & 7u);

		o->kind = K_ALU_RI;
		o->rd = (int)((h >> 8) & 7u);
		o->imm = h & 0xFFu;
		o->flags = (top == 5) ? F_ONLY : F_SET;
		switch (top) {
		case 4: o->op = JB_MOV; break;               /* movs */
		case 5: o->op = JB_SUB; o->rn = o->rd; break; /* cmp (F_ONLY) */
		case 6: o->op = JB_ADD; o->rn = o->rd; break; /* adds */
		default: o->op = JB_SUB; o->rn = o->rd; break; /* subs */
		}
		return;
	}
	if ((h & 0xFC00u) == 0x4000u) {           /* data-processing reg */
		int op = (int)((h >> 6) & 0xFu);

		o->rd = (int)(h & 7u);
		o->rm = (int)((h >> 3) & 7u);
		o->flags = F_SET;
		switch (op) {
		case 0x0:                           /* and */
			o->kind = K_ALU_RR;
			o->op = JB_AND;
			o->rn = o->rd;
			return;
		case 0x1:                           /* eor */
			o->kind = K_ALU_RR;
			o->op = JB_EOR;
			o->rn = o->rd;
			return;
		case 0xC:                           /* orr */
			o->kind = K_ALU_RR;
			o->op = JB_ORR;
			o->rn = o->rd;
			return;
		default:                            /* adc/sbc/ror/tst..mvn */
			o->kind = K_INTERP;
			return;
		}
	}
	if ((h & 0xFC00u) == 0x4400u) {           /* high-reg ops */
		int op = (int)((h >> 8) & 3u);
		int rd = (int)(h & 7u) | (int)((h >> 4) & 8u);
		int rm = (int)((h >> 3) & 0xFu);

		if (op == 0 && rd != 15) {          /* add rd,rm */
			o->kind = K_ALU_RR;
			o->op = JB_ADD;
			o->rd = rd;
			o->rn = rd;
			o->rm = rm;
			o->flags = F_NONE;
			return;
		}
		if (op == 2 && rd != 15) {          /* mov rd,rm */
			o->kind = K_ALU_RR;
			o->op = JB_MOV;
			o->rd = rd;
			o->rm = rm;
			o->flags = F_NONE;
			return;
		}
		o->kind = K_INTERP;                 /* cmp/cmn/bx/blx */
		return;
	}
	if ((h & 0xFF80u) == 0x4700u) {           /* bx/blx rm */
		o->kind = K_BRANCH_R;
		o->rm = (int)((h >> 3) & 0xFu);
		o->op = (h & 0x80u) ? 1 : 0;
		return;
	}
	if ((h & 0xF500u) == 0xB100u) {           /* cbz/cbnz */
		int nz = (h & 0x800u) != 0;

		o->kind = K_BRANCH_C;
		o->cond = nz ? COND_NE : COND_EQ;
		o->rd = (int)(h & 7u);
		o->op = 2;                        /* register-test marker */
		o->imm = pc + 4u +
			 ((uint32_t)(((h >> 3) & 0x1Fu) |
			 ((h >> 2) & 0x20u)) * 2u);
		return;
	}
	if ((h & 0xF000u) == 0x5000u ||           /* ldr/str reg-offset */
	    (h & 0xE000u) == 0x6000u ||           /* ldr/str imm5 */
	    (h & 0xF000u) == 0x8000u ||           /* ldrh/strh imm5 */
	    (h & 0xF000u) == 0x9000u) {           /* ldr/str rd,[sp,#] */
		o->kind = K_MEMOP;
		return;
	}
	if ((h & 0xF000u) == 0xD000u) {           /* b.cond / undef */
		if (((h >> 8) & 0xFu) >= 0xEu) {
			o->kind = K_INTERP;
			return;
		}
		o->kind = K_BRANCH_C;
		o->cond = (int)((h >> 8) & 0xFu);
		o->imm = pc + 4u +
			 ((uint32_t)(int32_t)(int8_t)(h & 0xFFu) * 2u);
		return;
	}
	if ((h & 0xF800u) == 0xE000u) {           /* b */
		int32_t off = (int32_t)(h & 0x7FFu);

		if (off & 0x400)
			off |= ~0x7FF;
		o->kind = K_BRANCH;
		o->imm = pc + 4u + (uint32_t)(off * 2);
		return;
	}

	/* ---- thumb32 ---- */
	if ((h & 0xF800u) == 0xE800u || (h & 0xF800u) == 0xF000u ||
	    (h & 0xF800u) == 0xF800u) {
		uint32_t h2 = jrd16(c, pc + 2u);

		if (c->trap) {
			o->kind = K_INTERP;
			return;
		}
		o->size = 4;

		/* b.w / bl.w:  imm32 = S:I1:I2:imm10:imm11:0, I = NOT(J xor S) */
		if ((h & 0xF800u) == 0xF000u && (h2 & 0xC000u) == 0xC000u) {
			uint32_t s = (h >> 10) & 1u;
			uint32_t j1 = (h2 >> 13) & 1u;
			uint32_t j2 = (h2 >> 11) & 1u;
			uint32_t i1 = (j1 ^ s) ^ 1u;
			uint32_t i2 = (j2 ^ s) ^ 1u;
			int32_t off = (int32_t)((s << 24) | (i1 << 23) |
				(i2 << 22) | ((h & 0x3FFu) << 12) |
				((h2 & 0x7FFu) << 1));

			off <<= 7;
			off >>= 7;
			o->kind = K_BRANCH;
			o->imm = pc + 4u + (uint32_t)off;
			o->op = ((h2 >> 12) & 3u) == 3u ? 1 : 0; /* BL? */
			return;
		}
		/* b.w cond:  imm32 = S:J2:J1:imm6:imm11:0 */
		if ((h & 0xF800u) == 0xF000u && (h2 & 0xD000u) == 0x8000u) {
			uint32_t s = (h >> 10) & 1u;
			uint32_t j1 = (h2 >> 13) & 1u;
			uint32_t j2 = (h2 >> 11) & 1u;
			int32_t off = (int32_t)((s << 20) | (j2 << 19) |
				(j1 << 18) | ((h & 0x3Fu) << 12) |
				((h2 & 0x7FFu) << 1));

			off <<= 11;
			off >>= 11;
			o->kind = K_BRANCH_C;
			o->cond = (int)((h >> 6) & 0xFu);
			o->imm = pc + 4u + (uint32_t)off;
			return;
		}
		/* movw / movt */
		if ((h & 0xFBF0u) == 0xF240u) {
			uint32_t imm4 = h & 0xFu;
			uint32_t i = (h >> 10) & 1u;
			uint32_t imm3 = (h2 >> 12) & 7u;
			uint32_t imm8 = h2 & 0xFFu;

			o->kind = K_MOVW;
			o->rd = (int)((h2 >> 8) & 0xFu);
			o->imm = (imm4 << 12) | (i << 11) | (imm3 << 8) |
				 imm8;
			o->op = (h & 0x0080u) ? 1 : 0;
			return;
		}
		/* DP modified immediate */
		if ((h & 0xFA00u) == 0xF000u && (h2 & 0x8000u) == 0) {
			uint32_t opx = (h >> 5) & 0xFu;
			int rn = (int)(h & 0xFu);
			int rd = (int)((h2 >> 8) & 0xFu);
			int s = (h & 0x0010u) != 0;
			uint32_t imm12 = ((h & 0x0400u) << 1) |
					 ((h >> 4) & 0x7Fu) | (h2 & 0xFFu);
			uint32_t rot = (imm12 >> 7) & 0x1Fu;
			int alu = -1;

			switch (opx) {
			case 0x0: alu = JB_AND; break;   /* AND/TST */
			case 0x2: alu = (rn == 15) ? JB_MOV : JB_ORR; break;
			case 0x3: alu = JB_MVN; break;   /* ORN/MVN */
			case 0x4: alu = JB_EOR; break;   /* EOR/TEQ */
			case 0x8: alu = JB_ADD; break;   /* ADD/CMN */
			case 0xD: alu = JB_SUB; break;   /* SUB/CMP */
			default: alu = -1; break;        /* BIC/ADC/SBC/RSB */
			}
			if (alu < 0 || (alu == JB_MVN && rn != 15)) {
				o->kind = K_INTERP;      /* ORN (rn != 15) */
			} else if ((alu == JB_AND || alu == JB_ORR ||
				    alu == JB_EOR) && s && rot != 0) {
				o->kind = K_INTERP;      /* shifter carry */
			} else {
				o->kind = K_ALU_RI;
				o->op = alu;
				o->rd = rd;
				o->rn = rn;
				o->flags = s ? F_SET : F_NONE;
				o->imm = thumb_expand_imm(imm12);
				if (rd == 15)
					o->kind = s ? K_INTERP : K_INTERP;
			}
			return;
		}
		/* DP register (non-shifted only) */
		if ((h & 0xFA00u) == 0xEA00u && (h2 & 0x8000u) == 0) {
			uint32_t opx = (h >> 5) & 0xFu;
			int rn = (int)(h & 0xFu);
			int rd = (int)((h2 >> 8) & 0xFu);
			int s = (h & 0x0010u) != 0;
			int rm = (int)(h2 & 0xFu);
			int alu = -1;

			switch (opx) {
			case 0x0: alu = JB_AND; break;
			case 0x2: alu = (rn == 15) ? JB_MOV : JB_ORR; break;
			case 0x3: alu = JB_MVN; break;
			case 0x4: alu = JB_EOR; break;
			case 0x8: alu = JB_ADD; break;
			case 0xD: alu = JB_SUB; break;
			default: alu = -1; break;
			}
			if (alu < 0 || (alu == JB_MVN && rn != 15) ||
			    ((h2 >> 4) & 0xFFu) != 0u || rd == 15) {
				o->kind = K_INTERP;
			} else {
				o->kind = K_ALU_RR;
				o->op = alu;
				o->rd = rd;
				o->rn = rn;
				o->rm = rm;
				o->flags = s ? F_SET : F_NONE;
			}
			return;
		}
		/* everything else in the 32-bit space: interpret */
		o->kind = K_INTERP;
		return;
	}

	o->kind = K_INTERP;
}

/* ---------------- translation ---------------- */

static int emit_op(t32_jb *b, const jop *o, uint32_t pc, uint32_t *next_pc)
{
	switch (o->kind) {
	case K_ALU_RR:
		t32_jb_alu_rr(b, o->op, o->rd, o->rn, o->rm, o->flags);
		break;
	case K_ALU_RI:
		t32_jb_alu_ri(b, o->op, o->rd, o->rn, o->imm, o->flags);
		break;
	case K_MOVW:
		t32_jb_movw(b, o->rd, o->imm, o->op);
		break;
	case K_SHIFT_IMM:
		t32_jb_shift_imm(b, o->op, o->rd, o->rm, o->sh, o->flags);
		break;
	case K_MEMOP:
		t32_jb_step(b, pc);
		break;
	case K_BRANCH:
		if (o->op)
			t32_jb_setlr(b, (pc + o->size) | 1u);
		t32_jb_exit(b, T32_JIT_NEXT, o->imm);
		*next_pc = 0;
		return 1;
	case K_BRANCH_R:
		if (o->op)
			t32_jb_setlr(b, (pc + o->size) | 1u);
		t32_jb_exit_ind(b, o->rm);
		*next_pc = 0;
		return 1;
	case K_BRANCH_C:
		if (o->op == 2)
			t32_jb_exit_cbr(b, o->rd, o->cond, o->imm,
					pc + o->size);
		else
			t32_jb_exit_cc(b, o->cond, o->imm, pc + o->size);
		*next_pc = 0;
		return 1;
	default:
		t32_jb_exit(b, T32_JIT_INTERP, pc);
		*next_pc = 0;
		return 1;
	}
	*next_pc = pc + o->size;
	return 0;
}

static uint8_t *translate_cpu(t32_jit *j, t32_cpu *c, uint32_t pc,
			      uint16_t *ninsns, size_t *blk_size)
{
	t32_jb b;
	uint8_t *code;
	uint32_t cur = pc;
	unsigned n;
	size_t max_code = 16384;

	code = arena_alloc(j, max_code);
	if (!code)
		return NULL;
	t32_jb_init(&b, code, max_code);
	t32_jb_prologue(&b);
	for (n = 0; n < JIT_BLOCK_MAX; n++) {
		jop o;
		uint32_t next;

		if (c->trap) {
			t32_jb_exit(&b, T32_JIT_TRAP, cur);
			break;
		}
		classify(c, cur, &o);
		if (emit_op(&b, &o, cur, &next))
			break;
		cur = next;
	}
	if (n == JIT_BLOCK_MAX)
		t32_jb_exit(&b, T32_JIT_NEXT, cur);
	if (b.err)
		return NULL;
	*ninsns = (uint16_t)(n + 1u);
	*blk_size = max_code;
	/* seal: RW -> RX */
	if (mprotect(code, max_code, PROT_READ | PROT_EXEC) != 0)
		return NULL;
	return code;
}

/* ---------------- cache ---------------- */

static inline uint32_t cache_hash(uint32_t pc)
{
	uint32_t x = pc >> 1;

	x ^= x >> 16;
	x *= 0x45D9F3Bu;
	x ^= x >> 16;
	return x & (JIT_CACHE_SIZE - 1u);
}

static jit_slot *cache_slot(t32_jit *j, uint32_t pc)
{
	uint32_t h = cache_hash(pc);
	unsigned i;

	for (i = 0; i < 8u; i++) {
		jit_slot *s = &j->cache[(h + i) & (JIT_CACHE_SIZE - 1u)];

		if (!s->code || s->pc == pc)
			return s;
	}
	return &j->cache[h];
}

/* ---------------- dispatcher ---------------- */

int t32_jit_run(t32_jit *j, t32_cpu *c, uint32_t pc, uint32_t *npc)
{
	for (;;) {
		jit_slot *s;
		int kind;
		uint32_t next = pc;

		if (c->trap) {
			*npc = pc;
			return T32_JIT_TRAP;
		}
		if (!(c->cpsr & T32_CPSR_T)) {
			*npc = pc;
			return T32_JIT_INTERP;    /* ARM mode: caller runs */
		}
		if (pc == T32_LR_SENTINEL) {
			*npc = pc;
			return T32_JIT_IND;
		}
		if (t32_shim_at(c->mem, pc)) {
			*npc = pc;
			return T32_JIT_SHIM;
		}
		s = cache_slot(j, pc);
		if (!s->code || s->pc != pc) {
			uint16_t ni = 0;
			size_t bsz = 0;
			uint8_t *code = translate_cpu(j, c, pc, &ni, &bsz);

			if (!code) {
				*npc = pc;
				return T32_JIT_INTERP;
			}
			s->pc = pc;
			s->code = code;
			s->ninsns = ni;
			j->blocks++;
		} else {
			j->hits++;
		}
		if (c->insn_budget <= s->ninsns) {
			c->trap = T32_TRAP_LIMIT;
			*npc = pc;
			return T32_JIT_TRAP;
		}
		c->insn_budget -= s->ninsns;
		kind = t32_jb_run(s->code, c, &next);
		switch (kind) {
		case T32_JIT_NEXT:
			pc = next;
			continue;
		case T32_JIT_IND:
			if (next & 1u)
				c->cpsr |= T32_CPSR_T;
			else
				c->cpsr &= ~T32_CPSR_T;
			pc = next & ~1u;
			continue;
		case T32_JIT_INTERP: {
			uint32_t step_next = pc;

			if (t32_fallback_step(c, next, &step_next)) {
				*npc = next;
				return T32_JIT_TRAP;
			}
			pc = step_next;
			continue;
		}
		case T32_JIT_TRAP:
			*npc = next;
			return T32_JIT_TRAP;
		default:
			*npc = next;
			return kind;
		}
	}
}

void t32_jit_stats(t32_jit *j, uint64_t *blocks, uint64_t *hits,
		   uint64_t *exits)
{
	if (blocks)
		*blocks = j->blocks;
	if (hits)
		*hits = j->hits;
	if (exits)
		*exits = j->arena_used;
}

/* ---------------- lifecycle ---------------- */

t32_jit *t32_jit_create(void)
{
	t32_jit *j = calloc(1, sizeof(*j));

	if (!j)
		return NULL;
	j->arena_cap = 16u << 20;
	j->arena = mmap(NULL, j->arena_cap, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (j->arena == MAP_FAILED) {
		j->arena = NULL;
		free(j);
		return NULL;
	}
	return j;
}

void t32_jit_destroy(t32_jit *j)
{
	if (!j)
		return;
	if (j->arena)
		munmap(j->arena, j->arena_cap);
	free(j);
}
