/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_GPU_PROTO_H
#define TRIANGO_GPU_PROTO_H

/*
 * Triango GPU thunking — shared-memory command channel protocol.
 *
 * Two single-producer/single-consumer byte rings live in a shared region:
 * the guest (arm32 side, via bridge/JIT) writes requests, triango-gpud
 * (arm64, holding the real driver) answers on the response ring.
 *
 * Region layout:
 *   [tgpu_region_hdr][req ctl][resp ctl][req ring bytes][resp ring bytes]
 *
 * Record framing (both rings): each record is
 *   [u32 rec_len][message bytes][pad to 8]
 * rec_len includes the 4-byte length field itself. Ring head/tail are free-
 * running byte counters (wrap-safe with unsigned arithmetic); records may
 * wrap the physical end of the ring (copy handled internally).
 *
 * Message:
 *   [tgpu_msg_hdr][nslots x u64][payload bytes]
 * nslots is carried in hdr->flags. Scalars/handles occupy one u64 slot each;
 * strings and buffers are described by a slot (offset<<32 | length) into the
 * payload area. The response reuses the request payload (buffer outputs are
 * mutated in place) and carries the function return value in the first u64
 * after the header.
 *
 * All fields are little-endian on every supported target (arm32/arm64/x86_64).
 */

#include <stddef.h>
#include <stdint.h>

#define TGPU_MAGIC 0x55504754u /* 'TGPU' */
#define TGPU_VERSION 1u

#define TGPU_MSG_MAX 4096u /* max message size incl. header */
#define TGPU_RING_MIN 256u

/* Transport/dispatch status (message header .status). */
#define TGPU_ST_OK 0
#define TGPU_ST_EAGAIN 1 /* transient (not used inside messages) */
#define TGPU_ST_ENOSYM -2 /* driver symbol missing */
#define TGPU_ST_ENOTSUP -3 /* needs struct conversion (roadmap) */
#define TGPU_ST_EINVAL -4
#define TGPU_ST_ERANGE -5
#define TGPU_ST_ENOSYS -6

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t region_size;
	uint32_t req_off;
	uint32_t req_size;
	uint32_t resp_off;
	uint32_t resp_size;
	uint32_t reserved;
} tgpu_region_hdr;

typedef struct {
	volatile uint32_t head; /* producer advances */
	volatile uint32_t tail; /* consumer advances */
} tgpu_ring_ctl;

typedef struct {
	tgpu_region_hdr hdr;
	tgpu_ring_ctl req;
	tgpu_ring_ctl resp;
} tgpu_region;

typedef struct {
	uint32_t len; /* total message length incl. this header */
	uint16_t op;
	uint16_t flags; /* = nslots */
	uint32_t seq;
	int32_t status;
} tgpu_msg_hdr; /* 16 bytes */

/* Initialize a region; computes ring offsets/sizes. Returns 0 or -1. */
int tgpu_region_init(void *base, size_t size);
tgpu_region *tgpu_region_get(void *base);

/* Ring primitives (ctl + backing store). 0 = ok, 1 = would block, -1 = bad. */
int tgpu_ring_send(tgpu_ring_ctl *ctl, uint8_t *data, uint32_t size,
		   const void *msg, uint32_t len);
int tgpu_ring_recv(tgpu_ring_ctl *ctl, uint8_t *data, uint32_t size,
		   void *out, uint32_t cap, uint32_t *out_len);
uint32_t tgpu_ring_used(const tgpu_ring_ctl *ctl, uint32_t size);
uint32_t tgpu_ring_free(const tgpu_ring_ctl *ctl, uint32_t size);

/* Message helpers. */
uint32_t tgpu_msg_total(uint32_t nslots, uint32_t payload_len);
tgpu_msg_hdr *tgpu_msg_at(void *msg);

#endif /* TRIANGO_GPU_PROTO_H */
