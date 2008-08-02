/*
 * Copyright (c) 1997, Stefan Esser <se@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
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
 * $FreeBSD: src/sys/pci/pci.c,v 1.141.2.15 2002/04/30 17:48:18 tmm Exp $
 * $DragonFly: src/sys/bus/pci/pci.c,v 1.53 2008/08/02 01:14:40 dillon Exp $
 *
 */

#include "opt_bus.h"
#include "opt_pci.h"

#include "opt_compat_oldpci.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/buf.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>

#include <sys/bus.h>
#include <sys/rman.h>
#include <machine/smp.h>
#include "pci_cfgreg.h"

#include <sys/pciio.h>
#include "pcireg.h"
#include "pcivar.h"
#include "pci_private.h"

#include "pcib_if.h"

devclass_t	pci_devclass;
const char	*pcib_owner;

static void		pci_read_capabilities(device_t dev, pcicfgregs *cfg);
static int		pcie_slotimpl(const pcicfgregs *);

struct pci_quirk {
	u_int32_t devid;	/* Vendor/device of the card */
	int	type;
#define PCI_QUIRK_MAP_REG	1 /* PCI map register in weird place */
	int	arg1;
	int	arg2;
};

struct pci_quirk pci_quirks[] = {
	/*
	 * The Intel 82371AB and 82443MX has a map register at offset 0x90.
	 */
	{ 0x71138086, PCI_QUIRK_MAP_REG,	0x90,	 0 },
	{ 0x719b8086, PCI_QUIRK_MAP_REG,	0x90,	 0 },
	/* As does the Serverworks OSB4 (the SMBus mapping register) */
	{ 0x02001166, PCI_QUIRK_MAP_REG,	0x90,	 0 },

	{ 0 }
};

/* map register information */
#define PCI_MAPMEM	0x01	/* memory map */
#define PCI_MAPMEMP	0x02	/* prefetchable memory map */
#define PCI_MAPPORT	0x04	/* port map */

static STAILQ_HEAD(devlist, pci_devinfo) pci_devq;
u_int32_t pci_numdevs = 0;
static u_int32_t pci_generation = 0;

device_t
pci_find_bsf(u_int8_t bus, u_int8_t slot, u_int8_t func)
{
	struct pci_devinfo *dinfo;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links) {
		if ((dinfo->cfg.bus == bus) &&
		    (dinfo->cfg.slot == slot) &&
		    (dinfo->cfg.func == func)) {
			return (dinfo->cfg.dev);
		}
	}

	return (NULL);
}

device_t
pci_find_device(u_int16_t vendor, u_int16_t device)
{
	struct pci_devinfo *dinfo;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links) {
		if ((dinfo->cfg.vendor == vendor) &&
		    (dinfo->cfg.device == device)) {
			return (dinfo->cfg.dev);
		}
	}

	return (NULL);
}

int
pcie_slot_implemented(device_t dev)
{
	struct pci_devinfo *dinfo = device_get_ivars(dev);

	return pcie_slotimpl(&dinfo->cfg);
}

/* return base address of memory or port map */

static u_int32_t
pci_mapbase(unsigned mapreg)
{
	int mask = 0x03;
	if ((mapreg & 0x01) == 0)
		mask = 0x0f;
	return (mapreg & ~mask);
}

/* return map type of memory or port map */

static int
pci_maptype(unsigned mapreg)
{
	static u_int8_t maptype[0x10] = {
		PCI_MAPMEM,		PCI_MAPPORT,
		PCI_MAPMEM,		0,
		PCI_MAPMEM,		PCI_MAPPORT,
		0,			0,
		PCI_MAPMEM|PCI_MAPMEMP,	PCI_MAPPORT,
		PCI_MAPMEM|PCI_MAPMEMP, 0,
		PCI_MAPMEM|PCI_MAPMEMP,	PCI_MAPPORT,
		0,			0,
	};

	return maptype[mapreg & 0x0f];
}

/* return log2 of map size decoded for memory or port map */

static int
pci_mapsize(unsigned testval)
{
	int ln2size;

	testval = pci_mapbase(testval);
	ln2size = 0;
	if (testval != 0) {
		while ((testval & 1) == 0)
		{
			ln2size++;
			testval >>= 1;
		}
	}
	return (ln2size);
}

/* return log2 of address range supported by map register */

static int
pci_maprange(unsigned mapreg)
{
	int ln2range = 0;
	switch (mapreg & 0x07) {
	case 0x00:
	case 0x01:
	case 0x05:
		ln2range = 32;
		break;
	case 0x02:
		ln2range = 20;
		break;
	case 0x04:
		ln2range = 64;
		break;
	}
	return (ln2range);
}

/* adjust some values from PCI 1.0 devices to match 2.0 standards ... */

static void
pci_fixancient(pcicfgregs *cfg)
{
	if (cfg->hdrtype != 0)
		return;

	/* PCI to PCI bridges use header type 1 */
	if (cfg->baseclass == PCIC_BRIDGE && cfg->subclass == PCIS_BRIDGE_PCI)
		cfg->hdrtype = 1;
}

/* read config data specific to header type 1 device (PCI to PCI bridge) */

static void *
pci_readppb(device_t pcib, int b, int s, int f)
{
	pcih1cfgregs *p;

	p = kmalloc(sizeof (pcih1cfgregs), M_DEVBUF, M_WAITOK | M_ZERO);

	p->secstat = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_SECSTAT_1, 2);
	p->bridgectl = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_BRIDGECTL_1, 2);

	p->seclat = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_SECLAT_1, 1);

	p->iobase = PCI_PPBIOBASE (PCIB_READ_CONFIG(pcib, b, s, f,
						    PCIR_IOBASEH_1, 2),
				   PCIB_READ_CONFIG(pcib, b, s, f,
				   		    PCIR_IOBASEL_1, 1));
	p->iolimit = PCI_PPBIOLIMIT (PCIB_READ_CONFIG(pcib, b, s, f,
						      PCIR_IOLIMITH_1, 2),
				     PCIB_READ_CONFIG(pcib, b, s, f,
				     		      PCIR_IOLIMITL_1, 1));

	p->membase = PCI_PPBMEMBASE (0,
				     PCIB_READ_CONFIG(pcib, b, s, f,
				     		      PCIR_MEMBASE_1, 2));
	p->memlimit = PCI_PPBMEMLIMIT (0,
				       PCIB_READ_CONFIG(pcib, b, s, f,
				       		        PCIR_MEMLIMIT_1, 2));

	p->pmembase = PCI_PPBMEMBASE (
		(pci_addr_t)PCIB_READ_CONFIG(pcib, b, s, f, PCIR_PMBASEH_1, 4),
		PCIB_READ_CONFIG(pcib, b, s, f, PCIR_PMBASEL_1, 2));

	p->pmemlimit = PCI_PPBMEMLIMIT (
		(pci_addr_t)PCIB_READ_CONFIG(pcib, b, s, f,
					     PCIR_PMLIMITH_1, 4),
		PCIB_READ_CONFIG(pcib, b, s, f, PCIR_PMLIMITL_1, 2));

	return (p);
}

/* read config data specific to header type 2 device (PCI to CardBus bridge) */

static void *
pci_readpcb(device_t pcib, int b, int s, int f)
{
	pcih2cfgregs *p;

	p = kmalloc(sizeof (pcih2cfgregs), M_DEVBUF, M_WAITOK | M_ZERO);

	p->secstat = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_SECSTAT_2, 2);
	p->bridgectl = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_BRIDGECTL_2, 2);
	
	p->seclat = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_SECLAT_2, 1);

	p->membase0 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_MEMBASE0_2, 4);
	p->memlimit0 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_MEMLIMIT0_2, 4);
	p->membase1 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_MEMBASE1_2, 4);
	p->memlimit1 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_MEMLIMIT1_2, 4);

	p->iobase0 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_IOBASE0_2, 4);
	p->iolimit0 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_IOLIMIT0_2, 4);
	p->iobase1 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_IOBASE1_2, 4);
	p->iolimit1 = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_IOLIMIT1_2, 4);

	p->pccardif = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_PCCARDIF_2, 4);
	return p;
}

/* extract header type specific config data */

static void
pci_hdrtypedata(device_t pcib, int b, int s, int f, pcicfgregs *cfg)
{
#define REG(n,w)	PCIB_READ_CONFIG(pcib, b, s, f, n, w)
	switch (cfg->hdrtype) {
	case 0:
		cfg->subvendor      = REG(PCIR_SUBVEND_0, 2);
		cfg->subdevice      = REG(PCIR_SUBDEV_0, 2);
		cfg->nummaps	    = PCI_MAXMAPS_0;
		break;
	case 1:
		cfg->subvendor      = REG(PCIR_SUBVEND_1, 2);
		cfg->subdevice      = REG(PCIR_SUBDEV_1, 2);
		cfg->secondarybus   = REG(PCIR_SECBUS_1, 1);
		cfg->subordinatebus = REG(PCIR_SUBBUS_1, 1);
		cfg->nummaps	    = PCI_MAXMAPS_1;
		cfg->hdrspec        = pci_readppb(pcib, b, s, f);
		break;
	case 2:
		cfg->subvendor      = REG(PCIR_SUBVEND_2, 2);
		cfg->subdevice      = REG(PCIR_SUBDEV_2, 2);
		cfg->secondarybus   = REG(PCIR_SECBUS_2, 1);
		cfg->subordinatebus = REG(PCIR_SUBBUS_2, 1);
		cfg->nummaps	    = PCI_MAXMAPS_2;
		cfg->hdrspec        = pci_readpcb(pcib, b, s, f);
		break;
	}
#undef REG
}

