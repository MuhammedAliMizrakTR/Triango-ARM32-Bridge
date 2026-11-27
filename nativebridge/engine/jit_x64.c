/* Triango ARM32 Bridge — x86-64 JIT backend (System V ABI).
 *
 * Block = int fn(t32_cpu *c, uint32_t *next);  ctx = rbx, next-ptr = r12.
 * Exits return kind in eax and store the exit pc through r12.
 * Flag-setting ops compute ARM NZCV (cpsr bits 31:28) exactly; logical
 * ops preserve C/V.  The single-step helper call keeps semantic parity
 * with the interpreter for loads/stores and other complex insns.
 */
#if defined(__x86_64__)

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

static void u8(t32_jb *b, uint8_t v)   { em(b, &v, 1); }
static void u32(t32_jb *b, uint32_t v) { em(b, &v, 4); }
static void u64(t32_jb *b, uint64_t v) { em(b, &v, 8); }

/* ---- instruction emitters ---- */

static void mov_r32_imm32(t32_jb *b, int r, uint32_t v)
{
	if (r >= 8)
		u8(b, 0x41u);
	u8(b, (uint8_t)(0xB8u + (r & 7u)));
	u32(b, v);
}

static void mov_r64_imm64(t32_jb *b, int r, uint64_t v)
{
	u8(b, (uint8_t)(0x48u | ((r >> 3) & 1u)));
	u8(b, (uint8_t)(0xB8u + (r & 7u)));
	u64(b, v);
}

static int goff(int g) { return R_OFF + 4 * g; }

/* mov r32, [rbx + off] */
static void ldr_ctx(t32_jb *b, int r, int off)
{
	if (r >= 8)
		u8(b, 0x44u);
	u8(b, 0x8Bu);
	u8(b, (uint8_t)(0x83u | ((r & 7u) << 3)));
	u32(b, (uint32_t)off);
}

/* mov [rbx + off], r32 */
static void str_ctx(t32_jb *b, int r, int off)
{
	if (r >= 8)
		u8(b, 0x44u);
	u8(b, 0x89u);
	u8(b, (uint8_t)(0x83u | ((r & 7u) << 3)));
	u32(b, (uint32_t)off);
}

/* op eax, r32 (01=add 29=sub 21=and 09=or 31=xor 89= mov) */
static void op_eax_r32(t32_jb *b, uint8_t opcode, int r)
{
	u8(b, opcode);
	u8(b, (uint8_t)(0xC0u | ((r & 7u) << 3)));
}

/* grp1 /ext with rm = eax: ext 0=add 1=or 4=and 5=sub 6=xor */
static void op_eax_imm32(t32_jb *b, int ext, uint32_t v)
{
	u8(b, 0x81u);
	u8(b, (uint8_t)(0xC0u | (ext << 3)));
	u32(b, v);
}

/* grp1 /ext with rm = edi */
static void op_edi_imm32(t32_jb *b, int ext, uint32_t v)
{
	u8(b, 0x81u);
	u8(b, (uint8_t)(0xC7u | (ext << 3)));
	u32(b, v);
}

static void test_eax_eax(t32_jb *b) { u8(b, 0x85u); u8(b, 0xC0u); }
static void test_edi_imm32(t32_jb *b, uint32_t v)
{
	u8(b, 0xF7u); u8(b, 0xC7u); u32(b, v);
}

/* setcc r8: cc 0=O 2=B(C) 3=AE(NC) 4=E(Z) 5=NE */
static void setcc(t32_jb *b, int cc, int r8)
{
	u8(b, 0x0Fu);
	u8(b, (uint8_t)(0x90u | cc));
	u8(b, (uint8_t)(0xC0u | (r8 & 7u)));
}

static void movzx_r32_r8(t32_jb *b, int dst, int src)
{
	u8(b, 0x0Fu);
	u8(b, 0xB6u);
	u8(b, (uint8_t)(0xC0u | ((dst & 7u) << 3) | (src & 7u)));
}

