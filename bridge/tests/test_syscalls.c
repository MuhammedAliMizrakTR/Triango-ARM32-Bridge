/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "triango/syscalls.h"

#include <string.h>

void test_syscalls(void)
{
	size_t i;
	const tb_syscall_map *m;
	char buf[128];

	/* Table must be strictly sorted by arm_nr (bsearch contract). */
	for (i = 1; i < tb_syscall_table_len; i++)
		CHECK(tb_syscall_table[i].arm_nr >
		      tb_syscall_table[i - 1].arm_nr);

	/* Every entry is complete. */
	for (i = 0; i < tb_syscall_table_len; i++) {
		CHECK(tb_syscall_table[i].name != NULL);
		CHECK(tb_syscall_table[i].a64_nr > 0);
	}

	/* Spot checks (Arm32 EABI -> AArch64). */
	m = tb_syscall_lookup(3); /* read */
	CHECK(m && m->a64_nr == 63);
	m = tb_syscall_lookup(4); /* write */
	CHECK(m && m->a64_nr == 64);
	m = tb_syscall_lookup(5); /* open -> openat(AT_FDCWD,...) */
	CHECK(m && m->a64_nr == 56 && (m->flags & TB_FIX_OPENAT));
	m = tb_syscall_lookup(1); /* exit */
	CHECK(m && m->a64_nr == 93);
	m = tb_syscall_lookup(248); /* exit_group */
	CHECK(m && m->a64_nr == 94);
	m = tb_syscall_lookup(240); /* futex */
	CHECK(m && m->a64_nr == 98);
	m = tb_syscall_lookup(192); /* mmap2 */
	CHECK(m && m->a64_nr == 222 && (m->flags & TB_FIX_MMAP2));
	m = tb_syscall_lookup(45); /* brk */
	CHECK(m && m->a64_nr == 214);
	m = tb_syscall_lookup(263); /* clock_gettime */
	CHECK(m && m->a64_nr == 113 && (m->flags & TB_FIX_TIMESPEC32_OUT));
	m = tb_syscall_lookup(174); /* rt_sigaction */
	CHECK(m && m->a64_nr == 134 && (m->flags & TB_FIX_SIGACTION32));
	m = tb_syscall_lookup(42); /* pipe -> pipe2 */
	CHECK(m && m->a64_nr == 59 && (m->flags & TB_FIX_PIPE2));
	m = tb_syscall_lookup(63); /* dup2 -> dup3 */
	CHECK(m && m->a64_nr == 24 && (m->flags & TB_FIX_DUP3));
	m = tb_syscall_lookup(122); /* uname */
	CHECK(m && m->a64_nr == 160);
	m = tb_syscall_lookup(120); /* clone */
	CHECK(m && m->a64_nr == 220 && (m->flags & TB_FIX_CLONE));

	/* Unknown numbers return NULL. */
	CHECK(tb_syscall_lookup(999999) == NULL);
	CHECK(tb_syscall_lookup(7) == NULL); /* waitpid: not in v0.1 subset */

	/* Fixup rendering. */
	tb_fixup_str(TB_FIX_NONE, buf, sizeof(buf));
	CHECK_STR(buf, "NONE");
	tb_fixup_str(TB_FIX_SIGACTION32 | TB_FIX_SIGSETSIZE, buf, sizeof(buf));
	CHECK_STR(buf, "SIGACTION32|SIGSETSIZE");
	tb_fixup_str(TB_FIX_MMAP2 | TB_FIX_TIMESPEC32, buf, sizeof(buf));
	CHECK_STR(buf, "MMAP2|TIMESPEC32");
}