/* read configuration header into pcicfgrect structure */

struct pci_devinfo *
pci_read_device(device_t pcib, int b, int s, int f, size_t size)
{
#define REG(n, w)	PCIB_READ_CONFIG(pcib, b, s, f, n, w)

	pcicfgregs *cfg = NULL;
	struct pci_devinfo *devlist_entry;
	struct devlist *devlist_head;

	devlist_head = &pci_devq;

	devlist_entry = NULL;

	if (PCIB_READ_CONFIG(pcib, b, s, f, PCIR_DEVVENDOR, 4) != -1) {

		devlist_entry = kmalloc(size, M_DEVBUF, M_WAITOK | M_ZERO);

		cfg = &devlist_entry->cfg;
		
		cfg->bus		= b;
		cfg->slot		= s;
		cfg->func		= f;
		cfg->vendor		= REG(PCIR_VENDOR, 2);
		cfg->device		= REG(PCIR_DEVICE, 2);
		cfg->cmdreg		= REG(PCIR_COMMAND, 2);
		cfg->statreg		= REG(PCIR_STATUS, 2);
		cfg->baseclass		= REG(PCIR_CLASS, 1);
		cfg->subclass		= REG(PCIR_SUBCLASS, 1);
		cfg->progif		= REG(PCIR_PROGIF, 1);
		cfg->revid		= REG(PCIR_REVID, 1);
		cfg->hdrtype		= REG(PCIR_HDRTYPE, 1);
		cfg->cachelnsz		= REG(PCIR_CACHELNSZ, 1);
		cfg->lattimer		= REG(PCIR_LATTIMER, 1);
		cfg->intpin		= REG(PCIR_INTPIN, 1);
		cfg->intline		= REG(PCIR_INTLINE, 1);

#ifdef APIC_IO
		/*
		 * If using the APIC the intpin is probably wrong, since it
		 * is often setup by the BIOS with the PIC in mind.
		 */
		if (cfg->intpin != 0) {
			int airq;

			airq = pci_apic_irq(cfg->bus, cfg->slot, cfg->intpin);
			if (airq >= 0) {
				/* PCI specific entry found in MP table */
				if (airq != cfg->intline) {
					undirect_pci_irq(cfg->intline);
					cfg->intline = airq;
				}
			} else {
				/* 
				 * PCI interrupts might be redirected to the
				 * ISA bus according to some MP tables. Use the
				 * same methods as used by the ISA devices
				 * devices to find the proper IOAPIC int pin.
				 */
				airq = isa_apic_irq(cfg->intline);
				if ((airq >= 0) && (airq != cfg->intline)) {
					/* XXX: undirect_pci_irq() ? */
					undirect_isa_irq(cfg->intline);
					cfg->intline = airq;
				}
			}
		}
#endif /* APIC_IO */

		cfg->mingnt		= REG(PCIR_MINGNT, 1);
		cfg->maxlat		= REG(PCIR_MAXLAT, 1);

		cfg->mfdev		= (cfg->hdrtype & PCIM_MFDEV) != 0;
		cfg->hdrtype		&= ~PCIM_MFDEV;

		pci_fixancient(cfg);
		pci_hdrtypedata(pcib, b, s, f, cfg);
		pci_read_capabilities(pcib, cfg);

		STAILQ_INSERT_TAIL(devlist_head, devlist_entry, pci_links);

		devlist_entry->conf.pc_sel.pc_bus = cfg->bus;
		devlist_entry->conf.pc_sel.pc_dev = cfg->slot;
		devlist_entry->conf.pc_sel.pc_func = cfg->func;
		devlist_entry->conf.pc_hdr = cfg->hdrtype;

		devlist_entry->conf.pc_subvendor = cfg->subvendor;
		devlist_entry->conf.pc_subdevice = cfg->subdevice;
		devlist_entry->conf.pc_vendor = cfg->vendor;
		devlist_entry->conf.pc_device = cfg->device;

		devlist_entry->conf.pc_class = cfg->baseclass;
		devlist_entry->conf.pc_subclass = cfg->subclass;
		devlist_entry->conf.pc_progif = cfg->progif;
		devlist_entry->conf.pc_revid = cfg->revid;

		pci_numdevs++;
		pci_generation++;
	}
	return (devlist_entry);
#undef REG
}

static int
pci_fixup_nextptr(int *nextptr0)
{
	int nextptr = *nextptr0;

	/* "Next pointer" is only one byte */
	KASSERT(nextptr <= 0xff, ("Illegal next pointer %d\n", nextptr));

	if (nextptr & 0x3) {
		/*
		 * PCI local bus spec 3.0:
		 *
		 * "... The bottom two bits of all pointers are reserved
		 *  and must be implemented as 00b although software must
		 *  mask them to allow for future uses of these bits ..."
		 */
		if (bootverbose) {
			kprintf("Illegal PCI extended capability "
				"offset, fixup 0x%02x -> 0x%02x\n",
				nextptr, nextptr & ~0x3);
		}
		nextptr &= ~0x3;
	}
	*nextptr0 = nextptr;

	if (nextptr < 0x40) {
		if (nextptr != 0) {
			kprintf("Illegal PCI extended capability "
				"offset 0x%02x", nextptr);
		}
		return 0;
	}
	return 1;
}

static void
pci_read_cap_pmgt(device_t pcib, int ptr, pcicfgregs *cfg)
{
#define REG(n, w)	\
	PCIB_READ_CONFIG(pcib, cfg->bus, cfg->slot, cfg->func, n, w)

	struct pcicfg_pmgt *pmgt = &cfg->pmgt;

	if (pmgt->pp_cap)
		return;

	pmgt->pp_cap = REG(ptr + PCIR_POWER_CAP, 2);
	pmgt->pp_status = ptr + PCIR_POWER_STATUS;
	pmgt->pp_pmcsr = ptr + PCIR_POWER_PMCSR;
	/*
	 * XXX
	 * Following way may be used to to test whether
	 * 'data' register exists:
	 * if 'data_select' register of
	 * PCIR_POWER_STATUS(bits[12,9]) is read-only
	 * then 'data' register is _not_ implemented.
	 */
	pmgt->pp_data = 0;

#undef REG
}

static int
pcie_slotimpl(const pcicfgregs *cfg)
{
	const struct pcicfg_expr *expr = &cfg->expr;
	uint16_t port_type;

	/*
	 * Only version 1 can be parsed currently 
	 */
	if ((expr->expr_cap & PCIEM_CAP_VER_MASK) != PCIEM_CAP_VER_1)
		return 0;

	/*
	 * - Slot implemented bit is meaningful iff current port is
	 *   root port or down stream port.
	 * - Testing for root port or down stream port is meanningful
	 *   iff PCI configure has type 1 header.
	 */

	if (cfg->hdrtype != 1)
		return 0;

	port_type = expr->expr_cap & PCIEM_CAP_PORT_TYPE;
	if (port_type != PCIE_ROOT_PORT && port_type != PCIE_DOWN_STREAM_PORT)
		return 0;

	if (!(expr->expr_cap & PCIEM_CAP_SLOT_IMPL))
		return 0;

	return 1;
}

static void
pci_read_cap_expr(device_t pcib, int ptr, pcicfgregs *cfg)
{
#define REG(n, w)	\
	PCIB_READ_CONFIG(pcib, cfg->bus, cfg->slot, cfg->func, n, w)

	struct pcicfg_expr *expr = &cfg->expr;

	expr->expr_ptr = ptr;
	expr->expr_cap = REG(ptr + PCIER_CAPABILITY, 2);

	/*
	 * Only version 1 can be parsed currently 
	 */
	if ((expr->expr_cap & PCIEM_CAP_VER_MASK) != PCIEM_CAP_VER_1)
		return;

	/*
	 * Read slot capabilities.  Slot capabilities exists iff
	 * current port's slot is implemented
	 */
	if (pcie_slotimpl(cfg))
		expr->expr_slotcap = REG(ptr + PCIER_SLOTCAP, 4);

#undef REG
}

static void
pci_read_capabilities(device_t pcib, pcicfgregs *cfg)
{
#define REG(n, w)	\
	PCIB_READ_CONFIG(pcib, cfg->bus, cfg->slot, cfg->func, n, w)

	int nextptr, ptrptr;

	if ((REG(PCIR_STATUS, 2) & PCIM_STATUS_CAPPRESENT) == 0) {
		/* No capabilities */
		return;
	}

	switch (cfg->hdrtype) {
	case 0:
	case 1:
		ptrptr = PCIR_CAP_PTR;
		break;
	case 2:
		ptrptr = PCIR_CAP_PTR_2;
		break;
	default:
		return;		/* No capabilities support */
	}
	nextptr = REG(ptrptr, 1);

	/*
	 * Read capability entries.
	 */
	while (pci_fixup_nextptr(&nextptr)) {
		int ptr = nextptr;

		/* Process this entry */
		switch (REG(ptr, 1)) {
		case PCIY_PMG:		/* PCI power management */
			pci_read_cap_pmgt(pcib, ptr, cfg);
			break;
		case PCIY_PCIX:		/* PCI-X */
			cfg->pcixcap_ptr = ptr;
			break;
		case PCIY_EXPRESS:	/* PCI Express */
			pci_read_cap_expr(pcib, ptr, cfg);
			break;
		default:
			break;
		}

		/* Find the next entry */
		nextptr = REG(ptr + 1, 1);
	}

#undef REG
}