static void shl_r32_imm(t32_jb *b, int r, uint8_t n)
{
	u8(b, 0xC1u); u8(b, (uint8_t)(0xE0u | (r & 7u))); u8(b, n);
}
static void shr_r32_imm(t32_jb *b, int r, uint8_t n)
{
	u8(b, 0xC1u); u8(b, (uint8_t)(0xE8u | (r & 7u))); u8(b, n);
}
static void sar_r32_imm(t32_jb *b, int r, uint8_t n)
{
	u8(b, 0xC1u); u8(b, (uint8_t)(0xF8u | (r & 7u))); u8(b, n);
}

static void push_r(t32_jb *b, int r) { u8(b, (uint8_t)(0x50u + (r & 7u))); }
static void pop_r(t32_jb *b, int r)  { u8(b, (uint8_t)(0x58u + (r & 7u))); }

static void epilogue(t32_jb *b)
{
	u8(b, 0x48u); u8(b, 0x83u); u8(b, 0xC4u); u8(b, 0x08u); /* add rsp,8 */
	u8(b, 0x41u); u8(b, 0x5Cu);                             /* pop r12 */
	u8(b, 0x5Bu);                                           /* pop rbx */
	u8(b, 0xC3u);                                           /* ret */
}

/* store NZCV into cpsr: nibble builder in esi (bits 31:28 pre-shifted),
 * keep_mask applied to the old cpsr first (0x0FFFFFFF = replace all,
 * 0x30000000 = keep C,V, 0x10000000 = keep V) */
static void commit_flags(t32_jb *b, uint32_t keep_mask)
{
	shl_r32_imm(b, 6, 28);
	ldr_ctx(b, 7, C_OFF);
	op_edi_imm32(b, 4, keep_mask);
	op_eax_r32(b, 0x09u, 6);              /* or edi,esi — rm=eax?? */
	/* or edi,esi = 09 F7 */
	b->p -= 2;
	u8(b, 0x09u); u8(b, 0xF7u);
	str_ctx(b, 7, C_OFF);
}

/* N|Z into esi (bit3 = N, bit2 = Z) — result in eax */
static void build_nz(t32_jb *b)
{
	u8(b, 0x89u); u8(b, 0xC6u);           /* mov esi,eax */
	shr_r32_imm(b, 6, 28);
	u8(b, 0x83u); u8(b, 0xE6u); u8(b, 0x08u);  /* and esi,8 (N) */
	test_eax_eax(b);
	setcc(b, 4, 2);                       /* sete dl */
	movzx_r32_r8(b, 2, 2);
	shl_r32_imm(b, 2, 2);
	u8(b, 0x09u); u8(b, 0xD6u);           /* or esi,edx (Z) */
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
	u8(b, 0x53u);                         /* push rbx */
	u8(b, 0x41u); u8(b, 0x54u);           /* push r12 */
	u8(b, 0x48u); u8(b, 0x83u); u8(b, 0xECu); u8(b, 0x08u); /* sub rsp,8 */
	u8(b, 0x48u); u8(b, 0x89u); u8(b, 0xFBu);  /* mov rbx,rdi */
	u8(b, 0x49u); u8(b, 0x89u); u8(b, 0xF4u);  /* mov r12,rsi */
}

void t32_jb_exit(t32_jb *b, int kind, uint32_t pc)
{
	u8(b, 0x41u); u8(b, 0xC7u); u8(b, 0x04u); u8(b, 0x24u);  /* mov [r12],imm */
	u32(b, pc);
	mov_r32_imm32(b, 0, (uint32_t)kind);
	epilogue(b);
}

void t32_jb_exit_ind(t32_jb *b, int greg)
{
	ldr_ctx(b, 0, goff(greg));
	u8(b, 0x41u); u8(b, 0x89u); u8(b, 0x04u); u8(b, 0x24u);  /* [r12]=eax */
	mov_r32_imm32(b, 0, T32_JIT_IND);
	epilogue(b);
}

/* cond -> al, nzcv nibble in edi.
 * setcc 4 = sete (test-zero), 5 = setne. */
