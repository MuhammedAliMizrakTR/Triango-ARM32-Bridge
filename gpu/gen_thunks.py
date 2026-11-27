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
  scalars     int32 uint32 int64 uint32 float double  -> one u64 slot each
  handle      opaque driver token (EGLDisplay, VkInstance, ...); guest side
              sees a 32-bit id (token table maps ids <-> 64-bit handles)
  string      const char*  -> payload copy (NUL included), slot = off<<32|len
  string_array const char *const * + count_arg -> [u32 count][count*u32 len]
              [blobs with NUL]; host rebuilds the pointer array
  buf_in      const void* + length (len_fixed / len_arg x len_scale / len_expr)
  buf_out     void* + length; mutated in place, copied back on response
  handle_out        void* out-slot for one handle (host writes u64)
  handle_array_out  array of handles (count_fixed max; capacity may come from
                    count_from: a scalar arg or an in/out int32 buffer)

A NULL buffer argument marshals to length 0 (and decodes to NULL on the
host).  A non-NULL *out* argument whose data does not fit the stub payload
fails fast with TGPU_ST_ERANGE.

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
    "string", "string_array", "buf_in", "buf_out", "handle_out",
    "handle_array_out",
}
RET_TYPES = {
    "void", "int32", "uint32", "int64", "uint64", "float", "double",
    "handle", "string",
}

C_TYPE = {
    "int32": "int32_t", "uint32": "uint32_t", "int64": "int64_t",
    "uint64": "uint64_t", "float": "float", "double": "double",
    "handle": "void *", "string": "const char *",
    "string_array": "const char *const *",
    "buf_in": "const void *", "buf_out": "void *", "handle_out": "void *",
    "handle_array_out": "void *",
}

PAYLOAD_TYPES = {"string", "string_array", "buf_in", "buf_out", "handle_out",
                 "handle_array_out"}
BUF_TYPES = {"buf_in", "buf_out", "handle_out", "handle_array_out"}


def die(msg):
    sys.stderr.write("gen_thunks: %s\n" % msg)
    sys.exit(1)


def load_defs(paths):
    apis = []
    for p in paths:
        with open(p) as f:
            api = json.load(f)
        for k in ("api", "base", "functions"):
            if k not in api:
                die("%s: missing %r" % (p, k))
        names = set()
        for fn in api["functions"]:
            if "name" not in fn:
                die("%s: function without name" % p)
            if fn["name"] in names:
                die("%s: duplicate function %s" % (p, fn["name"]))
            names.add(fn["name"])
            ret = fn.get("ret", "void")
            if ret not in RET_TYPES:
                die("%s: %s: bad ret %r" % (p, fn["name"], ret))
            argnames = set()
            for a in fn.get("args", []):
                if a.get("type") not in ARG_TYPES:
                    die("%s: %s: bad arg type %r" % (p, fn["name"],
                                                     a.get("type")))
                if a["name"] in argnames:
                    die("%s: %s: duplicate arg %s" % (p, fn["name"],
                                                      a["name"]))
                argnames.add(a["name"])
            for a in fn.get("args", []):
                if (a["type"] == "handle_array_out" and
                        "count_fixed" not in a):
                    die("%s: %s: handle_array_out needs count_fixed" %
                        (p, fn["name"]))
                if (a["type"] == "handle_array_out" and
                        a.get("elem", "u64") not in ("u32", "u64")):
                    die("%s: %s: handle_array_out elem must be u32/u64" %
                        (p, fn["name"]))
                if (a["type"] == "handle_array_out" and
                        a.get("count_from") not in (None,) and
                        a["count_from"] not in argnames):
                    die("%s: %s: count_from %r unknown" %
                        (p, fn["name"], a["count_from"]))
                if a["type"] == "string_array" and \
                        a.get("count_arg") not in argnames:
                    die("%s: %s: string_array needs count_arg" %
                        (p, fn["name"]))
                if a["type"] == "string_array" and "len_arg" in a and \
                        a["len_arg"] not in argnames:
                    die("%s: %s: len_arg %r unknown" %
                        (p, fn["name"], a["len_arg"]))
                if a["type"] == "buf_in" or a["type"] == "buf_out":
                    if not any(k in a for k in
                               ("len_fixed", "len_arg", "len_expr")):
                        die("%s: %s: %s needs len_fixed/len_arg/len_expr" %
                            (p, fn["name"], a["name"]))
        api["functions"] = list(api["functions"])
        apis.append(api)
    return apis