/* free pcicfgregs structure and all depending data structures */

int
pci_freecfg(struct pci_devinfo *dinfo)
{
	struct devlist *devlist_head;

	devlist_head = &pci_devq;

	if (dinfo->cfg.hdrspec != NULL)
		kfree(dinfo->cfg.hdrspec, M_DEVBUF);
	/* XXX this hasn't been tested */
	STAILQ_REMOVE(devlist_head, dinfo, pci_devinfo, pci_links);
	kfree(dinfo, M_DEVBUF);

	/* increment the generation count */
	pci_generation++;

	/* we're losing one device */
	pci_numdevs--;
	return (0);
}


/*
 * PCI power manangement
 */
int
pci_set_powerstate_method(device_t dev, device_t child, int state)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	pcicfgregs *cfg = &dinfo->cfg;
	struct pcicfg_pmgt *pmgt = &cfg->pmgt;
	u_int16_t status;
	int result;

	if (pmgt->pp_cap != 0) {
		status = PCI_READ_CONFIG(dev, child, pmgt->pp_status, 2) & ~PCIM_PSTAT_DMASK;
		result = 0;
		switch (state) {
		case PCI_POWERSTATE_D0:
			status |= PCIM_PSTAT_D0;
			break;
		case PCI_POWERSTATE_D1:
			if (pmgt->pp_cap & PCIM_PCAP_D1SUPP) {
				status |= PCIM_PSTAT_D1;
			} else {
				result = EOPNOTSUPP;
			}
			break;
		case PCI_POWERSTATE_D2:
			if (pmgt->pp_cap & PCIM_PCAP_D2SUPP) {
				status |= PCIM_PSTAT_D2;
			} else {
				result = EOPNOTSUPP;
			}
			break;
		case PCI_POWERSTATE_D3:
			status |= PCIM_PSTAT_D3;
			break;
		default:
			result = EINVAL;
		}
		if (result == 0)
			PCI_WRITE_CONFIG(dev, child, pmgt->pp_status, status, 2);
	} else {
		result = ENXIO;
	}
	return(result);
}

int
pci_get_powerstate_method(device_t dev, device_t child)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	pcicfgregs *cfg = &dinfo->cfg;
	struct pcicfg_pmgt *pmgt = &cfg->pmgt;
	u_int16_t status;
	int result;

	if (pmgt->pp_cap != 0) {
		status = PCI_READ_CONFIG(dev, child, pmgt->pp_status, 2);
		switch (status & PCIM_PSTAT_DMASK) {
		case PCIM_PSTAT_D0:
			result = PCI_POWERSTATE_D0;
			break;
		case PCIM_PSTAT_D1:
			result = PCI_POWERSTATE_D1;
			break;
		case PCIM_PSTAT_D2:
			result = PCI_POWERSTATE_D2;
			break;
		case PCIM_PSTAT_D3:
			result = PCI_POWERSTATE_D3;
			break;
		default:
			result = PCI_POWERSTATE_UNKNOWN;
			break;
		}
	} else {
		/* No support, device is always at D0 */
		result = PCI_POWERSTATE_D0;
	}
	return(result);
}

/*
 * Some convenience functions for PCI device drivers.
 */

static __inline void
pci_set_command_bit(device_t dev, device_t child, u_int16_t bit)
{
    u_int16_t	command;

    command = PCI_READ_CONFIG(dev, child, PCIR_COMMAND, 2);
    command |= bit;
    PCI_WRITE_CONFIG(dev, child, PCIR_COMMAND, command, 2);
}

static __inline void
pci_clear_command_bit(device_t dev, device_t child, u_int16_t bit)
{
    u_int16_t	command;

    command = PCI_READ_CONFIG(dev, child, PCIR_COMMAND, 2);
    command &= ~bit;
    PCI_WRITE_CONFIG(dev, child, PCIR_COMMAND, command, 2);
}

int
pci_enable_busmaster_method(device_t dev, device_t child)
{
    pci_set_command_bit(dev, child, PCIM_CMD_BUSMASTEREN);
    return(0);
}

int
pci_disable_busmaster_method(device_t dev, device_t child)
{
    pci_clear_command_bit(dev, child, PCIM_CMD_BUSMASTEREN);
    return(0);
}

int
pci_enable_io_method(device_t dev, device_t child, int space)
{
    uint16_t command;
    uint16_t bit;
    char *error;

    bit = 0;
    error = NULL;

    switch(space) {
    case SYS_RES_IOPORT:
	bit = PCIM_CMD_PORTEN;
	error = "port";
	break;
    case SYS_RES_MEMORY:
	bit = PCIM_CMD_MEMEN;
	error = "memory";
	break;
    default:
	return(EINVAL);
    }
    pci_set_command_bit(dev, child, bit);
    command = PCI_READ_CONFIG(dev, child, PCIR_COMMAND, 2);
    if (command & bit)
	return(0);
    device_printf(child, "failed to enable %s mapping!\n", error);
    return(ENXIO);
}

int
pci_disable_io_method(device_t dev, device_t child, int space)
{
    uint16_t command;
    uint16_t bit;
    char *error;

    bit = 0;
    error = NULL;

    switch(space) {
    case SYS_RES_IOPORT:
	bit = PCIM_CMD_PORTEN;
	error = "port";
	break;
    case SYS_RES_MEMORY:
	bit = PCIM_CMD_MEMEN;
	error = "memory";
	break;
    default:
	return (EINVAL);
    }
    pci_clear_command_bit(dev, child, bit);
    command = PCI_READ_CONFIG(dev, child, PCIR_COMMAND, 2);
    if (command & bit) {
	device_printf(child, "failed to disable %s mapping!\n", error);
	return (ENXIO);
    }
    return (0);
}

/*
 * This is the user interface to PCI configuration space.
 */
  
static int
pci_open(struct dev_open_args *ap)
{
	if ((ap->a_oflags & FWRITE) && securelevel > 0) {
		return EPERM;
	}
	return 0;
}

static int
pci_close(struct dev_close_args *ap)
{
	return 0;
}

/*
 * Match a single pci_conf structure against an array of pci_match_conf
 * structures.  The first argument, 'matches', is an array of num_matches
 * pci_match_conf structures.  match_buf is a pointer to the pci_conf
 * structure that will be compared to every entry in the matches array.
 * This function returns 1 on failure, 0 on success.
 */
static int
pci_conf_match(struct pci_match_conf *matches, int num_matches, 
	       struct pci_conf *match_buf)
{
	int i;

	if ((matches == NULL) || (match_buf == NULL) || (num_matches <= 0))
		return(1);

	for (i = 0; i < num_matches; i++) {
		/*
		 * I'm not sure why someone would do this...but...
		 */
		if (matches[i].flags == PCI_GETCONF_NO_MATCH)
			continue;

		/*
		 * Look at each of the match flags.  If it's set, do the
		 * comparison.  If the comparison fails, we don't have a
		 * match, go on to the next item if there is one.
		 */
		if (((matches[i].flags & PCI_GETCONF_MATCH_BUS) != 0)
		 && (match_buf->pc_sel.pc_bus != matches[i].pc_sel.pc_bus))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_DEV) != 0)
		 && (match_buf->pc_sel.pc_dev != matches[i].pc_sel.pc_dev))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_FUNC) != 0)
		 && (match_buf->pc_sel.pc_func != matches[i].pc_sel.pc_func))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_VENDOR) != 0) 
		 && (match_buf->pc_vendor != matches[i].pc_vendor))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_DEVICE) != 0)
		 && (match_buf->pc_device != matches[i].pc_device))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_CLASS) != 0)
		 && (match_buf->pc_class != matches[i].pc_class))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_UNIT) != 0)
		 && (match_buf->pd_unit != matches[i].pd_unit))
			continue;

		if (((matches[i].flags & PCI_GETCONF_MATCH_NAME) != 0)
		 && (strncmp(matches[i].pd_name, match_buf->pd_name,
			     sizeof(match_buf->pd_name)) != 0))
			continue;

		return(0);
	}

	return(1);
}

/*
 * Locate the parent of a PCI device by scanning the PCI devlist
 * and return the entry for the parent.
 * For devices on PCI Bus 0 (the host bus), this is the PCI Host.
 * For devices on secondary PCI busses, this is that bus' PCI-PCI Bridge.
 */

