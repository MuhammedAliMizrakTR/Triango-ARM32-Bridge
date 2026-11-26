/* SPDX-License-Identifier: MIT */
#include "triango/elf32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

int tb_elf32_validate(const void *hdr, size_t len)
{
	const uint8_t *b = hdr;

	if (len < TB_EHDR_SIZE)
		return -1;
	if (b[0] != TB_ELFMAG0 || b[1] != TB_ELFMAG1 || b[2] != TB_ELFMAG2 ||
	    b[3] != TB_ELFMAG3)
		return -1;
	if (b[4] != TB_ELFCLASS32)
		return -1;
	if (b[5] != TB_ELFDATA2LSB)
		return -1;
	if (b[6] != TB_EV_CURRENT)
		return -1;
	{
		uint16_t type = rd16(b + 16);
		uint16_t machine = rd16(b + 18);

		if (type != TB_ET_EXEC && type != TB_ET_DYN)
			return -1;
		if (machine != TB_EM_ARM)
			return -1;
	}
	return 0;
}

static int parse_phdrs(const uint8_t *buf, size_t len, tb_image *out)
{
	uint32_t phoff = out->phoff;
	uint32_t phentsize = out->phentsize;
	uint32_t phnum = out->phnum;
	uint32_t i;

	if (!phoff || !phentsize || !phnum)
		return -1;
	if (phentsize < TB_PHDR_SIZE)
		return -1;
	if (phnum > 512) /* sanity cap */
		return -1;
	if ((uint64_t)phoff + (uint64_t)phnum * phentsize > len)
		return -1;

	for (i = 0; i < phnum; i++) {
		const uint8_t *p = buf + phoff + (size_t)i * phentsize;
		uint32_t type = rd32(p + 0);

		if (type == TB_PT_LOAD && out->n_load < TB_MAX_LOADS) {
			tb_segment *s = &out->loads[out->n_load++];

			s->offset = rd32(p + 4);
			s->vaddr = rd32(p + 8);
			s->filesz = rd32(p + 16);
			s->memsz = rd32(p + 20);
			s->flags = rd32(p + 24);
		} else if (type == TB_PT_INTERP) {
			uint32_t off = rd32(p + 4);
			uint32_t size = rd32(p + 16);
			size_t n;

			if (size == 0 || off >= len)
				return -1;
			n = size;
			if (n > TB_INTERP_MAX - 1)
				n = TB_INTERP_MAX - 1;
			if (off + n > len)
				n = len - off;
			memcpy(out->interp, buf + off, n);
			out->interp[n] = '\0';
			out->has_interp = 1;
		}
	}

	if (out->has_interp &&
	    strncmp(out->interp, "/system/bin/linker",
		    strlen("/system/bin/linker")) == 0)
		out->is_android = 1;
	return 0;
}

int tb_elf32_parse(const void *buf, size_t len, tb_image *out)
{
	const uint8_t *b = buf;

	if (!buf || !out)
		return -1;
	if (tb_elf32_validate(buf, len) != 0)
		return -1;

	memset(out, 0, sizeof(*out));
	out->entry = rd32(b + 24);
	out->phoff = rd32(b + 28);
	out->phentsize = rd16(b + 42);
	out->phnum = rd16(b + 44);
	out->is_dynamic = (rd16(b + 16) == TB_ET_DYN);

	if (parse_phdrs(b, len, out) != 0)
		return -1;
	return 0;
}

int tb_elf32_parse_file(const char *path, tb_image *out, char *err, size_t errlen)
{
	uint8_t ehdr[TB_EHDR_SIZE];
	uint8_t *phdrs = NULL;
	int fd = -1;
	int rc = -1;
	uint32_t phoff, phentsize, phnum;
	size_t phsz;

	if (!path || !out)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (err)
			snprintf(err, errlen, "cannot open '%s': %s", path,
				 strerror(errno));
		return -1;
	}

	if (read(fd, ehdr, sizeof(ehdr)) != (ssize_t)sizeof(ehdr)) {
		if (err)
			snprintf(err, errlen, "cannot read ELF header of '%s'",
				 path);
		goto out;
	}
	if (tb_elf32_validate(ehdr, sizeof(ehdr)) != 0) {
		if (err)
			snprintf(err, errlen,
				 "'%s' is not a little-endian ELF32 ARM binary",
				 path);
		goto out;
	}

	phoff = rd32(ehdr + 28);
	phentsize = rd16(ehdr + 42);
	phnum = rd16(ehdr + 44);
	if (!phoff || !phentsize || !phnum || phentsize < TB_PHDR_SIZE ||
	    phnum > 512) {
		if (err)
			snprintf(err, errlen, "'%s': bad program headers",
				 path);
		goto out;
	}
	phsz = (size_t)phentsize * phnum;
	phdrs = malloc(phsz);
	if (!phdrs) {
		if (err)
			snprintf(err, errlen, "out of memory");
		goto out;
	}
	if (lseek(fd, (off_t)phoff, SEEK_SET) < 0 ||
	    read(fd, phdrs, phsz) != (ssize_t)phsz) {
		if (err)
			snprintf(err, errlen,
				 "'%s': cannot read program header table",
				 path);
		goto out;
	}

	memset(out, 0, sizeof(*out));
	out->entry = rd32(ehdr + 24);
	out->phoff = phoff;
	out->phentsize = phentsize;
	out->phnum = phnum;
	out->is_dynamic = (rd16(ehdr + 16) == TB_ET_DYN);

	{
		/*
		 * Walk the PHs using a small synthetic buffer approach:
		 * handle PT_LOAD directly, and read PT_INTERP content from
		 * the file at its p_offset.
		 */
		uint32_t i;

		for (i = 0; i < phnum; i++) {
			const uint8_t *p = phdrs + (size_t)i * phentsize;
			uint32_t type = rd32(p + 0);

			if (type == TB_PT_LOAD && out->n_load < TB_MAX_LOADS) {
				tb_segment *s = &out->loads[out->n_load++];

				s->offset = rd32(p + 4);
				s->vaddr = rd32(p + 8);
				s->filesz = rd32(p + 16);
				s->memsz = rd32(p + 20);
				s->flags = rd32(p + 24);
			} else if (type == TB_PT_INTERP) {
				uint32_t off = rd32(p + 4);
				uint32_t size = rd32(p + 16);
				size_t n = size;

				if (n == 0 || n > TB_INTERP_MAX - 1)
					n = TB_INTERP_MAX - 1;
				if (lseek(fd, (off_t)off, SEEK_SET) < 0 ||
				    read(fd, out->interp, n) != (ssize_t)n) {
					if (err)
						snprintf(err, errlen,
							 "'%s': cannot read PT_INTERP",
							 path);
					goto out;
				}
				out->interp[n] = '\0';
				out->has_interp = 1;
			}
		}
	}

	if (out->has_interp &&
	    strncmp(out->interp, "/system/bin/linker",
		    strlen("/system/bin/linker")) == 0)
		out->is_android = 1;
	rc = 0;
out:
	free(phdrs);
	if (fd >= 0)
		close(fd);
	return rc;
}
