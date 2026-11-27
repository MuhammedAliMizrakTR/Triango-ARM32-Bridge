/* SPDX-License-Identifier: MIT */
/*
 * Minimal ELF classifier: is this a 32-bit ARM shared object?
 * Used by NativeBridgeCallbacks.isSupported to gate arm32 libraries.
 */
#include "triango/nb.h"

#include <stdio.h>
#include <string.h>

#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EM_ARM 40

int triango_nb_is_arm32_elf(const char *path)
{
	unsigned char hdr[20];
	uint16_t machine;
	FILE *f;

	if (!path)
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	if (hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
		return 0;
	if (hdr[EI_CLASS] != ELFCLASS32)
		return 0;
	if (hdr[EI_DATA] != ELFDATA2LSB)
		return 0;
	memcpy(&machine, hdr + 18, 2);
	return machine == EM_ARM;
}