pcicfgregs *
pci_devlist_get_parent(pcicfgregs *cfg)
{
	struct devlist *devlist_head;
	struct pci_devinfo *dinfo;
	pcicfgregs *bridge_cfg;
	int i;

	dinfo = STAILQ_FIRST(devlist_head = &pci_devq);

	/* If the device is on PCI bus 0, look for the host */
	if (cfg->bus == 0) {
		for (i = 0; (dinfo != NULL) && (i < pci_numdevs);
		dinfo = STAILQ_NEXT(dinfo, pci_links), i++) {
			bridge_cfg = &dinfo->cfg;
			if (bridge_cfg->baseclass == PCIC_BRIDGE
				&& bridge_cfg->subclass == PCIS_BRIDGE_HOST
		    		&& bridge_cfg->bus == cfg->bus) {
				return bridge_cfg;
			}
		}
	}

	/* If the device is not on PCI bus 0, look for the PCI-PCI bridge */
	if (cfg->bus > 0) {
		for (i = 0; (dinfo != NULL) && (i < pci_numdevs);
		dinfo = STAILQ_NEXT(dinfo, pci_links), i++) {
			bridge_cfg = &dinfo->cfg;
			if (bridge_cfg->baseclass == PCIC_BRIDGE
				&& bridge_cfg->subclass == PCIS_BRIDGE_PCI
				&& bridge_cfg->secondarybus == cfg->bus) {
				return bridge_cfg;
			}
		}
	}

	return NULL; 
}

static int
pci_ioctl(struct dev_ioctl_args *ap)
{
	device_t pci, pcib;
	struct pci_io *io;
	const char *name;
	int error;

	if (!(ap->a_fflag & FWRITE))
		return EPERM;

	switch(ap->a_cmd) {
	case PCIOCGETCONF:
		{
		struct pci_devinfo *dinfo;
		struct pci_conf_io *cio;
		struct devlist *devlist_head;
		struct pci_match_conf *pattern_buf;
		int num_patterns;
		size_t iolen;
		int ionum, i;

		cio = (struct pci_conf_io *)ap->a_data;

		num_patterns = 0;
		dinfo = NULL;

		/*
		 * Hopefully the user won't pass in a null pointer, but it
		 * can't hurt to check.
		 */
		if (cio == NULL) {
			error = EINVAL;
			break;
		}

		/*
		 * If the user specified an offset into the device list,
		 * but the list has changed since they last called this
		 * ioctl, tell them that the list has changed.  They will
		 * have to get the list from the beginning.
		 */
		if ((cio->offset != 0)
		 && (cio->generation != pci_generation)){
			cio->num_matches = 0;	
			cio->status = PCI_GETCONF_LIST_CHANGED;
			error = 0;
			break;
		}

		/*
		 * Check to see whether the user has asked for an offset
		 * past the end of our list.
		 */
		if (cio->offset >= pci_numdevs) {
			cio->num_matches = 0;
			cio->status = PCI_GETCONF_LAST_DEVICE;
			error = 0;
			break;
		}

		/* get the head of the device queue */
		devlist_head = &pci_devq;

		/*
		 * Determine how much room we have for pci_conf structures.
		 * Round the user's buffer size down to the nearest
		 * multiple of sizeof(struct pci_conf) in case the user
		 * didn't specify a multiple of that size.
		 */
		iolen = min(cio->match_buf_len - 
			    (cio->match_buf_len % sizeof(struct pci_conf)),
			    pci_numdevs * sizeof(struct pci_conf));

		/*
		 * Since we know that iolen is a multiple of the size of
		 * the pciconf union, it's okay to do this.
		 */
		ionum = iolen / sizeof(struct pci_conf);

		/*
		 * If this test is true, the user wants the pci_conf
		 * structures returned to match the supplied entries.
		 */
		if ((cio->num_patterns > 0)
		 && (cio->pat_buf_len > 0)) {
			/*
			 * pat_buf_len needs to be:
			 * num_patterns * sizeof(struct pci_match_conf)
			 * While it is certainly possible the user just
			 * allocated a large buffer, but set the number of
			 * matches correctly, it is far more likely that
			 * their kernel doesn't match the userland utility
			 * they're using.  It's also possible that the user
			 * forgot to initialize some variables.  Yes, this
			 * may be overly picky, but I hazard to guess that
			 * it's far more likely to just catch folks that
			 * updated their kernel but not their userland.
			 */
			if ((cio->num_patterns *
			    sizeof(struct pci_match_conf)) != cio->pat_buf_len){
				/* The user made a mistake, return an error*/
				cio->status = PCI_GETCONF_ERROR;
				kprintf("pci_ioctl: pat_buf_len %d != "
				       "num_patterns (%d) * sizeof(struct "
				       "pci_match_conf) (%d)\npci_ioctl: "
				       "pat_buf_len should be = %d\n",
				       cio->pat_buf_len, cio->num_patterns,
				       (int)sizeof(struct pci_match_conf),
				       (int)sizeof(struct pci_match_conf) * 
				       cio->num_patterns);
				kprintf("pci_ioctl: do your headers match your "
				       "kernel?\n");
				cio->num_matches = 0;
				error = EINVAL;
				break;
			}

			/*
			 * Check the user's buffer to make sure it's readable.
			 */
			if (!useracc((caddr_t)cio->patterns,
				    cio->pat_buf_len, VM_PROT_READ)) {
				kprintf("pci_ioctl: pattern buffer %p, "
				       "length %u isn't user accessible for"
				       " READ\n", cio->patterns,
				       cio->pat_buf_len);
				error = EACCES;
				break;
			}
			/*
			 * Allocate a buffer to hold the patterns.
			 */
			pattern_buf = kmalloc(cio->pat_buf_len, M_TEMP,
					     M_WAITOK);
			error = copyin(cio->patterns, pattern_buf,
				       cio->pat_buf_len);
			if (error != 0)
				break;
			num_patterns = cio->num_patterns;

		} else if ((cio->num_patterns > 0)
			|| (cio->pat_buf_len > 0)) {
			/*
			 * The user made a mistake, spit out an error.
			 */
			cio->status = PCI_GETCONF_ERROR;
			cio->num_matches = 0;
			kprintf("pci_ioctl: invalid GETCONF arguments\n");
			error = EINVAL;
			break;
		} else
			pattern_buf = NULL;

		/*
		 * Make sure we can write to the match buffer.
		 */
		if (!useracc((caddr_t)cio->matches,
			     cio->match_buf_len, VM_PROT_WRITE)) {
			kprintf("pci_ioctl: match buffer %p, length %u "
			       "isn't user accessible for WRITE\n",
			       cio->matches, cio->match_buf_len);
			error = EACCES;
			break;
		}

		/*
		 * Go through the list of devices and copy out the devices
		 * that match the user's criteria.
		 */
		for (cio->num_matches = 0, error = 0, i = 0,
		     dinfo = STAILQ_FIRST(devlist_head);
		     (dinfo != NULL) && (cio->num_matches < ionum)
		     && (error == 0) && (i < pci_numdevs);
		     dinfo = STAILQ_NEXT(dinfo, pci_links), i++) {

			if (i < cio->offset)
				continue;

			/* Populate pd_name and pd_unit */
			name = NULL;
			if (dinfo->cfg.dev && dinfo->conf.pd_name[0] == '\0')
				name = device_get_name(dinfo->cfg.dev);
			if (name) {
				strncpy(dinfo->conf.pd_name, name,
					sizeof(dinfo->conf.pd_name));
				dinfo->conf.pd_name[PCI_MAXNAMELEN] = 0;
				dinfo->conf.pd_unit =
					device_get_unit(dinfo->cfg.dev);
			}

			if ((pattern_buf == NULL) ||
			    (pci_conf_match(pattern_buf, num_patterns,
					    &dinfo->conf) == 0)) {

				/*
				 * If we've filled up the user's buffer,
				 * break out at this point.  Since we've
				 * got a match here, we'll pick right back
				 * up at the matching entry.  We can also
				 * tell the user that there are more matches
				 * left.
				 */
				if (cio->num_matches >= ionum)
					break;

				error = copyout(&dinfo->conf,
					        &cio->matches[cio->num_matches],
						sizeof(struct pci_conf));
				cio->num_matches++;
			}
		}

		/*
		 * Set the pointer into the list, so if the user is getting
		 * n records at a time, where n < pci_numdevs,
		 */
		cio->offset = i;

		/*
		 * Set the generation, the user will need this if they make
		 * another ioctl call with offset != 0.
		 */
		cio->generation = pci_generation;
		
		/*
		 * If this is the last device, inform the user so he won't
		 * bother asking for more devices.  If dinfo isn't NULL, we
		 * know that there are more matches in the list because of
		 * the way the traversal is done.
		 */
		if (dinfo == NULL)
			cio->status = PCI_GETCONF_LAST_DEVICE;
		else
			cio->status = PCI_GETCONF_MORE_DEVS;

		if (pattern_buf != NULL)
			kfree(pattern_buf, M_TEMP);

		break;
		}
	case PCIOCREAD:
		io = (struct pci_io *)ap->a_data;
		switch(io->pi_width) {
		case 4:
		case 2:
		case 1:
			/*
			 * Assume that the user-level bus number is
			 * actually the pciN instance number. We map
			 * from that to the real pcib+bus combination.
			 */
			pci = devclass_get_device(pci_devclass,
						  io->pi_sel.pc_bus);
			if (pci) {
				/*
				 * pci is the pci device and may contain
				 * several children (for each function code).
				 * The governing pci bus is the parent to
				 * the pci device.
				 */
				int b;

				pcib = device_get_parent(pci);
				b = pcib_get_bus(pcib);
				io->pi_data = 
					PCIB_READ_CONFIG(pcib,
							 b,
							 io->pi_sel.pc_dev,
							 io->pi_sel.pc_func,
							 io->pi_reg,
							 io->pi_width);
				error = 0;
			} else {
				error = ENODEV;
			}
			break;
		default:
			error = ENODEV;
			break;
		}
		break;

	case PCIOCWRITE:
		io = (struct pci_io *)ap->a_data;
		switch(io->pi_width) {
		case 4:
		case 2:
		case 1:
			/*
			 * Assume that the user-level bus number is
			 * actually the pciN instance number. We map
			 * from that to the real pcib+bus combination.
			 */
			pci = devclass_get_device(pci_devclass,
						  io->pi_sel.pc_bus);
			if (pci) {
				/*
				 * pci is the pci device and may contain
				 * several children (for each function code).
				 * The governing pci bus is the parent to
				 * the pci device.
				 */
				int b;

				pcib = device_get_parent(pci);
				b = pcib_get_bus(pcib);
				PCIB_WRITE_CONFIG(pcib,
						  b,
						  io->pi_sel.pc_dev,
						  io->pi_sel.pc_func,
						  io->pi_reg,
						  io->pi_data,
						  io->pi_width);
				error = 0;
			} else {
				error = ENODEV;
			}
			break;
		default:
			error = ENODEV;
			break;
		}
		break;

	default:
		error = ENOTTY;
		break;
	}

	return (error);
}

