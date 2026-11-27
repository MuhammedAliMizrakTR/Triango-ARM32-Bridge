/* SPDX-License-Identifier: MIT */
/*
 * ARM/Thumb interpreter core (V1.1(a) baseline executor).
 *
 * Scope: the integer instruction set that real compiled C emits —
 * data processing (all shifts + flags), MOVW/MOVT, multiply/divide, CLZ,
 * LDR/STR family, LDM/STM (PUSH/POP), branches (B/BL/BX/BLX, cond),
 * ARM + Thumb-16 + the Thumb-2 wide forms, CBZ/CBNZ.  VFP/NEON, SVC and
 * exotic media instructions trap with a precise diagnostic (roadmap:
 * VFP in V1.2, JIT speed path in v0.4).
 *
 * Execution model: pure C, one t32_cpu per thread (re-entrant), guest
 * pointers are 32-bit.  Branches to shim entries call host code natively.
 */
#include "engine_internal.h"

#include <stdlib.h>
#include <string.h>

/* T32_LR_SENTINEL now in engine_internal.h */

static int t32_fault(t32_cpu *c, int trap, uint32_t pc, uint32_t insn)
{
	c->trap = trap;
	c->trap_pc = pc;
	c->trap_insn = insn;
	return -1;
}

/* ---- memory ---- */

static uint32_t rd32(t32_cpu *c, uint32_t addr)
{
	uint8_t *p = t32_ptr(c->mem, addr, 4);
	uint32_t v;

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return 0;
	}
	memcpy(&v, p, 4);
	return v;
}

static uint32_t rd16(t32_cpu *c, uint32_t addr)
{
	uint8_t *p = t32_ptr(c->mem, addr, 2);
	uint16_t v;

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return 0;
	}
	memcpy(&v, p, 2);
	return v;
}

static uint32_t rd8(t32_cpu *c, uint32_t addr)
{
	uint8_t *p = t32_ptr(c->mem, addr, 1);

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return 0;
	}
	return *p;
}

static void wr32(t32_cpu *c, uint32_t addr, uint32_t v)
{
	uint8_t *p = t32_ptr(c->mem, addr, 4);

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return;
	}
	memcpy(p, &v, 4);
}

static void wr16(t32_cpu *c, uint32_t addr, uint32_t v)
{
	uint8_t *p = t32_ptr(c->mem, addr, 2);
	uint16_t x = (uint16_t)v;

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return;
	}
	memcpy(p, &x, 2);
}

static void wr8(t32_cpu *c, uint32_t addr, uint32_t v)
{
	uint8_t *p = t32_ptr(c->mem, addr, 1);

	if (!p) {
		t32_fault(c, T32_TRAP_MEM, c->r[15], 0);
		return;
	}
	*p = (uint8_t)v;
}

/* ---- flags ---- */

static uint32_t clz32(uint32_t v)
{
	uint32_t n = 0;

	if (!v)
		return 32;
	while (!(v & 0x80000000u)) {
		v <<= 1;
		n++;
	}
	return n;
}

static uint32_t rbit32(uint32_t v)
{
	uint32_t r = 0;
	int i;

	for (i = 0; i < 32; i++)
		r |= ((v >> i) & 1u) << (31 - i);
	return r;
}

static void set_nz(t32_cpu *c, uint32_t r)
{
	c->cpsr &= ~(T32_CPSR_N | T32_CPSR_Z);
	if (r & 0x80000000u)
		c->cpsr |= T32_CPSR_N;
	if (!r)
		c->cpsr |= T32_CPSR_Z;
}

static void set_cv_add(t32_cpu *c, uint32_t a, uint32_t b, uint32_t r,
		       uint64_t wide)
{
	c->cpsr &= ~(T32_CPSR_C | T32_CPSR_V);
	if (wide >> 32)
		c->cpsr |= T32_CPSR_C;
	if ((~(a ^ b) & (a ^ r)) & 0x80000000u)
		c->cpsr |= T32_CPSR_V;
}

static uint32_t op_add(t32_cpu *c, uint32_t a, uint32_t b, int carry,
		       int use_carry, int setf)
{
	uint32_t cin = (use_carry && (c->cpsr & T32_CPSR_C)) ? 1u : 0u;
	uint64_t wide = (uint64_t)a + b + (carry ? cin : 0u);
	uint32_t r = (uint32_t)wide;

	if (setf) {
		set_nz(c, r);
		set_cv_add(c, a, b, r, wide);
	}
	return r;
}

static uint32_t op_sub(t32_cpu *c, uint32_t a, uint32_t b, int carry,
		       int use_carry, int setf)
{
	/* a - b (+ carry-in == borrow-not) */
	(void)carry;
	uint32_t cin = (!use_carry || (c->cpsr & T32_CPSR_C)) ? 1u : 0u;
	uint64_t wide = (uint64_t)a + (uint32_t)(~b) + cin;
	uint32_t r = (uint32_t)wide;

	if (setf) {
		set_nz(c, r);
		set_cv_add(c, a, ~b, r, wide);
	}
	return r;
}

static int cond_ok(t32_cpu *c, uint32_t cond)
{
	uint32_t f = c->cpsr;
	int n = !!(f & T32_CPSR_N), z = !!(f & T32_CPSR_Z);
	int cy = !!(f & T32_CPSR_C), v = !!(f & T32_CPSR_V);

	switch (cond) {
	case 0x0: return z;
	case 0x1: return !z;
	case 0x2: return cy;
	case 0x3: return !cy;
	case 0x4: return n;
	case 0x5: return !n;
	case 0x6: return v;
	case 0x7: return !v;
	case 0x8: return cy && !z;
	case 0x9: return !cy || z;
	case 0xA: return n == v;
	case 0xB: return n != v;
	case 0xC: return !z && n == v;
	case 0xD: return z || n != v;
	default: return 1; /* 0xE AL, 0xF NV (used by BLX) */
	}
}

/* ---- shifter (ARM operand2) ---- */

static uint32_t shift_imm(t32_cpu *c, uint32_t val, uint32_t type,
			  uint32_t amount, uint32_t *carry)
{
	*carry = c->cpsr & T32_CPSR_C ? 1u : 0u;
	switch (type) {
	case 0: /* LSL */
		if (amount == 0)
			return val;
		*carry = (val >> (32 - amount)) & 1u;
		return val << amount;
	case 1: /* LSR (0 -> 32) */
		if (amount == 0) {
			*carry = val >> 31;
			return 0;
		}
		*carry = (val >> (amount - 1)) & 1u;
		return val >> amount;
	case 2: /* ASR (0 -> 32) */
		if (amount == 0) {
			*carry = val >> 31;
			return (uint32_t)((int32_t)val >> 31);
		}
		*carry = (val >> (amount - 1)) & 1u;
		return (uint32_t)((int32_t)val >> amount);
	case 3: /* ROR (0 -> RRX) */
		if (amount == 0) {
			uint32_t cin = c->cpsr & T32_CPSR_C ? 1u : 0u;

			*carry = val & 1u;
			return (val >> 1) | (cin << 31);
		}
		*carry = (val >> (amount - 1)) & 1u;
		return (val >> amount) | (val << (32 - amount));
	}
	return val;
}

static uint32_t shift_reg(t32_cpu *c, uint32_t val, uint32_t type,
			  uint32_t amount, uint32_t *carry)
{
	*carry = c->cpsr & T32_CPSR_C ? 1u : 0u;
	amount &= 0xffu;
	if (amount == 0)
		return val;
	switch (type) {
	case 0: /* LSL */
		if (amount < 32) {
			*carry = (val >> (32 - amount)) & 1u;
			return val << amount;
		}
		*carry = (amount == 32) ? val & 1u : 0u;
		return 0;
	case 1: /* LSR */
		if (amount < 32) {
			*carry = (val >> (amount - 1)) & 1u;
			return val >> amount;
		}
		*carry = (amount == 32) ? val >> 31 : 0u;
		return 0;
	case 2: /* ASR */
		if (amount < 32) {
			*carry = (val >> (amount - 1)) & 1u;
			return (uint32_t)((int32_t)val >> amount);
		}
		*carry = val >> 31;
		return (uint32_t)((int32_t)val >> 31);
	case 3: /* ROR */
		amount &= 31u;
		if (amount == 0) {
			*carry = val >> 31;
			return val;
		}
		*carry = (val >> (amount - 1)) & 1u;
		return (val >> amount) | (val << (32 - amount));
	}
	return val;
}

