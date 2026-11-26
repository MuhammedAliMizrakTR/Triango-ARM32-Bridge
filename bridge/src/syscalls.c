/* SPDX-License-Identifier: MIT */
#include "triango/syscalls.h"

#include <stdio.h>
#include <string.h>

/*
 * Arm32 EABI numbers: asm/unistd.h (ARM). AArch64 numbers: asm-generic
 * unistd (the aarch64 table). Only mappings the project has verified are
 * listed; unknown numbers simply have no entry (lookup returns NULL).
 */
const tb_syscall_map tb_syscall_table[] = {
	/* arm_nr, a64_nr, fixups, name */
	{ 0, 128, TB_FIX_NONE, "restart_syscall" },
	{ 1, 93, TB_FIX_NONE, "exit" },
	{ 2, 220, TB_FIX_CLONE_FORK, "fork" },
	{ 3, 63, TB_FIX_NONE, "read" },
	{ 4, 64, TB_FIX_NONE, "write" },
	{ 5, 56, TB_FIX_OPENAT, "open" },
	{ 6, 57, TB_FIX_NONE, "close" },
	{ 8, 56, TB_FIX_OPENAT_CREAT, "creat" },
	{ 11, 221, TB_FIX_NONE, "execve" },
	{ 12, 49, TB_FIX_NONE, "chdir" },
	{ 19, 62, TB_FIX_NONE, "lseek" },
	{ 20, 172, TB_FIX_NONE, "getpid" },
	{ 24, 174, TB_FIX_NONE, "getuid" },
	{ 33, 48, TB_FIX_OPENAT, "access" },
	{ 37, 129, TB_FIX_NONE, "kill" },
	{ 38, 38, TB_FIX_RENAMEAT, "rename" },
	{ 39, 34, TB_FIX_MKDIRAT, "mkdir" },
	{ 40, 35, TB_FIX_UNLINKAT_RMDIR, "rmdir" },
	{ 41, 23, TB_FIX_NONE, "dup" },
	{ 42, 59, TB_FIX_PIPE2, "pipe" },
	{ 45, 214, TB_FIX_NONE, "brk" },
	{ 54, 29, TB_FIX_NONE, "ioctl" },
	{ 55, 25, TB_FIX_FCNTL, "fcntl" },
	{ 63, 24, TB_FIX_DUP3, "dup2" },
	{ 78, 169, TB_FIX_TIMESPEC32_OUT, "gettimeofday" },
	{ 83, 36, TB_FIX_SYMLINKAT, "symlink" },
	{ 85, 78, TB_FIX_READLINKAT, "readlink" },
	{ 91, 215, TB_FIX_NONE, "munmap" },
	{ 108, 80, TB_FIX_STAT64, "fstat" },
	{ 114, 260, TB_FIX_NONE, "wait4" },
	{ 118, 82, TB_FIX_NONE, "fsync" },
	{ 120, 220, TB_FIX_CLONE, "clone" },
	{ 122, 160, TB_FIX_NONE, "uname" },
	{ 125, 226, TB_FIX_NONE, "mprotect" },
	{ 126, 135, TB_FIX_SIGSETSIZE, "sigprocmask" },
	{ 133, 50, TB_FIX_NONE, "fchdir" },
	{ 140, 62, TB_FIX_LLSEEK, "_llseek" },
	{ 142, 72, TB_FIX_SELECT, "_newselect" },
	{ 143, 32, TB_FIX_NONE, "flock" },
	{ 144, 227, TB_FIX_NONE, "msync" },
	{ 145, 65, TB_FIX_NONE, "readv" },
	{ 146, 66, TB_FIX_NONE, "writev" },
	{ 148, 83, TB_FIX_NONE, "fdatasync" },
	{ 158, 124, TB_FIX_NONE, "sched_yield" },
	{ 162, 101, TB_FIX_TIMESPEC32, "nanosleep" },
	{ 163, 216, TB_FIX_NONE, "mremap" },
	{ 168, 73, TB_FIX_POLL, "poll" },
	{ 172, 167, TB_FIX_NONE, "prctl" },
	{ 174, 134, TB_FIX_SIGACTION32 | TB_FIX_SIGSETSIZE, "rt_sigaction" },
	{ 175, 135, TB_FIX_SIGSETSIZE, "rt_sigprocmask" },
	{ 180, 67, TB_FIX_NONE, "pread64" },
	{ 181, 68, TB_FIX_NONE, "pwrite64" },
	{ 183, 17, TB_FIX_NONE, "getcwd" },
	{ 192, 222, TB_FIX_MMAP2, "mmap2" },
	{ 195, 79, TB_FIX_STAT64, "stat64" },
	{ 196, 79, TB_FIX_STAT64, "lstat64" },
	{ 197, 80, TB_FIX_STAT64, "fstat64" },
	{ 199, 174, TB_FIX_NONE, "getuid32" },
	{ 200, 176, TB_FIX_NONE, "getgid32" },
	{ 201, 175, TB_FIX_NONE, "geteuid32" },
	{ 202, 177, TB_FIX_NONE, "getegid32" },
	{ 217, 61, TB_FIX_NONE, "getdents64" },
	{ 221, 233, TB_FIX_NONE, "madvise" },
	{ 240, 98, TB_FIX_NONE, "futex" },
	{ 248, 94, TB_FIX_NONE, "exit_group" },
	{ 263, 113, TB_FIX_TIMESPEC32_OUT, "clock_gettime" },
	{ 268, 131, TB_FIX_NONE, "tgkill" },
};