#define	PCI_CDEV	78

static struct dev_ops pcic_ops = {
	{ "pci", PCI_CDEV, 0 },
	.d_open =	pci_open,
	.d_close =	pci_close,
	.d_ioctl =	pci_ioctl,
};

#include "pci_if.h"

/*
 * New style pci driver.  Parent device is either a pci-host-bridge or a
 * pci-pci-bridge.  Both kinds are represented by instances of pcib.
 */
const char *
pci_class_to_string(int baseclass)
{
	const char *name;

	switch(baseclass) {
	case PCIC_OLD:
		name = "OLD";
		break;
	case PCIC_STORAGE:
		name = "STORAGE";
		break;
	case PCIC_NETWORK:
		name = "NETWORK";
		break;
	case PCIC_DISPLAY:
		name = "DISPLAY";
		break;
	case PCIC_MULTIMEDIA:
		name = "MULTIMEDIA";
		break;
	case PCIC_MEMORY:
		name = "MEMORY";
		break;
	case PCIC_BRIDGE:
		name = "BRIDGE";
		break;
	case PCIC_SIMPLECOMM:
		name = "SIMPLECOMM";
		break;
	case PCIC_BASEPERIPH:
		name = "BASEPERIPH";
		break;
	case PCIC_INPUTDEV:
		name = "INPUTDEV";
		break;
	case PCIC_DOCKING:
		name = "DOCKING";
		break;
	case PCIC_PROCESSOR:
		name = "PROCESSOR";
		break;
	case PCIC_SERIALBUS:
		name = "SERIALBUS";
		break;
	case PCIC_WIRELESS:
		name = "WIRELESS";
		break;
	case PCIC_I2O:
		name = "I20";
		break;
	case PCIC_SATELLITE:
		name = "SATELLITE";
		break;
	case PCIC_CRYPTO:
		name = "CRYPTO";
		break;
	case PCIC_SIGPROC:
		name = "SIGPROC";
		break;
	case PCIC_OTHER:
		name = "OTHER";
		break;
	default:
		name = "?";
		break;
	}
	return(name);
}

static void
pci_print_verbose_expr(const pcicfgregs *cfg)
{
	const struct pcicfg_expr *expr = &cfg->expr;
	const char *port_name;
	uint16_t port_type;

	if (!bootverbose)
		return;

	if (expr->expr_ptr == 0) /* No PCI Express capability */
		return;

	kprintf("\tPCI Express ver.%d cap=0x%04x",
		expr->expr_cap & PCIEM_CAP_VER_MASK, expr->expr_cap);
	if ((expr->expr_cap & PCIEM_CAP_VER_MASK) != PCIEM_CAP_VER_1)
		goto back;

	port_type = expr->expr_cap & PCIEM_CAP_PORT_TYPE;

	switch (port_type) {
	case PCIE_END_POINT:
		port_name = "DEVICE";
		break;
	case PCIE_LEG_END_POINT:
		port_name = "LEGDEV";
		break;
	case PCIE_ROOT_PORT:
		port_name = "ROOT";
		break;
	case PCIE_UP_STREAM_PORT:
		port_name = "UPSTREAM";
		break;
	case PCIE_DOWN_STREAM_PORT:
		port_name = "DOWNSTRM";
		break;
	case PCIE_PCIE2PCI_BRIDGE:
		port_name = "PCIE2PCI";
		break;
	case PCIE_PCI2PCIE_BRIDGE:
		port_name = "PCI2PCIE";
		break;
	default:
		port_name = NULL;
		break;
	}
	if ((port_type == PCIE_ROOT_PORT ||
	     port_type == PCIE_DOWN_STREAM_PORT) &&
	    !(expr->expr_cap & PCIEM_CAP_SLOT_IMPL))
		port_name = NULL;
	if (port_name != NULL)
		kprintf("[%s]", port_name);

	if (pcie_slotimpl(cfg)) {
		kprintf(", slotcap=0x%08x", expr->expr_slotcap);
		if (expr->expr_slotcap & PCIEM_SLTCAP_HP_CAP)
			kprintf("[HOTPLUG]");
	}
back:
	kprintf("\n");
}

void
pci_print_verbose(struct pci_devinfo *dinfo)
{
	if (bootverbose) {
		pcicfgregs *cfg = &dinfo->cfg;

		kprintf("found->\tvendor=0x%04x, dev=0x%04x, revid=0x%02x\n", 
		       cfg->vendor, cfg->device, cfg->revid);
		kprintf("\tbus=%d, slot=%d, func=%d\n",
		       cfg->bus, cfg->slot, cfg->func);
		kprintf("\tclass=[%s]%02x-%02x-%02x, hdrtype=0x%02x, mfdev=%d\n",
		       pci_class_to_string(cfg->baseclass),
		       cfg->baseclass, cfg->subclass, cfg->progif,
		       cfg->hdrtype, cfg->mfdev);
		kprintf("\tsubordinatebus=%x \tsecondarybus=%x\n",
		       cfg->subordinatebus, cfg->secondarybus);
#ifdef PCI_DEBUG
		kprintf("\tcmdreg=0x%04x, statreg=0x%04x, cachelnsz=%d (dwords)\n", 
		       cfg->cmdreg, cfg->statreg, cfg->cachelnsz);
		kprintf("\tlattimer=0x%02x (%d ns), mingnt=0x%02x (%d ns), maxlat=0x%02x (%d ns)\n",
		       cfg->lattimer, cfg->lattimer * 30, 
		       cfg->mingnt, cfg->mingnt * 250, cfg->maxlat, cfg->maxlat * 250);
#endif /* PCI_DEBUG */
		if (cfg->intpin > 0)
			kprintf("\tintpin=%c, irq=%d\n", cfg->intpin +'a' -1, cfg->intline);

		pci_print_verbose_expr(cfg);
	}
}

static int
pci_porten(device_t pcib, int b, int s, int f)
{
	return (PCIB_READ_CONFIG(pcib, b, s, f, PCIR_COMMAND, 2)
		& PCIM_CMD_PORTEN) != 0;
}

static int
pci_memen(device_t pcib, int b, int s, int f)
{
	return (PCIB_READ_CONFIG(pcib, b, s, f, PCIR_COMMAND, 2)
		& PCIM_CMD_MEMEN) != 0;
}

/*
 * Add a resource based on a pci map register. Return 1 if the map
 * register is a 32bit map register or 2 if it is a 64bit register.
 */
static int
pci_add_map(device_t pcib, int b, int s, int f, int reg,
	    struct resource_list *rl)
{
	u_int32_t map;
	u_int64_t base;
	u_int8_t ln2size;
	u_int8_t ln2range;
	u_int32_t testval;


#ifdef PCI_ENABLE_IO_MODES
	u_int16_t cmd;
#endif		
	int type;

	map = PCIB_READ_CONFIG(pcib, b, s, f, reg, 4);

	if (map == 0 || map == 0xffffffff)
		return 1; /* skip invalid entry */

	PCIB_WRITE_CONFIG(pcib, b, s, f, reg, 0xffffffff, 4);
	testval = PCIB_READ_CONFIG(pcib, b, s, f, reg, 4);
	PCIB_WRITE_CONFIG(pcib, b, s, f, reg, map, 4);

	base = pci_mapbase(map);
	if (pci_maptype(map) & PCI_MAPMEM)
		type = SYS_RES_MEMORY;
	else
		type = SYS_RES_IOPORT;
	ln2size = pci_mapsize(testval);
	ln2range = pci_maprange(testval);
	if (ln2range == 64) {
		/* Read the other half of a 64bit map register */
		base |= (u_int64_t) PCIB_READ_CONFIG(pcib, b, s, f, reg+4, 4);
	}

	/*
	 * This code theoretically does the right thing, but has
	 * undesirable side effects in some cases where
	 * peripherals respond oddly to having these bits
	 * enabled.  Leave them alone by default.
	 */
#ifdef PCI_ENABLE_IO_MODES
	if (type == SYS_RES_IOPORT && !pci_porten(pcib, b, s, f)) {
		cmd = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_COMMAND, 2);
		cmd |= PCIM_CMD_PORTEN;
		PCIB_WRITE_CONFIG(pcib, b, s, f, PCIR_COMMAND, cmd, 2);
	}
	if (type == SYS_RES_MEMORY && !pci_memen(pcib, b, s, f)) {
		cmd = PCIB_READ_CONFIG(pcib, b, s, f, PCIR_COMMAND, 2);
		cmd |= PCIM_CMD_MEMEN;
		PCIB_WRITE_CONFIG(pcib, b, s, f, PCIR_COMMAND, cmd, 2);
	}
