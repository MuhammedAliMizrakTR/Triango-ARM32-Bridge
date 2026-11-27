#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Generate GPU thunk guest shims + host dispatch from thunk JSON definitions.

Each JSON (see gpu/thunks/*.json) lists API functions with typed arguments.
The generator emits three C files:

  thunks_gen.h        op codes + guest prototypes
  thunks_gen_guest.c  guest-side stubs (arm32 shim / tests): marshal args into
                      the shared channel and call tg_call()
  thunks_gen_host.c   host-side dispatch (triango-gpud): decode the message,
                      resolve the real driver symbol and call it

Argument types
--------------
  scalars   int32 uint32 int64 uint64 float double   -> one u64 slot each
  handle    opaque pointer-sized token (EGLDisplay, VkInstance, ...)
  string    const char*  -> payload copy (NUL included), slot = off<<32|len
  buf_in    const void* + length (len_fixed / len_arg x len_scale)
  buf_out   void* + length; mutated in place, copied back on response
  handle_out        void* out-slot for one handle (host writes u64)
  handle_array_out  array of handles (count_fixed elems; host writes u64
                    elements, guest copies back pointer-sized elements)

A function flagged "struct_conv": true needs 32<->64 struct conversion
(VkInstanceCreateInfo & friends) and is emitted as TGPU_ST_ENOTSUP on the
host side until the converter library lands (docs/gpu-thunking.md).
"""

import argparse
import json
import os
import sys

ARG_TYPES = {
    "int32", "uint32", "int64", "uint64", "float", "double", "handle",
    "string", "buf_in", "buf_out", "handle_out", "handle_array_out",
}
RET_TYPES = {
    "void", "int32", "uint32", "int64", "uint64", "float", "double",
    "handle", "string",
}

C_TYPE = {
    "int32": "int32_t", "uint32": "uint32_t", "int64": "int64_t",
    "uint64": "uint64_t", "float": "float", "double": "double",
    "handle": "void *", "string": "const char *", "buf_in": "const void *",
    "buf_out": "void *", "handle_out": "void *",
    "handle_array_out": "void *",
}

PAYLOAD_TYPES = {"string", "buf_in", "buf_out", "handle_out",
                 "handle_array_out"}


def die(msg):
    sys.stderr.write("gen_thunks: %s\n" % msg)
    sys.exit(1)


def load_defs(paths):
    apis = []
    for p in paths:
        with open(p) as f:
            api = json.load(f)
        for key in ("api", "base", "functions"):
            if key not in api:
                die("%s: missing '%s'" % (p, key))
        for fn in api["functions"]:
            if fn.get("ret", "void") not in RET_TYPES:
                die("%s: %s: bad ret type" % (p, fn.get("name")))
            names = set()
            for a in fn.get("args", []):
                if a.get("type") not in ARG_TYPES:
                    die("%s: %s: bad arg type %r" % (p, fn["name"], a))
                if a["name"] in names:
                    die("%s: %s: duplicate arg %s" % (p, fn["name"], a["name"]))
                names.add(a["name"])
                if a["type"] in ("buf_in", "buf_out"):
                    if "len_fixed" not in a and "len_arg" not in a:
                        die("%s: %s: %s needs len_fixed/len_arg" %
                            (p, fn["name"], a["name"]))
                if a["type"] == "handle_array_out" and "count_fixed" not in a:
                    die("%s: %s: handle_array_out needs count_fixed" %
                        (p, fn["name"]))
                if a["type"] == "handle_array_out" and "count_from" in a:
                    names_later = [x["name"] for x in fn.get("args", [])]
                    if a["count_from"] not in names_later:
                        die("%s: %s: count_from %r unknown" %
                            (p, fn["name"], a["count_from"]))
            for a in fn.get("args", []):
                if "len_arg" in a and a["len_arg"] not in names:
                    die("%s: %s: len_arg %r unknown" %
                        (p, fn["name"], a["len_arg"]))
        apis.append(api)
    return apis


def op_name(api, fn):
    return "TGPU_OP_%s_%s" % (api["api"].upper(), fn["name"].upper())


def guest_ret_type(ret):
    special = {"void": "void", "string": "const char *", "handle": "void *"}
    if ret in special:
        return special[ret]
    return C_TYPE[ret]


def payload_len_expr_guest(a):
    """C expression for this arg's payload bytes, given stub parameter."""
    t = a["type"]
    if t == "handle_out":
        return "8u"
    if t == "handle_array_out":
        return "%du * 8u" % a["count_fixed"]
    if t == "string":
        return "(%s) ? (uint32_t)strlen(%s) + 1u : 0u" % (a["name"], a["name"])
    # buf_in / buf_out
    if "len_fixed" in a:
        base = "%du" % a["len_fixed"]
    else:
        scale = a.get("len_scale", 1)
        base = ("((int64_t)(%s) > 0 ? "
                "(uint32_t)((uint64_t)(%s) * %du) : 0u)"
                % (a["len_arg"], a["len_arg"], scale))
    return "((%s) ? (%s) : 0u)" % (a["name"], base)


def emit_guest_fn(f, api, fn):
    ret = fn.get("ret", "void")
    args = fn.get("args", [])
    cname = fn["name"]
    sig_args = ", ".join("%s %s" % (C_TYPE[a["type"]], a["name"])
                         for a in args) or "void"
    f.write("%s %s(%s)\n{\n" % (guest_ret_type(ret), cname, sig_args))
    n = len(args)
    f.write("\tuint64_t slots[%d];\n" % max(n, 1))
    if n == 0:
        f.write("\tslots[0] = 0;\n")
    f.write("\tstatic uint8_t payload[TGPU_STUB_PAYLOAD];\n")
    f.write("\tuint8_t outp[TGPU_STUB_PAYLOAD];\n")
    f.write("\tuint32_t poff = 0, outlen = 0;\n")
    f.write("\tint32_t status = 0;\n")
    f.write("\tint64_t rv;\n")
    for i, a in enumerate(args):
        if a["type"] in PAYLOAD_TYPES:
            f.write("\tuint32_t off%d = 0, l%d = 0;\n" % (i, i))
    f.write("\n")

    for i, a in enumerate(args):
        t, nm = a["type"], a["name"]
        if t in ("int32", "int64"):
            f.write("\tslots[%d] = (uint64_t)(int64_t)(%s);\n" % (i, nm))
        elif t in ("uint32", "uint64"):
            f.write("\tslots[%d] = (uint64_t)(%s);\n" % (i, nm))
        elif t == "float":
            f.write("\tslots[%d] = tg_f32_slot(%s);\n" % (i, nm))
        elif t == "double":
            f.write("\tslots[%d] = tg_f64_slot(%s);\n" % (i, nm))
        elif t == "handle":
            f.write("\tslots[%d] = (uint64_t)(uintptr_t)(%s);\n" % (i, nm))
        else:
            # Keep payload offsets 8-aligned: buffer payloads may carry
            # 8-byte elements (handles) on the host side.
            f.write("\tpoff = (poff + 7u) & ~7u;\n")
            f.write("\toff%d = poff;\n" % i)
            if t == "handle_array_out":
                cnt = a.get("count_from")
                if cnt:
                    f.write("\t{\n\t\tint32_t ne_in = (%s) ? "
                            "*(const int32_t *)(%s) : 0;\n"
                            % (cnt, cnt))
                    f.write("\t\tif (ne_in < 0) ne_in = 0;\n")
                    f.write("\t\tif (ne_in > %d) ne_in = %d;\n"
                            % (a["count_fixed"], a["count_fixed"]))
                    f.write("\t\tl%d = (uint32_t)ne_in * 8u;\n\t}\n" % i)
                else:
                    f.write("\tl%d = %du * 8u;\n" % (i, a["count_fixed"]))
            else:
                f.write("\tl%d = %s;\n" % (i, payload_len_expr_guest(a)))
            f.write("\tif (poff + l%d > TGPU_STUB_PAYLOAD) l%d = 0;\n" % (i, i))
            if t == "string":
                f.write("\tif (%s && l%d) memcpy(payload + off%d, %s, l%d);\n"
                        % (nm, i, i, nm, i))
            elif t == "buf_in":
                f.write("\tif (%s && l%d) memcpy(payload + off%d, %s, l%d);\n"
                        % (nm, i, i, nm, i))
            else:  # buf_out / handle_out / handle_array_out
                f.write("\tif (l%d) memset(payload + off%d, 0, l%d);\n"
                        % (i, i, i))
            f.write("\tslots[%d] = ((uint64_t)off%d << 32) | l%d;\n"
                    % (i, i, i))
            f.write("\tpoff += l%d;\n" % i)

    f.write("\trv = tg_call(%s, slots, %d, payload, poff, outp, "
            "sizeof(outp), &outlen, &status);\n" % (op_name(api, fn), n))
    if ret in ("int32", "int64"):
        # Integer returns propagate negative transport/dispatch status
        # codes (e.g. TGPU_ST_ENOTSUP for struct_conv functions).
        f.write("\tif (status < 0) return (%s)status;\n" % C_TYPE[ret])
    else:
        f.write("\t(void)status;\n")

    for i, a in enumerate(args):
        t, nm = a["type"], a["name"]
        if t == "buf_out":
            f.write("\tif (%s && l%d && outlen >= off%d + l%d) "
                    "memcpy(%s, outp + off%d, l%d);\n"
                    % (nm, i, i, i, nm, i, i))
        elif t == "handle_out":
            f.write("\tif (%s && outlen >= off%d + sizeof(void *)) "
                    "memcpy(%s, outp + off%d, sizeof(void *));\n"
                    % (nm, i, nm, i))
        elif t == "handle_array_out":
            f.write("\tif (%s) {\n" % nm)
            f.write("\t\tuint32_t e_, ne_ = l%d / 8u;\n" % i)
            f.write("\t\tfor (e_ = 0; e_ < ne_ && outlen >= off%d + "
                    "(e_ + 1u) * 8u; e_++) {\n" % i)
            f.write("\t\t\tvoid *hv_;\n")
            f.write("\t\t\tmemcpy(&hv_, outp + off%d + e_ * 8u, 8);\n" % i)
            f.write("\t\t\tmemcpy((uint8_t *)(%s) + e_ * sizeof(void *), "
                    "&hv_, sizeof(void *));\n" % nm)
            f.write("\t\t}\n\t}\n")

    if ret == "void":
        f.write("\t(void)rv;\n")
    elif ret == "string":
        f.write("\t{\n\t\tstatic char sbuf[TGU_STRING_RET_MAX];\n")
        f.write("\t\tuint32_t sl = (uint32_t)rv;\n")
        f.write("\t\tif (sl && outlen >= poff + sl) {\n")
        f.write("\t\t\tif (sl > sizeof(sbuf)) sl = sizeof(sbuf);\n")
        f.write("\t\t\tmemcpy(sbuf, outp + poff, sl);\n")
        f.write("\t\t\tsbuf[sizeof(sbuf) - 1] = 0;\n")
        f.write("\t\t\treturn sbuf;\n\t\t}\n\t\treturn NULL;\n\t}\n")
    elif ret == "handle":
        f.write("\treturn (void *)(uintptr_t)rv;\n")
    elif ret in ("float",):
        f.write("\treturn tg_slot_f32((uint64_t)rv);\n")
    elif ret in ("double",):
        f.write("\treturn tg_slot_f64((uint64_t)rv);\n")
    elif ret in ("int32", "int64"):
        f.write("\treturn (%s)rv;\n" % C_TYPE[ret])
    else:
        f.write("\treturn (%s)rv;\n" % C_TYPE[ret])
    f.write("}\n\n")


def host_call_expr(a, i):
    t = a["type"]
    if t in ("int32", "int64"):
        return "(%s)slots[%d]" % (C_TYPE[t], i)
    if t in ("uint32", "uint64"):
        return "(%s)slots[%d]" % (C_TYPE[t], i)
    if t == "float":
        return "tg_slot_f32(slots[%d])" % i
    if t == "double":
        return "tg_slot_f64(slots[%d])" % i
    if t == "handle":
        return "(void *)(uintptr_t)slots[%d]" % i
    if t == "string":
        return "(const char *)(payload + (slots[%d] >> 32))" % i
    # buf_in / buf_out / handle_out / handle_array_out
    return "(void *)(payload + (slots[%d] >> 32))" % i


def emit_host_fn(f, api, fn):
    ret = fn.get("ret", "void")
    args = fn.get("args", [])
    cname = fn["name"]
    name = "impl_%s_%s" % (api["api"], cname)
    f.write("static int %s(tgpu_host *h, const uint64_t *slots, "
            "uint32_t nslots, uint8_t *payload, uint32_t plen, "
            "uint32_t pcap, uint64_t *retp, uint32_t *out_plen)\n{\n"
            % name)
    f.write("\t(void)nslots;\n\t(void)plen;\n\t(void)slots;\n\t(void)payload;\n")
    f.write("\t*out_plen = plen;\n\t*retp = 0;\n")
    if ret != "string":
        f.write("\t(void)pcap;\n")
    if fn.get("struct_conv"):
        f.write("\t(void)h;\n\t(void)slots;\n\t(void)payload;\n\t(void)pcap;\n")
        f.write("\t/* struct_conv: needs 32<->64 struct conversion "
                "(roadmap) */\n\treturn TGPU_ST_ENOTSUP;\n}\n\n")
        return

    for i, a in enumerate(args):
        if a["type"] in PAYLOAD_TYPES:
            f.write("\tif ((slots[%d] >> 32) + (slots[%d] & 0xffffffffu) "
                    "> plen) return TGPU_ST_ERANGE;\n" % (i, i))

    host_types = ", ".join(
        ("const char *" if a["type"] == "string" else
         "const void *" if a["type"] == "buf_in" else
         "void *" if a["type"] in ("handle", "buf_out", "handle_out",
                                   "handle_array_out") else
         C_TYPE[a["type"]]) for a in args)
    special_ret = {"string": "const char *", "handle": "void *",
                   "void": "void"}
    rt = special_ret[ret] if ret in special_ret else C_TYPE[ret]
    f.write("\ttypedef %s (*fn_t)(%s);\n" % (rt, host_types or "void"))
    f.write("\tfn_t fn = (fn_t)tgpu_host_resolve(h, \"%s\");\n" % cname)
    f.write("\tif (!fn) return TGPU_ST_ENOSYM;\n")

    call_args = ", ".join(host_call_expr(a, i) for i, a in enumerate(args))
    call = "fn(%s)" % call_args

    if ret == "void":
        f.write("\t%s;\n" % call)
    elif ret == "string":
        f.write("\t{\n\t\tconst char *r = %s;\n" % call)
        f.write("\t\tif (r) {\n\t\t\tsize_t l = strlen(r) + 1;\n")
        f.write("\t\t\tif (plen + l <= pcap) {\n")
        f.write("\t\t\t\tmemcpy(payload + plen, r, l);\n")
        f.write("\t\t\t\t*retp = ((uint64_t)plen << 32) | (uint64_t)l;\n")
        f.write("\t\t\t\t*out_plen = plen + (uint32_t)l;\n")
        f.write("\t\t\t}\n\t\t}\n\t}\n")
    elif ret in ("float",):
        f.write("\t*retp = tg_f32_slot(%s);\n" % call)
    elif ret in ("double",):
        f.write("\t*retp = tg_f64_slot(%s);\n" % call)
    elif ret in ("int32", "int64"):
        f.write("\t*retp = (uint64_t)(int64_t)%s;\n" % call)
    else:  # uint32/uint64/handle
        f.write("\t*retp = (uint64_t)(uintptr_t)%s;\n" % call)
    f.write("\treturn TGPU_ST_OK;\n}\n\n")