const size_t tb_syscall_table_len =
	sizeof(tb_syscall_table) / sizeof(tb_syscall_table[0]);

const tb_syscall_map *tb_syscall_lookup(uint32_t arm_nr)
{
	/* Table is sorted; binary search. */
	size_t lo = 0, hi = tb_syscall_table_len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (tb_syscall_table[mid].arm_nr < arm_nr)
			lo = mid + 1;
		else if (tb_syscall_table[mid].arm_nr > arm_nr)
			hi = mid;
		else
			return &tb_syscall_table[mid];
	}
	return NULL;
}

static const struct {
	uint32_t bit;
	const char *name;
} fixup_names[] = {
	{ TB_FIX_OPENAT, "OPENAT" },
	{ TB_FIX_OPENAT_CREAT, "OPENAT_CREAT" },
	{ TB_FIX_UNLINKAT, "UNLINKAT" },
	{ TB_FIX_UNLINKAT_RMDIR, "UNLINKAT_RMDIR" },
	{ TB_FIX_MKDIRAT, "MKDIRAT" },
	{ TB_FIX_LINKAT, "LINKAT" },
	{ TB_FIX_SYMLINKAT, "SYMLINKAT" },
	{ TB_FIX_READLINKAT, "READLINKAT" },
	{ TB_FIX_PIPE2, "PIPE2" },
	{ TB_FIX_MMAP2, "MMAP2" },
	{ TB_FIX_TIMESPEC32, "TIMESPEC32" },
	{ TB_FIX_TIMESPEC32_OUT, "TIMESPEC32_OUT" },
	{ TB_FIX_STAT64, "STAT64" },
	{ TB_FIX_SIGACTION32, "SIGACTION32" },
	{ TB_FIX_SIGSETSIZE, "SIGSETSIZE" },
	{ TB_FIX_SELECT, "SELECT" },
	{ TB_FIX_POLL, "POLL" },
	{ TB_FIX_CLONE, "CLONE" },
	{ TB_FIX_CLONE_FORK, "CLONE_FORK" },
	{ TB_FIX_DUP3, "DUP3" },
	{ TB_FIX_FCNTL, "FCNTL" },
	{ TB_FIX_LLSEEK, "LLSEEK" },
	{ TB_FIX_RENAMEAT, "RENAMEAT" },
};

void tb_fixup_str(uint32_t flags, char *buf, size_t len)
{
	size_t used = 0;
	size_t i;

	if (!buf || !len)
		return;
	buf[0] = '\0';
	if (flags == TB_FIX_NONE) {
		snprintf(buf, len, "NONE");
		return;
	}
	for (i = 0; i < sizeof(fixup_names) / sizeof(fixup_names[0]); i++) {
		if (!(flags & fixup_names[i].bit))
			continue;
		used += (size_t)snprintf(buf + used, len - used, "%s%s",
					 used ? "|" : "", fixup_names[i].name);
		if (used >= len)
			return;
	}
}