#else
        if (type == SYS_RES_IOPORT && !pci_porten(pcib, b, s, f))
                return 1;
        if (type == SYS_RES_MEMORY && !pci_memen(pcib, b, s, f))
		return 1;
#endif

	resource_list_add(rl, type, reg,
			  base, base + (1 << ln2size) - 1,
			  (1 << ln2size));

	if (bootverbose) {
		kprintf("\tmap[%02x]: type %x, range %2d, base %08x, size %2d\n",
		       reg, pci_maptype(base), ln2range,
		       (unsigned int) base, ln2size);
	}

	return (ln2range == 64) ? 2 : 1;
}

#ifdef PCI_MAP_FIXUP
/*
 * For ATA devices we need to decide early on what addressing mode to use.
 * Legacy demands that the primary and secondary ATA ports sits on the
 * same addresses that old ISA hardware did. This dictates that we use
 * those addresses and ignore the BARs if we cannot set PCI native
 * addressing mode.
 */
static void
pci_ata_maps(device_t pcib, device_t bus, device_t dev, int b, int s, int f,
	     struct resource_list *rl)
{
	int rid, type, progif;
#if 0
	/* if this device supports PCI native addressing use it */
	progif = pci_read_config(dev, PCIR_PROGIF, 1);
	if ((progif &0x8a) == 0x8a) {
		if (pci_mapbase(pci_read_config(dev, PCIR_BAR(0), 4)) &&
		    pci_mapbase(pci_read_config(dev, PCIR_BAR(2), 4))) {
			kprintf("Trying ATA native PCI addressing mode\n");
			pci_write_config(dev, PCIR_PROGIF, progif | 0x05, 1);
		}
	}
#endif
	/*
	 * Because we return any preallocated resources for lazy
	 * allocation for PCI devices in pci_alloc_resource(), we can
	 * allocate our legacy resources here.
	 */
	progif = pci_read_config(dev, PCIR_PROGIF, 1);
	type = SYS_RES_IOPORT;
	if (progif & PCIP_STORAGE_IDE_MODEPRIM) {
		pci_add_map(pcib, b, s, f, PCIR_BAR(0), rl);
		pci_add_map(pcib, b, s, f, PCIR_BAR(1), rl);
	} else {
		rid = PCIR_BAR(0);
		resource_list_add(rl, type, rid, 0x1f0, 0x1f7, 8);
		resource_list_alloc(rl, bus, dev, type, &rid, 0x1f0, 0x1f7, 8,
				    0);
		rid = PCIR_BAR(1);
		resource_list_add(rl, type, rid, 0x3f6, 0x3f6, 1);
		resource_list_alloc(rl, bus, dev, type, &rid, 0x3f6, 0x3f6, 1,
				    0);
	}
	if (progif & PCIP_STORAGE_IDE_MODESEC) {
		pci_add_map(pcib, b, s, f, PCIR_BAR(2), rl);
		pci_add_map(pcib, b, s, f, PCIR_BAR(3), rl);
	} else {
		rid = PCIR_BAR(2);
		resource_list_add(rl, type, rid, 0x170, 0x177, 8);
		resource_list_alloc(rl, bus, dev, type, &rid, 0x170, 0x177, 8,
				    0);
		rid = PCIR_BAR(3);
		resource_list_add(rl, type, rid, 0x376, 0x376, 1);
		resource_list_alloc(rl, bus, dev, type, &rid, 0x376, 0x376, 1,
				    0);
	}
	pci_add_map(pcib, b, s, f, PCIR_BAR(4), rl);
	pci_add_map(pcib, b, s, f, PCIR_BAR(5), rl);
}
#endif /* PCI_MAP_FIXUP */

static void
pci_add_resources(device_t pcib, device_t bus, device_t dev)
{
	struct pci_devinfo *dinfo = device_get_ivars(dev);
	pcicfgregs *cfg = &dinfo->cfg;
	struct resource_list *rl = &dinfo->resources;
	struct pci_quirk *q;
	int b, i, f, s;
#if 0	/* WILL BE USED WITH ADDITIONAL IMPORT FROM FREEBSD-5 XXX */
	int irq;
#endif

	b = cfg->bus;
	s = cfg->slot;
	f = cfg->func;
#ifdef PCI_MAP_FIXUP
	/* atapci devices in legacy mode need special map treatment */
	if ((pci_get_class(dev) == PCIC_STORAGE) &&
	    (pci_get_subclass(dev) == PCIS_STORAGE_IDE) &&
	    ((pci_get_progif(dev) & PCIP_STORAGE_IDE_MASTERDEV) ||
	     (!pci_read_config(dev, PCIR_BAR(0), 4) &&
	      !pci_read_config(dev, PCIR_BAR(2), 4))) )
		pci_ata_maps(pcib, bus, dev, b, s, f, rl);
	else
#endif /* PCI_MAP_FIXUP */
		for (i = 0; i < cfg->nummaps;) {
			i += pci_add_map(pcib, b, s, f, PCIR_BAR(i),rl);
		}

	for (q = &pci_quirks[0]; q->devid; q++) {
		if (q->devid == ((cfg->device << 16) | cfg->vendor)
		    && q->type == PCI_QUIRK_MAP_REG)
			pci_add_map(pcib, b, s, f, q->arg1, rl);
	}

	if (cfg->intpin > 0 && cfg->intline != 255)
		resource_list_add(rl, SYS_RES_IRQ, 0,
				  cfg->intline, cfg->intline, 1);
}

void
pci_add_children(device_t dev, int busno, size_t dinfo_size)
{
#define REG(n, w)       PCIB_READ_CONFIG(pcib, busno, s, f, n, w)
	device_t pcib = device_get_parent(dev);
	struct pci_devinfo *dinfo;
	int maxslots;
	int s, f, pcifunchigh;
	uint8_t hdrtype;

	KKASSERT(dinfo_size >= sizeof(struct pci_devinfo));

	maxslots = PCIB_MAXSLOTS(pcib);

	for (s = 0; s <= maxslots; s++) {
		pcifunchigh = 0;
		f = 0;
		hdrtype = REG(PCIR_HDRTYPE, 1);
		if ((hdrtype & PCIM_HDRTYPE) > PCI_MAXHDRTYPE)
			continue;
		if (hdrtype & PCIM_MFDEV)
			pcifunchigh = PCI_FUNCMAX;
		for (f = 0; f <= pcifunchigh; f++) {
			dinfo = pci_read_device(pcib, busno, s, f, dinfo_size);
			if (dinfo != NULL) {
				pci_add_child(dev, dinfo);
			}
		}
	}
#undef REG
}

/*
 * The actual PCI child that we add has a NULL driver whos parent
 * device will be "pci".  The child contains the ivars, not the parent.
 */
void
pci_add_child(device_t bus, struct pci_devinfo *dinfo)
{
	device_t pcib;

	pcib = device_get_parent(bus);
	dinfo->cfg.dev = device_add_child(bus, NULL, -1);
	device_set_ivars(dinfo->cfg.dev, dinfo);
	pci_add_resources(pcib, bus, dinfo->cfg.dev);
	pci_print_verbose(dinfo);
}

/*
 * Probe the PCI bus.  Note: probe code is not supposed to add children
 * or call attach.
 */
static int
pci_probe(device_t dev)
{
	device_set_desc(dev, "PCI bus");

	/* Allow other subclasses to override this driver */
	return(-1000);
}

static int
pci_attach(device_t dev)
{
	int busno;
	int lunit = device_get_unit(dev);

	dev_ops_add(&pcic_ops, -1, lunit);
	make_dev(&pcic_ops, lunit, UID_ROOT, GID_WHEEL, 0644, "pci%d", lunit);

        /*
         * Since there can be multiple independantly numbered PCI
         * busses on some large alpha systems, we can't use the unit
         * number to decide what bus we are probing. We ask the parent
         * pcib what our bus number is.
	 *
	 * pcib_get_bus() must act on the pci bus device, not on the pci
	 * device, because it uses badly hacked nexus-based ivars to 
	 * store and retrieve the physical bus number.  XXX
         */
        busno = pcib_get_bus(device_get_parent(dev));
        if (bootverbose)
                device_printf(dev, "pci_attach() physical bus=%d\n", busno);

        pci_add_children(dev, busno, sizeof(struct pci_devinfo));

        return (bus_generic_attach(dev));
}

static int
pci_print_resources(struct resource_list *rl, const char *name, int type,
		    const char *format)
{
	struct resource_list_entry *rle;
	int printed, retval;

	printed = 0;
	retval = 0;
	/* Yes, this is kinda cheating */
	SLIST_FOREACH(rle, rl, link) {
		if (rle->type == type) {
			if (printed == 0)
				retval += kprintf(" %s ", name);
			else if (printed > 0)
				retval += kprintf(",");
			printed++;
			retval += kprintf(format, rle->start);
			if (rle->count > 1) {
				retval += kprintf("-");
				retval += kprintf(format, rle->start +
						 rle->count - 1);
			}
		}
	}
	return retval;
}

