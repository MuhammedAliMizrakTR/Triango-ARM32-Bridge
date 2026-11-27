/* SPDX-License-Identifier: MIT */
/*
 * Triango NB engine — VFPv3 / NEON (Advanced SIMD) execution.
 *
 * Decoder: generated table (tests/gen_vfp.py -> tests/vfp_table.h); the
 * (mask, value) rows are derived from keystone assembles, not hand-decoded.
 * Register model: c->s[32] = s0..s31; d[i] overlays s[2i],s[2i+1]; q[i]
 * overlays d[2i],d[2i+1].  FPSCR in c->fpscr (NZCV at 31:28).
 *
 * Test expectations live in tests/vfp_cases.h (generated).  Unhandled
 * encodings return 0 -> the interpreter raises the UNDEF trap.
 */
#include "engine_internal.h"
#include "../engine/tests/vfp_table.h"

#include <math.h>
#include <string.h>

/* ---------------- register helpers ---------------- */

static inline float s_f32(t32_cpu *c, int n)
{
	float f;

	memcpy(&f, &c->s[n & 31], 4);
	return f;
}

static inline void s_set_f32(t32_cpu *c, int n, float f)
{
	memcpy(&c->s[n & 31], &f, 4);
}

static inline double d_f64(t32_cpu *c, int n)
{
	uint64_t v = ((uint64_t)c->s[(2 * n + 1) & 31] << 32) |
		     c->s[(2 * n) & 31];
	double d;

	memcpy(&d, &v, 8);
	return d;
}

static inline void d_set_f64(t32_cpu *c, int n, double d)
{
	uint64_t v;

	memcpy(&v, &d, 8);
	c->s[(2 * n) & 31] = (uint32_t)v;
	c->s[(2 * n + 1) & 31] = (uint32_t)(v >> 32);
}

static inline uint64_t d_u64(t32_cpu *c, int n)
{
	return ((uint64_t)c->s[(2 * n + 1) & 31] << 32) | c->s[(2 * n) & 31];
}

static inline void d_set_u64(t32_cpu *c, int n, uint64_t v)
{
	c->s[(2 * n) & 31] = (uint32_t)v;
	c->s[(2 * n + 1) & 31] = (uint32_t)(v >> 32);
}

static void fcmp(t32_cpu *c, double a, double b)
{
	int n, z, cy, v;

	if (isnan(a) || isnan(b)) {
		n = 0;
		z = 0;
		cy = 1;
		v = 1;
	} else {
		n = a < b;
		z = a == b;
		cy = a >= b;
		v = 0;
	}
	c->fpscr &= 0x0FFFFFFFu;
	if (n)
		c->fpscr |= 0x80000000u;
	if (z)
		c->fpscr |= 0x40000000u;
	if (cy)
		c->fpscr |= 0x20000000u;
	if (v)
		c->fpscr |= 0x10000000u;
}

static double vfp_imm(uint32_t imm8)
{
	uint32_t b = (imm8 & 0x80u) ? 0x80000000u : 0u;
	uint32_t e = (imm8 >> 4) & 7u;
	uint32_t f = imm8 & 0xFu;
	uint32_t r = b | ((e ^ 4u) << 29) | (f << 19);
	float fv;

	memcpy(&fv, &r, 4);
	return (double)fv;
}

static int vld32(t32_cpu *c, uint32_t addr, uint32_t *out)
{
	uint8_t *p = t32_ptr(c->mem, addr, 4);

	if (!p) {
		c->trap = T32_TRAP_MEM;
		c->trap_pc = addr;
		return -1;
	}
	memcpy(out, p, 4);
	return 0;
}

static int vst32(t32_cpu *c, uint32_t addr, uint32_t v)
{
	uint8_t *p = t32_ptr(c->mem, addr, 4);

	if (!p) {
		c->trap = T32_TRAP_MEM;
		c->trap_pc = addr;
		return -1;
	}
	memcpy(p, &v, 4);
	return 0;
}

