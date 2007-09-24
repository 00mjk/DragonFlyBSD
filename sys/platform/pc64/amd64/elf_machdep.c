/*-
 * Copyright 1996-1998 John D. Polstra.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/i386/i386/elf_machdep.c,v 1.8 1999/12/21 11:14:02 eivind Exp $
 * $DragonFly: src/sys/platform/pc64/amd64/elf_machdep.c,v 1.2 2007/09/24 03:24:45 yanyh Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/linker.h>
#include <machine/elf.h>

/* Process one elf relocation with addend. */
int
elf_reloc(linker_file_t lf, const void *data, int type, const char *sym)
{
	Elf_Addr relocbase = (Elf_Addr) lf->address;
	Elf_Addr *where;
	Elf_Addr addr;
	Elf_Addr addend;
	Elf_Word rtype;
	caddr_t caddr;
	const Elf_Rel *rel;
	const Elf_Rela *rela;

	switch (type) {
	case ELF_RELOC_REL:
		rel = (const Elf_Rel *)data;
		where = (Elf_Addr *) (relocbase + rel->r_offset);
		addend = *where;
		rtype = ELF_R_TYPE(rel->r_info);
		break;
	case ELF_RELOC_RELA:
		rela = (const Elf_Rela *)data;
		where = (Elf_Addr *) (relocbase + rela->r_offset);
		addend = rela->r_addend;
		rtype = ELF_R_TYPE(rela->r_info);
		break;
	default:
		panic("unknown reloc type %d\n", type);
	}

	switch (rtype) {

		case R_X86_64_NONE:	/* none */
			break;

		case R_X86_64_64:		/* S + A */
			if (sym == NULL)
				return -1;
			if (linker_file_lookup_symbol(lf, sym, 1, &caddr) != 0)
				return -1;
			addr = (Elf_Addr)caddr;
			addr += addend;
			if (*where != addr)
				*where = addr;
			break;

		case R_X86_64_PC32:	/* S + A - P */
			if (sym == NULL)
				return -1;
			if (linker_file_lookup_symbol(lf, sym, 1, &caddr) != 0)
				return -1;
			addr = (Elf_Addr)caddr;
			addr += addend - (Elf_Addr)where;
			if (*where != addr)
				*where = addr;
			break;

		case R_X86_64_COPY:	/* none */
			/*
			 * There shouldn't be copy relocations in kernel
			 * objects.
			 */
			kprintf("kldload: unexpected R_COPY relocation\n");
			return -1;
			break;

		case R_X86_64_GLOB_DAT:	/* S */
			if (sym == NULL)
				return -1;
			if (linker_file_lookup_symbol(lf, sym, 1, &caddr) != 0)
				return -1;
			addr = (Elf_Addr)caddr;
			if (*where != addr)
				*where = addr;
			break;

		case R_X86_64_RELATIVE:	/* B + A */
			addr = relocbase + addend;
			if (*where != addr)
				*where = addr;
			break;

		default:
			kprintf("kldload: unexpected relocation type %d\n",
			       rtype);
			return -1;
	}
	return(0);
}
