/* SPDX-License-Identifier: MIT */
#include "triango/tg_runtime.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define TG_MAX_FN 256u

/* ============================ guest side ============================ */

static tgpu_region *g_region;
static uint32_t g_seq;
static tg_inline_server_fn g_inline;

int tg_guest_attach(void *region)
{
	g_region = tgpu_region_get(region);
	return g_region ? 0 : -1;
}

void *tg_guest_region(void)
{
	return g_region;
}

uint32_t tg_guest_next_seq(void)
{
	return ++g_seq;
}

void tg_set_inline_server(tg_inline_server_fn fn)
{
	g_inline = fn;
}

int64_t tg_call(uint16_t op, const uint64_t *slots, uint32_t nslots,
		const void *payload, uint32_t plen, void *out_payload,
		uint32_t outcap, uint32_t *outlen, int32_t *status)
{
	uint8_t req[TGPU_MSG_MAX];
	uint8_t resp[TGPU_MSG_MAX];
	tgpu_msg_hdr *h = (tgpu_msg_hdr *)req;
	tgpu_msg_hdr *rh;
	uint32_t total = tgpu_msg_total(nslots, plen);
	uint32_t seq, rlen = 0;
	int rc;

	if (!g_region || total > TGPU_MSG_MAX || nslots > 256) {
		if (status)
			*status = TGPU_ST_EINVAL;
		return -1;
	}

	memset(h, 0, sizeof(*h));
	h->len = total;
	h->op = op;
	h->flags = (uint16_t)nslots;
	seq = h->seq = tg_guest_next_seq();
	h->status = 0;
	if (nslots)
		memcpy(req + sizeof(*h), slots, 8u * nslots);
	if (plen)
		memcpy(req + sizeof(*h) + 8u * nslots, payload, plen);

	rc = tgpu_ring_send(&g_region->req, (uint8_t *)g_region +
			    g_region->hdr.req_off, g_region->hdr.req_size,
			    req, total);
	if (rc != 0) {
		if (status)
			*status = TGPU_ST_EAGAIN;
		return -1;
	}

	if (g_inline) {
		/* Single-process test hook: serve this request right away. */
		if (g_inline(g_region) != 0) {
			if (status)
				*status = TGPU_ST_EINVAL;
			return -1;
		}
	}

	for (;;) {
		rc = tgpu_ring_recv(&g_region->resp,
				    (uint8_t *)g_region + g_region->hdr.resp_off,
				    g_region->hdr.resp_size, resp, sizeof(resp),
				    &rlen);
		if (rc == 1) {
			if (status)
				*status = TGPU_ST_EAGAIN;
			return -1;
		}
		if (rc != 0 || rlen < sizeof(tgpu_msg_hdr)) {
			if (status)
				*status = TGPU_ST_EINVAL;
			return -1;
		}
		rh = (tgpu_msg_hdr *)resp;
		if (rh->seq != seq) {
			/* v0 channel is in-order; a mismatch is fatal. */
			if (status)
				*status = TGPU_ST_EINVAL;
			return -1;
		}
		break;
	}

	if (status)
		*status = rh->status;
	{
		uint32_t rpayload = rlen - sizeof(*rh) - 8u;

		if (out_payload && rpayload) {
			uint32_t n = rpayload < outcap ? rpayload : outcap;

			memcpy(out_payload, resp + sizeof(*rh) + 8u, n);
			if (outlen)
				*outlen = n;
		} else if (outlen) {
			*outlen = 0;
		}
	}
	{
		int64_t retv;

		memcpy(&retv, resp + sizeof(*rh), 8);
		return retv;
	}
}

/* ============================ host side ============================ */

void tgpu_host_init(tgpu_host *h, const tgpu_host_fn_desc *tab, uint32_t n)
{
	memset(h, 0, sizeof(*h));
	h->tab = tab;
	h->ntab = n;
}

void tgpu_host_set_lib(tgpu_host *h, int idx, void *dlh)
{
	if (idx >= 0 && idx < 4)
		h->dl[idx] = dlh;
}

static const tgpu_host_fn_desc *host_find(tgpu_host *h, uint16_t op)
{
	uint32_t i;

	for (i = 0; i < h->ntab; i++)
		if (h->tab[i].op == op)
			return &h->tab[i];
	return NULL;
}

