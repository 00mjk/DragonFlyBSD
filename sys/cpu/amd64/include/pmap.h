/*
 * Copyright (c) 2003 Matthew Dillon <dillon@backplane.com>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/sys/cpu/amd64/include/pmap.h,v 1.2 2007/09/23 04:29:30 yanyh Exp $
 */
#ifndef _CPU_PMAP_H_
#define	_CPU_PMAP_H_

/*
 * A four level page table is implemented by the amd64 hardware.  Each
 * page table represents 9 address bits and eats 4KB of space.  There are
 * 512 8-byte entries in each table.  The last page table contains PTE's
 * representing 4K pages (12 bits of address space).
 *
 * The page tables are named:
 *	PML4	Represents 512GB per entry (256TB total)	LEVEL4
 *	PDP	Represents 1GB per entry			LEVEL3
 *	PDE	Represents 2MB per entry			LEVEL2
 *	PTE	Represents 4KB per entry			LEVEL1
 *
 * PG_PAE	PAE 2MB extension.  In the PDE.  If 0 there is another level
 *		of page table and PG_D and PG_G are ignored.  If 1 this is
 *		the terminating page table and PG_D and PG_G apply.
 *
 * PG_PWT	Page write through.  If 1 caching is disabled for data
 *		represented by the page.
 * PG_PCD	Page Cache Disable.  If 1 the page table entry will not
 *		be cached in the data cache.
 *
 * Each entry in the PML4 table represents a 512GB VA space.  We use a fixed
 * PML4 and adjust entries within it to switch user spaces.
 */

#define PG_V		0x0001LL		/* P	Present		*/
#define PG_RW		0x0002LL		/* R/W  Writable	*/
#define PG_U		0x0004LL		/* U/S  User		*/
#define PG_PWT		0x0008LL		/* PWT  Page Write Through */
#define PG_PCD		0x0010LL		/* PCD  Page Cache Disable */
#define PG_A		0x0020LL		/* A    Accessed	*/
#define PG_D		0x0040LL		/* D 	Dirty	(pte only) */
#define PG_PS		0x0080LL		/* PAT 		(pte only) */
#define PG_G		0x0100LL		/* G 	Global	(pte only) */
#define PG_USR0		0x0200LL		/* available to os */
#define PG_USR1		0x0400LL		/* available to os */
#define PG_USR2		0x0800LL		/* available to os */
#define PG_PTE_PAT	PG_PAE			/* PAT bit for 4K pages */
#define PG_PDE_PAT	0x1000LL		/* PAT bit for 2M pages */
#define PG_FRAME	0x000000FFFFFF0000LL	/* 40 bit phys address */
#define PG_PHYSRESERVED	0x000FFF0000000000LL	/* reserved for future PA */
#define PG_USR3		0x0010000000000000LL	/* avilable to os */

/*
 * OS assignments
 */
#define PG_W		PG_USR0			/* Wired 	*/
#define	PG_MANAGED	PG_USR1			/* Managed 	*/
#define	PG_PROT		(PG_RW|PG_U)		/* all protection bits . */
#define PG_N		(PG_PWT|PG_PCD)		/* Non-cacheable */

/*
 * Page Protection Exception bits
 */

#define PGEX_P		0x01	/* Protection violation vs. not present */
#define PGEX_W		0x02	/* during a Write cycle */
#define PGEX_U		0x04	/* access from User mode (UPL) */

#define PGEX_MAILBOX	0x40
#define PGEX_FPFAULT	0x80

/*
 * User space is limited to one PML4 entry (512GB).  Kernel space is also
 * limited to one PML4 entry.  Other PML4 entries are used to map foreign
 * user spaces into KVM.  Typically each cpu in the system reserves two
 * PML4 entries for private use.
 */
#define UVA_MAXMEM	(512LL*1024*1024*1024)
#define KVA_MAXMEM	(512LL*1024*1024*1024)

/*
 * Pte related macros.  This is complicated by having to deal with
 * the sign extension of the 48th bit.
 */
#define KVADDR(l4, l3, l2, l1) ( \
	((unsigned long)-1 << 47) | \
	((unsigned long)(l4) << PML4SHIFT) | \
	((unsigned long)(l3) << PDPSHIFT) | \
	((unsigned long)(l2) << PDRSHIFT) | \
	((unsigned long)(l1) << PAGE_SHIFT))

#define UVADDR(l4, l3, l2, l1) ( \
	((unsigned long)(l4) << PML4SHIFT) | \
	((unsigned long)(l3) << PDPSHIFT) | \
	((unsigned long)(l2) << PDRSHIFT) | \
	((unsigned long)(l1) << PAGE_SHIFT))


#define NKPML4E		1
#define NKPDPE		1
#define NKPDE		(NKPDPE*NPDEPG)

#define NUPML4E		(NPML4EPG/2)
#define NUPDPE		(NUPML4E*NPDPEPG)
#define NUPDE		(NUPDPE*NPDEPG)


/*
 * The *PTDI values control the layout of virtual memory
 *
 * XXX This works for now, but I am not real happy with it, I'll fix it
 * right after I fix locore.s and the magic 28K hole
 *
 * SMP_PRIVPAGES: The per-cpu address space is 0xff80000 -> 0xffbfffff
 */
