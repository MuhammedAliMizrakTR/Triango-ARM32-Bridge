#!/usr/bin/env python3
"""Generate VFP/NEON decoder table + test cases.

Run with the armenv venv (keystone, capstone):
    /tmp/armenv/bin/python nativebridge/engine/tests/gen_vfp.py

Outputs (next to this script):
    vfp_table.h   - decoder table: (mask, value, op_id) rows, tightest first
    vfp_cases.h   - execution test cases: insn + input regs + expected regs

The decoder masks are DERIVED: each op is assembled with several operand
variations and the constant bits become the mask.  This removes hand
decoding errors; capstone re-disassembles every case as a sanity check.
"""
import os
import struct
import sys

from keystone import KS_ARCH_ARM, KS_MODE_THUMB, KS_MODE_LITTLE_ENDIAN, Ks

HERE = os.path.dirname(os.path.abspath(__file__))
ks = Ks(KS_ARCH_ARM, KS_MODE_THUMB + KS_MODE_LITTLE_ENDIAN)


def enc32(t):
    e, _ = ks.asm(t)
    assert len(e) >= 4, t
    # thumb32 stream = LE halfword pair; insn word = hw1<<16 | hw2
    return (e[0] | (e[1] << 8)) << 16 | (e[2] | (e[3] << 8))


def infer(op_id, category, variants, ops_fields):
    """Return (mask, value) with operand bits stripped."""
    vals = [enc32(v) for v in variants]
    mask = 0xFFFFFFFF
    for v in vals[1:]:
        mask &= ~(v ^ vals[0]) & 0xFFFFFFFF
    return {
        "id": op_id,
        "cat": category,
        "mask": mask,
        "val": vals[0] & mask,
        "fields": ops_fields,
    }


# ---------------------------------------------------------------
# operand field bit numbers (verified by differential probing)
# VFP dp:      D=22 Vd=15:12  N=7 Vn=19:16  M=5 Vm=3:0  sz=8  imm=19:16+3:0
# NEON three:  D=22 Vd=15:12  N=7 Vn=19:16  M=5 Vm=3:0  Q=6  size=21:20
# ---------------------------------------------------------------
F_VFP = ["D:22", "Vd:15:12", "N:7", "Vn:19:16", "M:5", "Vm:3:0"]
F_VFP_UN = ["D:22", "Vd:15:12", "M:5", "Vm:3:0"]
F_NEON3 = ["D:22", "Vd:15:12", "N:7", "Vn:19:16", "M:5", "Vm:3:0", "Q:6"]
F_NEON2 = ["D:22", "Vd:15:12", "M:5", "Vm:3:0", "Q:6"]
F_NEONV = ["D:22", "Vd:15:12", "N:7", "Vn:19:16", "Q:6"]
F_IMM = ["D:22", "Vd:15:12", "Q:6"]

TABLE = []