def generate(apis, outdir):
    os.makedirs(outdir, exist_ok=True)
    hpath = os.path.join(outdir, "thunks_gen.h")
    gpath = os.path.join(outdir, "thunks_gen_guest.c")
    spath = os.path.join(outdir, "thunks_gen_host.c")

    ops = []
    for api in apis:
        for k, fn in enumerate(api["functions"]):
            ops.append((op_name(api, fn), api["base"] + k))

    with open(hpath, "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* Generated by gen_thunks.py — do not edit. */\n")
        f.write("#ifndef TRIANGO_THUNKS_GEN_H\n#define TRIANGO_THUNKS_GEN_H\n\n")
        f.write("#include \"triango/tg_runtime.h\"\n\n")
        f.write("#define TGPU_STUB_PAYLOAD 4096u\n")
        f.write("#define TGU_STRING_RET_MAX 512u\n\n")
        f.write("enum {\n")
        for name, val in ops:
            f.write("\t%s = %d,\n" % (name, val))
        f.write("};\n\n")
        for api in apis:
            f.write("/* ---- %s ---- */\n" % api["api"])
            for fn in api["functions"]:
                args = fn.get("args", [])
                sig = ", ".join("%s %s" % (C_TYPE[a["type"]], a["name"])
                                for a in args) or "void"
                f.write("%s %s(%s);\n"
                        % (guest_ret_type(fn.get("ret", "void")),
                           fn["name"], sig))
            f.write("\n")
        f.write("#endif /* TRIANGO_THUNKS_GEN_H */\n")

    with open(gpath, "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* Generated by gen_thunks.py — guest stubs. */\n")
        f.write("#include \"thunks_gen.h\"\n")
        f.write("#include <string.h>\n\n")
        for api in apis:
            f.write("/* ---- %s ---- */\n\n" % api["api"])
            for fn in api["functions"]:
                emit_guest_fn(f, api, fn)

    with open(spath, "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* Generated by gen_thunks.py — host dispatch. */\n")
        f.write("#include \"thunks_gen.h\"\n")
        f.write("#include <string.h>\n\n")
        for api in apis:
            f.write("/* ---- %s ---- */\n\n" % api["api"])
            for fn in api["functions"]:
                emit_host_fn(f, api, fn)
        f.write("const tgpu_host_fn_desc tg_thunk_table[] = {\n")
        for api in apis:
            for fn in api["functions"]:
                f.write("\t{ %s, \"%s\", impl_%s_%s },\n"
                        % (op_name(api, fn), fn["name"], api["api"],
                           fn["name"]))
        f.write("};\n\n")
        f.write("const uint32_t tg_thunk_table_len = "
                "sizeof(tg_thunk_table) / sizeof(tg_thunk_table[0]);\n")

    print("gen_thunks: %d functions -> %s" % (len(ops), outdir))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("json", nargs="+", help="thunk definition files")
    ap.add_argument("-o", "--outdir", default="build/gen")
    args = ap.parse_args()
    generate(load_defs(args.json), args.outdir)


if __name__ == "__main__":
    main()