#define	APTDPTDI	(NPDEPG-1)	/* alt ptd entry that points to APTD */
#define MPPTDI		(APTDPTDI-1)	/* per cpu ptd entry */
#define	KPTDI		(MPPTDI-NKPDE)	/* start of kernel virtual pde's */
#define	PTDPTDI		(KPTDI-1)	/* ptd entry that points to ptd! */
#define	UMAXPTDI	(PTDPTDI-1)	/* ptd entry for user space end */
#define	UMAXPTEOFF	(NPTEPG)	/* pte entry for user space end */

#define KPML4I		(NPML4EPG-1)

#define KPDPI		(NPDPEPG-2)

/*
 * XXX doesn't really belong here I guess...
 */
#define ISA_HOLE_START    0xa0000
#define ISA_HOLE_LENGTH (0x100000-ISA_HOLE_START)

#ifndef LOCORE

#include <sys/queue.h>

/*
 * Address of current and alternate address space page table maps
 * and directories.
 */
#ifdef _KERNEL
extern pt_entry_t PTmap[], APTmap[], Upte;
extern pd_entry_t PTD[], APTD[], PTDpde, APTDpde, Upde;

extern pd_entry_t IdlePTD;	/* physical address of "Idle" state directory */
#endif

#ifdef _KERNEL
/*
 * virtual address to page table entry and
 * to physical address. Likewise for alternate address space.
 * Note: these work recursively, thus vtopte of a pte will give
 * the corresponding pde that in turn maps it.
 */
#define	vtopte(va)	(PTmap + i386_btop(va))

#define	avtopte(va)	(APTmap + i386_btop(va))

/*
 *	Routine:	pmap_kextract
 *	Function:
 *		Extract the physical page address associated
 *		kernel virtual address.
 */
static __inline vm_paddr_t
pmap_kextract(vm_offset_t va)
{
	vm_paddr_t pa;

	if ((pa = (vm_offset_t) PTD[va >> PDRSHIFT]) & PG_PS) {
		pa = (pa & ~(NBPDR - 1)) | (va & (NBPDR - 1));
	} else {
		pa = *(vm_offset_t *)vtopte(va);
		pa = (pa & PG_FRAME) | (va & PAGE_MASK);
	}
	return pa;
}

/*
 * XXX
 */
#define	vtophys(va)	pmap_kextract(((vm_offset_t)(va)))
#define	vtophys_pte(va)	((pt_entry_t)pmap_kextract(((vm_offset_t)(va))))

#define	avtophys(va)	(((vm_offset_t) (*avtopte(va))&PG_FRAME) | ((vm_offset_t)(va) & PAGE_MASK))

#endif

/*
 * Pmap stuff
 */
struct	pv_entry;

struct md_page {
	int pv_list_count;
	TAILQ_HEAD(,pv_entry)	pv_list;
};

/*
 * Each machine dependent implementation is expected to
 * keep certain statistics.  They may do this anyway they
 * so choose, but are expected to return the statistics
 * in the following structure.
 */
struct pmap_statistics {
	long resident_count;    /* # of pages mapped (total) */
	long wired_count;       /* # of pages wired */
};
typedef struct pmap_statistics *pmap_statistics_t;

struct vm_object;
struct vm_page;

struct pmap {
	pd_entry_t		*pm_pdir;	/* KVA of page directory */
	struct vm_object	*pm_pteobj;	/* Container for pte's */
	TAILQ_HEAD(,pv_entry)	pm_pvlist;	/* list of mappings in pmap */
	int			pm_count;	/* reference count */
	cpumask_t		pm_active;	/* active on cpus */
	struct pmap_statistics	pm_stats;	/* pmap statistics */
	struct	vm_page		*pm_ptphint;	/* pmap ptp hint */
};

#define pmap_resident_count(pmap) (pmap)->pm_stats.resident_count

typedef struct pmap	*pmap_t;

#ifdef _KERNEL
extern	struct pmap		kernel_pmap;
#endif

/*
 * For each vm_page_t, there is a list of all currently valid virtual
 * mappings of that page.  An entry is a pv_entry_t, the list is pv_list
 */
typedef struct pv_entry {
	pmap_t		pv_pmap;	/* pmap where mapping lies */
	vm_offset_t	pv_va;		/* virtual address for mapping */
	TAILQ_ENTRY(pv_entry)	pv_list;
	TAILQ_ENTRY(pv_entry)	pv_plist;
	struct vm_page	*pv_ptem;	/* VM page for pte */
} *pv_entry_t;

#ifdef	_KERNEL

#define NPPROVMTRR		8
#define PPRO_VMTRRphysBase0	0x200
#define PPRO_VMTRRphysMask0	0x201
struct ppro_vmtrr {
	u_int64_t base, mask;
};
extern struct ppro_vmtrr PPro_vmtrr[NPPROVMTRR];

extern caddr_t	CADDR1;
extern pt_entry_t *CMAP1;
extern vm_paddr_t avail_end;
extern vm_paddr_t avail_start;
extern vm_offset_t clean_eva;
extern vm_offset_t clean_sva;
extern char *ptvmmap;		/* poor name! */
extern vm_offset_t virtual_avail;

void	pmap_bootstrap ( vm_paddr_t, vm_paddr_t);
pmap_t	pmap_kernel (void);
void	*pmap_mapdev (vm_paddr_t, vm_size_t);
void	pmap_unmapdev (vm_offset_t, vm_size_t);
unsigned *pmap_pte (pmap_t, vm_offset_t) __pure2;
struct vm_page *pmap_use_pt (pmap_t, vm_offset_t);
#ifdef SMP
void	pmap_set_opt (void);
#endif

#endif /* _KERNEL */

#endif /* !LOCORE */

#endif /* !_CPU_PMAP_H_ */