def op_name(api, fn):
    return "TGPU_OP_%s_%s" % (api["api"].upper(), fn["name"].upper())


def guest_ret_type(ret):
    special = {"void": "void", "string": "const char *", "handle": "void *"}
    if ret in special:
        return special[ret]
    return C_TYPE[ret]


def ret_c_expr(ret):
    special = {"string": "const char *", "handle": "void *", "void": "void"}
    if ret in special:
        return special[ret]
    return C_TYPE[ret]


def payload_len_expr_guest(a):
    t = a["type"]
    nm = a["name"]
    if "len_expr" in a:
        return "((%s) ? (uint32_t)(%s) : 0u)" % (nm, a["len_expr"])
    if t == "string":
        return "((%s) ? (uint32_t)strlen(%s) + 1u : 0u)" % (nm, nm)
    if t in ("buf_in", "buf_out", "handle_out"):
        if "len_fixed" in a:
            base = "%du" % a["len_fixed"]
        elif "len_arg" in a:
            base = "(uint32_t)(%s) * %du" % (a["len_arg"],
                                             a.get("len_scale", 1))
        else:
            die("buffer arg %s needs len_fixed/len_arg/len_expr" % nm)
        return "((%s) ? %s : 0u)" % (nm, base)
    if t == "handle_array_out":
        return "unused"  # handled with statements
    die("no len expr for %s" % t)