/* ---------------- field extractors ---------------- */

static inline int f_D(uint32_t i) { return (int)((i >> 22) & 1u); }
static inline int f_Vd(uint32_t i) { return (int)((i >> 12) & 0xFu); }
static inline int f_N(uint32_t i) { return (int)((i >> 7) & 1u); }
static inline int f_Vn(uint32_t i) { return (int)((i >> 16) & 0xFu); }
static inline int f_M(uint32_t i) { return (int)((i >> 5) & 1u); }
static inline int f_Vm(uint32_t i) { return (int)(i & 0xFu); }
static inline int f_sD(uint32_t i) { return (f_Vd(i) << 1) | f_D(i); }
static inline int f_sN(uint32_t i) { return (f_Vn(i) << 1) | f_N(i); }
static inline int f_sM(uint32_t i) { return (f_Vm(i) << 1) | f_M(i); }
static inline int f_dD(uint32_t i) { return (f_D(i) << 4) | f_Vd(i); }
static inline int f_dN(uint32_t i) { return (f_N(i) << 4) | f_Vn(i); }
static inline int f_dM(uint32_t i) { return (f_M(i) << 4) | f_Vm(i); }
static inline int f_Q(uint32_t i) { return (int)((i >> 6) & 1u); }

/* ---------------- NEON three-same integer lane ops ---------------- */

static int neon_ts_family(int op)
{
	switch (op) {
	case T32V_NEON_VADD_I8:
	case T32V_NEON_VADD_I16:
	case T32V_NEON_VADD_I32:
		return 0;
	case T32V_NEON_VSUB_I8:
	case T32V_NEON_VSUB_I16:
	case T32V_NEON_VSUB_I32:
		return 1;
	case T32V_NEON_VMUL_I8:
	case T32V_NEON_VMUL_I16:
	case T32V_NEON_VMUL_I32:
		return 2;
	case T32V_NEON_VMLA_I8:
	case T32V_NEON_VMLA_I16:
	case T32V_NEON_VMLA_I32:
		return 3;
	case T32V_NEON_VQADD_S8:
	case T32V_NEON_VQADD_S16:
	case T32V_NEON_VQADD_S32:
		return 4;
	case T32V_NEON_VQSUB_S8:
	case T32V_NEON_VQSUB_S16:
	case T32V_NEON_VQSUB_S32:
		return 5;
	case T32V_NEON_VMAX_S8:
	case T32V_NEON_VMAX_S16:
	case T32V_NEON_VMAX_S32:
		return 6;
	case T32V_NEON_VMIN_U8:
	case T32V_NEON_VMIN_U16:
	case T32V_NEON_VMIN_U32:
		return 7;
	case T32V_NEON_VCGT_S8:
	case T32V_NEON_VCGT_S16:
	case T32V_NEON_VCGT_S32:
		return 8;
	case T32V_NEON_VCEQ_I8:
	case T32V_NEON_VCEQ_I16:
	case T32V_NEON_VCEQ_I32:
		return 9;
	default:
		return -1;
	}
}