static void cond_al(t32_jb *b, int cond)
{
	int rev;

	switch (cond) {
	case 0: case 1:  test_edi_imm32(b, 4u); break;  /* Z */
	case 2: case 3:  test_edi_imm32(b, 2u); break;  /* C */
	case 4: case 5:  test_edi_imm32(b, 8u); break;  /* N */
	case 6: case 7:  test_edi_imm32(b, 1u); break;  /* V */
	default: break;
	}
	switch (cond) {
	case 0: setcc(b, 5, 0); break;          /* EQ: Z=1 -> setne */
	case 1: setcc(b, 4, 0); break;          /* NE */
	case 2: setcc(b, 5, 0); break;          /* CS */
	case 3: setcc(b, 4, 0); break;          /* CC */
	case 4: setcc(b, 5, 0); break;          /* MI */
	case 5: setcc(b, 4, 0); break;          /* PL */
	case 6: setcc(b, 5, 0); break;          /* VS */
	case 7: setcc(b, 4, 0); break;          /* VC */
	case 8:                                 /* HI: C && !Z */
		test_edi_imm32(b, 2u);
		setcc(b, 5, 0);
		test_edi_imm32(b, 4u);
		setcc(b, 4, 1);
		u8(b, 0x20u); u8(b, 0xC8u);     /* and al,cl */
		break;
	case 9:                                 /* LS */
		test_edi_imm32(b, 2u);
		setcc(b, 5, 0);
		test_edi_imm32(b, 4u);
		setcc(b, 4, 1);
		u8(b, 0x20u); u8(b, 0xC8u);
		u8(b, 0x80u); u8(b, 0xF0u); u8(b, 0x01u);  /* xor al,1 */
		break;
	case 10: case 11:                       /* GE: N==V / LT */
		u8(b, 0x89u); u8(b, 0xFEu);     /* mov esi,edi */
		shr_r32_imm(b, 6, 3);
		u8(b, 0x31u); u8(b, 0xFEu);     /* xor esi,edi */
		u8(b, 0xF7u); u8(b, 0xC6u); u32(b, 1u);  /* test esi,1 */
		setcc(b, (cond == 10) ? 4 : 5, 0);
		break;
	case 12: case 13:                       /* GT / LE */
		test_edi_imm32(b, 4u);
		setcc(b, 4, 1);                 /* cl = !Z */
		u8(b, 0x89u); u8(b, 0xFEu);
		shr_r32_imm(b, 6, 3);
		u8(b, 0x31u); u8(b, 0xFEu);
		u8(b, 0xF7u); u8(b, 0xC6u); u32(b, 1u);
		setcc(b, 4, 0);                 /* al = N==V */
		u8(b, 0x20u); u8(b, 0xC8u);
		if (cond == 13)
			u8(b, 0x80u), u8(b, 0xF0u), u8(b, 0x01u);
		break;
	default:                                /* AL */
		mov_r32_imm32(b, 0, 1u);
		break;
	}
	(void)rev;
}

/* jne -> tpc-exit, else fpc-exit.  al holds the condition. */
static void emit_exit_pair(t32_jb *b, uint32_t tpc, uint32_t fpc)
{
	size_t at;
	int32_t rel;

	u8(b, 0x84u); u8(b, 0xC0u);           /* test al,al */
	u8(b, 0x0Fu); u8(b, 0x85u);           /* jne rel32 */
	at = (size_t)(b->p - b->start);
	u32(b, 0);
	t32_jb_exit(b, T32_JIT_NEXT, fpc);
	rel = (int32_t)((b->p - b->start) - (at + 4u));
	memcpy(b->start + at, &rel, 4);
	t32_jb_exit(b, T32_JIT_NEXT, tpc);
}

void t32_jb_exit_cc(t32_jb *b, int cond, uint32_t tpc, uint32_t fpc)
{
	ldr_ctx(b, 7, C_OFF);
	shr_r32_imm(b, 7, 28);
	cond_al(b, cond);
	emit_exit_pair(b, tpc, fpc);
}

void t32_jb_exit_cbr(t32_jb *b, int greg, int cond, uint32_t tpc,
		     uint32_t fpc)
{
	ldr_ctx(b, 0, goff(greg));
	test_eax_eax(b);
	/* cbz (cond EQ): taken when reg == 0 -> sete al */
	setcc(b, (cond == 0) ? 4 : 5, 0);
	emit_exit_pair(b, tpc, fpc);
}

