/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <phk@FreeBSD.org> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.   Poul-Henning Kamp
 * ----------------------------------------------------------------------------
 *
 * $FreeBSD: src/sys/pci/xrpu.c,v 1.19.2.1 2000/08/02 22:19:57 peter Exp $
 * $DragonFly: src/sys/dev/misc/xrpu/Attic/xrpu.c,v 1.13 2006/12/22 23:26:18 swildner Exp $
 *
 * A very simple device driver for PCI cards based on Xilinx 6200 series
 * FPGA/RPU devices.  Current Functionality is to allow you to open and
 * mmap the entire thing into your program.
 *
 * Hardware currently supported:
 *	www.vcc.com HotWorks 1 6216 based card.
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/timepps.h>
#include <sys/xrpuio.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>

#include "pci_if.h"

/*
 * Device driver initialization stuff
 */

static d_open_t	xrpu_open;
static d_close_t xrpu_close;
static d_ioctl_t xrpu_ioctl;
static d_mmap_t xrpu_mmap;

#define CDEV_MAJOR 100
static struct dev_ops xrpu_ops = {
	{ "xrpu", CDEV_MAJOR, 0 },
	.d_open =	xrpu_open,
	.d_close =	xrpu_close,
	.d_ioctl =	xrpu_ioctl,
	.d_mmap =	xrpu_mmap,
};

static MALLOC_DEFINE(M_XRPU, "xrpu", "XRPU related");

static devclass_t xrpu_devclass;

#define dev2unit(devt) (minor(devt) & 0xff)
#define dev2pps(devt) ((minor(devt) >> 16)-1)

struct softc {
	enum { NORMAL, TIMECOUNTER } mode;
	vm_offset_t virbase, physbase;
	u_int	*virbase62;
#if 0
	struct timecounter tc;
#endif
	u_int *trigger, *latch, dummy;
	struct pps_state pps[XRPU_MAX_PPS];
	u_int *assert[XRPU_MAX_PPS], *clear[XRPU_MAX_PPS];
};

#if 0

static unsigned         
xrpu_get_timecount(struct timecounter *tc)
{               
	struct softc *sc = tc->tc_priv;

	sc->dummy += *sc->trigger;
	return (*sc->latch & tc->tc_counter_mask);
}        

static void            
xrpu_poll_pps(struct timecounter *tc)
{               
        struct softc *sc = tc->tc_priv;
	int i, j;
        unsigned count1, ppscount; 
                
	for (i = 0; i < XRPU_MAX_PPS; i++) {
		if (sc->assert[i]) {
			ppscount = *(sc->assert[i]) & tc->tc_counter_mask;
			j = 0;
			do {
				count1 = ppscount;
				ppscount =  *(sc->assert[i]) & tc->tc_counter_mask;
			} while (ppscount != count1 && ++j < 5);
			pps_event(&sc->pps[i], tc, ppscount, PPS_CAPTUREASSERT);
		}
		if (sc->clear[i]) {
			j = 0;
			ppscount = *(sc->clear[i]) & tc->tc_counter_mask;
			do {
				count1 = ppscount;
				ppscount =  *(sc->clear[i]) & tc->tc_counter_mask;
			} while (ppscount != count1 && ++j < 5);
			pps_event(&sc->pps[i], tc, ppscount, PPS_CAPTURECLEAR);
		}
	}
}

#endif

static int
xrpu_open(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct softc *sc = devclass_get_softc(xrpu_devclass, dev2unit(dev));

	if (!sc)
		return (ENXIO);
	dev->si_drv1 = sc;
	return (0);
}

static int
xrpu_close(struct dev_close_args *ap)
{
	return (0);
}

static int
xrpu_mmap(struct dev_mmap_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct softc *sc = dev->si_drv1;

	if (ap->a_offset >= 0x1000000) 
		return (-1);
	return (i386_btop(sc->physbase + ap->a_offset));
}

static int
xrpu_ioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct softc *sc = dev->si_drv1;
	int i, error;

	if (sc->mode == TIMECOUNTER) {
		i = dev2pps(dev);
		if (i < 0 || i >= XRPU_MAX_PPS)
			return ENODEV;
		error =  pps_ioctl(ap->a_cmd, ap->a_data, &sc->pps[i]);
		return (error);
	}
		
