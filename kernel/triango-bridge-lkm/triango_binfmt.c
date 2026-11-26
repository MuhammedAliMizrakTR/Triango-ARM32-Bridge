// SPDX-License-Identifier: GPL-2.0-only
/*
 * triango_binfmt.c — Triango ARM32 Bridge, binfmt interception module
 *
 * Regains Arm32 (AArch32) *exec* on AArch64-only CPUs (no EL0 AArch32),
 * e.g. Qualcomm Oryon: SM8750 / SM8850 (Snapdragon 8 Elite / 8 Elite Gen 5).
 *
 * The CPU cannot execute AArch32 instructions at all, so this module does not
 * try to run Arm32 ELFs natively. Instead it registers a high-priority
 * linux_binfmt handler that recognises ELF32/LSB/EM_ARM binaries and re-execs
 * them under the Triango userspace bridge (an AArch64 binary), which then
 * launches an execution engine (qemu-arm user-mode in v0.1, the Triango JIT
 * later).
 *
 * Exec mechanics mirror fs/binfmt_script.c and fs/binfmt_misc.c as of Linux
 * v6.12 (verified against upstream source): the handler splices new argv
 * entries in reverse push order, points bprm->interpreter at the bridge and
 * returns 0. The exec core then loads the interpreter itself — there is no
 * recursive search_binary_handler() call here.
 *
 * Only plain EXPORT_SYMBOL() interfaces are used (__register_binfmt,
 * copy_string_kernel, bprm_change_interp, open_exec), so the module stays
 * GKI-ABI friendly and needs no kallsyms/kprobe tricks.
 *
 * argv contract handed to the bridge (push order is the reverse of the final
 * argv order, see the "done in reverse order" comment in fs/binfmt_script.c):
 *
 *     argv[0] = bridge_path
 *     argv[1] = path of the original AArch32 binary (bprm->interp)
 *     argv[2] = original argv[0]
 *     argv[3..] = original argv[1..]
 *
 * We deliberately do NOT call remove_arg_zero(): the original argv[0] is kept
 * so the bridge can restore it for the guest (qemu-arm's -0 option).
 *
 * Target kernels: GKI android16-6.12 (Samsung Galaxy S26 Ultra,
 * 6.12.30-android16-5-...-4k) and other 6.12+ Android GKI trees.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/binfmts.h>
#include <linux/elf.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/err.h>
#include <asm/byteorder.h>

#define TRIANGO_TAG "triango: "

#define triango_err(fmt, ...)  pr_err(TRIANGO_TAG fmt, ##__VA_ARGS__)
#define triango_info(fmt, ...) pr_info(TRIANGO_TAG fmt, ##__VA_ARGS__)
#define triango_dbg(fmt, ...)                      \
	do {                                       \
		if (verbose)                       \
			pr_info(TRIANGO_TAG fmt,   \
				##__VA_ARGS__);    \
	} while (0)

/* ------------------------------------------------------------------ */
/* Module parameters                                                   */
/* ------------------------------------------------------------------ */

static char *bridge_path = "/data/adb/triango/triango-bridge-lkm";
module_param(bridge_path, charp, 0644);
MODULE_PARM_DESC(bridge_path,
		 "Path to the AArch64 Triango bridge entrypoint (the shipped triango-bridge-lkm wrapper sets --conv=lkm)");

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Enable Arm32 ELF interception");

static bool android_only = true;
module_param(android_only, bool, 0644);
MODULE_PARM_DESC(android_only,
		 "Only intercept binaries whose PT_INTERP is /system/bin/linker (Android Arm32); when 0, intercept every ELF32/EM_ARM binary");

static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "Verbose logging");

/* Sanity cap for program-header scanning. */
#define TRIANGO_MAX_PHNUM 64
/* Enough for "/system/bin/linker" plus slack. */
#define TRIANGO_INTERP_MAX 128

/* ------------------------------------------------------------------ */
/* ELF32 identification                                                */
/* ------------------------------------------------------------------ */

/*
 * Minimal local view of the ELF32 header: bprm->buf holds at least
 * BINPRM_BUF_SIZE (256) bytes from prepare_binprm(), which comfortably
 * covers the 52-byte ELF32 header. We only read the fields we check.
 */
struct triango_elf32 {
	unsigned char e_ident[16];
	__le16 e_type;
	__le16 e_machine;
	__le32 e_version;
	__le32 e_entry;
	__le32 e_phoff;
	__le32 e_shoff;
	__le32 e_flags;
	__le16 e_ehsize;
	__le16 e_phentsize;
	__le16 e_phnum;
	__le16 e_shentsize;
	__le16 e_shnum;
	__le16 e_shstrndx;
} __packed;

struct triango_phdr32 {
	__le32 p_type;
	__le32 p_offset;
	__le32 p_vaddr;
	__le32 p_paddr;
	__le32 p_filesz;
	__le32 p_memsz;
	__le32 p_flags;
	__le32 p_align;
} __packed;

static bool triango_is_arm32_elf(const char *buf)
{
	const struct triango_elf32 *eh = (const struct triango_elf32 *)buf;

	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		return false;
	if (eh->e_ident[EI_CLASS] != ELFCLASS32)
		return false;
	if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return false;
	if (eh->e_ident[EI_VERSION] != EV_CURRENT)
		return false;
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
		return false;
	if (le16_to_cpu(eh->e_machine) != EM_ARM)
		return false;
	return true;
}

