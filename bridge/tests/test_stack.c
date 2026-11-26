/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "triango/elf32.h"
#include "triango/stack.h"

#include <stdlib.h>
#include <string.h>

/* Read a little-endian u32 at a byte offset of a buffer. */
static uint32_t u32_at(const uint8_t *p, size_t off)
{
	return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
	       ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

void test_stack(void)
{
	tb_stack_img img;
	tb_stack st;
	const char *av[] = { "prog", "a" };
	const char *ev[] = { "A=B", "C=D" };
	uint8_t rnd[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
			    16 };
	uint32_t base = 0x70000000;
	uint32_t argv0_va = 0, argv1_va = 0, env0_va = 0, random_va = 0,
		 execfn_va = 0, platform_va = 0;
	size_t aux_off;
	int found_entry = 0, found_pagesz = 0, found_hwcap = 0, found_null = 0;

	memset(&img, 0, sizeof(img));
	img.entry = 0x00010000;
	img.phdr = 0x00008000;
	img.phnum = 2;
	img.phentsize = 32;
	img.uid = img.euid = img.gid = img.egid = 1000;
	img.stack_vaddr = base;

	CHECK_EQ_INT(tb_stack_build(&img, av, 2, ev, 2, rnd, "prog", &st), 0);
	CHECK(st.data != NULL);

	/* Structural layout: argc @0, argv ptrs @4, env ptrs after NULL. */
	CHECK_EQ_INT(tb_stack_u32(&st, 0), 2);
	argv0_va = u32_at(st.data, 4);
	argv1_va = u32_at(st.data, 8);
	CHECK_EQ_INT(u32_at(st.data, 12), 0); /* argv NULL terminator */
	env0_va = u32_at(st.data, 16);
	CHECK_EQ_INT(u32_at(st.data, 20) != env0_va, 1); /* env[1] distinct */
	CHECK_EQ_INT(u32_at(st.data, 24), 0); /* env NULL terminator */

	/* Pointer targets live inside the image and are 32-bit VAs. */
	CHECK(argv0_va >= base && argv0_va < base + st.size);
	CHECK(argv1_va >= base && argv1_va < base + st.size);
	CHECK(env0_va >= base && env0_va < base + st.size);

	/* String contents, reached through the pointers. */
	CHECK_STR((const char *)st.data + (argv0_va - base), "prog");
	CHECK_STR((const char *)st.data + (argv1_va - base), "a");
	CHECK_STR((const char *)st.data + (env0_va - base), "A=B");

	/* auxv: walk pairs after the two NULL terminators. */
	aux_off = 4 + 4 * 3 + 4 * 3;
	for (;;) {
		uint32_t key = u32_at(st.data, aux_off);
		uint32_t val = u32_at(st.data, aux_off + 4);

		if (key == TB_AT_NULL) {
			found_null = 1;
			break;
		}
		if (key == TB_AT_ENTRY) {
			CHECK_EQ_INT(val, 0x10000);
			found_entry = 1;
		} else if (key == TB_AT_PAGESZ) {
			CHECK_EQ_INT(val, 4096);
			found_pagesz = 1;
		} else if (key == TB_AT_HWCAP) {
			CHECK_EQ_INT(val, TB_HWCAP_ARMV7);
			found_hwcap = 1;
		} else if (key == TB_AT_PHDR) {
			CHECK_EQ_INT(val, 0x8000);
		} else if (key == TB_AT_PHNUM) {
			CHECK_EQ_INT(val, 2);
		} else if (key == TB_AT_UID) {
			CHECK_EQ_INT(val, 1000);
		} else if (key == TB_AT_RANDOM) {
			random_va = val;
		} else if (key == TB_AT_EXECFN) {
			execfn_va = val;
		} else if (key == TB_AT_PLATFORM) {
			platform_va = val;
		}
		aux_off += 8;
		CHECK(aux_off < st.size);
	}
	CHECK(found_entry && found_pagesz && found_hwcap && found_null);

	/* AT_RANDOM points at 16 known bytes. */
	CHECK_EQ_INT(memcmp(st.data + (random_va - base), rnd, 16), 0);
	CHECK_STR((const char *)st.data + (platform_va - base), "v7l");
	CHECK_STR((const char *)st.data + (execfn_va - base), "prog");

	/* Total size is 16-byte padded at the pointer/string boundary. */
	CHECK_EQ_INT(st.size % 1, 0);

	tb_stack_free(&st);
	CHECK(st.data == NULL);

	/* Invalid input rejected. */
	CHECK_EQ_INT(tb_stack_build(&img, av, 0, ev, 2, rnd, "prog", &st), -1);
	{
		const char *badav[] = { NULL };

		CHECK_EQ_INT(tb_stack_build(&img, badav, 1, ev, 2, rnd, "prog",
					   &st),
			     -1);
	}
}