#if 0
	if (ap->a_cmd == XRPU_IOC_TIMECOUNTING) {
		struct xrpu_timecounting *xt = (struct xrpu_timecounting *)ap->a_data;

		/* Name SHALL be zero terminated */
		xt->xt_name[sizeof xt->xt_name - 1] = '\0';
		i = strlen(xt->xt_name);
		sc->tc.tc_name = (char *)kmalloc(i + 1, M_XRPU, M_WAITOK);
		strcpy(sc->tc.tc_name, xt->xt_name);
		sc->tc.tc_frequency = xt->xt_frequency;
		sc->tc.tc_get_timecount = xrpu_get_timecount;
		sc->tc.tc_poll_pps = xrpu_poll_pps;
		sc->tc.tc_priv = sc;
		sc->tc.tc_counter_mask = xt->xt_mask;
		sc->trigger = sc->virbase62 + xt->xt_addr_trigger;
		sc->latch = sc->virbase62 + xt->xt_addr_latch;

		for (i = 0; i < XRPU_MAX_PPS; i++) {
			if (xt->xt_pps[i].xt_addr_assert == 0
			    && xt->xt_pps[i].xt_addr_clear == 0)
				continue;
			make_dev(&xrpu_ops, (i+1)<<16, 
			    UID_ROOT, GID_WHEEL, 0600, "xpps%d", i);
			sc->pps[i].ppscap = 0;
			if (xt->xt_pps[i].xt_addr_assert) {
				sc->assert[i] = sc->virbase62 + xt->xt_pps[i].xt_addr_assert;
				sc->pps[i].ppscap |= PPS_CAPTUREASSERT;
			}
			if (xt->xt_pps[i].xt_addr_clear) {
				sc->clear[i] = sc->virbase62 + xt->xt_pps[i].xt_addr_clear;
				sc->pps[i].ppscap |= PPS_CAPTURECLEAR;
			}
			pps_init(&sc->pps[i]);
		}
		sc->mode = TIMECOUNTER;
		init_timecounter(&sc->tc);
		return (0);
	}
#endif
	error = ENOTTY;
	return (error);
}

/*
 * PCI initialization stuff
 */

static int
xrpu_probe(device_t self)
{
	char *desc;

	desc = NULL;
	switch (pci_get_devid(self)) {
	case 0x6216133e:
		desc = "VCC Hotworks-I xc6216";
		break;
	}
	if (desc == NULL)
		return ENXIO;

	device_set_desc(self, desc);
	return 0;
}

static int
xrpu_attach(device_t self)
{
	struct softc *sc;
	struct resource *res;
	int rid, unit;

	unit = device_get_unit(self);
	sc = device_get_softc(self);
	sc->mode = NORMAL;
	rid = PCIR_MAPS;
	res = bus_alloc_resource(self, SYS_RES_MEMORY, &rid,
				 0, ~0, 1, RF_ACTIVE);
	if (res == NULL) {
		device_printf(self, "Could not map memory\n");
		return ENXIO;
	}
	sc->virbase = (vm_offset_t)rman_get_virtual(res);
	sc->physbase = rman_get_start(res);
	sc->virbase62 = (u_int *)(sc->virbase + 0x800000);

	if (bootverbose)
		kprintf("Mapped physbase %#lx to virbase %#lx\n",
		    (u_long)sc->physbase, (u_long)sc->virbase);

	dev_ops_add(&xrpu_ops, -1, unit);
	make_dev(&xrpu_ops, unit, UID_ROOT, GID_WHEEL, 0600, "xrpu%d", unit);
	return 0;
}

static device_method_t xrpu_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		xrpu_probe),
	DEVMETHOD(device_attach,	xrpu_attach),
	DEVMETHOD(device_suspend,	bus_generic_suspend),
	DEVMETHOD(device_resume,	bus_generic_resume),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),

	{0, 0}
};
 
static driver_t xrpu_driver = {
	"xrpu",
	xrpu_methods,
	sizeof(struct softc)
};
 
 
DRIVER_MODULE(xrpu, pci, xrpu_driver, xrpu_devclass, 0, 0);
