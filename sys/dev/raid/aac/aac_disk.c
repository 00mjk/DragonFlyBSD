/*-
 * Copyright (c) 2000 Michael Smith
 * Copyright (c) 2001 Scott Long
 * Copyright (c) 2000 BSDi
 * Copyright (c) 2001 Adaptec, Inc.
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
 *	$FreeBSD: src/sys/dev/aac/aac_disk.c,v 1.3.2.8 2003/01/11 18:39:39 scottl Exp $
 *	$DragonFly: src/sys/dev/raid/aac/aac_disk.c,v 1.20 2008/01/20 03:40:35 pavalos Exp $
 */

#include "opt_aac.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/devicestat.h>
#include <sys/disk.h>
#include <sys/dtype.h>
#include <sys/rman.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/md_var.h>

#include "aacreg.h"
#include "aac_ioctl.h"
#include "aacvar.h"

/*
 * Interface to parent.
 */
static int aac_disk_probe(device_t dev);
static int aac_disk_attach(device_t dev);
static int aac_disk_detach(device_t dev);

/*
 * Interface to the device switch.
 */
static	d_open_t	aac_disk_open;
static	d_close_t	aac_disk_close;
static	d_strategy_t	aac_disk_strategy;
static	d_dump_t	aac_disk_dump;

#define AAC_DISK_CDEV_MAJOR	151

static struct dev_ops aac_disk_ops = {
	{ "aacd", AAC_DISK_CDEV_MAJOR, D_DISK },
	.d_open =		aac_disk_open,
	.d_close =		aac_disk_close,
	.d_read =		physread,
	.d_write =		physwrite,
	.d_strategy =		aac_disk_strategy,
	.d_dump =		aac_disk_dump,
};

static devclass_t	aac_disk_devclass;

static device_method_t aac_disk_methods[] = {
	DEVMETHOD(device_probe,	aac_disk_probe),
	DEVMETHOD(device_attach,	aac_disk_attach),
	DEVMETHOD(device_detach,	aac_disk_detach),
	{ 0, 0 }
};

static driver_t aac_disk_driver = {
	"aacd",
	aac_disk_methods,
	sizeof(struct aac_disk)
};

#define AAC_MAXIO	65536

DRIVER_MODULE(aacd, aac, aac_disk_driver, aac_disk_devclass, 0, 0);

/* sysctl tunables */
static unsigned int aac_iosize_max = AAC_MAXIO;	/* due to limits of the card */
TUNABLE_INT("hw.aac.iosize_max", &aac_iosize_max);

SYSCTL_DECL(_hw_aac);
SYSCTL_UINT(_hw_aac, OID_AUTO, iosize_max, CTLFLAG_RD, &aac_iosize_max, 0,
	    "Max I/O size per transfer to an array");

/*
 * Handle open from generic layer.
 *
 * This is called by the diskslice code on first open in order to get the 
 * basic device geometry paramters.
 */
static int
aac_disk_open(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct aac_disk	*sc;

	debug_called(0);

	sc = (struct aac_disk *)dev->si_drv1;
	
	if (sc == NULL) {
		kprintf("aac_disk_open: No Softc\n");
		return (ENXIO);
	}

	/* check that the controller is up and running */
	if (sc->ad_controller->aac_state & AAC_STATE_SUSPEND) {
		kprintf("Controller Suspended controller state = 0x%x\n",
		       sc->ad_controller->aac_state);
		return(ENXIO);
	}

	/* build synthetic label */
#if 0
	bzero(&info, sizeof(info));
	info.d_media_blksize= AAC_BLOCK_SIZE;		/* mandatory */
	info.d_media_blocks = sc->ad_size;

	info.d_type = DTYPE_ESDI;			/* optional */
	info.d_secpertrack   = sc->ad_sectors;
	info.d_nheads	= sc->ad_heads;
	info.d_ncylinders = sc->ad_cylinders;
	info.d_secpercyl  = sc->ad_sectors * sc->ad_heads;

	disk_setdiskinfo(&sc->ad_disk, &info);
#endif
	sc->ad_flags |= AAC_DISK_OPEN;
	return (0);
}

/*
 * Handle last close of the disk device.
 */
static int
aac_disk_close(struct dev_close_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct aac_disk	*sc;

	debug_called(0);

	sc = (struct aac_disk *)dev->si_drv1;
	
	if (sc == NULL)
		return (ENXIO);

	sc->ad_flags &= ~AAC_DISK_OPEN;
	return (0);
}

/*
 * Handle an I/O request.
 */
static int
aac_disk_strategy(struct dev_strategy_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct aac_disk	*sc;

	debug_called(4);

	sc = (struct aac_disk *)dev->si_drv1;

	/* bogus disk? */
	if (sc == NULL) {
		bp->b_flags |= B_ERROR;
		bp->b_error = EINVAL;
		biodone(bio);
		return(0);
	}

	/* do-nothing operation? */
	if (bp->b_bcount == 0) {
		bp->b_resid = bp->b_bcount;
		biodone(bio);
		return(0);
	}

	/* perform accounting */

	/* pass the bio to the controller - it can work out who we are */
	AAC_LOCK_ACQUIRE(&sc->ad_controller->aac_io_lock);
	devstat_start_transaction(&sc->ad_stats);
	aac_submit_bio(sc, bio);
	AAC_LOCK_RELEASE(&sc->ad_controller->aac_io_lock);

	return(0);
}

/*
 * Dump memory out to an array
 *
 * This queues blocks of memory of size AAC_MAXIO to the controller and waits
 * for the controller to complete the requests.
 */