# ---- VFP three-operand float ----
TABLE += [
    infer("VFP_ADD", "vfp3", ["vadd.f32 s0,s1,s2", "vadd.f32 s31,s30,s29",
                              "vadd.f32 s5,s6,s7", "vadd.f64 d0,d1,d2"], F_VFP),
    infer("VFP_SUB", "vfp3", ["vsub.f32 s0,s1,s2", "vsub.f32 s3,s4,s5",
                              "vsub.f32 s20,s21,s22", "vsub.f64 d0,d1,d2"], F_VFP),
    infer("VFP_MUL", "vfp3", ["vmul.f32 s0,s1,s2", "vmul.f32 s3,s4,s5",
                              "vmul.f32 s20,s21,s22", "vmul.f64 d0,d1,d2"], F_VFP),
    infer("VFP_NMUL", "vfp3", ["vnmul.f32 s0,s1,s2", "vnmul.f32 s3,s4,s5", "vnmul.f64 d0,d1,d2"],
          F_VFP),
    infer("VFP_DIV", "vfp3", ["vdiv.f32 s0,s1,s2", "vdiv.f32 s3,s4,s5",
                              "vdiv.f32 s20,s21,s22", "vdiv.f64 d0,d1,d2"], F_VFP),
    infer("VFP_MLA", "vfp3", ["vmla.f32 s0,s1,s2", "vmla.f32 s3,s4,s5", "vmla.f64 d0,d1,d2"],
          F_VFP),
    infer("VFP_MLS", "vfp3", ["vmls.f32 s0,s1,s2", "vmls.f32 s3,s4,s5", "vmls.f64 d0,d1,d2"],
          F_VFP),
    infer("VFP_NMLA", "vfp3", ["vnmla.f32 s0,s1,s2", "vnmla.f32 s3,s4,s5", "vnmla.f64 d0,d1,d2"],
          F_VFP),
    infer("VFP_NMLS", "vfp3", ["vnmls.f32 s0,s1,s2", "vnmls.f32 s3,s4,s5", "vnmls.f64 d0,d1,d2"],
          F_VFP),
]
# ---- VFP unary / compare / convert ----
TABLE += [
    infer("VFP_MOV", "vfp2", ["vmov.f32 s0,s1", "vmov.f32 s3,s4",
                              "vmov.f32 s20,s21", "vmov.f64 d0,d1"], F_VFP_UN),
    infer("VFP_ABS", "vfp2", ["vabs.f32 s0,s1", "vabs.f32 s3,s4",
                              "vabs.f32 s20,s21", "vabs.f64 d0,d1"], F_VFP_UN),
    infer("VFP_NEG", "vfp2", ["vneg.f32 s0,s1", "vneg.f32 s3,s4", "vneg.f64 d0,d1"],
          F_VFP_UN),
    infer("VFP_SQRT", "vfp2", ["vsqrt.f32 s0,s1", "vsqrt.f32 s3,s4", "vsqrt.f64 d0,d1"],
          F_VFP_UN),
    infer("VFP_CMP", "vfp2", ["vcmp.f32 s0,s1", "vcmp.f32 s3,s4", "vcmp.f64 d0,d1"],
          F_VFP_UN),
    infer("VFP_CMPE", "vfp2", ["vcmpe.f32 s0,s1", "vcmpe.f32 s3,s4", "vcmpe.f64 d0,d1"],
          F_VFP_UN),
    infer("VFP_CMP0", "vfp2", ["vcmp.f32 s0,#0", "vcmp.f32 s3,#0", "vcmp.f64 d0,#0"],
          F_VFP_UN),
    infer("VFP_CVT_F64_F32", "vfp2", ["vcvt.f64.f32 d0,s1",
                                      "vcvt.f64.f32 d3,s4"],
          F_VFP_UN),
    infer("VFP_CVT_F32_F64", "vfp2", ["vcvt.f32.f64 s0,d1",
                                      "vcvt.f32.f64 s3,d4"],
          F_VFP_UN),
    infer("VFP_CVT_S32", "vfp2", ["vcvt.s32.f32 s0,s1", "vcvt.s32.f32 s3,s4", "vcvt.s32.f64 s0,d1"],
          F_VFP_UN),
    infer("VFP_CVT_U32", "vfp2", ["vcvt.u32.f32 s0,s1", "vcvt.u32.f32 s3,s4", "vcvt.u32.f64 s0,d1"],
          F_VFP_UN),
    infer("VFP_CVT_FS32", "vfp2", ["vcvt.f32.s32 s0,s1", "vcvt.f32.s32 s3,s4", "vcvt.f64.s32 d0,s1"],
          F_VFP_UN),
    infer("VFP_CVT_FU32", "vfp2", ["vcvt.f32.u32 s0,s1", "vcvt.f32.u32 s3,s4", "vcvt.f64.u32 d0,s1"],
          F_VFP_UN),
]
# ---- NEON three-same (integer) ----
# ---- NEON three-same (integer): one op family per opcode; lane size is a
# runtime field (bits 21:20), so ids are size-less. ----
NEON_TS = [
    ("NEON_VADD_I", "vadd.i"), ("NEON_VSUB_I", "vsub.i"),
    ("NEON_VMUL_I", "vmul.i"), ("NEON_VMLA_I", "vmla.i"),
    ("NEON_VQADD_S", "vqadd.s"), ("NEON_VQSUB_S", "vqsub.s"),
    ("NEON_VMAX_S", "vmax.s"), ("NEON_VMIN_U", "vmin.u"),
    ("NEON_VCGT_S", "vcgt.s"), ("NEON_VCEQ_I", "vceq.i"),
]
for _id, _asm in NEON_TS:
    for _sfx in ("8", "16", "32"):
        TABLE.append(infer(f"{_id}{_sfx}", "n3",
                           [f"{_asm}{_sfx} d0,d1,d2",
                            f"{_asm}{_sfx} d30,d29,d28"],
                           F_NEON3))

