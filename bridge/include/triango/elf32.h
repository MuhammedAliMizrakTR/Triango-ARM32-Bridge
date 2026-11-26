/* SPDX-License-Identifier: MIT */
#ifndef TRIANGO_ELF32_H
#define TRIANGO_ELF32_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal ELF32 (AArch32) parsing for the Triango bridge.
 * Local constants avoid host <elf.h> differences; values match the ELF spec.
 */

#define TB_ELFMAG0 0x7f
#define TB_ELFMAG1 'E'
#define TB_ELFMAG2 'L'
#define TB_ELFMAG3 'F'

#define TB_ELFCLASS32 1
#define TB_ELFDATA2LSB 1
#define TB_EV_CURRENT 1

#define TB_ET_EXEC 2
#define TB_ET_DYN 3

#define TB_PT_LOAD 1
#define TB_PT_DYNAMIC 2
#define TB_PT_INTERP 3

#define TB_EM_ARM 40

#define TB_EI_NIDENT 16
#define TB_EHDR_SIZE 52 /* sizeof(Elf32_Ehdr) */
#define TB_PHDR_SIZE 32 /* sizeof(Elf32_Phdr) */

#define TB_MAX_LOADS 8
#define TB_INTERP_MAX 128

typedef struct {
	uint32_t vaddr;
	uint32_t memsz;
	uint32_t offset;
	uint32_t filesz;
	uint32_t flags;
} tb_segment;

typedef struct {
	uint32_t entry;
	uint32_t phoff;
	uint32_t phnum;
	uint32_t phentsize;
	int is_dynamic; /* ET_DYN (PIE) */
	int is_android; /* PT_INTERP starts with /system/bin/linker */
	int has_interp;
	char interp[TB_INTERP_MAX];
	int n_load;
	tb_segment loads[TB_MAX_LOADS];
} tb_image;

/*
 * Parse an in-memory image: @buf must contain the ELF header at offset 0 and
 * the full program-header table at e_phoff (as produced by the unit tests).
 * Returns 0 on success, -1 on rejection (bad/unsupported ELF).
 */
int tb_elf32_parse(const void *buf, size_t len, tb_image *out);

/*
 * Parse an ARM32 ELF file from disk. On failure returns -1 and writes a
 * human-readable reason into @err (when provided).
 */
int tb_elf32_parse_file(const char *path, tb_image *out, char *err, size_t errlen);

/* Validate just the identification/header fields of an ELF32 ARM header. */
int tb_elf32_validate(const void *hdr, size_t len);

#endif /* TRIANGO_ELF32_H */