void t32_jb_alu_rr(t32_jb *b, int op, int rd, int rn, int rm, int flags)
{
	int is_mov = (op == JB_MOV), is_mvn = (op == JB_MVN);

	if (is_mov || is_mvn)
		ldr_ctx(b, 0, goff(rm));
	else
		ldr_ctx(b, 0, goff(rn));
	if (!is_mov && !is_mvn)
		ldr_ctx(b, 1, goff(rm));
	switch (op) {
	case JB_ADD: op_eax_r32(b, 0x01u, 1); break;
	case JB_SUB: op_eax_r32(b, 0x29u, 1); break;
	case JB_AND: op_eax_r32(b, 0x21u, 1); break;
	case JB_ORR: op_eax_r32(b, 0x09u, 1); break;
	case JB_EOR: op_eax_r32(b, 0x31u, 1); break;
	case JB_MOV: break;                            /* eax already = r[rm] */
	case JB_MVN: u8(b, 0xF7u); u8(b, 0xD0u); break; /* not eax */
	default: b->err = 1; return;
	}
	if (flags == F_SET || flags == F_ONLY) {
		if (op == JB_ADD || op == JB_SUB) {
			int is_sub = (op == JB_SUB);

			u8(b, 0x0Fu);
			u8(b, (uint8_t)(is_sub ? 0x93u : 0x92u));
			u8(b, 0xC1u);              /* setc/setnc cl */
			u8(b, 0x0Fu); u8(b, 0x90u); u8(b, 0xC2u); /* seto dl */
			push_r(b, 2);              /* push rdx (V in dl) */
			push_r(b, 1);              /* push rcx (C in cl) */
			build_nz(b);
			pop_r(b, 1);               /* rcx: C */
			movzx_r32_r8(b, 2, 1);
			shl_r32_imm(b, 2, 1);
			u8(b, 0x09u); u8(b, 0xD6u); /* or esi,edx */
			pop_r(b, 2);               /* rdx: V */
			movzx_r32_r8(b, 2, 2);
			u8(b, 0x09u); u8(b, 0xD6u);
			commit_flags(b, 0x0FFFFFFFu);
		} else {
			build_nz(b);
			commit_flags(b, 0x3FFFFFFFu); /* keep C,V + ctrl */
		}
	}
	if (flags != F_ONLY)
		str_ctx(b, 0, goff(rd));
}

void t32_jb_alu_ri(t32_jb *b, int op, int rd, int rn, uint32_t imm,
		   int flags)
{
	if (op == JB_MOV) {
		mov_r32_imm32(b, 0, imm);
	} else if (op == JB_MVN) {
		mov_r32_imm32(b, 0, ~imm);
	} else {
		ldr_ctx(b, 0, goff(rn));
		switch (op) {
		case JB_ADD: op_eax_imm32(b, 0, imm); break;
		case JB_SUB: op_eax_imm32(b, 5, imm); break;
		case JB_AND: op_eax_imm32(b, 4, imm); break;
		case JB_ORR: op_eax_imm32(b, 1, imm); break;
		case JB_EOR: op_eax_imm32(b, 6, imm); break;
		default: b->err = 1; return;
		}
	}
	if (flags == F_SET || flags == F_ONLY) {
		if (op == JB_ADD || op == JB_SUB) {
			int is_sub = (op == JB_SUB);

			u8(b, 0x0Fu);
			u8(b, (uint8_t)(is_sub ? 0x93u : 0x92u));
			u8(b, 0xC1u);
			u8(b, 0x0Fu); u8(b, 0x90u); u8(b, 0xC2u);
			push_r(b, 2);
			push_r(b, 1);
			build_nz(b);
			pop_r(b, 1);
			movzx_r32_r8(b, 2, 1);
			shl_r32_imm(b, 2, 1);
			u8(b, 0x09u); u8(b, 0xD6u);
			pop_r(b, 2);
			movzx_r32_r8(b, 2, 2);
			u8(b, 0x09u); u8(b, 0xD6u);
			commit_flags(b, 0x0FFFFFFFu);
		} else {
			build_nz(b);
			commit_flags(b, 0x3FFFFFFFu);
		}
	}
	if (flags != F_ONLY)
		str_ctx(b, 0, goff(rd));
}