void *tgpu_host_resolve(tgpu_host *h, const char *sym)
{
	uint32_t i, n = h->ntab < TG_MAX_FN ? h->ntab : TG_MAX_FN;
	const tgpu_host_fn_desc *d;

	/* Override (tests) or cache lookup by descriptor index. */
	for (i = 0; i < n; i++) {
		d = &h->tab[i];
		if (d->sym && strcmp(d->sym, sym) == 0) {
			if (h->fn_override[i])
				return h->fn_override[i];
			if (h->fn_cache[i])
				return h->fn_cache[i];
			break;
		}
	}
#ifdef __ANDROID__
	{
		void *fn = NULL;
		int k;

		for (k = 0; k < 4 && !fn; k++)
			if (h->dl[k])
				fn = dlsym(h->dl[k], sym);
		if (fn && i < n)
			h->fn_cache[i] = fn;
		return fn;
	}
#else
	/* Host builds use tgpu_host_set_fn() (no driver libraries linked). */
	(void)d;
	return NULL;
#endif
}

void tgpu_host_set_fn(tgpu_host *h, uint16_t op, void *fn)
{
	uint32_t i, n = h->ntab < TG_MAX_FN ? h->ntab : TG_MAX_FN;

	for (i = 0; i < n; i++)
		if (h->tab[i].op == op) {
			h->fn_override[i] = fn;
			return;
		}
}

uint32_t tgpu_host_dispatch(tgpu_host *h, const void *req, uint32_t reqlen,
			    void *resp, uint32_t respcap)
{
	const tgpu_msg_hdr *rq = (const tgpu_msg_hdr *)req;
	tgpu_msg_hdr *rs = (tgpu_msg_hdr *)resp;
	const tgpu_host_fn_desc *d;
	uint32_t nslots, plen;
	const uint64_t *slots;
	uint8_t *payload;
	uint64_t ret = 0;
	int st = TGPU_ST_OK;

	if (reqlen < sizeof(*rq) || reqlen != rq->len ||
	    reqlen > TGPU_MSG_MAX || respcap < reqlen + 8u)
		return 0;

	nslots = rq->flags;
	if (sizeof(*rq) + 8u * nslots > reqlen)
		return 0;
	plen = reqlen - (uint32_t)sizeof(*rq) - 8u * nslots;
	slots = (const uint64_t *)(rq + 1);
	payload = (uint8_t *)(rq + 1) + 8u * nslots;

	d = host_find(h, rq->op);
	if (!d || !d->impl) {
		st = TGPU_ST_ENOSYS;
	} else {
		uint32_t out_plen = plen;
		uint32_t room = TGPU_MSG_MAX -
				(uint32_t)((uint8_t *)payload - (uint8_t *)rq);

		st = d->impl(h, slots, nslots, payload, plen, room, &ret,
			     &out_plen);
		if (out_plen > plen)
			plen = out_plen;
	}

	memset(rs, 0, sizeof(*rs));
	rs->len = (uint32_t)sizeof(*rs) + 8u + plen;
	rs->op = rq->op;
	rs->flags = rq->flags;
	rs->seq = rq->seq;
	rs->status = st;
	memcpy(rs + 1, &ret, 8);
	/* Return the (possibly mutated) payload for buffer outputs. */
	if (plen)
		memcpy((uint8_t *)(rs + 1) + 8, payload, plen);
	return rs->len;
}

int tgpu_host_serve(tgpu_host *h, void *region, int max_msgs)
{
	tgpu_region *r = tgpu_region_get(region);
	uint8_t req[TGPU_MSG_MAX];
	uint8_t resp[TGPU_MSG_MAX];
	int served = 0;

	if (!r)
		return -1;
	while (max_msgs <= 0 || served < max_msgs) {
		uint32_t rlen = 0, slen;
		int rc = tgpu_ring_recv(&r->req,
					(uint8_t *)r + r->hdr.req_off,
					r->hdr.req_size, req, sizeof(req),
					&rlen);

		if (rc == 1)
			break;
		if (rc != 0)
			break;
		slen = tgpu_host_dispatch(h, req, rlen, resp, sizeof(resp));
		if (!slen)
			break;
		if (tgpu_ring_send(&r->resp,
				   (uint8_t *)r + r->hdr.resp_off,
				   r->hdr.resp_size, resp, slen) != 0)
			break;
		served++;
	}
	return served;
}
