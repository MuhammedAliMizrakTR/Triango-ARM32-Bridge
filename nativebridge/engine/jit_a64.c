/* Triango ARM32 Bridge — AArch64 JIT backend (AAPCS64).
 *
 * Block = int fn(t32_cpu *c, uint32_t *next);  ctx = x19, next-ptr = x20.
 * adds/subs set PSTATE.NZCV with exactly the A32 meaning (subs C =
 * NOT-borrow), and `tst` sets N/Z while preserving C/V — so guest flags
 * merge into cpu->cpsr (same 31:28 layout) via `mrs x, nzcv`.
 */
#if defined(__aarch64__)

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "engine_internal.h"
#include "jit.h"
#include "jit_be.h"

#define R_OFF   ((int)offsetof(t32_cpu, r))
#define C_OFF   ((int)offsetof(t32_cpu, cpsr))

static void em(t32_jb *b, const void *d, size_t n)
{
	if (b->p + n > b->end) {
		b->err = 1;
		return;
	}
	memcpy(b->p, d, n);
	b->p += n;
}

static void u32(t32_jb *b, uint32_t v) { em(b, &v, 4); }
static int goff(int g) { return R_OFF + 4 * g; }

/* ---- base encoders ---- */

static void emit_movz(t32_jb *b, int rd, uint32_t imm16, int hw)
{
	u32(b, 0x52800000u | ((uint32_t)hw << 21) |
	     ((imm16 & 0xFFFFu) << 5) | (uint32_t)rd);
}
static void emit_movk(t32_jb *b, int rd, uint32_t imm16, int hw)
{
	u32(b, 0x72800000u | ((uint32_t)hw << 21) |
	     ((imm16 & 0xFFFFu) << 5) | (uint32_t)rd);
}
static void emit_mov32(t32_jb *b, int rd, uint32_t v)
{
	emit_movz(b, rd, v & 0xFFFFu, 0);
	if (v >> 16)
		emit_movk(b, rd, (uint16_t)(v >> 16), 1);
}

static void emit_ldr_ctx(t32_jb *b, int rt, int off)
{
	u32(b, 0xB9400000u | ((uint32_t)(off / 4) << 10) | (19u << 5) |
	     (uint32_t)rt);
}
static void emit_str_ctx(t32_jb *b, int rt, int off)
{
	u32(b, 0xB9000000u | ((uint32_t)(off / 4) << 10) | (19u << 5) |
	     (uint32_t)rt);
}

static void emit_add_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x0B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_adds_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x2B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_subs_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x6B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_and_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x0A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_orr_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x2A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_eor_rr(t32_jb *b, int rd, int rn, int rm)
{
	u32(b, 0x4A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);
}
static void emit_mvn_rr(t32_jb *b, int rd, int rm)
{
	u32(b, 0x2A200000u | ((uint32_t)rm << 16) | (31u << 5) |
	     (uint32_t)rd);                        /* orn wd, wzr, wm */
}
static void emit_tst_rr(t32_jb *b, int rn, int rm)
{
	u32(b, 0x6A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
	     31u);                                 /* ands wzr, wn, wm */
}
static void emit_mov_rr(t32_jb *b, int rd, int rn)
{
	u32(b, 0x2A000000u | ((uint32_t)rn << 16) | (31u << 5) |
	     (uint32_t)rd);
}