void t32_jb_movw(t32_jb *b, int rd, uint32_t imm16, int is_movt)
{
	if (is_movt) {
		ldr_ctx(b, 0, goff(rd));
		op_eax_imm32(b, 4, 0xFFFFu);                  /* and eax */
		op_eax_imm32(b, 1, (imm16 & 0xFFFFu) << 16);  /* or eax */
	} else {
		mov_r32_imm32(b, 0, imm16 & 0xFFFFu);
	}
	str_ctx(b, 0, goff(rd));
}

void t32_jb_shift_imm(t32_jb *b, int op, int rd, int rm, int sh, int flags)
{
	ldr_ctx(b, 0, goff(rm));
	if (flags == F_SET || flags == F_ONLY) {
		u8(b, 0x89u); u8(b, 0xC2u);       /* mov edx,eax */
		if (op == 0)
			shr_r32_imm(b, 2, (uint8_t)(32 - sh));
		else
			shr_r32_imm(b, 2, (uint8_t)(sh - 1));
		u8(b, 0x83u); u8(b, 0xE2u); u8(b, 0x01u); /* and edx,1 (C) */
	}
	switch (op) {
	case 0: shl_r32_imm(b, 0, (uint8_t)sh); break;
	case 1: shr_r32_imm(b, 0, (uint8_t)sh); break;
	default: sar_r32_imm(b, 0, (uint8_t)sh); break;
	}
	if (flags == F_SET || flags == F_ONLY) {
		push_r(b, 2);                     /* save C */
		build_nz(b);
		pop_r(b, 2);
		shl_r32_imm(b, 2, 29);            /* C into bit 29 */
		u8(b, 0x09u); u8(b, 0xD6u);
		commit_flags(b, 0x1FFFFFFFu);     /* keep V + ctrl */
	}
	if (flags != F_ONLY)
		str_ctx(b, 0, goff(rd));
}

void t32_jb_mul(t32_jb *b, int rd, int rn, int rm)
{
	ldr_ctx(b, 0, goff(rn));
	ldr_ctx(b, 1, goff(rm));
	u8(b, 0x0Fu); u8(b, 0xAFu); u8(b, 0xC1u);  /* imul eax,ecx */
	str_ctx(b, 0, goff(rd));
}

void t32_jb_setlr(t32_jb *b, uint32_t pc)
{
	mov_r32_imm32(b, 0, pc);
	str_ctx(b, 0, goff(14));
}

void t32_jb_step(t32_jb *b, uint32_t pc)
{
	u8(b, 0x48u); u8(b, 0x83u); u8(b, 0xECu); u8(b, 0x08u); /* sub rsp,8 */
	u8(b, 0x48u); u8(b, 0x89u); u8(b, 0xE2u);  /* mov rdx,rsp */
	u8(b, 0x48u); u8(b, 0x89u); u8(b, 0xDFu);  /* mov rdi,rbx */
	mov_r32_imm32(b, 6, pc);                   /* esi = pc */
	mov_r64_imm64(b, 0, (uint64_t)(uintptr_t)t32_fallback_step);
	u8(b, 0xFFu); u8(b, 0xD0u);                /* call rax */
	u8(b, 0x48u); u8(b, 0x83u); u8(b, 0xC4u); u8(b, 0x08u);
	u8(b, 0x85u); u8(b, 0xC0u);                /* test eax,eax */
	u8(b, 0x0Fu); u8(b, 0x84u);                /* je over */
	{
		size_t at = (size_t)(b->p - b->start);
		int32_t rel;

		u32(b, 0);
		t32_jb_exit(b, T32_JIT_TRAP, pc);   /* terminal */
		rel = (int32_t)((b->p - b->start) - (at + 4u));
		memcpy(b->start + at, &rel, 4);
	}
}

int t32_jb_run(uint8_t *code, void *cpu, uint32_t *next)
{
	int (*fn)(t32_cpu *, uint32_t *) =
		(int (*)(t32_cpu *, uint32_t *))(void *)code;

	return fn((t32_cpu *)cpu, next);
}

#else
typedef int jit_x64_unused;
#endif