/* ---- interpreter entry ---- */

static int exec_arm(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc);
static int exec_thumb(t32_cpu *c, uint32_t pc, uint32_t *npc);

static uint32_t get_reg(t32_cpu *c, uint32_t r, uint32_t pc, int thumb,
			int pc_bias_done)
{
	(void)pc_bias_done;
	if (r == 15)
		return pc + (thumb ? 4u : 8u);
	return c->r[r];
}

static int do_branch(t32_cpu *c, uint32_t target, int thumb_now,
		     uint32_t *npc)
{
	if (target == T32_LR_SENTINEL) {
		*npc = target;
		return 0;
	}
	if (target & 1u) {
		c->cpsr |= T32_CPSR_T;
		*npc = target & ~1u;
	} else if (!thumb_now && (target & 2u) && 0) {
		*npc = target & ~3u;
	} else {
		if (!(target & 1u) && !thumb_now)
			c->cpsr &= ~T32_CPSR_T;
		*npc = target & (thumb_now ? ~1u : ~3u);
	}
	return 0;
}

/* BX/BLX register */
static int branch_exchange(t32_cpu *c, uint32_t target, int link,
			   uint32_t pc, uint32_t *npc)
{
	if (link)
		c->r[14] = pc + 4;
	return do_branch(c, target, 0, npc);
}

/* =========================== ARM decoder =========================== */

static int arm_dp(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc)
{
	uint32_t opcode = (insn >> 21) & 0xfu;
	int s = (insn >> 20) & 1;
	uint32_t rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
	uint32_t op2 = 0, carry_out = 0, a, r = 0;
	int imm = (insn >> 25) & 1;

	(void)npc;
	if (imm) {
		uint32_t imm8 = insn & 0xffu, rot = ((insn >> 8) & 0xfu) * 2u;

		op2 = (imm8 >> rot) | (rot ? imm8 << (32 - rot) : 0);
		carry_out = rot ? (op2 >> 31) & 1u
				: (c->cpsr & T32_CPSR_C ? 1u : 0u);
	} else {
		uint32_t rm = insn & 0xfu;
		uint32_t type = (insn >> 5) & 3u;
		uint32_t val = get_reg(c, rm, pc, 0, 0);

		if ((insn >> 4) & 1u) {
			/* shift by register: PC reads +12; we use +8 (rare) */
			uint32_t rs = (insn >> 8) & 0xfu;
			uint32_t amount = c->r[rs] & 0xffu;

			op2 = shift_reg(c, val, type, amount, &carry_out);
		} else {
			uint32_t amount = (insn >> 7) & 0x1fu;

			op2 = shift_imm(c, val, type, amount, &carry_out);
		}
	}

	a = get_reg(c, rn, pc, 0, 0);
	switch (opcode) {
	case 0x0: r = a & op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	case 0x1: r = a ^ op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	case 0x2: r = op_sub(c, a, op2, 0, 0, s); break;
	case 0x3: r = op_sub(c, op2, a, 0, 0, s); break;
	case 0x4: r = op_add(c, a, op2, 0, 0, s); break;
	case 0x5: r = op_add(c, a, op2, 1, 1, s); break;
	case 0x6: r = op_sub(c, a, op2, 1, 1, s); break;
	case 0x7: r = op_sub(c, op2, a, 1, 1, s); break;
	case 0x8: r = a & op2; if (!s) return t32_fault(c, T32_TRAP_UNDEF, pc, insn); set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); break;
	case 0x9: r = a ^ op2; if (!s) return t32_fault(c, T32_TRAP_UNDEF, pc, insn); set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); break;
	case 0xA: r = op_sub(c, a, op2, 0, 0, 1); break;
	case 0xB: r = op_add(c, a, op2, 0, 0, 1); break;
	case 0xC: r = a | op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	case 0xD: r = op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	case 0xE: r = a & ~op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	case 0xF: r = ~op2; if (s) { set_nz(c, r); c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry_out << 29); } break;
	}

	/* TST/TEQ/CMP/CMN: flags only — never write rd (its field is 0!) */
	if (opcode >= 0x8 && opcode <= 0xB)
		return 0;

	if (rd == 15) {
		uint32_t t = r & ~1u;

		c->r[15] = t;
		return do_branch(c, r & 1u ? (r | 1u) : t, 0, npc);
	}
	c->r[rd] = r;
	return 0;
}