static void emit_lsl_imm(t32_jb *b, int rd, int rn, int sh)
{
	u32(b, 0x53000000u | ((uint32_t)((32 - sh) & 31) << 16) |
	     ((uint32_t)(31 - sh) << 10) | ((uint32_t)rn << 5) |
	     (uint32_t)rd);                        /* ubfm */
}
static void emit_lsr_imm(t32_jb *b, int rd, int rn, int sh)
{
	u32(b, 0x53000000u | ((uint32_t)sh << 16) | (31u << 10) |
	     ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void emit_asr_imm(t32_jb *b, int rd, int rn, int sh)
{
	u32(b, 0x13000000u | ((uint32_t)sh << 16) | (31u << 10) |
	     ((uint32_t)rn << 5) | (uint32_t)rd);
}

static void emit_mrs_nzcv(t32_jb *b, int rd)
{
	u32(b, 0xD53B4200u | (uint32_t)rd);
}

static void emit_cbz(t32_jb *b, int rt, int32_t off19)
{
	u32(b, 0x34000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) |
	     (uint32_t)rt);
}
static void emit_cbnz(t32_jb *b, int rt, int32_t off19)
{
	u32(b, 0x35000000u | (((uint32_t)off19 & 0x7FFFFu) << 5) |
	     (uint32_t)rt);
}
static void emit_blr(t32_jb *b, int rn)
{
	u32(b, 0xD63F0000u | ((uint32_t)rn << 5));
}
static void emit_ret(t32_jb *b) { u32(b, 0xD65F03C0u); }

static void emit_cmp_imm(t32_jb *b, int rn, uint32_t imm12)
{
	u32(b, 0x71000000u | ((imm12 & 0xFFFu) << 10) |
	     ((uint32_t)rn << 5) | 31u);
}
static void emit_cset(t32_jb *b, int rd, int cond)
{
	u32(b, 0x1A9F07E0u | ((uint32_t)((cond ^ 1) & 0xF) << 12) |
	     (uint32_t)rd);
}

/* patch helpers */
static void patch_cb(t32_jb *b, size_t at)
{
	int32_t rel = (int32_t)(((b->p - b->start) - at) / 4u);
	uint32_t ins;

	memcpy(&ins, b->start + at, 4);
	ins = (ins & ~0x00FFFFE0u) | (((uint32_t)rel & 0x7FFFFu) << 5);
	memcpy(b->start + at, &ins, 4);
}

/* merge PSTATE.NZCV (x3) into cpsr, keeping `keep` bits from memory */
static void commit_flags(t32_jb *b, uint32_t keep)
{
	emit_mrs_nzcv(b, 3);
	emit_ldr_ctx(b, 4, C_OFF);
	emit_mov32(b, 5, keep);
	emit_mov32(b, 6, ~keep & 0xF0000000u);
	u32(b, 0x0A060063u);              /* and w3, w3, w6 (mrs part) */
	u32(b, 0x0A050085u);              /* and w5, w4, w5 (kept part) */
	u32(b, 0x2A050063u);              /* orr w3, w3, w5 */
	emit_mov32(b, 5, 0x0FFFFFFFu);
	u32(b, 0x0A050084u);              /* and w4, w4, w5 */
	u32(b, 0x2A030084u);              /* orr w4, w4, w3 */
	emit_str_ctx(b, 4, C_OFF);
}

static void epilogue(t32_jb *b)
{
	u32(b, 0xA8C153F3u);   /* ldp x19, x20, [sp], #16 */
	u32(b, 0xA8C17BFDu);   /* ldp x29, x30, [sp], #16 */
	emit_ret(b);
}

/* ---- public API ---- */

void t32_jb_init(t32_jb *b, uint8_t *code, size_t cap)
{
	b->start = b->p = code;
	b->end = code + cap;
	b->err = 0;
}

void t32_jb_prologue(t32_jb *b)
{
	u32(b, 0xA9BF7BFDu);   /* stp x29, x30, [sp, #-16]! */
	u32(b, 0xA9BF53F3u);   /* stp x19, x20, [sp, #-16]! */
	u32(b, 0xAA0003F3u);   /* mov x19, x0 */
	u32(b, 0xAA0103F4u);   /* mov x20, x1 */
}

void t32_jb_exit(t32_jb *b, int kind, uint32_t pc)
{
	emit_mov32(b, 2, pc);
	u32(b, 0xB9000002u | (20u << 5));  /* str w2, [x20] */
	emit_mov32(b, 0, (uint32_t)kind);
	epilogue(b);
}

void t32_jb_exit_ind(t32_jb *b, int greg)
{
	emit_ldr_ctx(b, 2, goff(greg));
	u32(b, 0xB9000002u | (20u << 5));
	emit_mov32(b, 0, T32_JIT_IND);
	epilogue(b);
}

/* compute condition into w6 (0/1); nzcv nibble in w3 */
static void cond_w6(t32_jb *b, int cond)
{
	emit_mrs_nzcv(b, 3);
	u32(b, 0x531C7C63u);              /* lsr w3, w3, #28 */
	switch (cond) {
	case 0: case 1:
		emit_mov32(b, 4, 4u);
		u32(b, 0x0A040065u);      /* and w5, w3, w4 (Z) */
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 6, (cond == 0) ? 1 : 0);
		break;
	case 2: case 3:
		emit_mov32(b, 4, 2u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 6, (cond == 2) ? 1 : 0);
		break;
	case 4: case 5:
		emit_mov32(b, 4, 8u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 6, (cond == 4) ? 1 : 0);
		break;
	case 6: case 7:
		emit_mov32(b, 4, 1u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 6, (cond == 6) ? 1 : 0);
		break;
	case 8: case 9:                   /* HI: C && !Z ; LS: !(HI) */
		emit_mov32(b, 4, 2u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 6, 1);          /* C != 0 -> NE */
		emit_mov32(b, 4, 4u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 7, 0);          /* Z == 0 -> EQ */
		u32(b, 0x0A0700C6u);          /* and w6, w6, w7 */
		if (cond == 9)
			u32(b, 0x520000C6u);  /* eor w6, w6, #1 */
		break;
	case 10: case 11:                 /* GE: N == V */
		emit_mov32(b, 4, 8u);
		u32(b, 0x0A040065u);          /* w5 = N */
		emit_mov32(b, 4, 1u);
		u32(b, 0x0A040067u);          /* w7 = V */
		u32(b, 0x6B0700BFu | (7u << 16) | (5u << 5)); /* cmp w5, w7 */
		emit_cset(b, 6, (cond == 10) ? 0 : 1);
		break;
	case 12: case 13:                 /* GT: Z==0 && N==V */
		emit_mov32(b, 4, 4u);
		u32(b, 0x0A040065u);
		emit_cmp_imm(b, 5, 0u);
		emit_cset(b, 7, 0);          /* !Z -> EQ */
		emit_mov32(b, 4, 8u);
		u32(b, 0x0A040065u);
		emit_mov32(b, 4, 1u);
		u32(b, 0x0A040066u);          /* w6 = V */
		u32(b, 0x6B0600BFu | (6u << 16) | (5u << 5)); /* cmp w5, w6 */
		emit_cset(b, 6, 0);
		u32(b, 0x0A0700C6u);
		if (cond == 13)
			u32(b, 0x520000C6u);
		break;
	default:
		emit_mov32(b, 6, 1u);
		break;
	}
}

