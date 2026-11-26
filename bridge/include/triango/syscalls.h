/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_SYSCALLS_H
#define TRIANGO_SYSCALLS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Arm32 EABI -> AArch64 syscall mapping contract.
 *
 * This table is the ABI contract for the future Triango JIT engine: it names
 * each Arm32 syscall that v0.1 understands, its AArch64 equivalent and the
 * argument fixups a translator must perform (32-bit structs, path-argument
 * hoisting into *at() variants, pgoff scaling, ...). The qemu engine passes
 * syscalls through qemu's own translation; the table is still authoritative
 * for tooling, diagnostics and the JIT roadmap.
 */

/* Fixup flags (documented translator obligations). */
#define TB_FIX_NONE 0u
#define TB_FIX_OPENAT (1u << 0) /* open(path,fl,mo) -> openat(AT_FDCWD,...) */
#define TB_FIX_OPENAT_CREAT (1u << 1) /* creat(path,mo) -> openat w/ O_CREAT|O_WRONLY|O_TRUNC */
#define TB_FIX_UNLINKAT (1u << 2) /* unlink(path) -> unlinkat(AT_FDCWD,path,0) */
#define TB_FIX_UNLINKAT_RMDIR (1u << 3) /* rmdir(path) -> unlinkat(...,AT_REMOVEDIR) */
#define TB_FIX_MKDIRAT (1u << 4) /* mkdir(path,mo) -> mkdirat(AT_FDCWD,...) */
#define TB_FIX_LINKAT (1u << 5) /* link(a,b) -> linkat(AT_FDCWD,a,AT_FDCWD,b,0) */
#define TB_FIX_SYMLINKAT (1u << 6) /* symlink(t,l) -> symlinkat(t,AT_FDCWD,l) */
#define TB_FIX_READLINKAT (1u << 7) /* readlink(p,b,s) -> readlinkat(AT_FDCWD,...) */
#define TB_FIX_PIPE2 (1u << 8) /* pipe(fds) -> pipe2(fds,0) */
#define TB_FIX_MMAP2 (1u << 9) /* mmap2(...,pgoff) -> mmap(...,pgoff<<12) */
#define TB_FIX_TIMESPEC32 (1u << 10) /* 32-bit timespec arg(s) -> 64-bit */
#define TB_FIX_TIMESPEC32_OUT (1u << 11) /* 32-bit timespec result(s) <- 64-bit */
#define TB_FIX_STAT64 (1u << 12) /* arm32 stat64 layout -> struct stat */
#define TB_FIX_SIGACTION32 (1u << 13) /* arm32 sigaction layout -> a64 sigaction */
#define TB_FIX_SIGSETSIZE (1u << 14) /* append sigsetsize=8 argument */
#define TB_FIX_SELECT (1u << 15) /* pselect6 from select args + sigsetsize */
#define TB_FIX_POLL (1u << 16) /* poll -> ppoll(args, NULL sigset) */
#define TB_FIX_CLONE (1u << 17) /* arm32 clone flags/tls arg order -> a64 clone */
#define TB_FIX_CLONE_FORK (1u << 18) /* fork() -> clone(SIGCHLD, ...) */
#define TB_FIX_DUP3 (1u << 19) /* dup2(old,new) -> dup3(old,new,0) */
#define TB_FIX_FCNTL (1u << 20) /* fcntl: struct flock 32->64 on F_SETLK/GETLK */
#define TB_FIX_LLSEEK (1u << 21) /* _llseek(fd,off_hi,off_lo,res,whence) -> lseek */
#define TB_FIX_RENAMEAT (1u << 22) /* rename(a,b) -> renameat(AT_FDCWD,a,AT_FDCWD,b) */

typedef struct {
	uint32_t arm_nr;
	int32_t a64_nr;
	uint32_t flags;
	const char *name;
} tb_syscall_map;

/* Sorted by arm_nr (strictly increasing) — safe for bsearch. */
extern const tb_syscall_map tb_syscall_table[];
extern const size_t tb_syscall_table_len;

/* NULL when the Arm32 syscall number is unknown to v0.1. */
const tb_syscall_map *tb_syscall_lookup(uint32_t arm_nr);

/* Human-readable fixup flags, e.g. "OPENAT|STAT64" into @buf. */
void tb_fixup_str(uint32_t flags, char *buf, size_t len);

#endif /* TRIANGO_SYSCALLS_H */