int
pci_print_child(device_t dev, device_t child)
{
	struct pci_devinfo *dinfo;
	struct resource_list *rl;
	pcicfgregs *cfg;
	int retval = 0;

	dinfo = device_get_ivars(child);
	cfg = &dinfo->cfg;
	rl = &dinfo->resources;

	retval += bus_print_child_header(dev, child);

	retval += pci_print_resources(rl, "port", SYS_RES_IOPORT, "%#lx");
	retval += pci_print_resources(rl, "mem", SYS_RES_MEMORY, "%#lx");
	retval += pci_print_resources(rl, "irq", SYS_RES_IRQ, "%ld");
	if (device_get_flags(dev))
		retval += kprintf(" flags %#x", device_get_flags(dev));

	retval += kprintf(" at device %d.%d", pci_get_slot(child),
			 pci_get_function(child));

	retval += bus_print_child_footer(dev, child);

	return (retval);
}

void
pci_probe_nomatch(device_t dev, device_t child)
{
	struct pci_devinfo *dinfo;
	pcicfgregs *cfg;
	const char *desc;
	int unknown;

	unknown = 0;
	dinfo = device_get_ivars(child);
	cfg = &dinfo->cfg;
	desc = pci_ata_match(child);
	if (!desc) desc = pci_usb_match(child);
	if (!desc) desc = pci_vga_match(child);
	if (!desc) desc = pci_chip_match(child);
	if (!desc) {
		desc = "unknown card";
		unknown++;
	}
	device_printf(dev, "<%s>", desc);
	if (bootverbose || unknown) {
		kprintf(" (vendor=0x%04x, dev=0x%04x)",
			cfg->vendor,
			cfg->device);
	}
	kprintf(" at %d.%d",
		pci_get_slot(child),
		pci_get_function(child));
	if (cfg->intpin > 0 && cfg->intline != 255) {
		kprintf(" irq %d", cfg->intline);
	}
	kprintf("\n");
                                      
	return;
}

int
pci_read_ivar(device_t dev, device_t child, int which, uintptr_t *result)
{
	struct pci_devinfo *dinfo;
	pcicfgregs *cfg;

	dinfo = device_get_ivars(child);
	cfg = &dinfo->cfg;

	switch (which) {
	case PCI_IVAR_SUBVENDOR:
		*result = cfg->subvendor;
		break;
	case PCI_IVAR_SUBDEVICE:
		*result = cfg->subdevice;
		break;
	case PCI_IVAR_VENDOR:
		*result = cfg->vendor;
		break;
	case PCI_IVAR_DEVICE:
		*result = cfg->device;
		break;
	case PCI_IVAR_DEVID:
		*result = (cfg->device << 16) | cfg->vendor;
		break;
	case PCI_IVAR_CLASS:
		*result = cfg->baseclass;
		break;
	case PCI_IVAR_SUBCLASS:
		*result = cfg->subclass;
		break;
	case PCI_IVAR_PROGIF:
		*result = cfg->progif;
		break;
	case PCI_IVAR_REVID:
		*result = cfg->revid;
		break;
	case PCI_IVAR_INTPIN:
		*result = cfg->intpin;
		break;
	case PCI_IVAR_IRQ:
		*result = cfg->intline;
		break;
	case PCI_IVAR_BUS:
		*result = cfg->bus;
		break;
	case PCI_IVAR_SLOT:
		*result = cfg->slot;
		break;
	case PCI_IVAR_FUNCTION:
		*result = cfg->func;
		break;
	case PCI_IVAR_SECONDARYBUS:
		*result = cfg->secondarybus;
		break;
	case PCI_IVAR_SUBORDINATEBUS:
		*result = cfg->subordinatebus;
		break;
	case PCI_IVAR_ETHADDR:
		/*
		 * The generic accessor doesn't deal with failure, so
		 * we set the return value, then return an error.
		 */
		*result = 0;
		return (EINVAL);
	case PCI_IVAR_PCIXCAP_PTR:
		*result = cfg->pcixcap_ptr;
		break;
	case PCI_IVAR_PCIECAP_PTR:
		*result = cfg->expr.expr_ptr;
		break;
	default:
		return ENOENT;
	}
	return 0;
}

int
pci_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{
	struct pci_devinfo *dinfo;
	pcicfgregs *cfg;

	dinfo = device_get_ivars(child);
	cfg = &dinfo->cfg;

	switch (which) {
	case PCI_IVAR_SUBVENDOR:
	case PCI_IVAR_SUBDEVICE:
	case PCI_IVAR_VENDOR:
	case PCI_IVAR_DEVICE:
	case PCI_IVAR_DEVID:
	case PCI_IVAR_CLASS:
	case PCI_IVAR_SUBCLASS:
	case PCI_IVAR_PROGIF:
	case PCI_IVAR_REVID:
	case PCI_IVAR_INTPIN:
	case PCI_IVAR_IRQ:
	case PCI_IVAR_BUS:
	case PCI_IVAR_SLOT:
	case PCI_IVAR_FUNCTION:
	case PCI_IVAR_ETHADDR:
	case PCI_IVAR_PCIXCAP_PTR:
	case PCI_IVAR_PCIECAP_PTR:
		return EINVAL;	/* disallow for now */

	case PCI_IVAR_SECONDARYBUS:
		cfg->secondarybus = value;
		break;
	case PCI_IVAR_SUBORDINATEBUS:
		cfg->subordinatebus = value;
		break;
	default:
		return ENOENT;
	}
	return 0;
}

#ifdef PCI_MAP_FIXUP
static struct resource *
pci_alloc_map(device_t dev, device_t child, int type, int *rid, u_long start,
	      u_long end, u_long count, u_int flags)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	struct resource_list *rl = &dinfo->resources;
	struct resource_list_entry *rle;
	struct resource *res;
	uint32_t map, testval;
	int mapsize;

	/*
	 * Weed out the bogons, and figure out how large the BAR/map
	 * is. BARs that read back 0 here are bogus and unimplemented.
	 *
	 * Note: atapci in legacy mode are special and handled elsewhere
	 * in the code. If you have an atapci device in legacy mode and
	 * it fails here, that other code is broken.
	 */
	res = NULL;
	map = pci_read_config(child, *rid, 4);
	pci_write_config(child, *rid, 0xffffffff, 4);
	testval = pci_read_config(child, *rid, 4);
	if (pci_mapbase(testval) == 0)
		goto out;
	if (pci_maptype(testval) & PCI_MAPMEM) {
		if (type != SYS_RES_MEMORY) {
			if (bootverbose)
				device_printf(dev, "child %s requested type %d"
					      " for rid %#x, but the BAR says "
					      "it is a memio\n",
					      device_get_nameunit(child), type,
					      *rid);
			goto out;
		}
	} else {
		if (type != SYS_RES_IOPORT) {
			if (bootverbose)
				device_printf(dev, "child %s requested type %d"
					      " for rid %#x, but the BAR says "
					      "it is an ioport\n",
					      device_get_nameunit(child), type,
					      *rid);
			goto out;
		}
	}
	/*
	 * For real BARs, we need to override the size that
	 * the driver requests, because that's what the BAR
	 * actually uses and we would otherwise have a
	 * situation where we might allocate the excess to
	 * another driver, which won't work.
	 */
	mapsize = pci_mapsize(testval);
	count = 1 << mapsize;
	if (RF_ALIGNMENT(flags) < mapsize)
		flags = (flags & ~RF_ALIGNMENT_MASK) |
		   RF_ALIGNMENT_LOG2(mapsize);
	/*
	 * Allocate enough resource, and then write back the
	 * appropriate BAR for that resource.
	 */
	res = BUS_ALLOC_RESOURCE(device_get_parent(dev), child, type, rid,
				 start, end, count, flags);
	if (res == NULL) {
		device_printf(child, "%#lx bytes at rid %#x res %d failed "
			      "(%#lx, %#lx)\n", count, *rid, type, start, end);
		goto out;
	}
	resource_list_add(rl, type, *rid, start, end, count);
	rle = resource_list_find(rl, type, *rid);
	if (rle == NULL)
		panic("pci_alloc_map: unexpectedly can't find resource.");
	rle->res = res;
	rle->start = rman_get_start(res);
	rle->end = rman_get_end(res);
	rle->count = count;
	if (bootverbose)
		device_printf(child, "lazy allocation of %#lx bytes rid %#x "
			      "type %d at %#lx\n", count, *rid, type,
			      rman_get_start(res));
	map = rman_get_start(res);
out:;
	pci_write_config(child, *rid, map, 4);
	return res;
}
#endif /* PCI_MAP_FIXUP */

struct resource *
pci_alloc_resource(device_t dev, device_t child, int type, int *rid,
		   u_long start, u_long end, u_long count, u_int flags)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	struct resource_list *rl = &dinfo->resources;
#ifdef PCI_MAP_FIXUP
	struct resource_list_entry *rle;