static void emit_exit_pair(t32_jb *b, uint32_t tpc, uint32_t fpc)
{
	size_t at;

	emit_cbnz(b, 6, 0);
	at = (size_t)(b->p - b->start) - 4u;
	t32_jb_exit(b, T32_JIT_NEXT, fpc);
	patch_cb(b, at);
	t32_jb_exit(b, T32_JIT_NEXT, tpc);
}

void t32_jb_exit_cc(t32_jb *b, int cond, uint32_t tpc, uint32_t fpc)
{
	if (cond == 14) {
		t32_jb_exit(b, T32_JIT_NEXT, tpc);
		return;
	}
	cond_w6(b, cond);
	emit_exit_pair(b, tpc, fpc);
}

void t32_jb_exit_cbr(t32_jb *b, int greg, int cond, uint32_t tpc,
		     uint32_t fpc)
{
	size_t at;

	emit_ldr_ctx(b, 2, goff(greg));
	emit_cbnz(b, 2, 0);
	at = (size_t)(b->p - b->start) - 4u;
	if (cond == 0) {
		/* cbz: reg == 0 -> tpc */
		t32_jb_exit(b, T32_JIT_NEXT, tpc);
		patch_cb(b, at);
		t32_jb_exit(b, T32_JIT_NEXT, fpc);
	} else {
		/* cbnz: reg != 0 -> tpc */
		t32_jb_exit(b, T32_JIT_NEXT, fpc);
		patch_cb(b, at);
		t32_jb_exit(b, T32_JIT_NEXT, tpc);
	}
}