static int exec_arm(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc)
{
	uint32_t cond = insn >> 28;

	*npc = pc + 4;
	if (cond != 0xE && cond != 0xF && !cond_ok(c, cond))
		return 0;

	/* BLX imm (H bit) */
	if ((insn & 0xFE000000u) == 0xFA000000u) {
		uint32_t off = ((int32_t)(insn << 8) >> 6) + ((insn >> 23) & 2u);

		c->r[14] = pc + 4;
		c->cpsr |= T32_CPSR_T;
		*npc = pc + 8 + off;
		return 0;
	}
	/* BX / BLX reg */
	if ((insn & 0x0FFFFFF0u) == 0x012FFF10u ||
	    (insn & 0x0FFFFFF0u) == 0x012FFF30u) {
		uint32_t target = get_reg(c, insn & 0xfu, pc, 0, 0);
		int link = ((insn >> 5) & 3u) == 3u;

		return branch_exchange(c, target, link, pc, npc);
	}
	/* MOVW / MOVT */
	if ((insn & 0x0FF00000u) == 0x03000000u ||
	    (insn & 0x0FF00000u) == 0x03400000u) {
		uint32_t rd = (insn >> 12) & 0xfu;
		uint32_t v = (insn & 0xfffu) | (((insn >> 16) & 0xfu) << 12);

		if ((insn & 0x0FF00000u) == 0x03400000u) /* MOVT */
			c->r[rd] = (c->r[rd] & 0xffffu) | (v << 16);
		else /* MOVW */
			c->r[rd] = v;
		if (rd == 15)
			return do_branch(c, c->r[15], 0, npc);
		return 0;
	}
	/* CLZ */
	if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) {
		uint32_t v = c->r[insn & 0xfu], n = 0;

		if (v == 0)
			n = 32;
		else
			while (!(v & 0x80000000u)) {
				v <<= 1;
				n++;
			}
		c->r[(insn >> 12) & 0xfu] = n;
		return 0;
	}
	/* UDIV / SDIV */
	if ((insn & 0x0FF0FFF0u) == 0x0710F010u ||
	    (insn & 0x0FF0FFF0u) == 0x0730F010u) {
		uint32_t rd = (insn >> 12) & 0xfu;
		uint32_t a = c->r[(insn >> 16) & 0xfu], b = c->r[insn & 0xfu];

		if ((insn & 0x0FF0FFF0u) == 0x0710F010u) /* UDIV */
			c->r[rd] = b ? a / b : 0;
		else /* SDIV */
			c->r[rd] = b ? (uint32_t)((int32_t)a / (int32_t)b) : 0;
		return 0;
	}
	/* B / BL */
	if ((insn & 0x0E000000u) == 0x0A000000u) {
		int32_t off = (int32_t)(insn << 8) >> 6; /* sign-extend 24<<2 */

		if (insn & (1u << 24))
			c->r[14] = pc + 4;
		*npc = pc + 8 + (uint32_t)off;
		return 0;
	}
	/* MUL / MLA */
	if ((insn & 0x0FC000F0u) == 0x00000090u) {
		uint32_t rd = (insn >> 16) & 0xfu, rn = (insn >> 12) & 0xfu;
		uint32_t rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
		uint32_t r = c->r[rm] * c->r[rs];

		if ((insn >> 21) & 1u) /* MLA */
			r += c->r[rn];
		c->r[rd] = r;
		if ((insn >> 20) & 1u)
			set_nz(c, r);
		return 0;
	}
	/* UMULL/UMLAL/SMULL/SMLAL */
	if ((insn & 0x0F8000F0u) == 0x00800090u) {
		uint32_t rdhi = (insn >> 16) & 0xfu, rdlo = (insn >> 12) & 0xfu;
		uint32_t rs = (insn >> 8) & 0xfu, rm = insn & 0xfu;
		int signed_op = (insn >> 22) & 1, acc = (insn >> 21) & 1;
		uint64_t r;

		if (signed_op)
			r = (uint64_t)(int64_t)((int32_t)c->r[rm] *
					       (int32_t)c->r[rs]);
		else
			r = (uint64_t)c->r[rm] * c->r[rs];
		if (acc)
			r += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
		c->r[rdlo] = (uint32_t)r;
		c->r[rdhi] = (uint32_t)(r >> 32);
		if ((insn >> 20) & 1u) {
			c->cpsr &= ~(T32_CPSR_N | T32_CPSR_Z);
			if (r >> 63)
				c->cpsr |= T32_CPSR_N;
			if (!r)
				c->cpsr |= T32_CPSR_Z;
		}
		return 0;
	}
	/* halfword / signed transfers */
	if ((insn & 0x0E000090u) == 0x00000090u && (insn & 0x60u)) {
		uint32_t rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
		int p = (insn >> 24) & 1, u = (insn >> 23) & 1;
		int i = (insn >> 22) & 1, w = (insn >> 21) & 1;
		int l = (insn >> 20) & 1, s = (insn >> 6) & 1, h = (insn >> 5) & 1;
		uint32_t off;

		if (i)
			off = ((insn >> 4) & 0xf0u) | (insn & 0xfu);
		else
			off = get_reg(c, insn & 0xfu, pc, 0, 0);
		{
			uint32_t base = get_reg(c, rn, pc, 0, 0);
			uint32_t addr = p ? (u ? base + off : base - off) : base;

			if (l) {
				uint32_t v;

				if (s && h)
					v = (uint32_t)(int32_t)(int16_t)rd16(c, addr);
				else if (s && !h)
					v = (uint32_t)(int32_t)(int8_t)rd8(c, addr);
				else if (!s && h)
					v = rd16(c, addr);
				else {
					return t32_fault(c, T32_TRAP_UNDEF, pc,
							 insn);
				}
				if (c->trap)
					return -1;
				c->r[rd] = v;
			} else {
				if (!h)
					return t32_fault(c, T32_TRAP_UNDEF, pc,
							 insn);
				wr16(c, addr, c->r[rd]);
				if (c->trap)
					return -1;
			}
			if (!p) {
				c->r[rn] = u ? base + off : base - off;
			} else if (w) {
				c->r[rn] = addr;
			}
		}
		return 0;
	}
	/* LDR / STR (word/byte, imm/reg offset) */
	if ((insn & 0x0C000000u) == 0x04000000u) {
		uint32_t rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
		int i = (insn >> 25) & 1, p = (insn >> 24) & 1;
		int u = (insn >> 23) & 1, b = (insn >> 22) & 1;
		int w = (insn >> 21) & 1, l = (insn >> 20) & 1;
		uint32_t off;

		if (i) {
			uint32_t sh = (insn >> 5) & 3u;
			uint32_t amt = (insn >> 7) & 0x1fu;
			uint32_t carry;

			off = shift_imm(c, c->r[insn & 0xfu], sh, amt, &carry);
		} else {
			off = insn & 0xfffu;
		}
		{
			uint32_t base = get_reg(c, rn, pc, 0, 0);
			uint32_t addr = p ? (u ? base + off : base - off) : base;

			if (l) {
				uint32_t v = b ? rd8(c, addr) : rd32(c, addr);

				if (c->trap)
					return -1;
				if (rd == 15)
					return do_branch(c, v & ~1u, 0, npc);
				c->r[rd] = v;
			} else {
				uint32_t v = get_reg(c, rd, pc, 0, 0);

				if (b)
					wr8(c, addr, v);
				else
					wr32(c, addr, v);
				if (c->trap)
					return -1;
			}
			if (!p)
				c->r[rn] = u ? base + off : base - off;
			else if (w)
				c->r[rn] = addr;
		}
		return 0;
	}
	/* LDM / STM */
	if ((insn & 0x0E000000u) == 0x08000000u) {
		uint32_t rn = (insn >> 16) & 0xfu, rlist = insn & 0xffffu;
		int p = (insn >> 24) & 1, u = (insn >> 23) & 1;
		int s = (insn >> 22) & 1, w = (insn >> 21) & 1, l = (insn >> 20) & 1;
		uint32_t base = c->r[rn];
		uint32_t n = 0, addr, i;

		if (s)
			return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
		for (i = 0; i < 16; i++)
			if (rlist & (1u << i))
				n++;
		if (!n)
			return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
		if (u)
			addr = p ? base + 4u : base;
		else
			addr = p ? base - 4u * n : base - 4u * n + 4u;
		if (!u)
			addr = base - 4u * n + (p ? 0u : 4u);
		for (i = 0; i < 16; i++) {
			if (!(rlist & (1u << i)))
				continue;
			if (l) {
				uint32_t v = rd32(c, addr);

				if (c->trap)
					return -1;
				if (i == 15) {
					if (w)
						c->r[rn] = u ? base + 4u * n
							     : base - 4u * n;
					return do_branch(c, v & ~1u, 0, npc);
				}
				c->r[i] = v;
			} else {
				wr32(c, addr, get_reg(c, i, pc, 0, 0));
				if (c->trap)
					return -1;
			}
			addr += 4u;
		}
		if (w) {
			if (rn == 15)
				return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
			c->r[rn] = u ? base + 4u * n : base - 4u * n;
		}
		return 0;
	}
	/* data processing */
	if ((insn & 0x0C000000u) == 0x00000000u &&
	    !((insn & 0x0E000090u) == 0x00000090u)) {
		if ((insn & 0x0E000010u) == 0x00000010u &&
		    (insn & 0x0F0000F0u) != 0x00000090u)
			return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
		return arm_dp(c, pc, insn, npc);
	}
	/* SVC / BKPT / coprocessor (VFP) */
	if ((insn & 0x0F000000u) == 0x0F000000u)
		return t32_fault(c, T32_TRAP_SVC, pc, insn);
	if ((insn & 0x0E000000u) == 0x0C000000u ||
	    (insn & 0x0F000010u) == 0x0E000010u) {
		if (t32_vfp_exec(c, pc, insn, npc))
			return 0;
		return t32_fault(c, T32_TRAP_UNDEF, pc, insn); /* VFP gap */
	}
	/* NEON (unconditional 0xF2/0xF3 data-processing, 0xF4 ld/st) */
	if ((insn & 0xFE000000u) == 0xF2000000u ||
	    (insn & 0xFE000000u) == 0xF4000000u) {
		if (t32_vfp_exec(c, pc, insn, npc))
			return 0;
	}
	return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
}

/* =========================== Thumb decoder =========================== */