#endif /* PCI_MAP_FIXUP */
	pcicfgregs *cfg = &dinfo->cfg;

	/*
	 * Perform lazy resource allocation
	 */
	if (device_get_parent(child) == dev) {
		switch (type) {
		case SYS_RES_IRQ:
#ifdef __i386__
		/*
		 * If device doesn't have an interrupt routed, and is
		 * deserving of an interrupt, try to assign it one.
		 */
			if ((cfg->intline == 255 || cfg->intline == 0) &&
			    (cfg->intpin != 0) &&
			    (start == 0) && (end == ~0UL)) {
				cfg->intline = PCIB_ROUTE_INTERRUPT(
					device_get_parent(dev), child,
					cfg->intpin);
				if (cfg->intline != 255) {
					pci_write_config(child, PCIR_INTLINE,
					    cfg->intline, 1);
					resource_list_add(rl, SYS_RES_IRQ, 0,
					    cfg->intline, cfg->intline, 1);
				}
			}
			break;
#endif
		case SYS_RES_IOPORT:
			/* FALLTHROUGH */
		case SYS_RES_MEMORY:
			if (*rid < PCIR_BAR(cfg->nummaps)) {
				/*
				 * Enable the I/O mode.  We should
				 * also be assigning resources too
				 * when none are present.  The
				 * resource_list_alloc kind of sorta does
				 * this...
				 */
				if (PCI_ENABLE_IO(dev, child, type))
					return (NULL);
			}
#ifdef PCI_MAP_FIXUP
			rle = resource_list_find(rl, type, *rid);
			if (rle == NULL)
				return pci_alloc_map(dev, child, type, rid,
						     start, end, count, flags);
#endif /* PCI_MAP_FIXUP */
			break;
		}
#ifdef PCI_MAP_FIXUP
		/*
		 * If we've already allocated the resource, then
		 * return it now. But first we may need to activate
		 * it, since we don't allocate the resource as active
		 * above. Normally this would be done down in the
		 * nexus, but since we short-circuit that path we have
		 * to do its job here. Not sure if we should free the
		 * resource if it fails to activate.
		 *
		 * Note: this also finds and returns resources for
		 * atapci devices in legacy mode as allocated in
		 * pci_ata_maps().
		 */
		rle = resource_list_find(rl, type, *rid);
		if (rle != NULL && rle->res != NULL) {
			if (bootverbose)
				device_printf(child, "reserved %#lx bytes for "
					      "rid %#x type %d at %#lx\n",
					      rman_get_size(rle->res), *rid,
					      type, rman_get_start(rle->res));
			if ((flags & RF_ACTIVE) &&
			    bus_generic_activate_resource(dev, child, type,
							  *rid, rle->res) != 0)
				return NULL;
			return rle->res;
		}
#endif /* PCI_MAP_FIXUP */
	}
	return resource_list_alloc(rl, dev, child, type, rid,
				   start, end, count, flags);
}

static int
pci_release_resource(device_t dev, device_t child, int type, int rid,
		     struct resource *r)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	struct resource_list *rl = &dinfo->resources;

	return resource_list_release(rl, dev, child, type, rid, r);
}

static int
pci_set_resource(device_t dev, device_t child, int type, int rid,
		 u_long start, u_long count)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	struct resource_list *rl = &dinfo->resources;

	resource_list_add(rl, type, rid, start, start + count - 1, count);
	return 0;
}

static int
pci_get_resource(device_t dev, device_t child, int type, int rid,
		 u_long *startp, u_long *countp)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	struct resource_list *rl = &dinfo->resources;
	struct resource_list_entry *rle;

	rle = resource_list_find(rl, type, rid);
	if (!rle)
		return ENOENT;
	
	if (startp)
		*startp = rle->start;
	if (countp)
		*countp = rle->count;

	return 0;
}

void
pci_delete_resource(device_t dev, device_t child, int type, int rid)
{
	kprintf("pci_delete_resource: PCI resources can not be deleted\n");
}

struct resource_list *
pci_get_resource_list (device_t dev, device_t child)
{
	struct pci_devinfo *dinfo = device_get_ivars(child); 

	if (dinfo == NULL)
		return (NULL);
	return (&dinfo->resources);
}

u_int32_t
pci_read_config_method(device_t dev, device_t child, int reg, int width)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	pcicfgregs *cfg = &dinfo->cfg;

	return PCIB_READ_CONFIG(device_get_parent(dev),
				 cfg->bus, cfg->slot, cfg->func,
				 reg, width);
}

void
pci_write_config_method(device_t dev, device_t child, int reg,
			u_int32_t val, int width)
{
	struct pci_devinfo *dinfo = device_get_ivars(child);
	pcicfgregs *cfg = &dinfo->cfg;

	PCIB_WRITE_CONFIG(device_get_parent(dev),
			  cfg->bus, cfg->slot, cfg->func,
			  reg, val, width);
}

int
pci_child_location_str_method(device_t cbdev, device_t child, char *buf,
    size_t buflen)
{
	struct pci_devinfo *dinfo;

	dinfo = device_get_ivars(child);
	ksnprintf(buf, buflen, "slot=%d function=%d", pci_get_slot(child),
	    pci_get_function(child));
	return (0);
}

int
pci_child_pnpinfo_str_method(device_t cbdev, device_t child, char *buf,
    size_t buflen)
{
	struct pci_devinfo *dinfo;
	pcicfgregs *cfg;

	dinfo = device_get_ivars(child);
	cfg = &dinfo->cfg;
	ksnprintf(buf, buflen, "vendor=0x%04x device=0x%04x subvendor=0x%04x "
	    "subdevice=0x%04x class=0x%02x%02x%02x", cfg->vendor, cfg->device,
	    cfg->subvendor, cfg->subdevice, cfg->baseclass, cfg->subclass,
	    cfg->progif);
	return (0);
}

int
pci_assign_interrupt_method(device_t dev, device_t child)
{                       
        struct pci_devinfo *dinfo = device_get_ivars(child);
        pcicfgregs *cfg = &dinfo->cfg;
                         
        return (PCIB_ROUTE_INTERRUPT(device_get_parent(dev), child,
            cfg->intpin));
}

static int
pci_modevent(module_t mod, int what, void *arg)
{
	switch (what) {
	case MOD_LOAD:
		STAILQ_INIT(&pci_devq);
		break;
	case MOD_UNLOAD:
		break;
	}

	return 0;
}

int
pci_resume(device_t dev)
{
        int                     numdevs;
        int                     i;
        device_t                *children;
        device_t                child;
        struct pci_devinfo      *dinfo;
        pcicfgregs              *cfg;

        device_get_children(dev, &children, &numdevs);

        for (i = 0; i < numdevs; i++) {
                child = children[i];

                dinfo = device_get_ivars(child);
                cfg = &dinfo->cfg;
                if (cfg->intpin > 0 && PCI_INTERRUPT_VALID(cfg->intline)) {
                        cfg->intline = PCI_ASSIGN_INTERRUPT(dev, child);
                        if (PCI_INTERRUPT_VALID(cfg->intline)) {
                                pci_write_config(child, PCIR_INTLINE,
                                    cfg->intline, 1);
                        }
                }
        }

        kfree(children, M_TEMP);

        return (bus_generic_resume(dev));
}

static device_method_t pci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		pci_probe),
	DEVMETHOD(device_attach,	pci_attach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD(device_suspend,	bus_generic_suspend),
	DEVMETHOD(device_resume,	pci_resume),

	/* Bus interface */
	DEVMETHOD(bus_print_child,	pci_print_child),
	DEVMETHOD(bus_probe_nomatch,	pci_probe_nomatch),
	DEVMETHOD(bus_read_ivar,	pci_read_ivar),
	DEVMETHOD(bus_write_ivar,	pci_write_ivar),
	DEVMETHOD(bus_driver_added,	bus_generic_driver_added),
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),

	DEVMETHOD(bus_get_resource_list,pci_get_resource_list),
	DEVMETHOD(bus_set_resource,	pci_set_resource),
	DEVMETHOD(bus_get_resource,	pci_get_resource),
	DEVMETHOD(bus_delete_resource,	pci_delete_resource),
	DEVMETHOD(bus_alloc_resource,	pci_alloc_resource),
	DEVMETHOD(bus_release_resource,	pci_release_resource),
	DEVMETHOD(bus_activate_resource, bus_generic_activate_resource),
	DEVMETHOD(bus_deactivate_resource, bus_generic_deactivate_resource),
	DEVMETHOD(bus_child_pnpinfo_str, pci_child_pnpinfo_str_method),
	DEVMETHOD(bus_child_location_str, pci_child_location_str_method),

	/* PCI interface */
	DEVMETHOD(pci_read_config,	pci_read_config_method),
	DEVMETHOD(pci_write_config,	pci_write_config_method),
	DEVMETHOD(pci_enable_busmaster,	pci_enable_busmaster_method),
	DEVMETHOD(pci_disable_busmaster, pci_disable_busmaster_method),
	DEVMETHOD(pci_enable_io,	pci_enable_io_method),
	DEVMETHOD(pci_disable_io,	pci_disable_io_method),
	DEVMETHOD(pci_get_powerstate,	pci_get_powerstate_method),
	DEVMETHOD(pci_set_powerstate,	pci_set_powerstate_method),
	DEVMETHOD(pci_assign_interrupt, pci_assign_interrupt_method),   

	{ 0, 0 }
};

driver_t pci_driver = {
	"pci",
	pci_methods,
	1,			/* no softc */
};

DRIVER_MODULE(pci, pcib, pci_driver, pci_devclass, pci_modevent, 0);
MODULE_VERSION(pci, 1);