void t32_jb_alu_rr(t32_jb *b, int op, int rd, int rn, int rm, int flags)
{
	int is_mov = (op == JB_MOV), is_mvn = (op == JB_MVN);

	if (is_mov || is_mvn)
		emit_ldr_ctx(b, 2, goff(rm));
	else
		emit_ldr_ctx(b, 2, goff(rn));
	if (!is_mov && !is_mvn)
		emit_ldr_ctx(b, 3, goff(rm));
	switch (op) {
	case JB_ADD:
		if (flags)
			emit_adds_rr(b, 4, 2, 3);
		else
			emit_add_rr(b, 4, 2, 3);
		break;
	case JB_SUB:
		emit_subs_rr(b, 4, 2, 3);
		break;
	case JB_AND:
		emit_and_rr(b, 4, 2, 3);
		if (flags)
			emit_tst_rr(b, 4, 4);
		break;
	case JB_ORR:
		emit_orr_rr(b, 4, 2, 3);
		if (flags)
			emit_tst_rr(b, 4, 4);
		break;
	case JB_EOR:
		emit_eor_rr(b, 4, 2, 3);
		if (flags)
			emit_tst_rr(b, 4, 4);
		break;
	case JB_MOV:
		emit_mov_rr(b, 4, 2);
		if (flags)
			emit_tst_rr(b, 4, 4);
		break;
	case JB_MVN:
		emit_mvn_rr(b, 4, 2);
		if (flags)
			emit_tst_rr(b, 4, 4);
		break;
	default:
		b->err = 1;
		return;
	}
	if (flags == F_SET || flags == F_ONLY) {
		if (op == JB_ADD || op == JB_SUB)
			commit_flags(b, 0u);
		else
			commit_flags(b, 0x30000000u);
	}
	if (flags != F_ONLY)
		emit_str_ctx(b, 4, goff(rd));
}

void t32_jb_alu_ri(t32_jb *b, int op, int rd, int rn, uint32_t imm,
		   int flags)
{
	if (op == JB_MOV) {
		emit_mov32(b, 4, imm);
	} else if (op == JB_MVN) {
		emit_mov32(b, 4, ~imm);
	} else {
		emit_ldr_ctx(b, 2, goff(rn));
		emit_mov32(b, 3, imm);
		switch (op) {
		case JB_ADD:
			if (flags)
				emit_adds_rr(b, 4, 2, 3);
			else
				emit_add_rr(b, 4, 2, 3);
			break;
		case JB_SUB:
			emit_subs_rr(b, 4, 2, 3);
			break;
		case JB_AND:
			emit_and_rr(b, 4, 2, 3);
			if (flags)
				emit_tst_rr(b, 4, 4);
			break;
		case JB_ORR:
			emit_orr_rr(b, 4, 2, 3);
			if (flags)
				emit_tst_rr(b, 4, 4);
			break;
		case JB_EOR:
			emit_eor_rr(b, 4, 2, 3);
			if (flags)
				emit_tst_rr(b, 4, 4);
			break;
		default:
			b->err = 1;
			return;
		}
	}
	if (flags == F_SET || flags == F_ONLY) {
		if (op == JB_ADD || op == JB_SUB)
			commit_flags(b, 0u);
		else
			commit_flags(b, 0x30000000u);
	}
	if (flags != F_ONLY)
		emit_str_ctx(b, 4, goff(rd));
}

void t32_jb_movw(t32_jb *b, int rd, uint32_t imm16, int is_movt)
{
	if (is_movt) {
		emit_ldr_ctx(b, 2, goff(rd));
		emit_mov32(b, 3, 0xFFFFu);
		emit_and_rr(b, 2, 2, 3);
		emit_mov32(b, 3, (imm16 & 0xFFFFu) << 16);
		emit_orr_rr(b, 4, 2, 3);
	} else {
		emit_mov32(b, 4, imm16 & 0xFFFFu);
	}
	emit_str_ctx(b, 4, goff(rd));
}

