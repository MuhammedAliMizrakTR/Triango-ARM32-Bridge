/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "triango/elf32.h"

#include <string.h>

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/*
 * Build a synthetic ELF32/EM_ARM image:
 *   [0..51]   ehdr (e_phoff=52, e_phentsize=32, e_phnum=2)
 *   [52..83]  phdr0: PT_LOAD
 *   [84..115] phdr1: PT_INTERP (p_offset=116, p_filesz=strlen(interp)+1)
 *   [116..]   interp string
 */
static size_t build_image(uint8_t *buf, size_t cap, uint16_t type,
			  const char *interp)
{
	size_t ilen = interp ? strlen(interp) + 1 : 0;
	size_t total = 116 + ilen;

	if (total > cap)
		return 0;
	memset(buf, 0, total);

	buf[0] = 0x7f;
	buf[1] = 'E';
	buf[2] = 'L';
	buf[3] = 'F';
	buf[4] = 1; /* ELFCLASS32 */
	buf[5] = 1; /* ELFDATA2LSB */
	buf[6] = 1; /* EV_CURRENT */
	wr16(buf + 16, type);
	wr16(buf + 18, 40); /* EM_ARM */
	wr32(buf + 24, 0x0000c000); /* e_entry */
	wr32(buf + 28, 52); /* e_phoff */
	wr16(buf + 40, 52); /* e_ehsize */
	wr16(buf + 42, 32); /* e_phentsize */
	wr16(buf + 44, 2); /* e_phnum */

	/* phdr0: PT_LOAD (p_type@52, p_offset@56, p_vaddr@60, p_paddr@64,
	 * p_filesz@68, p_memsz@72, p_flags@76, p_align@80) */
	wr32(buf + 52, 1); /* p_type */
	wr32(buf + 56, 0); /* p_offset */
	wr32(buf + 60, 0x00008000); /* p_vaddr */
	wr32(buf + 68, (uint32_t)total); /* p_filesz */
	wr32(buf + 72, 0x4000); /* p_memsz */
	wr32(buf + 76, 5); /* p_flags: R|X */

	/* phdr1: PT_INTERP (base 84) */
	wr32(buf + 84, 3); /* p_type */
	wr32(buf + 88, 116); /* p_offset */
	wr32(buf + 92, 0x00008100); /* p_vaddr */
	wr32(buf + 100, (uint32_t)ilen); /* p_filesz */
	wr32(buf + 104, (uint32_t)ilen); /* p_memsz */

	if (interp)
		memcpy(buf + 116, interp, ilen);
	return total;
}

void test_elf32(void)
{
	uint8_t buf[256];
	tb_image img;
	size_t n;

	/* 1. Valid Android Arm32 executable. */
	n = build_image(buf, sizeof(buf), 2 /* ET_EXEC */, "/system/bin/linker");
	CHECK(n > 0);
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), 0);
	CHECK_EQ_INT(img.entry, 0xc000);
	CHECK_EQ_INT(img.phoff, 52);
	CHECK_EQ_INT(img.phnum, 2);
	CHECK_EQ_INT(img.is_dynamic, 0);
	CHECK_EQ_INT(img.has_interp, 1);
	CHECK_EQ_INT(img.is_android, 1);
	CHECK_STR(img.interp, "/system/bin/linker");
	CHECK_EQ_INT(img.n_load, 1);
	CHECK_EQ_INT(img.loads[0].vaddr, 0x8000);

	/* 2. PIE (ET_DYN). */
	n = build_image(buf, sizeof(buf), 3 /* ET_DYN */, "/system/bin/linker");
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), 0);
	CHECK_EQ_INT(img.is_dynamic, 1);

	/* 3. Non-Android interp -> not android. */
	n = build_image(buf, sizeof(buf), 2, "/lib/ld-linux-armhf.so.3");
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), 0);
	CHECK_EQ_INT(img.is_android, 0);
	CHECK_EQ_INT(img.has_interp, 1);

	/* 4. Big-endian rejected. */
	n = build_image(buf, sizeof(buf), 2, "/system/bin/linker");
	buf[5] = 2;
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), -1);

	/* 5. ELFCLASS64 rejected. */
	n = build_image(buf, sizeof(buf), 2, "/system/bin/linker");
	buf[4] = 2;
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), -1);

	/* 6. Wrong machine rejected. */
	n = build_image(buf, sizeof(buf), 2, "/system/bin/linker");
	wr16(buf + 18, 3); /* EM_386 */
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), -1);

	/* 7. Bad magic rejected. */
	n = build_image(buf, sizeof(buf), 2, "/system/bin/linker");
	buf[1] = 'X';
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), -1);

	/* 8. Bad type rejected. */
	n = build_image(buf, sizeof(buf), 1 /* ET_REL */, "/system/bin/linker");
	CHECK_EQ_INT(tb_elf32_parse(buf, n, &img), -1);

	/* 9. Truncated program-header table rejected. */
	n = build_image(buf, sizeof(buf), 2, "/system/bin/linker");
	CHECK_EQ_INT(tb_elf32_parse(buf, 60, &img), -1);
}
