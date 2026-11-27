/* SPDX-License-Identifier: MIT */
/*
 * ELF32 EM_ARM shared-object loader.
 *
 * One contiguous low-4GB image per module (PT_LOAD segments copied into a
 * single mapping), then DT_REL relocations:
 *   R_ARM_ABS32 / R_ARM_GLOB_DAT / R_ARM_JUMP_SLOT : sym + addend
 *   R_ARM_RELATIVE                                  : base + addend
 * Undefined symbols bind to host-backed shims (memcpy, malloc, ...).
 */
#include "engine_internal.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2u
#define R_ARM_GLOB_DAT 21u
#define R_ARM_JUMP_SLOT 22u
#define R_ARM_RELATIVE 23u
#endif

typedef struct {
	char name[64];
	uint32_t value; /* guest VA (bit0 set for Thumb funcs) */
	uint32_t size;
} t32_sym;

typedef struct {
	t32_module pub;
	char path[256];
	t32_sym *syms;
	int nsyms;
} t32_mod_priv;

static void seterr(char *err, size_t errlen, const char *msg, const char *a)
{
	if (err && errlen)
		snprintf(err, errlen, msg, a ? a : "");
}

static uint32_t find_sym(const t32_mod_priv *m, const char *name)
{
	int i;

	for (i = 0; i < m->nsyms; i++)
		if (!strcmp(m->syms[i].name, name))
			return m->syms[i].value;
	return 0;
}

uint32_t t32_elf_symbol(const t32_module *m, const char *name)
{
	if (!m || !m->priv || !name)
		return 0;
	return find_sym((const t32_mod_priv *)m->priv, name);
}