def handle_meta(fn):
    """(handle_args_mask, handle_out_mask, handle_arr_mask, ret_is_handle)

    handle_arr_mask marks only 8-byte-element arrays (EGL/Vulkan tokens);
    4-byte GL object names pass through unmapped.
    """
    ha = ho = har = 0
    for i, a in enumerate(fn.get("args", [])):
        if a["type"] == "handle":
            ha |= 1 << i
        elif a["type"] == "handle_out":
            ho |= 1 << i
        elif (a["type"] == "handle_array_out" and
              a.get("elem", "u64") == "u64"):
            har |= 1 << i
    rh = 1 if fn.get("ret", "void") == "handle" else 0
    return ha, ho, har, rh


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
    f.write("\tuint8_t payload[TGPU_STUB_PAYLOAD] = { 0 };\n")
    f.write("\tuint8_t outp[TGPU_STUB_PAYLOAD];\n")
    f.write("\tuint32_t poff = 0, outlen = 0;\n")
    f.write("\tint32_t status = 0;\n")
    f.write("\tint64_t rv;\n")
    for i, a in enumerate(args):
        if a["type"] in PAYLOAD_TYPES:
            f.write("\tuint32_t off%d = 0, l%d = 0;\n" % (i, i))
    if ret != "void":
        f.write("\t(void)status;\n")

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
            # Payload argument: keep offsets 8-aligned for handle arrays.
            f.write("\tpoff = (poff + 7u) & ~7u;\n")
            f.write("\toff%d = poff;\n" % i)
            if t == "handle_array_out":
                esz = 4 if a.get("elem", "u64") == "u32" else 8
                f.write("\t{\n")
                f.write("\t\tuint32_t ne_in;\n")
                cnt = a.get("count_from")
                if cnt:
                    cta = next((x for x in args if x["name"] == cnt), None)
                    if cta and cta["type"] in ("int32", "uint32", "int64",
                                               "uint64"):
                        f.write("\t\tne_in = (uint32_t)(%s);\n" % cnt)
                    else:
                        f.write("\t\tne_in = (%s) ? (uint32_t)"
                                "(*(const int32_t *)(%s)) : 0u;\n"
                                % (cnt, cnt))
                else:
                    f.write("\t\tne_in = %du;\n" % a["count_fixed"])
                f.write("\t\tif (!(%s)) ne_in = 0;\n" % nm)
                f.write("\t\tif (ne_in > %du) ne_in = %du;\n"
                        % (a["count_fixed"], a["count_fixed"]))
                f.write("\t\tl%d = ne_in * %du;\n" % (i, esz))
                f.write("\t}\n")
            elif t == "string_array":
                f.write("\tl%d = tg_pack_strings(NULL, 0, %s, %s, "
                        "(uint32_t)(%s));\n"
                        % (i, nm, a.get("len_arg", "NULL"),
                           a["count_arg"]))
            else:
                f.write("\tl%d = %s;\n" % (i, payload_len_expr_guest(a)))
            f.write("\tif (poff + l%d > TGPU_STUB_PAYLOAD) l%d = 0;\n"
                    % (i, i))
            if t in ("buf_out", "handle_out", "handle_array_out",
                     "string_array"):
                # Never let the driver write through a NULL out pointer or
                # an unmarshalled string array.
                f.write("\tif ((%s) && !l%d) return %s;\n"
                        % (nm, i, "(%s)TGPU_ST_ERANGE" % C_TYPE[ret]
                           if ret != "void" else ""))
            if t == "string_array":
                f.write("\ttg_pack_strings(payload + off%d, l%d, %s, %s, "
                        "(uint32_t)(%s));\n"
                        % (i, i, nm, a.get("len_arg", "NULL"),
                           a["count_arg"]))
            elif t == "string":
                f.write("\tif (l%d) memcpy(payload + off%d, %s, l%d);\n"
                        % (i, i, nm, i))
            else:
                f.write("\tif (l%d) memcpy(payload + off%d, %s, l%d);\n"
                        % (i, i, nm, i))
            f.write("\tpoff += l%d;\n" % i)
            f.write("\tslots[%d] = ((uint64_t)off%d << 32) | l%d;\n"
                    % (i, i, i))

    f.write("\trv = tg_call(%s, slots, %d, payload, poff, outp, "
            "sizeof(outp), &outlen, &status);\n" % (op_name(api, fn), n))
    if ret in ("int32", "int64"):
        f.write("\tif (status < 0) return (%s)status;\n" % C_TYPE[ret])
    elif ret != "void":
        f.write("\tif (rv < 0) return (%s)0;\n" % ret_c_expr(ret))

    for i, a in enumerate(args):
        t = a["type"]
        if t in ("buf_out", "handle_out"):
            f.write("\tif (l%d && %s) memcpy(%s, outp + off%d, l%d);\n"
                    % (i, a["name"], a["name"], i, i))
        elif t == "handle_array_out":
            if a.get("elem", "u64") == "u32":
                # GL object names: identical u32 layout both sides.
                f.write("\tif (l%d && %s) memcpy(%s, outp + off%d, l%d);\n"
                        % (i, a["name"], a["name"], i, i))
            else:
                f.write("\tif (%s) {\n" % a["name"])
                f.write("\t\tuint32_t e;\n")
                f.write("\t\tfor (e = 0; e < l%d / 8u; e++) {\n" % i)
                f.write("\t\t\tuint64_t hv_;\n")
                f.write("\t\t\tmemcpy(&hv_, outp + off%d + 8u * e, 8);\n" % i)
                f.write("\t\t\tmemcpy((uint8_t *)(%s) + e * sizeof(void *), "
                        "&hv_, sizeof(void *));\n" % a["name"])
                f.write("\t\t}\n\t}\n")

    if ret == "void":
        f.write("\t(void)rv;\n")
    elif ret == "string":
        f.write("\tif (status < 0 || !rv) return NULL;\n")
        f.write("\t{\n\t\tuint32_t soff = (uint32_t)((uint64_t)rv >> 32);\n")
        f.write("\t\tuint32_t slen = (uint32_t)(uint64_t)rv;\n")
        f.write("\t\tif (soff + slen > outlen || !slen) return NULL;\n")
        f.write("\t\tif (slen > TGU_STRING_RET_MAX) "
                "slen = TGU_STRING_RET_MAX;\n")
        f.write("\t\tmemcpy(g_tgu_str_ret, outp + soff, slen);\n")
        f.write("\t\tg_tgu_str_ret[TGU_STRING_RET_MAX - 1] = 0;\n")
        f.write("\t\treturn g_tgu_str_ret;\n\t}\n")
    elif ret == "handle":
        f.write("\treturn (void *)(uintptr_t)(uint64_t)rv;\n")
    elif ret == "float":
        f.write("\treturn tg_slot_f32((uint64_t)rv);\n")
    elif ret == "double":
        f.write("\treturn tg_slot_f64((uint64_t)rv);\n")
    else:
        f.write("\treturn (%s)rv;\n" % C_TYPE[ret])
    f.write("}\n\n")