static int
aac_disk_dump(struct dev_dump_args *ap)
{
	kprintf("dumps on aac are currently broken, not dumping\n");
	return (ENXIO);
#if 0
	cdev_t dev = ap->a_head.a_dev;
	struct aac_disk *ad;
	struct aac_softc *sc;
	vm_offset_t addr;
	long blkcnt;
	int dumppages;
	int i, error;

	ad = dev->si_drv1;
	addr = 0;
	dumppages = AAC_MAXIO / PAGE_SIZE;

	if (ad == NULL)
		return (ENXIO);

	sc= ad->ad_controller;

	blkcnt = howmany(PAGE_SIZE, ap->a_secsize);

	while (ap->a_count > 0) {
		caddr_t va = NULL;

		if ((ap->a_count / blkcnt) < dumppages)
			dumppages = ap->a_count / blkcnt;

		for (i = 0; i < dumppages; ++i) {
			vm_offset_t a = addr + (i * PAGE_SIZE);
			if (is_physical_memory(a)) {
				va = pmap_kenter_temporary(trunc_page(a), i);
			} else {
				va = pmap_kenter_temporary(trunc_page(0), i);
			}
		}

retry:
		/*
		 * Queue the block to the controller.  If the queue is full,
		 * EBUSY will be returned.
		 */
		error = aac_dump_enqueue(ad, ap->a_blkno, va, dumppages);
		if (error && (error != EBUSY))
			return (error);

		if (!error) {
			if (dumpstatus(addr, (off_t)(ap->a_count * DEV_BSIZE)) < 0)
			return (EINTR);

			ap->a_blkno += blkcnt * dumppages;
			ap->a_count -= blkcnt * dumppages;
			addr += PAGE_SIZE * dumppages;
			if (ap->a_count > 0)
			continue;
		}

		/*
		 * Either the queue was full on the last attemp, or we have no
		 * more data to dump.  Let the queue drain out and retry the
		 * block if the queue was full.
		 */
		aac_dump_complete(sc);

		if (error == EBUSY)
			goto retry;
	}

	return (0);
#endif
}

/*
 * Handle completion of an I/O request.
 */
void
aac_biodone(struct bio *bio, const char *code)
{
	struct buf *bp = bio->bio_buf;
	struct aac_disk	*sc;

	debug_called(4);

	sc = (struct aac_disk *)bio->bio_driver_info;

	devstat_end_transaction_buf(&sc->ad_stats, bp);
	if (bp->b_flags & B_ERROR) {
		diskerr(bio, sc->ad_dev_t,
			code, 0, 0);
	}
	biodone(bio);
}

/*
 * Stub only.
 */
static int
aac_disk_probe(device_t dev)
{

	debug_called(2);

	return (0);
}

/*
 * Attach a unit to the controller.
 */
static int
aac_disk_attach(device_t dev)
{
	struct disk_info info;
	struct aac_disk	*sc;
	
	debug_called(0);

	sc = (struct aac_disk *)device_get_softc(dev);

	/* initialise our softc */
	sc->ad_controller =
	    (struct aac_softc *)device_get_softc(device_get_parent(dev));
	sc->ad_container = device_get_ivars(dev);
	sc->ad_dev = dev;

	/*
	 * require that extended translation be enabled - other drivers read the
	 * disk!
	 */
	sc->ad_size = sc->ad_container->co_mntobj.Capacity;
	if (sc->ad_size >= (2 * 1024 * 1024)) {		/* 2GB */
		sc->ad_heads = 255;
		sc->ad_sectors = 63;
	} else if (sc->ad_size >= (1 * 1024 * 1024)) {	/* 1GB */
		sc->ad_heads = 128;
		sc->ad_sectors = 32;
	} else {
		sc->ad_heads = 64;
		sc->ad_sectors = 32;
	}
	sc->ad_cylinders = (sc->ad_size / (sc->ad_heads * sc->ad_sectors));

	device_printf(dev, "%uMB (%u sectors)\n",
		      sc->ad_size / ((1024 * 1024) / AAC_BLOCK_SIZE),
		      sc->ad_size);

	devstat_add_entry(&sc->ad_stats, "aacd", device_get_unit(dev),
			  AAC_BLOCK_SIZE, DEVSTAT_NO_ORDERED_TAGS,
			  DEVSTAT_TYPE_STORARRAY | DEVSTAT_TYPE_IF_OTHER, 
			  DEVSTAT_PRIORITY_ARRAY);

	/* attach a generic disk device to ourselves */
	sc->ad_dev_t = disk_create(device_get_unit(dev), &sc->ad_disk,
				   &aac_disk_ops);
	sc->ad_dev_t->si_drv1 = sc;

	sc->ad_dev_t->si_iosize_max = aac_iosize_max;
	sc->unit = device_get_unit(dev);

	/*
	 * Set disk info, as it appears that all needed data is available already.
	 * Setting the disk info will also cause the probing to start.
	 */
	bzero(&info, sizeof(info));
	info.d_media_blksize= AAC_BLOCK_SIZE;		/* mandatory */
	info.d_media_blocks = sc->ad_size;

	info.d_type = DTYPE_ESDI;			/* optional */
	info.d_secpertrack   = sc->ad_sectors;
	info.d_nheads	= sc->ad_heads;
	info.d_ncylinders = sc->ad_cylinders;
	info.d_secpercyl  = sc->ad_sectors * sc->ad_heads;

	disk_setdiskinfo(&sc->ad_disk, &info);

	return (0);
}

/*
 * Disconnect ourselves from the system.
 */
static int
aac_disk_detach(device_t dev)
{
	struct aac_disk *sc;

	debug_called(2);

	sc = (struct aac_disk *)device_get_softc(dev);

	if (sc->ad_flags & AAC_DISK_OPEN)
		return(EBUSY);

	devstat_remove_entry(&sc->ad_stats);
	disk_destroy(&sc->ad_disk);
	return(0);
}