/*
 * Scan the program headers for PT_INTERP and copy its path (up to
 * TRIANGO_INTERP_MAX - 1 bytes) into @interp. Returns 0 when PT_INTERP was
 * found and read, -ENOENT when absent/unreadable.
 */
static int triango_read_interp(struct linux_binprm *bprm,
			       const struct triango_elf32 *eh, char *interp,
			       size_t interp_len)
{
	struct triango_phdr32 ph;
	loff_t pos;
	u16 phnum = le16_to_cpu(eh->e_phnum);
	u16 phentsize = le16_to_cpu(eh->e_phentsize);
	u16 i;

	if (!phnum || !phentsize || phentsize < sizeof(ph))
		return -ENOENT;
	if (phnum > TRIANGO_MAX_PHNUM)
		phnum = TRIANGO_MAX_PHNUM;

	for (i = 0; i < phnum; i++) {
		size_t n;

		pos = le32_to_cpu(eh->e_phoff) + (loff_t)i * phentsize;
		if (kernel_read(bprm->file, &ph, sizeof(ph), &pos) !=
		    sizeof(ph))
			return -ENOENT;
		if (le32_to_cpu(ph.p_type) != PT_INTERP)
			continue;

		n = le32_to_cpu(ph.p_filesz);
		if (n == 0 || n > interp_len - 1)
			n = interp_len - 1;
		pos = le32_to_cpu(ph.p_offset);
		if (kernel_read(bprm->file, interp, n, &pos) != n)
			return -ENOENT;
		interp[n] = '\0';
		return 0;
	}
	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* binfmt handler                                                      */
/* ------------------------------------------------------------------ */

static int triango_load_binary(struct linux_binprm *bprm)
{
	const struct triango_elf32 *eh;
	struct file *file;
	char interp[TRIANGO_INTERP_MAX];
	int retval;

	if (!enabled)
		return -ENOEXEC;
	if (!bridge_path || !bridge_path[0])
		return -ENOEXEC;

	/* Fast path: only ELF32/LSB/EM_ARM binaries are ours. */
	if (!triango_is_arm32_elf(bprm->buf))
		return -ENOEXEC;

	/*
	 * If the original path cannot be resolved after exec we cannot hand
	 * it to the bridge (mirrors binfmt_misc/binfmt_script behaviour).
	 */
	if (bprm->interp_flags & BINPRM_FLAGS_PATH_INACCESSIBLE)
		return -ENOENT;

	if (android_only) {
		retval = triango_read_interp(bprm,
				(const struct triango_elf32 *)bprm->buf,
				interp, sizeof(interp));
		if (retval < 0) {
			triango_dbg("skip non-Android arm32 binary (no PT_INTERP)\n");
			return -ENOEXEC;
		}
		if (strncmp(interp, "/system/bin/linker",
			    strlen("/system/bin/linker")) != 0) {
			triango_dbg("skip arm32 binary with interp '%s'\n",
				    interp);
			return -ENOEXEC;
		}
	}

	/*
	 * Splice the new argv entries in reverse order (the user environment
	 * and arguments are stored in reverse push order — same rule as the
	 * "done in reverse order" section in fs/binfmt_script.c):
	 *   push target path  -> argv[1]
	 *   push bridge path  -> argv[0]
	 * The original argv (including the original argv[0]) stays at
	 * argv[2..]; we never call remove_arg_zero().
	 */
	retval = copy_string_kernel(bprm->interp, bprm);
	if (retval < 0)
		return retval;
	bprm->argc++;

	retval = copy_string_kernel(bridge_path, bprm);
	if (retval < 0)
		return retval;
	bprm->argc++;

	/* Update interp so the rest of exec sees the real interpreter. */
	retval = bprm_change_interp(bridge_path, bprm);
	if (retval < 0)
		return retval;

	/* Hand the bridge to the exec core as the interpreter to load. */
	file = open_exec(bridge_path);
	if (IS_ERR(file)) {
		retval = PTR_ERR(file);
		triango_err("open_exec('%s') failed: %d\n", bridge_path,
			    retval);
		return retval;
	}
	bprm->interpreter = file;

	triango_dbg("redirect arm32 exec -> %s (argc now %d)\n", bridge_path,
		    bprm->argc);
	return 0;
}

static struct linux_binfmt triango_binfmt = {
	.module = THIS_MODULE,
	.load_binary = triango_load_binary,
};

static int __init triango_init(void)
{
	/* insert_binfmt(): register at the head of the format list so we are
	 * tried before the in-tree ELF loader (and its COMPAT path).
	 */
	insert_binfmt(&triango_binfmt);
	triango_info("arm32 bridge binfmt registered (bridge='%s', android_only=%d)\n",
		     bridge_path, android_only);
	return 0;
}

static void __exit triango_exit(void)
{
	unregister_binfmt(&triango_binfmt);
	triango_info("arm32 bridge binfmt unregistered\n");
}

module_init(triango_init);
module_exit(triango_exit);

MODULE_DESCRIPTION("Triango ARM32 Bridge — Arm32 exec interception for AArch64-only SoCs");
MODULE_AUTHOR("MuhammedAliMizrakTR");
MODULE_LICENSE("GPL");