void t32_jb_shift_imm(t32_jb *b, int op, int rd, int rm, int sh, int flags)
{
	emit_ldr_ctx(b, 2, goff(rm));
	if (flags) {
		int cbit = (op == 0) ? (31 - sh) : (sh - 1);

		emit_lsr_imm(b, 5, 2, (uint32_t)cbit);
		emit_mov32(b, 6, 1u);
		u32(b, 0x0A0600A5u);          /* and w5, w5, w6 (C) */
	}
	switch (op) {
	case 0: emit_lsl_imm(b, 4, 2, (uint32_t)sh); break;
	case 1: emit_lsr_imm(b, 4, 2, (uint32_t)sh); break;
	default: emit_asr_imm(b, 4, 2, (uint32_t)sh); break;
	}
	if (flags == F_SET || flags == F_ONLY) {
		emit_tst_rr(b, 4, 4);         /* N/Z; C/V stale */
		/* build: N,Z from mrs; V from memory; C from w5 */
		emit_mrs_nzcv(b, 3);
		emit_ldr_ctx(b, 6, C_OFF);
		emit_mov32(b, 7, 0xC0000000u);
		u32(b, 0x0A070063u);          /* w3 = N|Z */
		emit_mov32(b, 7, 0x10000000u);
		u32(b, 0x0A0700C7u);          /* w7 = old V */
		u32(b, 0x2A070063u);          /* w3 |= V */
		u32(b, 0x530308A5u);          /* lsl w5, w5, #29 (C bit) */
		u32(b, 0x2A050063u);          /* w3 |= C */
		emit_mov32(b, 7, 0x0FFFFFFFu);
		u32(b, 0x0A0700C6u);          /* w6 &= ~NZCV */
		u32(b, 0x2A0300C6u);          /* w6 |= w3 */
		emit_str_ctx(b, 6, C_OFF);
	}
	if (flags != F_ONLY)
		emit_str_ctx(b, 4, goff(rd));
}

void t32_jb_mul(t32_jb *b, int rd, int rn, int rm)
{
	emit_ldr_ctx(b, 2, goff(rn));
	emit_ldr_ctx(b, 3, goff(rm));
	u32(b, 0x1B007C00u | (3u << 16) | (2u << 5) | (uint32_t)4);
	emit_str_ctx(b, 4, goff(rd));
}

void t32_jb_setlr(t32_jb *b, uint32_t pc)
{
	emit_mov32(b, 4, pc);
	emit_str_ctx(b, 4, goff(14));
}

void t32_jb_step(t32_jb *b, uint32_t pc)
{
	uint64_t fn = (uint64_t)(uintptr_t)t32_fallback_step;
	size_t at;

	u32(b, 0xAA1303E0u);              /* mov x0, x19 */
	emit_mov32(b, 1, pc);
	u32(b, 0xD10043FFu);              /* sub sp, sp, #16 */
	u32(b, 0x910003E2u);              /* mov x2, sp (npc scratch) */
	emit_movz(b, 8, (uint16_t)(fn & 0xFFFFu), 0);
	emit_movk(b, 8, (uint16_t)(fn >> 16), 1);
	emit_movk(b, 8, (uint16_t)(fn >> 32), 2);
	emit_movk(b, 8, (uint16_t)(fn >> 48), 3);
	emit_blr(b, 8);
	u32(b, 0x910043FFu);              /* add sp, sp, #16 */
	emit_cbnz(b, 0, 0);               /* trap when nonzero */
	at = (size_t)(b->p - b->start) - 4u;
	t32_jb_exit(b, T32_JIT_TRAP, pc);
	patch_cb(b, at);
}

int t32_jb_run(uint8_t *code, void *cpu, uint32_t *next)
{
	int (*fn)(t32_cpu *, uint32_t *) =
		(int (*)(t32_cpu *, uint32_t *))(void *)code;

	return fn((t32_cpu *)cpu, next);
}

#else
typedef int jit_a64_unused;
#endif