static uint64_t lane_op(int fam, int esize, uint64_t a, uint64_t b, uint64_t e)
{
	int nlanes = 64 / esize;
	uint64_t r = 0;
	int lane;

	for (lane = 0; lane < nlanes; lane++) {
		int sh = lane * esize;
		uint64_t mask = (esize == 64) ? ~0ull : ((1ull << esize) - 1);
		uint64_t x = (a >> sh) & mask;
		uint64_t y = (b >> sh) & mask;
		uint64_t z = 0;
		int64_t xs = (int64_t)x, ys = (int64_t)y;

		if (esize < 64) {
			if (x >> (esize - 1))
				xs -= 1ll << esize;
			if (y >> (esize - 1))
				ys -= 1ll << esize;
		}
		switch (fam) {
		case 0:
			z = x + y;
			break;
		case 1:
			z = x - y;
			break;
		case 2:
			z = x * y;
			break;
		case 3:
			z = ((e >> sh) & mask) + x * y;
			break;
		case 4: {
			int64_t lo = -(1ll << (esize - 1));
			int64_t hi = (1ll << (esize - 1)) - 1;
			int64_t s = xs + ys;

			if (s > hi)
				s = hi;
			if (s < lo)
				s = lo;
			z = (uint64_t)s;
			break;
		}
		case 5: {
			int64_t lo = -(1ll << (esize - 1));
			int64_t hi = (1ll << (esize - 1)) - 1;
			int64_t s = xs - ys;

			if (s > hi)
				s = hi;
			if (s < lo)
				s = lo;
			z = (uint64_t)s;
			break;
		}
		case 6:
			z = (uint64_t)(xs > ys ? xs : ys);
			break;
		case 7:
			z = x < y ? x : y;
			break;
		case 8:
			z = (xs > ys) ? mask : 0;
			break;
		case 9:
			z = (x == y) ? mask : 0;
			break;
		}
		r |= (z & mask) << sh;
	}
	return r;
}

/* ---------------- exec by op ---------------- */