static int thumb16(t32_cpu *c, uint32_t pc, uint32_t insn, uint32_t *npc)
{
	*npc = pc + 2;

	/* 000xx: LSL/LSR/ASR imm5 */
	if ((insn & 0xE000u) == 0x0000u) {
		uint32_t op = (insn >> 11) & 3u;
		uint32_t rm = (insn >> 3) & 7u, rd = insn & 7u;
		uint32_t imm = (insn >> 6) & 0x1fu, carry, v;

		if (op == 3) { /* ADD/SUB reg/imm3 */
			uint32_t rn = (insn >> 6) & 7u;
			int sub = (insn >> 9) & 1, immf = (insn >> 10) & 1;
			uint32_t b = immf ? rn : c->r[rn];

			c->r[rd] = sub ? op_sub(c, c->r[rm], b, 0, 0, 1)
				       : op_add(c, c->r[rm], b, 0, 0, 1);
			return 0;
		}
		v = shift_imm(c, c->r[rm], op, imm, &carry);
		c->r[rd] = v;
		set_nz(c, v);
		c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry << 29);
		return 0;
	}
	/* MOV/CMP/ADD/SUB imm8 */
	if ((insn & 0xE000u) == 0x2000u) {
		uint32_t rd = (insn >> 8) & 7u, imm = insn & 0xffu;
		uint32_t op = (insn >> 11) & 3u;

		switch (op) {
		case 0: c->r[rd] = imm; set_nz(c, imm); break;
		case 1: (void)op_sub(c, c->r[rd], imm, 0, 0, 1); break;
		case 2: c->r[rd] = op_add(c, c->r[rd], imm, 0, 0, 1); break;
		case 3: c->r[rd] = op_sub(c, c->r[rd], imm, 0, 0, 1); break;
		}
		return 0;
	}
	/* 010000: ALU ops */
	if ((insn & 0xFC00u) == 0x4000u) {
		uint32_t op = (insn >> 6) & 0xfu;
		uint32_t rs = (insn >> 3) & 7u, rd = insn & 7u;
		uint32_t a = c->r[rd], b = c->r[rs], r, carry;

		switch (op) {
		case 0x0: r = a & b; c->r[rd] = r; set_nz(c, r); break;
		case 0x1: r = a ^ b; c->r[rd] = r; set_nz(c, r); break;
		case 0x2: r = shift_reg(c, a, 0, b & 0xffu, &carry);
			c->r[rd] = r; set_nz(c, r);
			c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry << 29); break;
		case 0x3: r = shift_reg(c, a, 1, b & 0xffu, &carry);
			c->r[rd] = r; set_nz(c, r);
			c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry << 29); break;
		case 0x4: r = shift_reg(c, a, 2, b & 0xffu, &carry);
			c->r[rd] = r; set_nz(c, r);
			c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry << 29); break;
		case 0x5: c->r[rd] = op_add(c, a, b, 1, 1, 1); break;
		case 0x6: c->r[rd] = op_sub(c, a, b, 1, 1, 1); break;
		case 0x7: r = shift_reg(c, a, 3, b & 0xffu, &carry);
			c->r[rd] = r; set_nz(c, r);
			c->cpsr = (c->cpsr & ~T32_CPSR_C) | (carry << 29); break;
		case 0x8: r = a & b; set_nz(c, r); break;
		case 0x9: c->r[rd] = op_sub(c, 0, b, 0, 0, 1); break;
		case 0xA: (void)op_sub(c, a, b, 0, 0, 1); break;
		case 0xB: (void)op_add(c, a, b, 0, 0, 1); break;
		case 0xC: r = a | b; c->r[rd] = r; set_nz(c, r); break;
		case 0xD: r = a * b; c->r[rd] = r; set_nz(c, r); break;
		case 0xE: r = a & ~b; c->r[rd] = r; set_nz(c, r); break;
		case 0xF: r = ~b; c->r[rd] = r; set_nz(c, r); break;
		}
		return 0;
	}
	/* 010001: hi-reg ops / BX */
	if ((insn & 0xFC00u) == 0x4400u) {
		uint32_t op = (insn >> 8) & 3u;
		uint32_t rs = ((insn >> 3) & 7u) | ((insn >> 3) & 8u);
		uint32_t rd = (insn & 7u) | ((insn >> 4) & 8u);
		uint32_t b = get_reg(c, rs, pc, 1, 0);

		switch (op) {
		case 0: {
			uint32_t r = c->r[rd] + b;

			if (rd == 15)
				return do_branch(c, r & ~1u, 1, npc);
			c->r[rd] = r;
			break;
		}
		case 1: (void)op_sub(c, c->r[rd], b, 0, 0, 1); break;
		case 2:
			if (rd == 15)
				return do_branch(c, b & ~1u, 1, npc);
			c->r[rd] = b;
			break;
		case 3:
			return branch_exchange(c, b, 0, pc, npc);
		}
		return 0;
	}
	/* LDR literal (PC-relative) */
	if ((insn & 0xF800u) == 0x4800u) {
		uint32_t rd = (insn >> 8) & 7u;
		uint32_t addr = ((pc + 4u) & ~3u) + ((insn & 0xffu) * 4u);

		c->r[rd] = rd32(c, addr);
		return c->trap ? -1 : 0;
	}
	/* 0101xx: load/store reg offset */
	if ((insn & 0xF000u) == 0x5000u) {
		uint32_t rm = (insn >> 6) & 7u, rn = (insn >> 3) & 7u;
		uint32_t rd = insn & 7u;
		uint32_t addr = c->r[rn] + c->r[rm];
		uint32_t op = (insn >> 9) & 7u;

		switch (op) {
		case 0: wr32(c, addr, c->r[rd]); break;
		case 1: wr16(c, addr, c->r[rd]); break;
		case 2: wr8(c, addr, c->r[rd]); break;
		case 3: c->r[rd] = (uint32_t)(int32_t)(int8_t)rd8(c, addr); break;
		case 4: c->r[rd] = rd32(c, addr); break;
		case 5: c->r[rd] = rd16(c, addr); break;
		case 6: c->r[rd] = rd8(c, addr); break;
		case 7: c->r[rd] = (uint32_t)(int32_t)(int16_t)rd16(c, addr); break;
		}
		return c->trap ? -1 : 0;
	}
	/* 011xx: LDR/STR imm5 (word/byte) */
	if ((insn & 0xE000u) == 0x6000u) {
		uint32_t b = (insn >> 12) & 1u, l = (insn >> 11) & 1u;
		uint32_t imm = (insn >> 6) & 0x1fu;
		uint32_t rn = (insn >> 3) & 7u, rd = insn & 7u;
		uint32_t addr = c->r[rn] + (b ? imm : imm * 4u);

		if (l)
			c->r[rd] = b ? rd8(c, addr) : rd32(c, addr);
		else if (b)
			wr8(c, addr, c->r[rd]);
		else
			wr32(c, addr, c->r[rd]);
		return c->trap ? -1 : 0;
	}
	/* 1000x: LDRH/STRH imm5 */
	if ((insn & 0xF000u) == 0x8000u) {
		uint32_t l = (insn >> 11) & 1u;
		uint32_t addr = c->r[(insn >> 3) & 7u] + ((insn >> 5) & 0x1fu) * 2u;
		uint32_t rd = insn & 7u;

		if (l)
			c->r[rd] = rd16(c, addr);
		else
			wr16(c, addr, c->r[rd]);
		return c->trap ? -1 : 0;
	}
	/* 1001x: SP-relative LDR/STR */
	if ((insn & 0xF000u) == 0x9000u) {
		uint32_t rd = (insn >> 8) & 7u;
		uint32_t addr = c->r[13] + (insn & 0xffu) * 4u;

		if ((insn >> 11) & 1u)
			c->r[rd] = rd32(c, addr);
		else
			wr32(c, addr, c->r[rd]);
		return c->trap ? -1 : 0;
	}
	/* 1010x: ADD rd, PC/SP, imm8 */
	if ((insn & 0xF000u) == 0xA000u) {
		uint32_t rd = (insn >> 8) & 7u;
		uint32_t base = ((insn >> 11) & 1u) ? c->r[13]
						    : ((pc + 4u) & ~3u);

		c->r[rd] = base + (insn & 0xffu) * 4u;
		return 0;
	}
	/* 1011 misc */
	if ((insn & 0xF000u) == 0xB000u) {
		/* ADD/SUB SP imm7 */
		if ((insn & 0xFF00u) == 0xB000u) {
			uint32_t imm = (insn & 0x7fu) * 4u;

			c->r[13] = (insn & 0x80u) ? c->r[13] + imm
						  : c->r[13] - imm;
			return 0;
		}
		/* PUSH/POP */
		if ((insn & 0xF600u) == 0xB400u) {
			int l = (insn >> 11) & 1;
			int r = (insn >> 8) & 1;
			uint32_t rlist = insn & 0xffu;
			uint32_t n = 0, addr, i;

			for (i = 0; i < 8; i++)
				if (rlist & (1u << i))
					n++;
			n += (uint32_t)r;
			if (l) { /* POP */
				addr = c->r[13];
				for (i = 0; i < 8; i++)
					if (rlist & (1u << i)) {
						c->r[i] = rd32(c, addr);
						if (c->trap)
							return -1;
						addr += 4;
					}
				if (r) {
					uint32_t v = rd32(c, addr);

					if (c->trap)
						return -1;
					c->r[13] = addr + 4;
					return do_branch(c, v & ~1u, 1, npc);
				}
				c->r[13] = addr;
			} else { /* PUSH */
				addr = c->r[13] - 4u * n;
				c->r[13] = addr;
				for (i = 0; i < 8; i++)
					if (rlist & (1u << i)) {
						wr32(c, addr, c->r[i]);
						if (c->trap)
							return -1;
						addr += 4;
					}
				if (r) {
					wr32(c, addr, c->r[14]);
					if (c->trap)
						return -1;
				}
			}
			return 0;
		}
		/* CBZ / CBNZ */
		if ((insn & 0xF500u) == 0xB100u) {
			int nz = (insn >> 11) & 1;
			uint32_t imm = (((insn >> 3) & 0x1fu) << 1) |
				       (((insn >> 9) & 1u) << 6);
			uint32_t v = c->r[insn & 7u];

			if (!!(v == 0) == !nz)
				*npc = pc + 4 + imm;
			return 0;
		}
		/* sign extend (SXTB/SXTH/UXTH/REV...) */
		if ((insn & 0xFFC0u) == 0xB200u ||
		    (insn & 0xFFC0u) == 0xB240u ||
		    (insn & 0xFFC0u) == 0xB280u ||
		    (insn & 0xFFC0u) == 0xB2C0u) {
			uint32_t op = (insn >> 6) & 3u;
			uint32_t rm = (insn >> 3) & 7u, rd = insn & 7u;
			uint32_t v = c->r[rm];

			if (op == 0) /* SXTB */
				c->r[rd] = (uint32_t)(int32_t)(int8_t)v;
			else if (op == 1) /* SXTH */
				c->r[rd] = (uint32_t)(int32_t)(int16_t)v;
			else if (op == 2) /* UXTB */
				c->r[rd] = v & 0xffu;
			else /* UXTH */
				c->r[rd] = v & 0xffffu;
			return 0;
		}
		/* REV / REV16 / REVSH (1011 1010 xx Rm Rd) */
		if ((insn & 0xFFC0u) == 0xBA00u) {
			uint32_t op = (insn >> 6) & 3u;
			uint32_t rm = (insn >> 3) & 7u, rd = insn & 7u;
			uint32_t v = c->r[rm];

			if (op == 0) /* REV */
				c->r[rd] = (v >> 24) | ((v >> 8) & 0xFF00u) |
					   ((v << 8) & 0xFF0000u) | (v << 24);
			else if (op == 1) /* REV16 */
				c->r[rd] = ((v >> 8) & 0x00FF00FFu) |
					   ((v << 8) & 0xFF00FF00u);
			else if (op == 3) /* REVSH */
				c->r[rd] = (uint32_t)(int32_t)(int16_t)
					   (((v & 0xFFu) << 8) |
					    ((v >> 8) & 0xFFu));
			else
				return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
			return 0;
		}
		return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
	}
	/* 1100x: STMIA/LDMIA */
	if ((insn & 0xF000u) == 0xC000u) {
		int l = (insn >> 11) & 1;
		uint32_t rn = (insn >> 8) & 7u, rlist = insn & 0xffu;
		uint32_t addr = c->r[rn], i;

		if (!rlist)
			return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
		for (i = 0; i < 8; i++) {
			if (!(rlist & (1u << i)))
				continue;
			if (l) {
				c->r[i] = rd32(c, addr);
				if (c->trap)
					return -1;
			} else {
				wr32(c, addr, c->r[i]);
				if (c->trap)
					return -1;
			}
			addr += 4;
		}
		if (!(l && (rlist & (1u << rn))))
			c->r[rn] = addr;
		return 0;
	}
	/* 1101: cond branch / SVC */
	if ((insn & 0xF000u) == 0xD000u) {
		uint32_t cond = (insn >> 8) & 0xfu;

		if (cond == 0xF) /* SVC */
			return t32_fault(c, T32_TRAP_SVC, pc, insn);
		if (cond == 0xE)
			return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
		if (cond_ok(c, cond)) {
			int32_t off = (int32_t)(int8_t)(insn & 0xffu) * 2;

			*npc = pc + 4 + (uint32_t)off;
		}
		return 0;
	}
	/* 11100: unconditional B */
	if ((insn & 0xF800u) == 0xE000u) {
		int32_t off = (int32_t)(insn << 21) >> 20; /* imm11 << 1 */

		*npc = pc + 4 + (uint32_t)off;
		return 0;
	}
	/* wide prefixes (0xE800/0xF000/0xF800) route to thumb32() in
	 * exec_thumb; anything else here is undefined */
	return t32_fault(c, T32_TRAP_UNDEF, pc, insn);
}