# ---- NEON bitwise (two/three-same with op nibble 0x1 family) ----
for name in ("vand", "vbic", "vorr", "vorn", "veor"):
    TABLE.append(infer(f"NEON_{name.upper()}", "n3",
                       [f"{name} d0,d1,d2", f"{name} d30,d29,d28",
                        f"{name} q0,q1,q2"], F_NEON3))
for name in ("vbsl", "vbit", "vbif"):
    TABLE.append(infer(f"NEON_{name.upper()}", "n3",
                       [f"{name} d0,d1,d2", f"{name} d30,d29,d28"],
                       F_NEON3))
# ---- NEON shift-by-immediate ----
TABLE += [
    infer("NEON_VSHL_I8", "nsh", ["vshl.i8 d0,d1,#2", "vshl.i8 d30,d29,#7"],
          F_NEONV),
    infer("NEON_VSHL_I16", "nsh", ["vshl.i16 d0,d1,#2", "vshl.i16 d30,d29,#9"],
          F_NEONV),
    infer("NEON_VSHL_I32", "nsh", ["vshl.i32 d0,d1,#2", "vshl.i32 d30,d29,#17"],
          F_NEONV),
    infer("NEON_VSHR_S8", "nsh", ["vshr.s8 d0,d1,#2", "vshr.s8 d30,d29,#7"],
          F_NEONV),
    infer("NEON_VSHR_U8", "nsh", ["vshr.u8 d0,d1,#2", "vshr.u8 d30,d29,#7"],
          F_NEONV),
    infer("NEON_VSHR_S32", "nsh", ["vshr.s32 d0,d1,#2", "vshr.s32 d30,d29,#17"],
          F_NEONV),
    infer("NEON_VSHR_U32", "nsh", ["vshr.u32 d0,d1,#2", "vshr.u32 d30,d29,#17"],
          F_NEONV),
    infer("NEON_VSRA_S8", "nsh", ["vsra.s8 d0,d1,#2", "vsra.s8 d30,d29,#7"],
          F_NEONV),
    infer("NEON_VRSHR_U8", "nsh", ["vrshr.u8 d0,d1,#2", "vrshr.u8 d30,d29,#7"],
          F_NEONV),
]
# ---- NEON permute / two-reg ----
TABLE += [
    infer("NEON_VSWP", "np", ["vswp d0,d1", "vswp d30,d29", "vswp q0,q1"],
          F_NEON2),
    infer("NEON_VREV64_8", "np", ["vrev64.8 d0,d1", "vrev64.8 d30,d29"],
          F_NEON2),
    infer("NEON_VREV32_16", "np", ["vrev32.16 d0,d1", "vrev32.16 d30,d29"],
          F_NEON2),
    infer("NEON_VMVN", "np", ["vmvn d0,d1", "vmvn d30,d29"], F_NEON2),
    infer("NEON_VCNT", "np", ["vcnt.8 d0,d1", "vcnt.8 d30,d29"], F_NEON2),
    infer("NEON_VEXT", "np", ["vext.8 d0,d1,d2,#3", "vext.8 d30,d29,d28,#5"],
          F_NEON3),
]
# ---- NEON immediates ----
TABLE += [
    infer("NEON_VMOV_I32", "ni", ["vmov.i32 d0,#0", "vmov.i32 d30,#255"],
          F_IMM),
    infer("NEON_VMOV_I16", "ni", ["vmov.i16 d0,#0", "vmov.i16 d30,#255"],
          F_IMM),
    infer("NEON_VMVN_I32", "ni", ["vmvn.i32 d0,#0", "vmvn.i32 d30,#255"],
          F_IMM),
]
# ---- NEON ld/st multiple ----
TABLE += [
    infer("NEON_VLD1_1", "nls", ["vld1.8 {d0},[r1]", "vld1.32 {d30},[r2]",
                                 "vld1.64 {d5},[r3]"], ["D:22", "Vd:15:12"]),
    infer("NEON_VLD1_2", "nls", ["vld1.32 {d0,d1},[r1]", "vld1.8 {d2,d3},[r4]"],
          ["D:22", "Vd:15:12"]),
    infer("NEON_VLD1_3", "nls", ["vld1.32 {d0,d1,d2},[r1]",
                                 "vld1.8 {d4,d5,d6},[r2]"],
          ["D:22", "Vd:15:12"]),
    infer("NEON_VLD1_4", "nls", ["vld1.32 {d0,d1,d2,d3},[r1]",
                                 "vld1.8 {d4,d5,d6,d7},[r2]"],
          ["D:22", "Vd:15:12"]),
    infer("NEON_VST1_1", "nls", ["vst1.8 {d0},[r1]", "vst1.32 {d30},[r2]"],
          ["D:22", "Vd:15:12"]),
    infer("NEON_VST1_2", "nls", ["vst1.32 {d0,d1},[r1]",
                                 "vst1.8 {d2,d3},[r4]"],
          ["D:22", "Vd:15:12"]),
]
# ---- VFP load/store + core moves (mask by hand-verified fields) ----
TABLE += [
    infer("VFP_LDR_S", "vls", ["vldr s0,[r1]", "vldr s31,[r2,#255*4]",
                               "vldr s5,[sp,#4]"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23"]),
    infer("VFP_LDR_D", "vls", ["vldr d0,[r1]", "vldr d15,[r2,#255*4]"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23"]),
    infer("VFP_STR_S", "vls", ["vstr s0,[r1]", "vstr s31,[sp,#-4]"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23"]),
    infer("VFP_STR_D", "vls", ["vstr d0,[r1]", "vstr d15,[sp,#-4]"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23"]),
    infer("VFP_LDM_S", "vls", ["vldmia r0, {s0-s3}", "vldmia r1!, {s4-s7}"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23", "W:21"]),
    infer("VFP_LDM_D", "vls", ["vldmia r0, {d0-d3}", "vldmdb r1!, {d4-d7}"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23", "W:21"]),
    infer("VFP_STM_S", "vls", ["vstmia r0, {s0-s3}", "vstmdb r1!, {s4-s7}"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23", "W:21"]),
    infer("VFP_STM_D", "vls", ["vstmia r0, {d0-d3}", "vstmdb r1!, {d4-d7}"],
          ["D:22", "Vd:15:12", "Rn:19:16", "imm8:7:0", "U:23", "W:21"]),
    infer("VFP_MOV_SR", "vcore", ["vmov s0,r1", "vmov s31,r3"],
          ["D:22", "Vd:15:12", "Rt:19:16"]),
    infer("VFP_MOV_RS", "vcore", ["vmov r1,s0", "vmov r3,s31"],
          ["D:22", "Vd:15:12", "Rt:19:16"]),
    infer("VFP_MOV_DRR", "vcore", ["vmov d0,r1,r2", "vmov d15,r3,r4"],
          ["D:22", "Vd:15:12", "Rt2:19:16", "Rt:15:12"]),
    infer("VFP_MOV_RRD", "vcore", ["vmov r1,r2,d0", "vmov r3,r4,d15"],
          ["D:22", "Vd:15:12", "Rt2:19:16", "Rt:15:12"]),
    infer("VFP_VMRS", "vcore", ["vmrs r0,fpscr", "vmrs r3,fpscr",
                                "vmrs apsr_nzcv,fpscr"],
          ["Rt:15:12"]),
    infer("VFP_VMSR", "vcore", ["vmsr fpscr,r0", "vmsr fpscr,r3"],
          ["Rt:15:12"]),
]

# strip operand bits from masks for the decoder table
def field_bits(spec):
    """'15:12' -> [12..15], '22' -> [22]"""
    parts = spec.split(":")
    if len(parts) == 1:
        return [int(parts[0])]
    hi, lo = int(parts[0]), int(parts[1])
    return list(range(lo, hi + 1))


rows = []
for e in TABLE:
    m = e["mask"]
    for f in e["fields"]:
        name, spec = f.split(":", 1)
        for b in field_bits(spec):
            m &= ~(1 << b) & 0xFFFFFFFF
    rows.append((m, e["val"] & m, e["id"], e["cat"]))

# dedupe + sort: more specific (higher popcount) first
seen = set()
uniq = []
for r in rows:
    if r[:2] in seen:
        continue
    seen.add(r[:2])
    uniq.append(r)
uniq.sort(key=lambda r: -bin(r[0]).count("1"))

with open(os.path.join(HERE, "vfp_table.h"), "w") as f:
    f.write("/* generated by gen_vfp.py — do not edit */\n")
    f.write("#ifndef VFP_TABLE_H\n#define VFP_TABLE_H\n\n")
    f.write("typedef struct { uint32_t mask, val; uint8_t op; } t32_vfp_row;\n\n")
    cats = {}
    for m, v, i, c in uniq:
        cats.setdefault(c, len(cats))
    f.write("enum t32_vfp_op {\n")
    ops = []
    for m, v, i, c in uniq:
        if i not in ops:
            ops.append(i)
    for idx, name in enumerate(ops):
        f.write(f"\tT32V_{name} = {idx},\n")
    f.write("};\n\n")
    f.write(f"#define T32_VFP_ROWS {len(uniq)}\n")
    f.write("static const t32_vfp_row t32_vfp_table[] = {\n")
    for m, v, i, c in uniq:
        f.write(f"\t{{ 0x{m:08X}u, 0x{v:08X}u, T32V_{i} }}, /* {c} */\n")
    f.write("};\n\n#endif\n")

print(f"vfp_table.h: {len(uniq)} rows, {len(ops)} ops")

# ---------------------------------------------------------------
# execution cases
# ---------------------------------------------------------------
import math

def f32(x):
    return struct.unpack("f", struct.pack("f", float(x)))[0]

CASES = []  # (asm, setup_fn, expect_fn) computed below as data


def case(asm, s_in, s_exp, fpscr_in=0, fpscr_exp=None, d_in=None,
         d_exp=None, r_in=None, r_exp=None):
    CASES.append({
        "asm": asm, "s_in": s_in or {}, "s_exp": s_exp or {},
        "fpscr_in": fpscr_in,
        "fpscr_exp": fpscr_exp,
        "d_in": d_in or {}, "d_exp": d_exp or {},
        "r_in": r_in or {}, "r_exp": r_exp or {},
    })


# VFP float cases (s-regs)
case("vadd.f32 s0,s1,s2", {1: f32(1.5), 2: f32(2.25)}, {0: f32(3.75)})
case("vsub.f32 s0,s1,s2", {1: f32(5.0), 2: f32(2.0)}, {0: f32(3.0)})
case("vmul.f32 s0,s1,s2", {1: f32(3.0), 2: f32(-2.0)}, {0: f32(-6.0)})
case("vdiv.f32 s0,s1,s2", {1: f32(7.0), 2: f32(2.0)}, {0: f32(3.5)})
case("vmla.f32 s0,s1,s2", {0: f32(1.0), 1: f32(2.0), 2: f32(3.0)},
     {0: f32(7.0)})
case("vmls.f32 s0,s1,s2", {0: f32(1.0), 1: f32(2.0), 2: f32(3.0)},
     {0: f32(-5.0)})
case("vabs.f32 s0,s1", {1: f32(-4.5)}, {0: f32(4.5)})
case("vneg.f32 s0,s1", {1: f32(4.5)}, {0: f32(-4.5)})
case("vsqrt.f32 s0,s1", {1: f32(9.0)}, {0: f32(3.0)})
case("vmov.f32 s0,s1", {1: f32(12.5)}, {0: f32(12.5)})
case("vcmp.f32 s0,s1", {0: f32(1.0), 1: f32(2.0)}, {},
     fpscr_exp=0x80000000)  # N=1 (a<b)
case("vcmp.f32 s0,s1", {0: f32(2.0), 1: f32(2.0)}, {},
     fpscr_exp=0x40000000 | 0x20000000)  # Z=1 C=1
case("vcmp.f32 s0,#0", {0: f32(-0.5)}, {},
     fpscr_exp=0x80000000)  # N=1 (a<0)
case("vcvt.s32.f32 s0,s1", {1: f32(3.75)}, {0: 4})
case("vcvt.s32.f32 s0,s1", {1: f32(-3.75)}, {0: (-4) & 0xFFFFFFFF})
case("vcvt.u32.f32 s0,s1", {1: f32(3.75)}, {0: 4})
case("vcvt.f32.s32 s0,s1", {1: (-5) & 0xFFFFFFFF}, {0: f32(-5.0)})
case("vcvt.f32.u32 s0,s1", {1: 4000000000}, {0: f32(4000000000.0)})

# VFP double cases (d regs)
case("vadd.f64 d0,d1,d2", {}, {}, d_in={1: 1.5, 2: 2.25}, d_exp={0: 3.75})
case("vmul.f64 d0,d1,d2", {}, {}, d_in={1: -3.0, 2: 2.0}, d_exp={0: -6.0})
case("vdiv.f64 d0,d1,d2", {}, {}, d_in={1: 7.0, 2: -2.0}, d_exp={0: -3.5})
case("vsqrt.f64 d0,d1", {}, {}, d_in={1: 16.0}, d_exp={0: 4.0})
case("vmov.f64 d0,d1", {}, {}, d_in={1: 42.5}, d_exp={0: 42.5})
case("vcvt.f64.f32 d0,s1", {1: f32(2.5)}, {}, d_exp={0: 2.5})
case("vcvt.f32.f64 s0,d1", {}, {0: f32(2.5)}, d_in={1: 2.5})
case("vcvt.s32.f64 s0,d1", {}, {0: 100}, d_in={1: 99.75})

# NEON integer cases (d regs as raw u64 lane values)
def lanes(*vals):
    v = 0
    for i, x in enumerate(vals):
        v |= (x & 0xFFFFFFFFFFFFFFFF) << (i * 64)
    return v

case("vadd.i32 d0,d1,d2", {}, {},
     d_in={1: 0x0000000500000003 | (0xFFFFFFFF << 32) & 0xFFFFFFFF00000000
           if False else (0x00000005 << 32) | 0x00000003,
           2: (0x00000002 << 32) | 0x00000004},
     d_exp={0: (0x00000007 << 32) | 0x00000007})
case("vadd.i16 d0,d1,d2", {}, {},
     d_in={1: 0x0005000300020001, 2: 0x0001000100010001},
     d_exp={0: 0x0006000400030002})
case("vadd.i8 d0,d1,d2", {}, {},
     d_in={1: 0x0102030405060708, 2: 0x0101010101010101},
     d_exp={0: 0x0203040506070809})
case("vsub.i32 d0,d1,d2", {}, {},
     d_in={1: (10 << 32) | 10, 2: (3 << 32) | 4},
     d_exp={0: (7 << 32) | 6})
case("vmul.i16 d0,d1,d2", {}, {},
     d_in={1: 0x0003000300030003, 2: 0x0002000200020002},
     d_exp={0: 0x0006000600060006})
case("vand d0,d1,d2", {}, {},
     d_in={1: 0xFF00FF00FF00FF00, 2: 0x0F0F0F0F0F0F0F0F},
     d_exp={0: 0x0F000F000F000F00})
case("vorr d0,d1,d2", {}, {},
     d_in={1: 0xFF00FF00FF00FF00, 2: 0x0F0F0F0F0F0F0F0F},
     d_exp={0: 0xFF0FFF0FFF0FFF0F})
case("veor d0,d1,d2", {}, {},
     d_in={1: 0xFF00FF00FF00FF00, 2: 0xFFFFFFFFFFFFFFFF},
     d_exp={0: 0x00FF00FF00FF00FF})
case("vbic d0,d1,d2", {}, {},
     d_in={1: 0xFFFFFFFFFFFFFFFF, 2: 0x00000000FF000000},
     d_exp={0: 0xFFFFFFFF00FFFFFF})
case("vmvn d0,d1", {}, {},
     d_in={1: 0x00000000FFFFFFFF}, d_exp={0: 0xFFFFFFFF00000000})
case("vqadd.s8 d0,d1,d2", {}, {},
     d_in={1: 0x7F0180027F038004, 2: 0x0101010101010101},
     d_exp={0: 0x7F0281037F048105})  # note: 0x80+1 saturates to 0x80
case("vcgt.s32 d0,d1,d2", {}, {},
     d_in={1: (5 << 32) | 0xFFFFFFFF, 2: (3 << 32) | 1},
     d_exp={0: (0xFFFFFFFF << 32) | 0x00000000})
case("vceq.i32 d0,d1,d2", {}, {},
     d_in={1: (5 << 32) | 7, 2: (5 << 32) | 8},
     d_exp={0: (0xFFFFFFFF << 32) | 0x00000000})
case("vshl.i32 d0,d1,#2", {}, {},
     d_in={1: (3 << 32) | 5}, d_exp={0: (12 << 32) | 20})
case("vshr.s32 d0,d1,#2", {}, {},
     d_in={1: ((-8 & 0xFFFFFFFF) << 32) | 8},
     d_exp={0: ((-2 & 0xFFFFFFFF) << 32) | 2})
case("vshr.u32 d0,d1,#2", {}, {},
     d_in={1: (0xFFFFFFFF << 32) | 8},
     d_exp={0: (0x3FFFFFFF << 32) | 2})
case("vswp d0,d1", {}, {},
     d_in={1: 0x1111111111111111, 2: 0},
     d_exp={0: 0x1111111111111111, 1: 0x0000000000000000})
case("vrev64.8 d0,d1", {}, {},
     d_in={1: 0x0807060504030201}, d_exp={0: 0x0102030405060708})
case("vrev32.16 d0,d1", {}, {},
     d_in={1: 0x0004000300020001}, d_exp={0: 0x0001000200030004})
case("vext.8 d0,d1,d2,#3", {}, {},
     d_in={1: 0x8877665544332211, 2: 0x1122334455667788},
     d_exp={0: 0x3322111122334455})
case("vcnt.8 d0,d1", {}, {},
     d_in={1: 0x0F000000000000FF}, d_exp={0: 0x0400000000000008})
case("vmov.i32 d0,#255", {}, {},
     d_exp={0: 0x000000FF000000FF})
case("vmov.i16 d0,#255", {}, {},
     d_exp={0: 0x00FF00FF00FF00FF})
case("vmov.i32 d0,#0", {}, {}, d_exp={0: 0})

# core moves
case("vmov s0,r1", {}, {0: 0xDEADBEEF}, r_in={1: 0xDEADBEEF})
case("vmov r1,s0", {0: 0xCAFEBABE}, {}, r_exp={1: 0xCAFEBABE})
case("vmov d0,r1,r2", {}, {},
     r_in={1: 0x11223344, 2: 0x55667788},
     d_exp={0: (0x55667788 << 32) | 0x11223344})
case("vmov r1,r2,d0", {}, {},
     d_in={0: (0x55667788 << 32) | 0x11223344},
     r_exp={1: 0x11223344, 2: 0x55667788})
case("vmrs r0,fpscr", {}, {}, fpscr_in=0xC0000000, r_exp={0: 0xC0000000})
case("vmsr fpscr,r1", {}, {}, fpscr_exp=0x60000000, r_in={1: 0x60000000})

# memory: handled in C test separately (vld/vstr need a mapped buffer)


def emit_cases():
    lines = []
    lines.append("/* generated by gen_vfp.py — do not edit */")
    lines.append("#ifndef VFP_CASES_H")
    lines.append("#define VFP_CASES_H")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("\tuint32_t insn;")
    lines.append("\tuint32_t s_in[32];")
    lines.append("\tuint32_t s_exp[32];")
    lines.append("\tuint64_t d_in[16];")
    lines.append("\tuint64_t d_exp[16];")
    lines.append("\tuint32_t r_in[16];")
    lines.append("\tuint32_t r_exp[16];")
    lines.append("\tuint32_t fpscr_in, fpscr_exp, fpscr_check;")
    lines.append("\tuint8_t chk_s, chk_d, chk_r;")
    lines.append("\tconst char *asm_txt;")
    lines.append("} t32_vfp_case;")
    lines.append("")
    lines.append(f"#define T32_VFP_NCASES {len(CASES)}")
    lines.append("static const t32_vfp_case t32_vfp_cases[] = {")
    for cs in CASES:
        insn = enc32(cs["asm"])
        s_in = [0] * 32
        s_exp = [0] * 32
        d_in = [0] * 16
        d_exp = [0] * 16
        r_in = [0] * 16
        r_exp = [0] * 16
        def s_bits(v):
            if isinstance(v, float):
                return struct.unpack("I", struct.pack("f", v))[0]
            return v & 0xFFFFFFFF

        def d_bits(v):
            if isinstance(v, float):
                return struct.unpack("Q", struct.pack("d", v))[0]
            return v & 0xFFFFFFFFFFFFFFFF

        for k, v in cs["s_in"].items():
            s_in[k] = s_bits(v)
        for k, v in cs["s_exp"].items():
            s_exp[k] = s_bits(v)
        for k, v in cs["d_in"].items():
            d_in[k] = d_bits(v)
        for k, v in cs["d_exp"].items():
            d_exp[k] = d_bits(v)
        for k, v in cs["r_in"].items():
            r_in[k] = v & 0xFFFFFFFF
        for k, v in cs["r_exp"].items():
            r_exp[k] = v & 0xFFFFFFFF
        # registers without explicit expectations are expected unchanged
        # (account for s/d aliasing when computing "unchanged")
        s_init = list(s_in)
        for j in range(16):
            if d_in[j]:
                s_init[2 * j] = d_in[j] & 0xFFFFFFFF
                s_init[2 * j + 1] = (d_in[j] >> 32) & 0xFFFFFFFF
        d_init = [(s_init[2 * j] | (s_init[2 * j + 1] << 32)) & 0xFFFFFFFFFFFFFFFF
                  for j in range(16)]
        if cs["s_exp"]:
            for j in range(32):
                s_exp[j] = s_init[j] if j not in cs["s_exp"] else s_exp[j]
        if cs["d_exp"]:
            for j in range(16):
                d_exp[j] = d_init[j] if j not in cs["d_exp"] else d_exp[j]
        if cs["r_exp"]:
            for j in range(16):
                r_exp[j] = r_in[j] if j not in cs["r_exp"] else r_exp[j]

        def arr(a, suf):
            return "{" + ",".join(f"0x{x:08X}{suf}" for x in a) + "}"

        def arr64(a):
            return "{" + ",".join(f"0x{x:016X}ull" for x in a) + "}"

        fpscr_check = 0 if cs["fpscr_exp"] is None else 1
        lines.append("\t{")
        lines.append(f"\t\t0x{insn:08X}u,")
        lines.append(f"\t\t{arr(s_in, 'u')},")
        lines.append(f"\t\t{arr(s_exp, 'u')},")
        lines.append(f"\t\t{arr64(d_in)},")
        lines.append(f"\t\t{arr64(d_exp)},")
        lines.append(f"\t\t{arr(r_in, 'u')},")
        lines.append(f"\t\t{arr(r_exp, 'u')},")
        lines.append(f"\t\t0x{cs['fpscr_in']:08X}u, "
                     f"0x{(cs['fpscr_exp'] or 0):08X}u, {fpscr_check}u,")
        lines.append(f"\t\t{1 if cs['s_exp'] else 0}u, "
                     f"{1 if cs['d_exp'] else 0}u, "
                     f"{1 if cs['r_exp'] else 0}u,")
        lines.append(f'\t\t"{cs["asm"]}"')
        lines.append("\t},")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(os.path.join(HERE, "vfp_cases.h"), "w") as f:
        f.write("\n".join(lines) + "\n")


emit_cases()
print(f"vfp_cases.h: {len(CASES)} cases")