static int exec_op(t32_cpu *c, uint32_t pc, uint32_t insn, int op)
{
	int fam = neon_ts_family(op);
	int regs, i, lane;

	if (fam >= 0) {
		int vd = f_dD(insn), vn = f_dN(insn), vm = f_dM(insn);
		int esize = 8 << (((insn >> 20) & 3u) & 3u);

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t r = lane_op(fam, esize,
					     d_u64(c, (vn + i) & 31),
					     d_u64(c, (vm + i) & 31),
					     d_u64(c, (vd + i) & 31));

			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}

	switch (op) {
	/* ---- VFP three-operand ---- */
	case T32V_VFP_ADD:
	case T32V_VFP_SUB:
	case T32V_VFP_MUL:
	case T32V_VFP_NMUL:
	case T32V_VFP_DIV:
	case T32V_VFP_MLA:
	case T32V_VFP_MLS:
	case T32V_VFP_NMLA:
	case T32V_VFP_NMLS: {
		int dbl = (int)((insn >> 8) & 1u);
		double a = dbl ? d_f64(c, f_dN(insn)) :
				     (double)s_f32(c, f_sN(insn));
		double b = dbl ? d_f64(c, f_dM(insn)) :
				     (double)s_f32(c, f_sM(insn));
		double acc = dbl ? d_f64(c, f_dD(insn)) :
				      (double)s_f32(c, f_sD(insn));
		double r = 0;

		switch (op) {
		case T32V_VFP_ADD:
			r = a + b;
			break;
		case T32V_VFP_SUB:
			r = a - b;
			break;
		case T32V_VFP_MUL:
			r = a * b;
			break;
		case T32V_VFP_NMUL:
			r = -(a * b);
			break;
		case T32V_VFP_DIV:
			r = b != 0.0 ? a / b : (a == 0.0 ? NAN : INFINITY);
			break;
		case T32V_VFP_MLA:
			r = acc + a * b;
			break;
		case T32V_VFP_MLS:
			r = acc - a * b;
			break;
		case T32V_VFP_NMLA:
			r = -acc + a * b;
			break;
		default:
			r = -acc - a * b;
			break;
		}
		if (dbl)
			d_set_f64(c, f_dD(insn), r);
		else
			s_set_f32(c, f_sD(insn), (float)r);
		return 1;
	}

	/* ---- VFP unary / compare / convert ---- */
	case T32V_VFP_MOV:
	case T32V_VFP_ABS:
	case T32V_VFP_NEG:
	case T32V_VFP_SQRT:
	case T32V_VFP_CMP:
	case T32V_VFP_CMPE:
	case T32V_VFP_CMP0:
	case T32V_VFP_CVT_F64_F32:
	case T32V_VFP_CVT_F32_F64:
	case T32V_VFP_CVT_S32:
	case T32V_VFP_CVT_U32:
	case T32V_VFP_CVT_FS32:
	case T32V_VFP_CVT_FU32: {
		int dbl = (int)((insn >> 8) & 1u);
		double a = dbl ? d_f64(c, f_dM(insn)) :
				     (double)s_f32(c, f_sM(insn));
		double r;

		switch (op) {
		case T32V_VFP_MOV:
			r = a;
			break;
		case T32V_VFP_ABS:
			r = fabs(a);
			break;
		case T32V_VFP_NEG:
			r = -a;
			break;
		case T32V_VFP_SQRT:
			r = a < 0 ? NAN : sqrt(a);
			break;
		case T32V_VFP_CMP:
		case T32V_VFP_CMPE: {
			double x = dbl ? d_f64(c, f_dD(insn)) :
					(double)s_f32(c, f_sD(insn));

			fcmp(c, x, a);
			return 1;
		}
		case T32V_VFP_CMP0:
			fcmp(c, a, 0.0);
			return 1;
		case T32V_VFP_CVT_F64_F32:
			d_set_f64(c, f_dD(insn),
				  (double)s_f32(c, f_sM(insn)));
			return 1;
		case T32V_VFP_CVT_F32_F64:
			s_set_f32(c, f_sD(insn),
				  (float)d_f64(c, f_dM(insn)));
			return 1;
		case T32V_VFP_CVT_S32: {
			int32_t q = (int32_t)floor(a + 0.5);

			c->s[f_sD(insn) & 31] = (uint32_t)q;
			return 1;
		}
		case T32V_VFP_CVT_U32: {
			uint32_t q = (uint32_t)floor(a + 0.5);

			c->s[f_sD(insn) & 31] = q;
			return 1;
		}
		case T32V_VFP_CVT_FS32: {
			int32_t x = (int32_t)c->s[f_sM(insn) & 31];

			s_set_f32(c, f_sD(insn), (float)x);
			return 1;
		}
		default: {
			uint32_t x = c->s[f_sM(insn) & 31];

			s_set_f32(c, f_sD(insn), (float)x);
			return 1;
		}
		}
		if (dbl)
			d_set_f64(c, f_dD(insn), r);
		else
			s_set_f32(c, f_sD(insn), (float)r);
		return 1;
	}

	/* ---- VFP load/store ---- */
	case T32V_VFP_LDR_S:
	case T32V_VFP_LDR_D:
	case T32V_VFP_STR_S:
	case T32V_VFP_STR_D: {
		uint32_t U = (insn >> 23) & 1u;
		uint32_t rn = (insn >> 16) & 0xFu;
		uint32_t imm8 = insn & 0xFFu;
		uint32_t base = (rn == 15) ? ((pc + 8) & ~3u) : c->r[rn];
		uint32_t addr = U ? base + imm8 * 4u : base - imm8 * 4u;
		int dbl = (op == T32V_VFP_LDR_D || op == T32V_VFP_STR_D);
		int load = (op == T32V_VFP_LDR_S || op == T32V_VFP_LDR_D);
		int reg = dbl ? f_dD(insn) : f_sD(insn);

		if (load) {
			uint32_t lo, hi;

			if (vld32(c, addr, &lo) != 0)
				return 1;
			if (dbl) {
				if (vld32(c, addr + 4, &hi) != 0)
					return 1;
				d_set_u64(c, reg & 15,
					  ((uint64_t)hi << 32) | lo);
			} else {
				c->s[reg & 31] = lo;
			}
		} else {
			if (dbl) {
				uint64_t v = d_u64(c, reg & 15);

				if (vst32(c, addr, (uint32_t)v) != 0)
					return 1;
				if (vst32(c, addr + 4,
					  (uint32_t)(v >> 32)) != 0)
					return 1;
			} else if (vst32(c, addr, c->s[reg & 31]) != 0) {
				return 1;
			}
		}
		return 1;
	}

	/* ---- VLDM / VSTM ---- */
	case T32V_VFP_LDM_S:
	case T32V_VFP_LDM_D:
	case T32V_VFP_STM_S:
	case T32V_VFP_STM_D: {
		uint32_t U = (insn >> 23) & 1u;
		uint32_t P = (insn >> 24) & 1u;
		uint32_t W = (insn >> 21) & 1u;
		uint32_t rn = (insn >> 16) & 0xFu;
		uint32_t imm8 = insn & 0xFFu;
		int dbl = (op == T32V_VFP_LDM_D || op == T32V_VFP_STM_D);
		int load = (op == T32V_VFP_LDM_S || op == T32V_VFP_LDM_D);
		uint32_t base = c->r[rn];
		uint32_t n = dbl ? (imm8 + 1u) / 2u : imm8;
		uint32_t total = n * (dbl ? 8u : 4u);
		uint32_t addr;
		int reg0 = dbl ? f_dD(insn) : f_sD(insn);
		uint32_t i2;

		if (U)
			addr = P ? base + 4u : base;
		else
			addr = P ? base - total : base - total + 4u;
		for (i2 = 0; i2 < n; i2++) {
			if (load) {
				uint32_t lo, hi;

				if (vld32(c, addr, &lo) != 0)
					return 1;
				if (dbl) {
					if (vld32(c, addr + 4, &hi) != 0)
						return 1;
					d_set_u64(c, (reg0 + (int)i2) & 31,
						  ((uint64_t)hi << 32) | lo);
				} else {
					c->s[(reg0 + (int)i2) & 31] = lo;
				}
			} else {
				if (dbl) {
					uint64_t v = d_u64(c,
						(reg0 + (int)i2) & 31);

					if (vst32(c, addr, (uint32_t)v) != 0)
						return 1;
					if (vst32(c, addr + 4,
						  (uint32_t)(v >> 32)) != 0)
						return 1;
				} else if (vst32(c, addr,
						 c->s[(reg0 + (int)i2) & 31]) != 0) {
					return 1;
				}
			}
			addr += dbl ? 8u : 4u;
		}
		if (W)
			c->r[rn] = U ? base + total : base - total;
		return 1;
	}

	/* ---- core <-> VFP moves (MCR/MCRR layouts) ---- */
	case T32V_VFP_MOV_SR:
		c->s[f_sN(insn) & 31] = c->r[f_Vd(insn)];
		return 1;
	case T32V_VFP_MOV_RS: {
		int rt = f_Vd(insn);

		if (rt != 15)
			c->r[rt] = c->s[f_sN(insn) & 31];
		return 1;
	}
	case T32V_VFP_MOV_DRR: {
		int rt2 = f_Vn(insn), rt = f_Vd(insn);

		d_set_u64(c, f_dM(insn) & 31,
			  ((uint64_t)c->r[rt2] << 32) | c->r[rt]);
		return 1;
	}
	case T32V_VFP_MOV_RRD: {
		int rt2 = f_Vn(insn), rt = f_Vd(insn);
		uint64_t v = d_u64(c, f_dM(insn) & 31);

		if (rt != 15)
			c->r[rt] = (uint32_t)v;
		if (rt2 != 15)
			c->r[rt2] = (uint32_t)(v >> 32);
		return 1;
	}
	case T32V_VFP_VMRS: {
		int rt = f_Vd(insn);
		uint32_t v = c->fpscr;

		if (rt == 15) {
			c->cpsr &= 0x0FFFFFFFu;
			c->cpsr |= v & 0xF0000000u;
		} else {
			c->r[rt] = v;
		}
		return 1;
	}
	case T32V_VFP_VMSR:
		c->fpscr = c->r[f_Vd(insn)];
		return 1;

	/* ---- NEON bitwise three-same ---- */
	case T32V_NEON_VAND:
	case T32V_NEON_VBIC:
	case T32V_NEON_VORR:
	case T32V_NEON_VORN:
	case T32V_NEON_VEOR:
	case T32V_NEON_VBSL:
	case T32V_NEON_VBIT:
	case T32V_NEON_VBIF: {
		int vd = f_dD(insn), vn = f_dN(insn), vm = f_dM(insn);

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t a = d_u64(c, (vn + i) & 31);
			uint64_t b = d_u64(c, (vm + i) & 31);
			uint64_t dcur = d_u64(c, (vd + i) & 31);
			uint64_t r;

			switch (op) {
			case T32V_NEON_VAND:
				r = a & b;
				break;
			case T32V_NEON_VBIC:
				r = a & ~b;
				break;
			case T32V_NEON_VORR:
				r = a | b;
				break;
			case T32V_NEON_VORN:
				r = a | ~b;
				break;
			case T32V_NEON_VEOR:
				r = a ^ b;
				break;
			case T32V_NEON_VBSL:
				r = (dcur & b) | (a & ~b);
				break;
			case T32V_NEON_VBIT:
				r = (dcur & ~b) | (a & b);
				break;
			default:
				r = (dcur & ~b) | (a & ~b);
				break;
			}
			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}

	/* ---- NEON shift by immediate ---- */
	case T32V_NEON_VSHL_I8:
	case T32V_NEON_VSHL_I16:
	case T32V_NEON_VSHL_I32:
	case T32V_NEON_VSHR_S8:
	case T32V_NEON_VSHR_U8:
	case T32V_NEON_VSHR_S32:
	case T32V_NEON_VSHR_U32:
	case T32V_NEON_VSRA_S8:
	case T32V_NEON_VRSHR_U8: {
		int vd = f_dD(insn), vn = f_dM(insn);
		int esize = (op == T32V_NEON_VSHL_I16) ? 16 :
			    ((op == T32V_NEON_VSHL_I32 ||
			      op == T32V_NEON_VSHR_S32 ||
			      op == T32V_NEON_VSHR_U32) ? 32 : 8);
		int is_left = (op == T32V_NEON_VSHL_I8 ||
			       op == T32V_NEON_VSHL_I16 ||
			       op == T32V_NEON_VSHL_I32);
		int is_signed = (op == T32V_NEON_VSHR_S8 ||
				 op == T32V_NEON_VSHR_S32 ||
				 op == T32V_NEON_VSRA_S8);
		int is_acc = (op == T32V_NEON_VSRA_S8);
		int is_round = (op == T32V_NEON_VRSHR_U8);
		uint32_t imm6 = (insn >> 16) & 0x3Fu;
		int shift;

		if (is_left) {
			shift = (int)imm6 - esize;
			if (shift < 0 || shift >= esize)
				return 0;
		} else {
			shift = 2 * esize - (int)imm6;
			if (shift <= 0 || shift > esize)
				return 0;
		}
		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t a = d_u64(c, (vn + i) & 31);
			uint64_t dcur = d_u64(c, (vd + i) & 31);
			uint64_t r = is_acc ? dcur : 0;

			for (lane = 0; lane < 64 / esize; lane++) {
				int sh = lane * esize;
				uint64_t mask = (esize == 64) ?
					~0ull : ((1ull << esize) - 1);
				uint64_t x = (a >> sh) & mask;
				uint64_t z;

				if (is_left) {
					z = x << shift;
				} else if (is_signed) {
					int64_t sx = (int64_t)x;

					if (esize < 64 &&
					    (x >> (esize - 1)))
						sx -= 1ll << esize;
					z = (uint64_t)(sx >> shift);
				} else if (is_round) {
					uint64_t rnd = (shift > 1) ?
						(1ull << (shift - 1)) : 0;

					z = (x + rnd) >> shift;
				} else {
					z = x >> shift;
				}
				if (is_acc) {
					uint64_t old = (r >> sh) & mask;

					z = (old + z) & mask;
				}
				r &= ~(mask << sh);
				r |= (z & mask) << sh;
			}
			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}

	/* ---- NEON permute / two-reg ---- */
	case T32V_NEON_VSWP: {
		int vd = f_dD(insn), vm = f_dM(insn);

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t a = d_u64(c, (vd + i) & 31);
			uint64_t b = d_u64(c, (vm + i) & 31);

			d_set_u64(c, (vd + i) & 31, b);
			d_set_u64(c, (vm + i) & 31, a);
		}
		return 1;
	}
	case T32V_NEON_VREV64_8:
	case T32V_NEON_VREV32_16: {
		int vd = f_dD(insn), vm = f_dM(insn);
		int pair = (op == T32V_NEON_VREV64_8) ? 8 : 4;
		int esize = (op == T32V_NEON_VREV64_8) ? 8 : 16;

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t a = d_u64(c, (vm + i) & 31);
			uint64_t r = 0;

			for (lane = 0; lane < 64 / esize; lane++) {
				uint64_t mask = (esize == 64) ?
					~0ull : ((1ull << esize) - 1);
				uint64_t ea = (a >> (lane * esize)) & mask;
				int idx = (lane / pair) * pair +
					  (pair - 1 - (lane % pair));

				r |= ea << (idx * esize);
			}
			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}
	case T32V_NEON_VMVN: {
		int vd = f_dD(insn), vm = f_dM(insn);

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++)
			d_set_u64(c, (vd + i) & 31,
				  ~d_u64(c, (vm + i) & 31));
		return 1;
	}
	case T32V_NEON_VCNT: {
		int vd = f_dD(insn), vm = f_dM(insn);

		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			uint64_t a = d_u64(c, (vm + i) & 31);
			uint64_t r = 0;

			for (lane = 0; lane < 8; lane++) {
				uint64_t ea = (a >> (lane * 8)) & 0xFF;
				int cnt = 0, b2;

				for (b2 = 0; b2 < 8; b2++)
					cnt += (int)((ea >> b2) & 1);
				r |= (uint64_t)cnt << (lane * 8);
			}
			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}
	case T32V_NEON_VEXT: {
		int vd = f_dD(insn), vn = f_dN(insn), vm = f_dM(insn);
		uint32_t imm8 = (insn >> 8) & 0xFu;

		if (imm8 >= 8)
			return 0;
		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++) {
			/* ARM: Vd = (Vn:Vm) >> imm8*8 — Vn high, Vm low */
			uint64_t a = d_u64(c, (vm + i) & 31);
			uint64_t b = d_u64(c, (vn + i) & 31);
			uint64_t r = (imm8 == 0) ? a :
				((a >> (imm8 * 8)) | (b << ((8 - imm8) * 8)));

			d_set_u64(c, (vd + i) & 31, r);
		}
		return 1;
	}

	/* ---- NEON immediates ---- */
	case T32V_NEON_VMOV_I32:
	case T32V_NEON_VMOV_I16:
	case T32V_NEON_VMVN_I32: {
		int vd = f_dD(insn);
		uint32_t cmode = (insn >> 8) & 0xFu;
		/* T32 modified-immediate: imm8 = insn<28>:insn<18:16>:insn<3:0>
		 * (verified against LLVM-as thumb output) */
		uint32_t imm8 = (((insn >> 28) & 1u) << 7) |
				(((insn >> 16) & 7u) << 4) | (insn & 0xFu);
		uint32_t pattern;
		int do_vmvn = (op == T32V_NEON_VMVN_I32);

		switch (cmode) {
		case 0x0:
		case 0x2:
		case 0x4:
		case 0x6:
			pattern = imm8 << ((cmode >> 1) * 8);
			break;
		case 0x1:
			pattern = (imm8 << 24) | (imm8 << 8);
			break;
		case 0x3:
			pattern = (imm8 << 16) | imm8;
			break;
		case 0x5:
		case 0x7: {
			/* f32 modified immediate */
			uint32_t s = (imm8 & 0x80u) ? 0x80000000u : 0u;
			uint32_t e = 0x3F800000u ^
				     (((imm8 >> 4) & 7u) << 25);
			uint32_t f = (imm8 & 0xFu) << 19;

			pattern = s | e | f;
			break;
		}
		case 0x8:
			pattern = (imm8 << 16) | imm8;
			break;
		case 0x9:
			pattern = (imm8 << 24) | (imm8 << 8);
			break;
		case 0xA:
			pattern = (imm8 << 24) | (imm8 << 16);
			break;
		case 0xB:
			pattern = (imm8 << 24) | (imm8 << 16) |
				  (imm8 << 8) | imm8;
			break;
		case 0xC:
		case 0xD:
			pattern = imm8 * 0x01010101u;
			break;
		default:
			pattern = imm8 << 24;
			break;
		}
		if (op == T32V_NEON_VMOV_I16 || cmode == 0x8u || cmode == 0x9u)
			pattern = (op == T32V_NEON_VMOV_I16 && cmode < 8u) ?
				  ((imm8 << 16) | imm8) : pattern;
		if (do_vmvn)
			pattern = ~pattern;
		regs = f_Q(insn) ? 2 : 1;
		for (i = 0; i < regs; i++)
			d_set_u64(c, (vd + i) & 31,
				  ((uint64_t)pattern << 32) | pattern);
		return 1;
	}

	/* ---- NEON VLD1/VST1 multiple ---- */
	case T32V_NEON_VLD1_1:
	case T32V_NEON_VLD1_2:
	case T32V_NEON_VLD1_3:
	case T32V_NEON_VLD1_4:
	case T32V_NEON_VST1_1:
	case T32V_NEON_VST1_2: {
		int l = (op == T32V_NEON_VLD1_1 || op == T32V_NEON_VLD1_2 ||
			 op == T32V_NEON_VLD1_3 || op == T32V_NEON_VLD1_4);
		int rn = (insn >> 16) & 0xFu;
		int vd = f_dD(insn);
		int wb = (insn >> 21) & 1u;
		uint32_t addr = c->r[rn];

		switch (op) {
		case T32V_NEON_VLD1_1:
		case T32V_NEON_VST1_1:
			regs = 1;
			break;
		case T32V_NEON_VLD1_2:
		case T32V_NEON_VST1_2:
			regs = 2;
			break;
		case T32V_NEON_VLD1_3:
			regs = 3;
			break;
		default:
			regs = 4;
			break;
		}
		for (i = 0; i < regs; i++) {
			if (l) {
				uint32_t lo, hi;

				if (vld32(c, addr, &lo) != 0)
					return 1;
				if (vld32(c, addr + 4, &hi) != 0)
					return 1;
				d_set_u64(c, (vd + i) & 31,
					  ((uint64_t)hi << 32) | lo);
			} else {
				uint64_t v = d_u64(c, (vd + i) & 31);

				if (vst32(c, addr, (uint32_t)v) != 0)
					return 1;
				if (vst32(c, addr + 4,
					  (uint32_t)(v >> 32)) != 0)
					return 1;
			}
			addr += 8;
		}
		if (wb)
			c->r[rn] = addr;
		return 1;
	}
	default:
		break;
	}
	(void)pc;
	return 0;
}

/* ---------------- entry ---------------- */

int t32_vfp_exec(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc)
{
	size_t i;

	(void)npc;
	for (i = 0; i < T32_VFP_ROWS; i++) {
		const t32_vfp_row *r = &t32_vfp_table[i];

		if ((insn & r->mask) == r->val)
			return exec_op(c, pc, insn, r->op);
	}
	return 0;
}