def host_call_expr(a, i, unpacked):
    t = a["type"]
    nm = a["name"]
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
        return ("((slots[%d] & 0xffffffffu) ? (const char *)(payload + "
                "(slots[%d] >> 32)) : NULL)" % (i, i))
    if t == "string_array":
        return "(const char *const *)%s" % unpacked[i]
    # buf_in / buf_out / handle_out / handle_array_out: len 0 decodes NULL
    return ("((slots[%d] & 0xffffffffu) ? (void *)(payload + "
            "(slots[%d] >> 32)) : NULL)" % (i, i))


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

    unpacked = {}
    for i, a in enumerate(args):
        if a["type"] == "string_array":
            v = "sa%d" % i
            unpacked[i] = v
            f.write("\tconst char *%s[TGPU_STRARR_MAX];\n" % v)
            f.write("\tuint32_t ca%d = tg_unpack_strings(payload + "
                    "(slots[%d] >> 32), (uint32_t)slots[%d], %s, "
                    "TGPU_STRARR_MAX);\n" % (i, i, i, v))
            f.write("\tif (!ca%d && (slots[%d] & 0xffffffffu) > 4u) "
                    "return TGPU_ST_ERANGE;\n" % (i, i))

    host_types = ", ".join(
        ("const char *const *" if a["type"] == "string_array" else
         "const char *" if a["type"] == "string" else
         "const void *" if a["type"] == "buf_in" else
         "void *" if a["type"] in ("handle", "buf_out", "handle_out",
                                   "handle_array_out") else
         C_TYPE[a["type"]]) for a in args)
    rt = ret_c_expr(ret)
    f.write("\ttypedef %s (*fn_t)(%s);\n" % (rt, host_types or "void"))
    f.write("\tfn_t fn = (fn_t)tgpu_host_resolve(h, \"%s\");\n" % cname)
    f.write("\tif (!fn) return TGPU_ST_ENOSYM;\n")

    call_args = ", ".join(host_call_expr(a, i, unpacked)
                          for i, a in enumerate(args))
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
        f.write("/* TLS buffer backing string returns (glGetString etc.). */\n")
        f.write("static __thread char g_tgu_str_ret[TGU_STRING_RET_MAX];\n\n")
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
                ha, ho, har, rh = handle_meta(fn)
                f.write("\t{ %s, \"%s\", impl_%s_%s, 0x%xu, 0x%xu, 0x%xu, "
                        "%d },\n"
                        % (op_name(api, fn), fn["name"], api["api"],
                           fn["name"], ha, ho, har, rh))
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
