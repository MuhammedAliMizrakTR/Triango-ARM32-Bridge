/* SPDX-License-Identifier: MIT */
#include "triango/gpu_proto.h"

#include <string.h>

/*
 * Byte rings with free-running counters. `size` must be a multiple of 8 and
 * >= TGPU_RING_MIN; used bytes = head - tail (unsigned wrap-safe as long as
 * used <= size <= 2^31).
 */

static uint32_t rec_align(uint32_t len)
{
	return (len + 7u) & ~7u;
}

/* Copy n bytes to the ring at free-running position pos. */
static void ring_copy_in(uint8_t *data, uint32_t size, uint32_t pos,
			 const void *src, uint32_t n)
{
	uint32_t off = pos % size;
	uint32_t first = size - off;

	if (first > n)
		first = n;
	memcpy(data + off, src, first);
	if (n > first)
		memcpy(data, (const uint8_t *)src + first, n - first);
}

static void ring_copy_out(const uint8_t *data, uint32_t size, uint32_t pos,
			  void *dst, uint32_t n)
{
	uint32_t off = pos % size;
	uint32_t first = size - off;

	if (first > n)
		first = n;
	memcpy(dst, data + off, first);
	if (n > first)
		memcpy((uint8_t *)dst + first, data, n - first);
}

uint32_t tgpu_ring_used(const tgpu_ring_ctl *ctl, uint32_t size)
{
	uint32_t head = __atomic_load_n(&ctl->head, __ATOMIC_ACQUIRE);
	uint32_t tail = __atomic_load_n(&ctl->tail, __ATOMIC_ACQUIRE);

	(void)size;
	return head - tail;
}

uint32_t tgpu_ring_free(const tgpu_ring_ctl *ctl, uint32_t size)
{
	return size - tgpu_ring_used(ctl, size);
}

int tgpu_ring_send(tgpu_ring_ctl *ctl, uint8_t *data, uint32_t size,
		   const void *msg, uint32_t len)
{
	uint32_t head, tail, free_bytes, rec;

	if (!ctl || !data || !msg || !size || (size % 8u) != 0)
		return -1;
	if (len == 0 || len > size / 2u)
		return -1;

	rec = rec_align(4u + len);
	if (rec > size / 2u)
		return -1;

	head = __atomic_load_n(&ctl->head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&ctl->tail, __ATOMIC_ACQUIRE);
	free_bytes = size - (head - tail);
	if (rec > free_bytes)
		return 1; /* EAGAIN */

	/* Frame: [u32 msg_len][message][pad to 8]. msg_len excludes the
	 * length field and padding; rec is what the ring head advances by. */
	ring_copy_in(data, size, head, &len, 4);
	ring_copy_in(data, size, head + 4, msg, len);
	if (rec > 4 + len) {
		uint8_t zpad[7] = { 0 };

		ring_copy_in(data, size, head + 4 + len, zpad,
			     rec - 4 - len);
	}
	__atomic_store_n(&ctl->head, head + rec, __ATOMIC_RELEASE);
	return 0;
}

int tgpu_ring_recv(tgpu_ring_ctl *ctl, uint8_t *data, uint32_t size,
		   void *out, uint32_t cap, uint32_t *out_len)
{
	uint32_t head, tail, used, rec, msg_len;

	if (!ctl || !data || !out || !size || (size % 8u) != 0)
		return -1;

	head = __atomic_load_n(&ctl->head, __ATOMIC_ACQUIRE);
	tail = __atomic_load_n(&ctl->tail, __ATOMIC_RELAXED);
	used = head - tail;
	if (used == 0)
		return 1; /* EAGAIN */

	ring_copy_out(data, size, tail, &msg_len, 4);
	if (msg_len == 0 || msg_len > size / 2u)
		return -1;
	rec = rec_align(4u + msg_len);
	if (rec > used)
		return -1;
	if (msg_len > cap)
		return -1;

	ring_copy_out(data, size, tail + 4, out, msg_len);
	__atomic_store_n(&ctl->tail, tail + rec, __ATOMIC_RELEASE);
	if (out_len)
		*out_len = msg_len;
	return 0;
}

int tgpu_region_init(void *base, size_t size)
{
	tgpu_region *r = (tgpu_region *)base;
	uint32_t u32size = (uint32_t)size;
	uint32_t hdr_bytes, ring_size;

	if (!base || size != (size_t)u32size)
		return -1;
	hdr_bytes = (uint32_t)sizeof(tgpu_region);
	if (u32size < hdr_bytes + 2u * TGPU_RING_MIN)
		return -1;
	/* Split the remainder into two equal power-of-two-free rings. */
	ring_size = (u32size - hdr_bytes) / 2u;
	ring_size &= ~7u;
	if (ring_size < TGPU_RING_MIN)
		return -1;

	memset(base, 0, hdr_bytes);
	r->hdr.magic = TGPU_MAGIC;
	r->hdr.version = TGPU_VERSION;
	r->hdr.region_size = u32size;
	r->hdr.req_off = hdr_bytes;
	r->hdr.req_size = ring_size;
	r->hdr.resp_off = hdr_bytes + ring_size;
	r->hdr.resp_size = ring_size;
	return 0;
}

tgpu_region *tgpu_region_get(void *base)
{
	tgpu_region *r = (tgpu_region *)base;

	if (!base || r->hdr.magic != TGPU_MAGIC || r->hdr.version != TGPU_VERSION)
		return NULL;
	return r;
}

uint32_t tgpu_msg_total(uint32_t nslots, uint32_t payload_len)
{
	return (uint32_t)sizeof(tgpu_msg_hdr) + 8u * nslots + payload_len;
}

tgpu_msg_hdr *tgpu_msg_at(void *msg)
{
	return (tgpu_msg_hdr *)msg;
}
