/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "triango/gpu_proto.h"

#include <stdlib.h>
#include <string.h>

void test_proto(void)
{
	static uint8_t region[65536];
	tgpu_region *r;
	uint8_t msg[512], out[512];
	uint32_t i, out_len = 0;

	/* Region init + validation. */
	CHECK_EQ_INT(tgpu_region_init(region, sizeof(region)), 0);
	r = tgpu_region_get(region);
	CHECK(r != NULL);
	CHECK_EQ_INT(tgpu_region_init(region, 128), -1); /* too small */
	CHECK_EQ_INT(tgpu_region_init(region, sizeof(region)), 0);
	r = tgpu_region_get(region);
	CHECK(r != NULL);
	CHECK_EQ_INT(tgpu_region_get(NULL) == NULL, 1);
	CHECK_EQ_INT(r->hdr.req_off + r->hdr.req_size <= r->hdr.region_size, 1);
	CHECK_EQ_INT(r->hdr.resp_off + r->hdr.resp_size <= r->hdr.region_size, 1);

	/* Empty recv -> EAGAIN. */
	CHECK_EQ_INT(tgpu_ring_recv(&r->req, (uint8_t *)r + r->hdr.req_off,
				    r->hdr.req_size, out, sizeof(out),
				    &out_len),
		     1);

	/* Send/recv preserves order and content. */
	for (i = 0; i < 3; i++) {
		memset(msg, 0, sizeof(msg));
		msg[0] = (uint8_t)(0xA0 + i);
		msg[37] = (uint8_t)i; /* unaligned byte survives */
		CHECK_EQ_INT(tgpu_ring_send(&r->req,
					    (uint8_t *)r + r->hdr.req_off,
					    r->hdr.req_size, msg, 38),
			     0);
	}
	for (i = 0; i < 3; i++) {
		CHECK_EQ_INT(tgpu_ring_recv(&r->req,
					    (uint8_t *)r + r->hdr.req_off,
					    r->hdr.req_size, out, sizeof(out),
					    &out_len),
			     0);
		CHECK_EQ_INT(out_len, 38);
		CHECK_EQ_INT(out[0], 0xA0 + i);
		CHECK_EQ_INT(out[37], i);
	}

	/* Wraparound: pump many records so head/tail cross the ring end. */
	{
		uint32_t sent = 0, recvd = 0;

		for (i = 0; i < 2000; i++) {
			uint8_t buf[64];

			memset(buf, (uint8_t)i, sizeof(buf));
			buf[0] = (uint8_t)(i & 0xff);
			buf[1] = (uint8_t)((i >> 8) & 0xff);
			if (tgpu_ring_send(&r->req,
					   (uint8_t *)r + r->hdr.req_off,
					   r->hdr.req_size, buf,
					   sizeof(buf)) == 0)
				sent++;
			else {
				/* drain one and retry */
				uint8_t o[64];
				uint32_t ol = 0;

				CHECK_EQ_INT(tgpu_ring_recv(
						     &r->req,
						     (uint8_t *)r + r->hdr.req_off,
						     r->hdr.req_size, o,
						     sizeof(o), &ol),
					     0);
				CHECK_EQ_INT(ol, 64);
				recvd++;
				CHECK_EQ_INT(tgpu_ring_send(
						     &r->req,
						     (uint8_t *)r + r->hdr.req_off,
						     r->hdr.req_size, buf,
						     sizeof(buf)),
					     0);
				sent++;
			}
			/* Also drain randomly to force wrap. */
			if ((i % 3) == 0) {
				uint8_t o[64];
				uint32_t ol = 0;

				if (tgpu_ring_recv(&r->req,
						   (uint8_t *)r + r->hdr.req_off,
						   r->hdr.req_size, o,
						   sizeof(o), &ol) == 0) {
					uint16_t idx = (uint16_t)o[0] |
						       ((uint16_t)o[1] << 8);

					CHECK(idx <= i);
					CHECK_EQ_INT(ol, 64);
					recvd++;
				}
			}
		}
		/* Drain the rest. */
		for (;;) {
			uint8_t o[64];
			uint32_t ol = 0;

			if (tgpu_ring_recv(&r->req,
					   (uint8_t *)r + r->hdr.req_off,
					   r->hdr.req_size, o, sizeof(o),
					   &ol) != 0)
				break;
			recvd++;
		}
		CHECK_EQ_INT(sent, recvd);
		CHECK(tgpu_ring_used(&r->req, r->hdr.req_size) == 0);
	}

	/* Bad inputs. */
	CHECK_EQ_INT(tgpu_ring_send(&r->req, (uint8_t *)r + r->hdr.req_off,
				    r->hdr.req_size, msg, 0),
		     -1);
	/* Record larger than the caller's cap -> -1 (and not consumed). */
	memset(msg, 0x5A, 32);
	CHECK_EQ_INT(tgpu_ring_send(&r->req, (uint8_t *)r + r->hdr.req_off,
				    r->hdr.req_size, msg, 32),
		     0);
	CHECK_EQ_INT(tgpu_ring_recv(&r->req, (uint8_t *)r + r->hdr.req_off,
				    r->hdr.req_size, out, 4, &out_len),
		     -1);
	CHECK_EQ_INT(tgpu_ring_recv(&r->req, (uint8_t *)r + r->hdr.req_off,
				    r->hdr.req_size, out, sizeof(out),
				    &out_len),
		     0);
	CHECK_EQ_INT(out_len, 32);

	/* Message size helper. */
	CHECK_EQ_INT(tgpu_msg_total(0, 0), 16);
	CHECK_EQ_INT(tgpu_msg_total(2, 7), 16 + 16 + 7);
}