int t32_elf_load(t32_space *mem, const char *path, t32_module *out,
		 const t32_module *deps, int ndeps, char *err, size_t errlen)
{
	FILE *f = NULL;
	Elf32_Ehdr eh;
	Elf32_Phdr *ph = NULL;
	Elf32_Dyn *dyn = NULL;
	Elf32_Sym *symtab = NULL;
	const char *strtab = NULL;
	Elf32_Rel *rels = NULL;
	uint32_t nrels = 0, nsym = 0;
	uint32_t min_va = 0xFFFFFFFFu, max_va = 0;
	uint32_t base = 0, delta = 0;
	uint8_t *image = NULL;
	t32_mod_priv *m;
	int i, j;

	if (err && errlen)
		err[0] = 0;
	m = calloc(1, sizeof(*m));
	if (!m) {
		seterr(err, errlen, "out of memory%s", NULL);
		return -1;
	}
	snprintf(m->path, sizeof(m->path), "%s", path);

	f = fopen(path, "rb");
	if (!f) {
		seterr(err, errlen, "cannot open %s", path);
		goto fail;
	}
	if (fread(&eh, sizeof(eh), 1, f) != 1 ||
	    memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
	    eh.e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh.e_machine != EM_ARM ||
	    (eh.e_type != ET_DYN && eh.e_type != ET_EXEC)) {
		seterr(err, errlen, "not an ELF32 ARM DYN/EXEC: %s", path);
		goto fail;
	}
	ph = calloc(eh.e_phnum, sizeof(*ph));
	if (!ph ||
	    fseek(f, (long)eh.e_phoff, SEEK_SET) != 0 ||
	    fread(ph, sizeof(*ph), eh.e_phnum, f) != eh.e_phnum) {
		seterr(err, errlen, "bad program headers: %s", path);
		goto fail;
	}

	for (i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < min_va)
			min_va = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > max_va)
			max_va = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (min_va > max_va) {
		seterr(err, errlen, "no PT_LOAD: %s", path);
		goto fail;
	}
	min_va &= ~0xFFFu;
	max_va = (max_va + 0xFFFu) & ~0xFFFu;

	if (eh.e_type == ET_EXEC) {
		image = t32_map_fixed(mem, min_va, max_va - min_va, NULL, 0);
		if (!image) {
			seterr(err, errlen, "cannot map fixed: %s", path);
			goto fail;
		}
		delta = 0;
		base = min_va;
	} else {
		for (j = 0; j < 8; j++) {
			base = 0x30000000u + (uint32_t)j * 0x08000000u;
			image = t32_map_fixed(mem, base, max_va - min_va,
					      NULL, 0);
			if (image)
				break;
		}
		if (!image) {
			seterr(err, errlen, "cannot place image: %s", path);
			goto fail;
		}
		delta = base - min_va;
	}

	/* copy segments into the image (image starts at guest base == min_va+delta) */
	for (i = 0; i < eh.e_phnum; i++) {
		uint8_t *dst;

		if (ph[i].p_type != PT_LOAD)
			continue;
		dst = image + (ph[i].p_vaddr - min_va);
		if (fseek(f, (long)ph[i].p_offset, SEEK_SET) != 0 ||
		    fread(dst, 1, ph[i].p_filesz, f) != ph[i].p_filesz) {
			seterr(err, errlen, "segment read failed: %s", path);
			goto fail;
		}
	}

	/* PT_DYNAMIC -> symtab / strtab / rel (vaddrs relative to min_va) */
	for (i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_DYNAMIC)
			continue;
		if (fseek(f, (long)ph[i].p_offset, SEEK_SET) != 0)
			break;
		dyn = calloc(ph[i].p_memsz / sizeof(Elf32_Dyn) + 1,
			     sizeof(Elf32_Dyn));
		if (!dyn)
			goto fail;
		if (fread(dyn, sizeof(Elf32_Dyn),
			  ph[i].p_memsz / sizeof(Elf32_Dyn), f) !=
		    ph[i].p_memsz / sizeof(Elf32_Dyn)) {
			seterr(err, errlen, "dynamic read failed: %s", path);
			goto fail;
		}
		break;
	}
	if (dyn) {
		uint32_t symva = 0, strva = 0, relva = 0;

		for (i = 0; dyn[i].d_tag != DT_NULL; i++) {
			switch (dyn[i].d_tag) {
			case DT_SYMTAB: symva = dyn[i].d_un.d_ptr; break;
			case DT_STRTAB: strva = dyn[i].d_un.d_ptr; break;
			case DT_REL: relva = dyn[i].d_un.d_ptr; break;
			case DT_RELSZ:
				nrels = dyn[i].d_un.d_val / sizeof(Elf32_Rel);
				break;
			case DT_HASH: {
				uint32_t *h = (uint32_t *)(image +
					(dyn[i].d_un.d_ptr - min_va));

				nsym = h[1];
				break;
			}
			default: break;
			}
		}
		if (symva && strva) {
			symtab = (Elf32_Sym *)(image + (symva - min_va));
			strtab = (const char *)(image + (strva - min_va));
			if (!nsym)
				nsym = 256;
			m->syms = calloc(nsym, sizeof(t32_sym));
			if (!m->syms)
				goto fail;
			for (j = 0; (uint32_t)j < nsym; j++) {
				const char *nm;
				uint32_t ty;

				if (!symtab[j].st_name || !strtab)
					continue;
				if (symtab[j].st_shndx == SHN_UNDEF)
					continue;
				ty = ELF32_ST_TYPE(symtab[j].st_info);
				if (ty != STT_FUNC && ty != STT_OBJECT)
					continue;
				nm = strtab + symtab[j].st_name;
				snprintf(m->syms[m->nsyms].name,
					 sizeof(m->syms[0].name), "%s", nm);
				m->syms[m->nsyms].value = symtab[j].st_value +
							 delta;
				m->syms[m->nsyms].size = symtab[j].st_size;
				m->nsyms++;
			}
			if (relva && nrels)
				rels = (Elf32_Rel *)(image + (relva - min_va));
		}
	}

	/* apply relocations */
	if (rels) {
		for (i = 0; (uint32_t)i < nrels; i++) {
			uint32_t type = ELF32_R_TYPE(rels[i].r_info);
			uint32_t symi = ELF32_R_SYM(rels[i].r_info);
			uint32_t *where = (uint32_t *)(image +
					(rels[i].r_offset - min_va));
			uint32_t symval = 0;

			if (type == R_ARM_RELATIVE) {
				*where = *where + delta;
				continue;
			}
			if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
			    type != R_ARM_JUMP_SLOT) {
				seterr(err, errlen, "unsupported reloc type%s",
				       NULL);
				goto fail;
			}
			if (symi && symtab && strtab) {
				const char *nm = strtab + symtab[symi].st_name;

				if (symtab[symi].st_shndx != SHN_UNDEF) {
					symval = symtab[symi].st_value + delta;
				} else {
					int d;

					for (d = 0; d < ndeps && !symval; d++)
						symval = t32_elf_symbol(&deps[d],
									nm);
					if (!symval)
						symval = t32_shim_find(mem, nm);
					if (!symval) {
						char buf[160];

						snprintf(buf, sizeof(buf),
							 "undefined symbol: %s",
							 nm);
						seterr(err, errlen, "%s", buf);
						goto fail;
					}
				}
			}
			if (type == R_ARM_ABS32)
				*where = symval + *where;
			else
				*where = symval;
		}
	}

	m->pub.base = base;
	m->pub.entry = eh.e_entry ? eh.e_entry + delta : 0;
	m->pub.mem = mem;
	m->pub.priv = m;
	free(ph);
	free(dyn);
	fclose(f);
	*out = m->pub;
	return 0;

fail:
	free(ph);
	free(dyn);
	if (f)
		fclose(f);
	free(m->syms);
	free(m);
	return -1;
}

void t32_elf_free(t32_module *m)
{
	t32_mod_priv *mp;

	if (!m || !m->priv)
		return;
	mp = (t32_mod_priv *)m->priv;
	free(mp->syms);
	free(mp);
	m->priv = NULL;
}