static uint32_t thumb_expand_imm(uint32_t imm12)
{
	if ((imm12 & 0xC00u) == 0) {
		uint32_t imm8 = imm12 & 0xFFu;

		switch ((imm12 >> 8) & 3u) {
		case 0:
			return imm8;
		case 1:
			return (imm8 << 16) | imm8;
		case 2:
			return (imm8 << 24) | (imm8 << 8);
		default:
			return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
		}
	} else {
		uint32_t unrot = 0x80u | (imm12 & 0x7Fu);
		uint32_t rot = imm12 >> 7; /* 8..31 */

		return (unrot >> rot) | (unrot << (32u - rot));
	}
}

/* Thumb-2 data-processing core (op map verified against capstone):
 * 0=AND 1=BIC 2=ORR/MOV 3=ORN/MVN 4=EOR 8=ADD 0xA=ADC 0xB=SBC
 * 0xD=SUB 0xE=RSB.  rn==15 turns ORR/ORN into MOV/MVN; rd==15 turns
 * ADD/SUB/ADC/SBC/RSB into flag-only (CMP/CMN) or branch, and AND/EOR
 * into TST/TEQ. */
static int thumb_dp(t32_cpu *c, uint32_t op, uint32_t rn, uint32_t rd,
		    uint32_t a, uint32_t b, int s, uint32_t pc, uint32_t *npc)
{
	uint32_t r;
	int write = 1;
	int branch = 0;

	switch (op) {
	case 0x0: r = a & b; break;
	case 0x1: r = a & ~b; break;
	case 0x2: r = (rn == 15) ? b : (a | b); break;
	case 0x3: r = (rn == 15) ? ~b : (a | ~b); break;
	case 0x4: r = a ^ b; break;
	case 0x8:
		r = op_add(c, a, b, 0, 0, s);
		goto arith;
	case 0xA:
		r = op_add(c, a, b, 1, 1, s);
		goto arith;
	case 0xB:
		r = op_sub(c, a, b, 1, 1, s);
		goto arith;
	case 0xD:
		r = op_sub(c, a, b, 0, 0, s);
		goto arith;
	case 0xE:
		r = op_sub(c, b, a, 0, 0, s);
		goto arith;
	default:
		return t32_fault(c, T32_TRAP_UNDEF, pc, (op << 8) | rd);
	}

	/* logical */
	if (rd == 15) {
		write = 0;
		if (rn == 15) /* MOV pc / MVN pc */
			branch = 1;
		else /* TST / TEQ */
			s = 1;
	}
	if (s)
		set_nz(c, r);
	if (branch)
		return do_branch(c, r & ~1u, 1, npc);
	if (write)
		c->r[rd] = r;
	return 0;

arith:
	if (rd == 15) {
		/* rd==pc && S -> CMP/CMN form: flags only, no branch.
		 * rd==pc && !S -> add/sub pc: branch, flags unaffected. */
		if (s)
			return 0;
		return do_branch(c, r & ~1u, 1, npc);
	}
	c->r[rd] = r;
	return 0;
}

