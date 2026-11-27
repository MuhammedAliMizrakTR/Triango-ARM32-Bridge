/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_TG_RUNTIME_H
#define TRIANGO_TG_RUNTIME_H

/*
 * Guest/host runtime glue for generated GPU thunks (see gen_thunks.py).
 *
 * Guest side (arm32 shim / host tests):
 *   tg_guest_attach(region)     — bind the shared channel
 *   tg_set_inline_server(fn)    — single-process test hook (serves inline)
 *   tg_call(op, slots, ...)     — synchronous request/response, used by the
 *                                 generated guest stubs
 *
 * Host side (triango-gpud / host tests):
 *   tgpu_host_init(...), tgpu_host_set_fn(...)  — driver dispatch registry
 *   tgpu_host_serve(h, region, max)             — process queued requests
 */

#include "triango/gpu_proto.h"

#include <stdint.h>
#include <string.h>

/* ---- guest ---- */

int tg_guest_attach(void *region);
void *tg_guest_region(void);
uint32_t tg_guest_next_seq(void);

typedef int (*tg_inline_server_fn)(void *region);
void tg_set_inline_server(tg_inline_server_fn fn);

/*
 * Synchronous call. Slots carry scalars/handles; payload carries copied
 * strings/buffers. On success returns the function return value (u64) and
 * *status = TGPU_ST_*; out_payload receives the (possibly mutated) payload
 * for buffer outputs. Returns -1 on transport failure.
 */
int64_t tg_call(uint16_t op, const uint64_t *slots, uint32_t nslots,
		const void *payload, uint32_t plen, void *out_payload,
		uint32_t outcap, uint32_t *outlen, int32_t *status);

/* ---- host ---- */

typedef struct tgpu_host tgpu_host;

/*
 * Host impl signature: decode slots+payload, call the real driver function,
 * write the return value into *retp. Buffer outputs are mutated in place;
 * string returns are appended at payload[plen] and *out_plen is updated to
 * plen + strlen + 1 (default *out_plen = plen). Return TGPU_ST_*.
 */
typedef int (*tgpu_host_impl)(tgpu_host *h, const uint64_t *slots,
			      uint32_t nslots, uint8_t *payload, uint32_t plen,
			      uint32_t pcap, uint64_t *retp, uint32_t *out_plen);

typedef struct {
	uint16_t op;
	const char *sym; /* driver symbol to resolve lazily */
	tgpu_host_impl impl;
} tgpu_host_fn_desc;

struct tgpu_host {
	const tgpu_host_fn_desc *tab;
	uint32_t ntab;
	void *fn_cache[256];
	void *fn_override[256];
	void *dl[4]; /* dlopen handles: egl, gles2, vulkan, spare */
};

void tgpu_host_init(tgpu_host *h, const tgpu_host_fn_desc *tab, uint32_t n);
void *tgpu_host_resolve(tgpu_host *h, const char *sym);
void tgpu_host_set_fn(tgpu_host *h, uint16_t op, void *fn);
void tgpu_host_set_lib(tgpu_host *h, int idx, void *dlh);

/* Process up to max_msgs (0 = until empty). Returns number served. */
int tgpu_host_serve(tgpu_host *h, void *region, int max_msgs);

/* Dispatch a single request message into a response message.
 * resp must have room for TGPU_MSG_MAX. Returns response length. */
uint32_t tgpu_host_dispatch(tgpu_host *h, const void *req, uint32_t reqlen,
			    void *resp, uint32_t respcap);

/* Generated thunk table (thunks_gen_host.c). */
extern const tgpu_host_fn_desc tg_thunk_table[];
extern const uint32_t tg_thunk_table_len;

static inline float tg_slot_f32(uint64_t u)
{
	float f;
	uint32_t x = (uint32_t)u;

	memcpy(&f, &x, 4);
	return f;
}

static inline double tg_slot_f64(uint64_t u)
{
	double d;

	memcpy(&d, &u, 8);
	return d;
}

static inline uint64_t tg_f32_slot(float f)
{
	uint32_t x;

	memcpy(&x, &f, 4);
	return x;
}

static inline uint64_t tg_f64_slot(double d)
{
	uint64_t u;

	memcpy(&u, &d, 8);
	return u;
}

#endif /* TRIANGO_TG_RUNTIME_H */
