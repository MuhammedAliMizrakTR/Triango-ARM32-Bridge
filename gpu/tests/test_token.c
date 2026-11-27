/* SPDX-License-Identifier: MIT */
/*
 * Guest-visible handle token table: 64-bit driver handles wrap into dense
 * 32-bit ids so arm32 guests can hold them losslessly in pointer-sized
 * variables.  Id 0 is NULL.
 */
#include "harness.h"
#include "triango/tg_runtime.h"

void test_token(void)
{
	tg_token_map m;
	uint32_t a, b, c, again;

	tg_token_map_init(&m);
	CHECK_EQ_INT(tg_token_resolve(&m, 0), 0);
	CHECK_EQ_INT(tg_token_resolve(&m, 1), 0);
	CHECK_EQ_INT(tg_token_alloc(&m, 0), 0); /* NULL stays 0 */

	a = tg_token_alloc(&m, 0x1122334455667788ull);
	b = tg_token_alloc(&m, 0xCAFEBABEDEADBEEFull);
	CHECK(a != 0 && b != 0 && a != b);
	CHECK(a <= TG_TOKEN_MAP_SIZE && b <= TG_TOKEN_MAP_SIZE);
	CHECK_EQ_INT(tg_token_resolve(&m, a), 0x1122334455667788ull);
	CHECK_EQ_INT(tg_token_resolve(&m, b), 0xCAFEBABEDEADBEEFull);

	/* Same host handle maps to the same id. */
	again = tg_token_alloc(&m, 0x1122334455667788ull);
	CHECK_EQ_INT(again, a);

	/* Release frees the slot; a new handle can reuse it. */
	tg_token_release(&m, a);
	CHECK_EQ_INT(tg_token_resolve(&m, a), 0);
	c = tg_token_alloc(&m, 0x777ull);
	CHECK(c != 0);
	CHECK_EQ_INT(tg_token_resolve(&m, c), 0x777ull);
	CHECK_EQ_INT(tg_token_resolve(&m, b), 0xCAFEBABEDEADBEEFull);

	/* Ids above the table size resolve to 0. */
	CHECK_EQ_INT(tg_token_resolve(&m, TG_TOKEN_MAP_SIZE + 1), 0);
	CHECK_EQ_INT(tg_token_resolve(&m, 0xFFFFFFFFu), 0);
}