static int thumb32(t32_cpu *c, uint32_t pc, uint32_t h1, uint32_t *npc)
{
	uint32_t h2 = rd16(c, pc + 2);

	if (c->trap)
		return -1;
	*npc = pc + 4;

	/* BL / BLX (11110 S imm10 / 11 J1 1 J2 imm11) */
	if ((h1 & 0xF800u) == 0xF000u && (h2 & 0xC000u) == 0xC000u) {
		uint32_t s = (h1 >> 10) & 1u;
		uint32_t j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
		uint32_t i1 = (~(j1 ^ s)) & 1u;
		uint32_t i2 = (~(j2 ^ s)) & 1u;
		int32_t off = (int32_t)(((uint32_t)(s ? 0xFF000000 : 0)) |
					(i1 << 23) | (i2 << 22) |
					((h1 & 0x3ffu) << 12) |
					((h2 & 0x7ffu) << 1));

		c->r[14] = (pc + 4) | 1u;
		if ((h2 & 0x1000u) == 0) { /* BLX */
			*npc = (pc + 4u + (uint32_t)off) & ~3u;
			c->cpsr &= ~T32_CPSR_T;
		} else { /* BL */
			*npc = pc + 4u + (uint32_t)off;
		}
		return 0;
	}

	/* B.W / Bcc.W (11110 S cond/imm6 | 10 J1 0 J2 imm11) */
	if ((h1 & 0xF800u) == 0xF000u && (h2 & 0xD000u) == 0x8000u) {
		uint32_t s = (h1 >> 10) & 1u;
		uint32_t j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
		uint32_t i1 = (~(j1 ^ s)) & 1u;
		uint32_t i2 = (~(j2 ^ s)) & 1u;
		int32_t off = (int32_t)(((uint32_t)(s ? 0xFF000000 : 0)) |
					(i1 << 23) | (i2 << 22) |
					((h1 & 0x3ffu) << 12) |
					((h2 & 0x7ffu) << 1));
		uint32_t cond = (h1 >> 6) & 0xfu;

		if (cond >= 0xEu || cond_ok(c, cond))
			*npc = pc + 4u + (uint32_t)off;
		return 0;
	}

	/* MOVW / MOVT (11110 i 10 0100 0 imm4 / 11110 i 10 0110 0 imm4) */
	if ((h1 & 0xFB70u) == 0xF240u) {
		uint32_t i = (h1 >> 10) & 1u;
		uint32_t imm4 = h1 & 0xfu;
		uint32_t imm3 = (h2 >> 12) & 7u;
		uint32_t imm8 = h2 & 0xffu;
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t v = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;

		if ((h1 & 0x0080u) == 0)
			c->r[rd] = v;
		else
			c->r[rd] = (c->r[rd] & 0xFFFFu) | (v << 16);
		return 0;
	}

	/* ADDW / SUBW (11110 i 10 0000 0 / 11110 i 10 1010 0) */
	if ((h1 & 0xFBF0u) == 0xF200u || (h1 & 0xFBF0u) == 0xF2A0u) {
		uint32_t i = (h1 >> 10) & 1u;
		uint32_t imm4 = h1 & 0xfu;
		uint32_t imm3 = (h2 >> 12) & 7u;
		uint32_t imm8 = h2 & 0xffu;
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t rn = h1 & 0xfu;
		uint32_t v = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
		uint32_t a = rn == 15 ? ((pc + 4u) & ~3u) : c->r[rn];

		if ((h1 & 0x00A0u) == 0x00A0u) /* SUBW */
			c->r[rd] = op_sub(c, a, v, 0, 0, 0);
		else /* ADDW */
			c->r[rd] = op_add(c, a, v, 0, 0, 0);
		return 0;
	}

	/* TBB / TBH (11101 001 1101 Rn | Rt 0000 0000 000x Rm) */
	if ((h1 & 0xFFF0u) == 0xE8D0u && (h2 & 0x0FF0u) == 0x0000u &&
	    (h2 & 0xF000u) == 0xF000u) {
		uint32_t rn = h1 & 0xfu, rm = h2 & 0xfu;
		uint32_t idx;

		if (h2 & 0x0010u) /* TBH: [Rn + Rm*2] */
			idx = rd16(c, c->r[rn] + c->r[rm] * 2u);
		else /* TBB: [Rn + Rm] */
			idx = rd8(c, c->r[rn] + c->r[rm]);
		if (c->trap)
			return -1;
		*npc = pc + 4u + idx * 2u;
		return 0;
	}

	/* load/store single (11111 00 0 ...):
	 *   h1: bits15:9=1111100, bit8=signed, bit7=t4, size=bits6:5,
	 *       bit4=L, Rn=h1[3:0]
	 *   T4 imm12 : bit7=1            -> addr = base + imm12
	 *   T3 imm8  : bit7=0, h2[11]=1  -> P=h2[10] U=h2[9] W=h2[8]
	 *   reg      : bit7=0, h2[11]=0  -> addr = base + (Rm << h2[5:4])
	 */
	if ((h1 & 0xFE00u) == 0xF800u) {
		int l = (h1 >> 4) & 1;

		/* T32 AdvSIMD structural load/store shares this h1 space —
		 * precise VLD1/VST1 rows in the VFP/NEON table take priority */
		if (t32_vfp_exec(c, pc, (h1 << 16) | h2, npc) || c->trap)
			return 0;
		int sign = (h1 >> 8) & 1;
		uint32_t rn = h1 & 0xfu;
		uint32_t rt = (h2 >> 12) & 0xfu;
		uint32_t size = (h1 >> 5) & 3u;
		uint32_t base = (rn == 15) ? ((pc + 4u) & ~3u) : c->r[rn];
		uint32_t addr, wb = 0;
		int is_wb = 0;

		if ((h1 >> 7) & 1u) { /* T4 imm12 */
			addr = base + (h2 & 0xfffu);
		} else if (h2 & 0x0800u) { /* T3 imm8 */
			uint32_t P = (h2 >> 10) & 1u;
			uint32_t U = (h2 >> 9) & 1u;
			uint32_t W = (h2 >> 8) & 1u;
			uint32_t imm8 = h2 & 0xffu;

			if (P) {
				addr = U ? base + imm8 : base - imm8;
				if (W) {
					wb = addr;
					is_wb = 1;
				}
			} else {
				addr = base;
				wb = U ? base + imm8 : base - imm8;
				is_wb = 1;
			}
		} else { /* register, LSL 0..3, add only */
			uint32_t rm = h2 & 0xfu;
			uint32_t amt = (h2 >> 4) & 3u;

			addr = base + (c->r[rm] << amt);
		}

		if (l) {
			uint32_t v;

			if (size == 0)
				v = sign ? (uint32_t)(int32_t)(int8_t)rd8(c, addr)
					 : rd8(c, addr);
			else if (size == 1)
				v = sign ? (uint32_t)(int32_t)(int16_t)rd16(c, addr)
					 : rd16(c, addr);
			else
				v = rd32(c, addr);
			if (c->trap)
				return -1;
			if (rt == 15) {
				if (is_wb && rn != 15)
					c->r[rn] = wb;
				return do_branch(c, v & ~1u, 1, npc);
			}
			c->r[rt] = v;
		} else {
			if (size == 0)
				wr8(c, addr, c->r[rt]);
			else if (size == 1)
				wr16(c, addr, c->r[rt]);
			else
				wr32(c, addr, c->r[rt]);
			if (c->trap)
				return -1;
		}
		if (is_wb && rn != 15)
			c->r[rn] = wb;
		return 0;
	}

	/* LDM/STM (1110 100 P U S W L Rn | reglist), LDREX/STREX */
	if ((h1 & 0xFE00u) == 0xE800u) {
		uint32_t P = (h1 >> 8) & 1u;
		uint32_t U = (h1 >> 7) & 1u;
		uint32_t S = (h1 >> 6) & 1u;
		uint32_t W = (h1 >> 5) & 1u;
		uint32_t L = (h1 >> 4) & 1u;
		uint32_t rn = h1 & 0xfu;

		/* LDREX: h2 = Rt:1111:0000:0000 */
		if ((h2 & 0x0FFFu) == 0x0F00u && L) {
			uint32_t rt = (h2 >> 12) & 0xfu;

			c->r[rt] = rd32(c, c->r[rn]);
			return c->trap ? -1 : 0;
		}
		/* STREX: h2 = Rt:Rd:0000:0000 */
		if ((h2 & 0x00FFu) == 0x0000u && !L && (h2 >> 12)) {
			uint32_t rt = (h2 >> 12) & 0xfu;
			uint32_t rd = (h2 >> 8) & 0xfu;

			wr32(c, c->r[rn], c->r[rt]);
			c->r[rd] = 0;
			if (c->trap)
				return -1;
			if (W)
				c->r[rn] += 4u;
			return 0;
		}
		if (S) /* user-mode / CPSR forms: not in V1.1(a) */
			return t32_fault(c, T32_TRAP_UNDEF, pc, (h1 << 16) | h2);
		{
			uint32_t list = h2 & 0xFFFFu;
			int n = 0, i;
			uint32_t addr;

			for (i = 0; i < 16; i++)
				if (list & (1u << i))
					n++;
			/* P=0 U=1: IA; P=1 U=0: DB; P=1 U=1: IB; P=0 U=0: DA */
			if (U)
				addr = P ? c->r[rn] + 4u : c->r[rn];
			else
				addr = P ? c->r[rn] - (uint32_t)n * 4u
					 : c->r[rn] - (uint32_t)n * 4u + 4u;
			for (i = 0; i < 16; i++) {
				if (!(list & (1u << i)))
					continue;
				if (L) {
					uint32_t v = rd32(c, addr);

					if (c->trap)
						return -1;
					if (i == 15)
						return do_branch(c, v & ~1u, 1,
								 npc);
					c->r[i] = v;
				} else {
					wr32(c, addr, c->r[i]);
					if (c->trap)
						return -1;
				}
				addr += 4u;
			}
			if (W && rn != 15) {
				if (!(L && (list & (1u << rn))))
					c->r[rn] = U ? c->r[rn] + (uint32_t)n * 4u
						     : c->r[rn] - (uint32_t)n * 4u;
			}
		}
		return 0;
	}

	/* data processing, modified immediate (11110 i 0 op S Rn | ...) */
	if ((h1 & 0xFA00u) == 0xF000u && (h2 & 0x8000u) == 0x0000u) {
		uint32_t op = (h1 >> 5) & 0xfu;
		uint32_t rn = h1 & 0xfu, rd = (h2 >> 8) & 0xfu;
		uint32_t s = (h1 >> 4) & 1u;
		uint32_t imm12 = (((h1 >> 10) & 1u) << 11) |
				 (((h2 >> 12) & 7u) << 8) | (h2 & 0xffu);
		uint32_t imm = thumb_expand_imm(imm12);

		return thumb_dp(c, op, rn, rd,
					rn == 15 ? ((pc + 4u) & ~3u) : c->r[rn],
					imm, (int)s, pc, npc);
	}

	/* data processing register (11101 0 ...) — same op map */
	if ((h1 & 0xFF00u) == 0xEA00u || (h1 & 0xFF00u) == 0xEB00u) {
		uint32_t op = (h1 >> 5) & 0xfu;
		uint32_t rn = h1 & 0xfu, rd = (h2 >> 8) & 0xfu;
		uint32_t s = (h1 >> 4) & 1u;
		uint32_t rm = h2 & 0xfu;
		uint32_t sh = (h2 >> 4) & 3u;
		uint32_t amt = (h2 >> 6) & 0x3Fu;
		uint32_t carry_dummy = c->cpsr & T32_CPSR_C ? 1u : 0u;
		uint32_t b = shift_imm(c, c->r[rm], sh, amt, &carry_dummy);

		return thumb_dp(c, op, rn, rd,
					rn == 15 ? ((pc + 4u) & ~3u) : c->r[rn],
					b, (int)s, pc, npc);
	}

	/* MUL / MLA / MLS (11111 011 0 ...) */
	if ((h1 & 0xFFE0u) == 0xFB00u) {
		uint32_t rn = h1 & 0xfu;
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t ra = (h2 >> 12) & 0xfu;
		uint32_t rm = h2 & 0xfu;

		if (h2 & 0x0010u) /* MLS */
			c->r[rd] = c->r[ra] - c->r[rn] * c->r[rm];
		else if (ra == 15) /* MUL */
			c->r[rd] = c->r[rn] * c->r[rm];
		else /* MLA */
			c->r[rd] = c->r[rn] * c->r[rm] + c->r[ra];
		return 0;
	}

	/* UDIV / SDIV (11111 0111 011/1101 Rn | 1111 Rd 1111 Rm) */
	if ((h1 & 0xFFF0u) == 0xFBB0u || (h1 & 0xFFF0u) == 0xFBD0u) {
		uint32_t rn = h1 & 0xfu;
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t rm = h2 & 0xfu;

		if ((h1 & 0xFFF0u) == 0xFBD0u) { /* SDIV */
			int32_t d = (int32_t)c->r[rm];

			c->r[rd] = d == 0 ? 0u :
				   (uint32_t)((int32_t)c->r[rn] / d);
		} else { /* UDIV */
			c->r[rd] = c->r[rm] == 0 ? 0u : c->r[rn] / c->r[rm];
		}
		return 0;
	}

	/* SMULL/UMULL/SMLAL/UMLAL (11111 011 1 ...) */
	if ((h1 & 0xFF80u) == 0xFB80u && ((h1 >> 4) & 1u) == 0u) {
		uint32_t rn = h1 & 0xfu;
		uint32_t rdhi = (h2 >> 8) & 0xfu;
		uint32_t rdlo = (h2 >> 12) & 0xfu;
		uint32_t rm = h2 & 0xfu;
		uint32_t kind = (h1 >> 5) & 3u;
		uint64_t p;

		if (kind == 0) { /* SMULL */
			p = (uint64_t)(int64_t)((int32_t)c->r[rn] * (int32_t)c->r[rm]);
			c->r[rdlo] = (uint32_t)p;
			c->r[rdhi] = (uint32_t)(p >> 32);
		} else if (kind == 1) { /* UMULL */
			p = (uint64_t)c->r[rn] * (uint64_t)c->r[rm];
			c->r[rdlo] = (uint32_t)p;
			c->r[rdhi] = (uint32_t)(p >> 32);
		} else if (kind == 2) { /* SMLAL */
			p = (uint64_t)(int64_t)((int32_t)c->r[rn] * (int32_t)c->r[rm]);
			p += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
			c->r[rdlo] = (uint32_t)p;
			c->r[rdhi] = (uint32_t)(p >> 32);
		} else { /* UMLAL */
			p = (uint64_t)c->r[rn] * (uint64_t)c->r[rm];
			p += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
			c->r[rdlo] = (uint32_t)p;
			c->r[rdhi] = (uint32_t)(p >> 32);
		}
		return 0;
	}


	/* CLZ (1111 1010 1011 Rn | 1111 Rd 1000 Rm) */
	if ((h1 & 0xFFF0u) == 0xFAB0u && ((h2 >> 4) & 0xfu) == 8u) {
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t rm = h2 & 0xfu;

		c->r[rd] = clz32(c->r[rm]);
		return 0;
	}

	/* REV/REV16/REVSH/RBIT (1111 1010 1001 Rn | 1111 Rd xxxx Rm) */
	if ((h1 & 0xFFF0u) == 0xFA90u) {
		uint32_t rd = (h2 >> 8) & 0xfu;
		uint32_t rm = h2 & 0xfu;
		uint32_t kind = (h2 >> 4) & 0xfu;
		uint32_t v = c->r[rm];

		if (kind == 8) /* REV */
			c->r[rd] = (v >> 24) | ((v >> 8) & 0xFF00u) |
				   ((v << 8) & 0xFF0000u) | (v << 24);
		else if (kind == 9) /* REV16 */
			c->r[rd] = ((v >> 8) & 0x00FF00FFu) |
				   ((v << 8) & 0xFF00FF00u);
		else if (kind == 0xB) /* REVSH */
			c->r[rd] = (uint32_t)(int32_t)(int16_t)
				   (((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
		else if (kind == 0xA) /* RBIT */
			c->r[rd] = rbit32(v);
		else
			return t32_fault(c, T32_TRAP_UNDEF, pc, (h1 << 16) | h2);
		return 0;
	}

	/* data processing, plain binary: bitfield / saturate / hints */
	if ((h1 & 0xFA00u) == 0xF200u) {
		uint32_t rn = h1 & 0xfu;
		uint32_t rd = (h2 >> 8) & 0xfu;

		/* SSAT / USAT (h1: 11110 0 11 op sh 0 Rn; bit7 = USAT,
		 * bit5 = sh; both are family bits, not discriminators) */
		if ((h1 & 0xFF50u) == 0xF300u) {
			uint32_t sat_field = h2 & 0x1Fu;
			uint32_t sh = (h1 >> 5) & 1u;
			uint32_t imm5 = (((h2 >> 12) & 0x7u) << 2) |
					((h2 >> 6) & 0x3u);
			uint32_t carry_dummy;
			uint32_t v;
			int32_t sv;

			(void)carry_dummy;
			if (sh) {
				uint32_t a = imm5 ? imm5 : 32;

				v = (uint32_t)((int32_t)c->r[rn] >>
					       (a > 31 ? 31 : a));
			} else {
				v = c->r[rn] << imm5;
			}
			sv = (int32_t)v;
			if ((h1 & 0x0080u) == 0) { /* SSAT */
				int32_t maxv = sat_field == 31 ? 0x7FFFFFFF :
					       ((1 << sat_field) - 1);
				int32_t minv = sat_field == 31 ? (int32_t)0x80000000 :
					       -((1 << sat_field) - 1) - 1;

				if (sv > maxv)
					v = (uint32_t)maxv;
				else if (sv < minv)
					v = (uint32_t)minv;
			} else { /* USAT */
				uint32_t umax = sat_field >= 32 ? 0xFFFFFFFFu :
						((1u << sat_field) - 1u);

				if (sv < 0)
					v = 0;
				else if ((uint32_t)sv > umax)
					v = umax;
			}
			c->r[rd] = v;
			return 0;
		}
		/* BFI / BFC */
		if ((h1 & 0xFFF0u) == 0xF360u) {
			uint32_t msb = h2 & 0x1Fu;
			uint32_t lsb = (((h2 >> 12) & 7u) << 2) |
				       ((h2 >> 6) & 3u);
			uint32_t width = msb - lsb + 1u;
			uint32_t mask = width >= 32 ? 0xFFFFFFFFu :
					       ((1u << width) - 1u) << lsb;

			if (rn == 15) /* BFC */
				c->r[rd] = c->r[rd] & ~mask;
			else /* BFI */
				c->r[rd] = (c->r[rd] & ~mask) |
					   ((c->r[rn] << lsb) & mask);
			return 0;
		}
		/* SBFX / UBFX */
		if ((h1 & 0xFFF0u) == 0xF340u || (h1 & 0xFFF0u) == 0xF3C0u) {
			uint32_t wm1 = h2 & 0x1Fu;
			uint32_t lsb = (((h2 >> 12) & 7u) << 2) |
				       ((h2 >> 6) & 3u);
			uint32_t width = wm1 + 1u;
			uint32_t v = c->r[rn] >> lsb;

			if (width < 32)
				v &= (1u << width) - 1u;
			if ((h1 & 0xFFF0u) == 0xF340u && width < 32 &&
			    (v & (1u << (width - 1))))
				v |= ~((1u << width) - 1u);
			c->r[rd] = v;
			return 0;
		}
		/* NOP / hints / SEV / WFE / WFI / YIELD */
		if ((h1 & 0xFFF0u) == 0xF3A0u && ((h2 >> 12) & 0xfu) == 8u)
			return 0;
		/* MRS (APSR/CPSR) */
		if ((h1 & 0xFFF0u) == 0xF3E0u && ((h2 >> 12) & 0xfu) == 8u) {
			c->r[rd] = c->cpsr;
			return 0;
		}
		/* MSR: trap (rare; V1.2) */
		return t32_fault(c, T32_TRAP_UNDEF, pc, (h1 << 16) | h2);
	}

	/* VFP/NEON: delegate to the coprocessor/SIMD unit */
	{
		uint32_t full = (h1 << 16) | h2;

		if (t32_vfp_exec(c, pc, full, npc))
			return 0;
		return t32_fault(c, T32_TRAP_UNDEF, pc, full);
	}
}

static int exec_thumb(t32_cpu *c, uint32_t pc, uint32_t *npc)
{
	uint32_t insn = rd16(c, pc);

	if (c->trap)
		return -1;
	/* 32-bit Thumb-2 prefixes */
	if ((insn & 0xF800u) == 0xE800u || (insn & 0xF800u) == 0xF000u ||
	    (insn & 0xF800u) == 0xF800u)
		return thumb32(c, pc, insn, npc);
	return thumb16(c, pc, insn, npc);
}

/* =========================== public entry =========================== */

static t32_space *g_call_mem_unused;

/* ---- JIT glue ---- */

#include "jit.h"

int t32_fallback_step(t32_cpu *c, uint32_t pc, uint32_t *npc)
{
	if (c->cpsr & T32_CPSR_T)
		(void)exec_thumb(c, pc, npc);
	else
		exec_arm(c, pc, rd32(c, pc), npc);
	return c->trap ? -1 : 0;
}

static t32_jit *g_jit;
static int g_jit_state;   /* 0 = lazy, 1 = on, -1 = off */

static int jit_enabled(void)
{
	if (g_jit_state == 0) {
		const char *e = getenv("TRIANGO_NO_JIT");

		if (e && e[0] && e[0] != '0') {
			g_jit_state = -1;
		} else {
			g_jit = t32_jit_create();
			g_jit_state = g_jit ? 1 : -1;
		}
	}
	return g_jit_state == 1;
}

uint32_t t32_call(t32_cpu *c, t32_space *mem, uint32_t fn,
		  const uint32_t *args, int nargs)
{
	static __thread uint32_t stack_base, stack_top;
	uint32_t pc;
	int i;

	(void)g_call_mem_unused;
	memset(c, 0, sizeof(*c));
	c->mem = mem;
	if (!stack_top) {
		void *p = t32_map(mem, 0x100000u, &stack_base);

		if (!p) {
			c->trap = T32_TRAP_MEM;
			return 0;
		}
		stack_top = stack_base + 0x100000u;
	}
	c->r[13] = (stack_top - 64u) & ~7u;
	c->r[14] = T32_LR_SENTINEL;
	for (i = 0; i < nargs && i < 4; i++)
		c->r[i] = args[i];
	if (fn & 1u) {
		c->cpsr = T32_CPSR_T;
		pc = fn & ~1u;
	} else {
		c->cpsr = 0;
		pc = fn & ~3u;
	}
	c->insn_budget = 500000000ull;

	for (;;) {
		uint32_t npc = pc + (c->cpsr & T32_CPSR_T ? 2u : 4u);
		int thumb = !!(c->cpsr & T32_CPSR_T);
		t32_shim_fn sf;

		if (pc == T32_LR_SENTINEL)
			return c->r[0];
		if (c->insn_budget-- == 0) {
			t32_fault(c, T32_TRAP_LIMIT, pc, 0);
			return 0;
		}
		sf = t32_shim_at(mem, pc);
		if (sf) {
			uint64_t rv = sf(c->r[0], c->r[1], c->r[2], c->r[3]);

			/* AAPCS: r0 = low half; r1 = high half (caller-saved,
			 * used by __aeabi_*divmod for the remainder) */
			c->r[0] = (uint32_t)rv;
			c->r[1] = (uint32_t)(rv >> 32);
			pc = c->r[14];
			if ((pc & 1u) && pc != T32_LR_SENTINEL) {
				c->cpsr |= T32_CPSR_T;
				pc &= ~1u;
			} else if (pc == T32_LR_SENTINEL) {
				return c->r[0];
			}
			continue;
		}
		if (thumb && jit_enabled()) {
			int x = t32_jit_run(g_jit, c, pc, &npc);

			if (x == T32_JIT_TRAP)
				return 0;
			/* SHIM / sentinel: loop-top checks handle npc == pc */
		} else if (thumb) {
			exec_thumb(c, pc, &npc);
		} else {
			exec_arm(c, pc, rd32(c, pc), &npc);
		}
		if (c->trap)
			return 0;
		pc = npc;
	}
}
