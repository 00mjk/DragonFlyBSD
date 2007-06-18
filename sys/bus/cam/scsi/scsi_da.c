/*
 * Implementation of SCSI Direct Access Peripheral driver for CAM.
 *
 * Copyright (c) 1997 Justin T. Gibbs.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/cam/scsi/scsi_da.c,v 1.42.2.46 2003/10/21 22:18:19 thomas Exp $
 * $DragonFly: src/sys/bus/cam/scsi/scsi_da.c,v 1.38 2007/06/18 00:38:08 dillon Exp $
 */

#ifdef _KERNEL
#include "opt_hw_wdog.h"
#endif /* _KERNEL */

#include <sys/param.h>
#include <sys/bootmaj.h>

#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#endif /* _KERNEL */

#include <sys/devicestat.h>
#include <sys/conf.h>
#ifdef _KERNEL
#include <sys/disk.h>
#include <sys/dtype.h>
#include <sys/eventhandler.h>
#include <sys/malloc.h>
#include <sys/cons.h>
#include <sys/proc.h>
#endif
#include <sys/buf2.h>
#ifdef _KERNEL
#include <sys/thread2.h>
#endif

#include <machine/md_var.h>

#ifdef _KERNEL
#include <vm/vm.h>
#include <vm/pmap.h>
#endif

#ifndef _KERNEL
#include <stdio.h>
#include <string.h>
#endif /* _KERNEL */

#include "../cam.h"
#include "../cam_ccb.h"
#include "../cam_extend.h"
#include "../cam_periph.h"
#include "../cam_xpt_periph.h"

#include "scsi_message.h"

#ifndef _KERNEL 
#include "scsi_da.h"
#endif /* !_KERNEL */

#ifdef _KERNEL
typedef enum {
	DA_STATE_PROBE,
	DA_STATE_PROBE2,
	DA_STATE_NORMAL
} da_state;

typedef enum {
	DA_FLAG_PACK_INVALID	= 0x001,
	DA_FLAG_NEW_PACK	= 0x002,
	DA_FLAG_PACK_LOCKED	= 0x004,
	DA_FLAG_PACK_REMOVABLE	= 0x008,
	DA_FLAG_TAGGED_QUEUING	= 0x010,
	DA_FLAG_NEED_OTAG	= 0x020,
	DA_FLAG_WENT_IDLE	= 0x040,
	DA_FLAG_RETRY_UA	= 0x080,
	DA_FLAG_OPEN		= 0x100,
	DA_FLAG_SCTX_INIT	= 0x200
} da_flags;

typedef enum {
	DA_Q_NONE		= 0x00,
	DA_Q_NO_SYNC_CACHE	= 0x01,
	DA_Q_NO_6_BYTE		= 0x02,
	DA_Q_NO_PREVENT		= 0x04
} da_quirks;

typedef enum {
	DA_CCB_PROBE		= 0x01,
	DA_CCB_PROBE2		= 0x02,
	DA_CCB_BUFFER_IO	= 0x03,
	DA_CCB_WAITING		= 0x04,
	DA_CCB_DUMP		= 0x05,
	DA_CCB_TYPE_MASK	= 0x0F,
	DA_CCB_RETRY_UA		= 0x10
} da_ccb_state;

/* Offsets into our private area for storing information */
#define ccb_state	ppriv_field0
#define ccb_bio		ppriv_ptr1

struct disk_params {
	u_int8_t  heads;
	u_int32_t cylinders;
	u_int8_t  secs_per_track;
	u_int32_t secsize;	/* Number of bytes/sector */
	u_int64_t sectors;	/* total number sectors */
};

struct da_softc {
	struct	 bio_queue_head bio_queue;
	struct	 devstat device_stats;
	SLIST_ENTRY(da_softc) links;
	LIST_HEAD(, ccb_hdr) pending_ccbs;
	da_state state;
	da_flags flags;	
	da_quirks quirks;
	int	 minimum_cmd_size;
	int	 ordered_tag_count;
	struct	 disk_params params;
	struct	 disk disk;
	union	 ccb saved_ccb;
	struct task		sysctl_task;
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;
};

struct da_quirk_entry {
	struct scsi_inquiry_pattern inq_pat;
	da_quirks quirks;
};

static const char quantum[] = "QUANTUM";
static const char microp[] = "MICROP";

static struct da_quirk_entry da_quirk_table[] =
{
	/* SPI, FC devices */
	{
		/*
		 * Fujitsu M2513A MO drives.
		 * Tested devices: M2513A2 firmware versions 1200 & 1300.
		 * (dip switch selects whether T_DIRECT or T_OPTICAL device)
		 * Reported by: W.Scholten <whs@xs4all.nl>
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "FUJITSU", "M2513A", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/* See above. */
		{T_OPTICAL, SIP_MEDIA_REMOVABLE, "FUJITSU", "M2513A", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * This particular Fujitsu drive doesn't like the
		 * synchronize cache command.
		 * Reported by: Tom Jackson <toj@gorilla.net>
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, "FUJITSU", "M2954*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	
	},
	{
		/*
		 * This drive doesn't like the synchronize cache command
		 * either.  Reported by: Matthew Jacob <mjacob@feral.com>
		 * in NetBSD PR kern/6027, August 24, 1998.
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, microp, "2217*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * This drive doesn't like the synchronize cache command
		 * either.  Reported by: Hellmuth Michaelis (hm@kts.org)
		 * (PR 8882).
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, microp, "2112*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Doesn't like the synchronize cache command.
		 * Reported by: Blaz Zupan <blaz@gold.amis.net>
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, "NEC", "D3847*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Doesn't like the synchronize cache command.
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, quantum, "MAVERICK 540S", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Doesn't like the synchronize cache command.
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, quantum, "LPS525S", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Doesn't work correctly with 6 byte reads/writes.
		 * Returns illegal request, and points to byte 9 of the
		 * 6-byte CDB.
		 * Reported by:  Adam McDougall <bsdx@spawnet.com>
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, quantum, "VIKING 4*", "*"},
		/*quirks*/ DA_Q_NO_6_BYTE
	},
	{
		/* See above. */
		{T_DIRECT, SIP_MEDIA_FIXED, quantum, "VIKING 2*", "*"},
		/*quirks*/ DA_Q_NO_6_BYTE
	},
	/* XXX USB floppy quirks temporarily enabled for 4.9R */
	/* USB floppy devices supported by umass(4) */
	{
		/*
		 * This USB floppy drive uses the UFI command set. This
		 * command set is a derivative of the ATAPI command set and
		 * does not support READ_6 commands only READ_10. It also does
		 * not support sync cache (0x35).
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Y-E DATA", "USB-FDU", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/* Another USB floppy */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "MATSHITA", "FDD CF-VFDU*","*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * The vendor, product and version strings coming from the
		 * controller are null terminated instead of being padded with
		 * spaces. The trailing wildcard character '*' is required.
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "SMSC*", "USB FDC*","*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * SmartDisk (Mitsumi) USB floppy drive
		 * PR: kern/50226
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "MITSUMI", "USB FDD", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * The CISS RAID controllers do not support SYNC_CACHE
		 */
		{T_DIRECT, SIP_MEDIA_FIXED, "COMPAQ", "RAID*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
#ifdef DA_OLD_QUIRKS
	/* USB mass storage devices supported by umass(4) */
	{
		/*
		 * Sony Memory Stick adapter MSAC-US1 and
		 * Sony PCG-C1VJ Internal Memory Stick Slot (MSC-U01).
		 * Make all sony MS* products use this quirk.
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Sony", "MS*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Sony Memory Stick adapter for the CLIE series
		 * of PalmOS PDA's
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Sony", "CLIE*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Intelligent Stick USB disk-on-key
		 * PR: kern/53005
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "USB Card",
		 "IntelligentStick*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Sony DSC cameras (DSC-S30, DSC-S50, DSC-S70)
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Sony", "Sony DSC", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Microtech USB CameraMate
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "eUSB    Compact*",
		 "Compact Flash*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Olympus digital cameras (C-3040ZOOM, C-2040ZOOM, C-1)
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "OLYMPUS", "C-*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Olympus digital cameras (E-100RS, E-10).
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "OLYMPUS", "E-*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * KingByte Pen Drives
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "NO BRAND", "PEN DRIVE", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
 	},
	{
		/*
		 * iRiver iFP MP3 player (with UMS Firmware)
		 * PR: kern/54881, i386/63941, kern/66124
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "iRiver", "iFP*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
 	},
	{
		/*
		 * Frontier Labs NEX IA+ Digital Audio Player, rev 1.10/0.01
		 * PR: kern/70158
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "FL" , "NexIA+*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
 	{
		/*
		 * FujiFilm Camera
		 */
 		{T_DIRECT, SIP_MEDIA_REMOVABLE, "FUJIFILMUSB-DRIVEUNIT",
		 "USB-DRIVEUNIT", "*"},
 		/*quirks*/ DA_Q_NO_SYNC_CACHE
 	},
	{
		/*
		 * Minolta Dimage E203
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "MINOLTA", "DiMAGE E203", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Apacer HandyDrive
		 * PR: kern/43627
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Apacer", "HandyDrive", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Daisy Technology PhotoClip on Zoran chip
		 * PR: kern/43580
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "ZORAN", "COACH", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Sony USB Key-Storage
		 * PR: kern/46386
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "Sony", "Storage Media", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
#endif /* DA_OLD_QUIRKS */
	{
		/*
		 * EXATELECOM (Sigmatel) i-Bead 100/105 USB Flash MP3 Player
		 * PR: kern/51675
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "EXATEL", "i-BEAD10*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Jungsoft NEXDISK USB flash key
		 * PR: kern/54737
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "JUNGSOFT", "NEXDISK*", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE
	},
	{
		/*
		 * Creative Nomad MUVO mp3 player (USB)
		 * PR: kern/53094
		 */
		{T_DIRECT, SIP_MEDIA_REMOVABLE, "CREATIVE", "NOMAD_MUVO", "*"},
		/*quirks*/ DA_Q_NO_SYNC_CACHE|DA_Q_NO_PREVENT
	},
};

static	d_open_t	daopen;
static	d_close_t	daclose;
static	d_strategy_t	dastrategy;
static	d_ioctl_t	daioctl;
static	d_dump_t	dadump;
static	periph_init_t	dainit;
static	void		daasync(void *callback_arg, u_int32_t code,
				struct cam_path *path, void *arg);
static	int		dacmdsizesysctl(SYSCTL_HANDLER_ARGS);
static	periph_ctor_t	daregister;
static	periph_dtor_t	dacleanup;
static	periph_start_t	dastart;
static	periph_oninv_t	daoninvalidate;
static	void		dadone(struct cam_periph *periph,
			       union ccb *done_ccb);
static  int		daerror(union ccb *ccb, u_int32_t cam_flags,
				u_int32_t sense_flags);
static void		daprevent(struct cam_periph *periph, int action);
static int		dagetcapacity(struct cam_periph *periph);
static void		dasetgeom(struct cam_periph *periph, uint32_t block_len,
				  uint64_t maxsector);

static timeout_t	dasendorderedtag;
static void		dashutdown(void *arg, int howto);

#ifndef DA_DEFAULT_TIMEOUT
#define DA_DEFAULT_TIMEOUT 60	/* Timeout in seconds */
#endif

#ifndef	DA_DEFAULT_RETRY
#define	DA_DEFAULT_RETRY	4
#endif

static int da_retry_count = DA_DEFAULT_RETRY;
static int da_default_timeout = DA_DEFAULT_TIMEOUT;
static struct callout dasendorderedtag_ch;

SYSCTL_NODE(_kern_cam, OID_AUTO, da, CTLFLAG_RD, 0,
            "CAM Direct Access Disk driver");
SYSCTL_INT(_kern_cam_da, OID_AUTO, retry_count, CTLFLAG_RW,
           &da_retry_count, 0, "Normal I/O retry count");
TUNABLE_INT("kern.cam.da.retry_count", &da_retry_count);
SYSCTL_INT(_kern_cam_da, OID_AUTO, default_timeout, CTLFLAG_RW,
           &da_default_timeout, 0, "Normal I/O timeout (in seconds)");
TUNABLE_INT("kern.cam.da.default_timeout", &da_default_timeout);

/*
 * DA_ORDEREDTAG_INTERVAL determines how often, relative
 * to the default timeout, we check to see whether an ordered
 * tagged transaction is appropriate to prevent simple tag
 * starvation.  Since we'd like to ensure that there is at least
 * 1/2 of the timeout length left for a starved transaction to
 * complete after we've sent an ordered tag, we must poll at least
 * four times in every timeout period.  This takes care of the worst
 * case where a starved transaction starts during an interval that
 * meets the requirement "don't send an ordered tag" test so it takes
 * us two intervals to determine that a tag must be sent.
 */
#ifndef DA_ORDEREDTAG_INTERVAL
#define DA_ORDEREDTAG_INTERVAL 4
#endif

static struct periph_driver dadriver =
{
	dainit, "da",
	TAILQ_HEAD_INITIALIZER(dadriver.units), /* generation */ 0
};

DATA_SET(periphdriver_set, dadriver);

/* For 2.2-stable support */
#ifndef D_DISK
#define D_DISK 0
#endif

static struct dev_ops da_ops = {
	{ "da", DA_CDEV_MAJOR, D_DISK },
	.d_open =	daopen,
	.d_close =	daclose,
	.d_read =	physread,
	.d_write =	physwrite,
	.d_ioctl =	daioctl,
	.d_strategy =	dastrategy,
	.d_dump =	dadump
};

static SLIST_HEAD(,da_softc) softc_list;
static struct extend_array *daperiphs;

static int
daopen(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct cam_periph *periph;
	struct da_softc *softc;
	struct disk_info info;
	int unit;
	int part;
	int error;

	unit = dkunit(dev);
	part = dkpart(dev);
	periph = cam_extend_get(daperiphs, unit);
	if (periph == NULL)
		return (ENXIO);	

	softc = (struct da_softc *)periph->softc;

	CAM_DEBUG(periph->path, CAM_DEBUG_TRACE,
	    ("daopen: dev=%s (unit %d , partition %d)\n", devtoname(dev),
	     unit, part));

	if ((error = cam_periph_lock(periph, PCATCH)) != 0) {
		return (error); /* error code from tsleep */
	}

	if ((softc->flags & DA_FLAG_OPEN) == 0) {
		if (cam_periph_acquire(periph) != CAM_REQ_CMP)
			return(ENXIO);
		softc->flags |= DA_FLAG_OPEN;
	}

	crit_enter();
	if ((softc->flags & DA_FLAG_PACK_INVALID) != 0) {
		/* Invalidate our pack information. */
		disk_invalidate(&softc->disk);
		softc->flags &= ~DA_FLAG_PACK_INVALID;
	}
	crit_exit();

	error = dagetcapacity(periph);

#if 0
	/* Do a read capacity */
	{
		struct scsi_read_capacity_data *rcap;
		union  ccb *ccb;

		rcap = kmalloc(sizeof(*rcap), M_TEMP, M_INTWAIT | M_ZERO);
		
		ccb = cam_periph_getccb(periph, /*priority*/1);
		scsi_read_capacity(&ccb->csio,
				   /*retries*/1,
				   /*cbfncp*/dadone,
				   MSG_SIMPLE_Q_TAG,
				   rcap,
				   SSD_FULL_SIZE,
				   /*timeout*/60000);
		ccb->ccb_h.ccb_bio = NULL;

		error = cam_periph_runccb(ccb, daerror, /*cam_flags*/0,
					  /*sense_flags*/SF_RETRY_UA |
							 SF_RETRY_SELTO,
					  &softc->device_stats);

		xpt_release_ccb(ccb);

		if (error == 0) {
			dasetgeom(periph, rcap);
		}

		kfree(rcap, M_TEMP);
	}
#endif

	if (error == 0) {
		struct ccb_getdev cgd;

		/* Build disk information structure */
		bzero(&info, sizeof(info));
		info.d_type = DTYPE_SCSI;

		/*
		 * Grab the inquiry data to get the vendor and product names.
		 * Put them in the typename and packname for the label.
		 */
		xpt_setup_ccb(&cgd.ccb_h, periph->path, /*priority*/ 1);
		cgd.ccb_h.func_code = XPT_GDEV_TYPE;
		xpt_action((union ccb *)&cgd);

#if 0
		strncpy(label->d_typename, cgd.inq_data.vendor,
			min(SID_VENDOR_SIZE, sizeof(label->d_typename)));
		strncpy(label->d_packname, cgd.inq_data.product,
			min(SID_PRODUCT_SIZE, sizeof(label->d_packname)));
#endif
		
		/*
		 * Mandatory fields
		 */
		info.d_media_blksize = softc->params.secsize;
		info.d_media_blocks = softc->params.sectors;
		info.d_media_size = 0;

		/*
		 * Optional fields
		 */
		info.d_secpertrack = softc->params.secs_per_track;
		info.d_nheads = softc->params.heads;
		info.d_ncylinders = softc->params.cylinders;
		info.d_secpercyl = softc->params.heads *
				   softc->params.secs_per_track;
		disk_setdiskinfo(&softc->disk, &info);

		if ((softc->flags & DA_FLAG_PACK_REMOVABLE) != 0 &&
		    (softc->quirks & DA_Q_NO_PREVENT) == 0)
			daprevent(periph, PR_PREVENT);
	
		/*
		 * Check to see whether or not the blocksize is set yet.
		 * If it isn't, set it and then clear the blocksize
		 * unavailable flag for the device statistics.
		 */
		if ((softc->device_stats.flags & DEVSTAT_BS_UNAVAILABLE) != 0){
			softc->device_stats.block_size = softc->params.secsize;
			softc->device_stats.flags &= ~DEVSTAT_BS_UNAVAILABLE;
		}
	}
	
	if (error != 0) {
		if ((softc->flags & DA_FLAG_PACK_REMOVABLE) != 0 &&
		    (softc->quirks & DA_Q_NO_PREVENT) == 0)
			daprevent(periph, PR_ALLOW);
		softc->flags &= ~DA_FLAG_OPEN;
		cam_periph_release(periph);
	}
	cam_periph_unlock(periph);
	return (error);
}

static int
daclose(struct dev_close_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct	cam_periph *periph;
	struct	da_softc *softc;
	int	unit;
	int	error;

	unit = dkunit(dev);
	periph = cam_extend_get(daperiphs, unit);
	if (periph == NULL)
		return (ENXIO);	

	softc = (struct da_softc *)periph->softc;

	if ((error = cam_periph_lock(periph, 0)) != 0) {
		return (error); /* error code from tsleep */
	}

	if ((softc->quirks & DA_Q_NO_SYNC_CACHE) == 0) {
		union	ccb *ccb;

		ccb = cam_periph_getccb(periph, /*priority*/1);

		scsi_synchronize_cache(&ccb->csio,
				       /*retries*/1,
				       /*cbfcnp*/dadone,
				       MSG_SIMPLE_Q_TAG,
				       /*begin_lba*/0,/* Cover the whole disk */
				       /*lb_count*/0,
				       SSD_FULL_SIZE,
				       5 * 60 * 1000);

		cam_periph_runccb(ccb, /*error_routine*/NULL, /*cam_flags*/0,
				  /*sense_flags*/SF_RETRY_UA,
				  &softc->device_stats);

		if ((ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			if ((ccb->ccb_h.status & CAM_STATUS_MASK) ==
			     CAM_SCSI_STATUS_ERROR) {
				int asc, ascq;
				int sense_key, error_code;

				scsi_extract_sense(&ccb->csio.sense_data,
						   &error_code,
						   &sense_key, 
						   &asc, &ascq);
				if (sense_key != SSD_KEY_ILLEGAL_REQUEST)
					scsi_sense_print(&ccb->csio);
			} else {
				xpt_print_path(periph->path);
				kprintf("Synchronize cache failed, status "
				       "== 0x%x, scsi status == 0x%x\n",
				       ccb->csio.ccb_h.status,
				       ccb->csio.scsi_status);
			}
		}

		if ((ccb->ccb_h.status & CAM_DEV_QFRZN) != 0)
			cam_release_devq(ccb->ccb_h.path,
					 /*relsim_flags*/0,
					 /*reduction*/0,
					 /*timeout*/0,
					 /*getcount_only*/0);

		xpt_release_ccb(ccb);

	}

	if ((softc->flags & DA_FLAG_PACK_REMOVABLE) != 0) {
		if ((softc->quirks & DA_Q_NO_PREVENT) == 0)
			daprevent(periph, PR_ALLOW);
		/*
		 * If we've got removeable media, mark the blocksize as
		 * unavailable, since it could change when new media is
		 * inserted.
		 */
		softc->device_stats.flags |= DEVSTAT_BS_UNAVAILABLE;
	}

	/*
	 * Don't compound any ref counting software bugs with more.
	 */
	if (softc->flags & DA_FLAG_OPEN) {
		softc->flags &= ~DA_FLAG_OPEN;
		cam_periph_release(periph);
	} else {
		xpt_print_path(periph->path);
		kprintf("daclose() called on an already closed device!\n");
	}
	cam_periph_unlock(periph);
	return (0);	
}

/*
 * Actually translate the requested transfer into one the physical driver
 * can understand.  The transfer is described by a buf and will include
 * only one physical transfer.
 */
static int
dastrategy(struct dev_strategy_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct cam_periph *periph;
	struct da_softc *softc;
	u_int  unit;
	u_int  part;
	
	unit = dkunit(dev);
	part = dkpart(dev);
	periph = cam_extend_get(daperiphs, unit);
	if (periph == NULL) {
		bp->b_error = ENXIO;
		goto bad;		
	}
	softc = (struct da_softc *)periph->softc;
#if 0
	/*
	 * check it's not too big a transfer for our adapter
	 */
	scsi_minphys(bp, &sd_switch);
#endif

	/*
	 * Mask interrupts so that the pack cannot be invalidated until
	 * after we are in the queue.  Otherwise, we might not properly
	 * clean up one of the buffers.
	 */
	crit_enter();
	
	/*
	 * If the device has been made invalid, error out
	 */
	if ((softc->flags & DA_FLAG_PACK_INVALID)) {
		crit_exit();
		bp->b_error = ENXIO;
		goto bad;
	}
	
	/*
	 * Place it in the queue of disk activities for this disk
	 */
	bioqdisksort(&softc->bio_queue, bio);

	crit_exit();
	
	/*
	 * Schedule ourselves for performing the work.
	 */
	xpt_schedule(periph, /* XXX priority */1);

	return(0);
bad:
	bp->b_flags |= B_ERROR;

	/*
	 * Correctly set the buf to indicate a completed xfer
	 */
	bp->b_resid = bp->b_bcount;
	biodone(bio);
	return(0);
}

/* For 2.2-stable support */
#ifndef ENOIOCTL
#define ENOIOCTL -1
#endif

static int
daioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct cam_periph *periph;
	struct da_softc *softc;
	int unit;
	int error;

	unit = dkunit(dev);
	periph = cam_extend_get(daperiphs, unit);
	if (periph == NULL)
		return (ENXIO);	

	softc = (struct da_softc *)periph->softc;

	CAM_DEBUG(periph->path, CAM_DEBUG_TRACE, ("daioctl\n"));

	if ((error = cam_periph_lock(periph, PCATCH)) != 0) {
		return (error); /* error code from tsleep */
	}	

	error = cam_periph_ioctl(periph, ap->a_cmd, ap->a_data, daerror);

	cam_periph_unlock(periph);
	
	return (error);
}

static int
dadump(struct dev_dump_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct	    cam_periph *periph;
	struct	    da_softc *softc;
	u_int	    unit;
	long	    blkcnt;
	vm_paddr_t  addr;	
	struct	    ccb_scsiio csio;
	int         dumppages = MAXDUMPPGS;
	int         i;

	/* toss any characters present prior to dump */
	while (cncheckc() != -1)
		;

	unit = dkunit(dev);
	periph = cam_extend_get(daperiphs, unit);
	if (periph == NULL) {
		return (ENXIO);
	}
	softc = (struct da_softc *)periph->softc;
	
	if ((softc->flags & DA_FLAG_PACK_INVALID) != 0)
		return (ENXIO);

	addr = 0;	/* starting address */
	blkcnt = howmany(PAGE_SIZE, ap->a_secsize);

	while (ap->a_count > 0) {
		caddr_t va = NULL;

		if ((ap->a_count / blkcnt) < dumppages)
			dumppages = ap->a_count / blkcnt;

		for (i = 0; i < dumppages; ++i) {
			vm_paddr_t a = addr + (i * PAGE_SIZE);
			if (is_physical_memory(a))
				va = pmap_kenter_temporary(trunc_page(a), i);
			else
				va = pmap_kenter_temporary(trunc_page(0), i);
		}

		xpt_setup_ccb(&csio.ccb_h, periph->path, /*priority*/1);
		csio.ccb_h.ccb_state = DA_CCB_DUMP;
		scsi_read_write(&csio,
				/*retries*/1,
				dadone,
				MSG_ORDERED_Q_TAG,
				/*read*/FALSE,
				/*byte2*/0,
				/*minimum_cmd_size*/ softc->minimum_cmd_size,
				ap->a_blkno,
				blkcnt * dumppages,
				/*data_ptr*/(u_int8_t *) va,
				/*dxfer_len*/blkcnt * ap->a_secsize * dumppages,
				/*sense_len*/SSD_FULL_SIZE,
				DA_DEFAULT_TIMEOUT * 1000);		
		xpt_polled_action((union ccb *)&csio);

		if ((csio.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			kprintf("Aborting dump due to I/O error.\n");
			if ((csio.ccb_h.status & CAM_STATUS_MASK) ==
			     CAM_SCSI_STATUS_ERROR)
				scsi_sense_print(&csio);
			else
				kprintf("status == 0x%x, scsi status == 0x%x\n",
				       csio.ccb_h.status, csio.scsi_status);
			return(EIO);
		}
		
		if (dumpstatus(addr, (off_t)ap->a_count * softc->params.secsize) < 0)
			return (EINTR);

		/* update block count */
		ap->a_count -= blkcnt * dumppages;
		ap->a_blkno += blkcnt * dumppages;
		addr += PAGE_SIZE * dumppages;
	}

	/*
	 * Sync the disk cache contents to the physical media.
	 */
	if ((softc->quirks & DA_Q_NO_SYNC_CACHE) == 0) {

		xpt_setup_ccb(&csio.ccb_h, periph->path, /*priority*/1);
		csio.ccb_h.ccb_state = DA_CCB_DUMP;
		scsi_synchronize_cache(&csio,
				       /*retries*/1,
				       /*cbfcnp*/dadone,
				       MSG_SIMPLE_Q_TAG,
				       /*begin_lba*/0,/* Cover the whole disk */
				       /*lb_count*/0,
				       SSD_FULL_SIZE,
				       5 * 60 * 1000);
		xpt_polled_action((union ccb *)&csio);

		if ((csio.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			if ((csio.ccb_h.status & CAM_STATUS_MASK) ==
			     CAM_SCSI_STATUS_ERROR) {
				int asc, ascq;
				int sense_key, error_code;

				scsi_extract_sense(&csio.sense_data,
						   &error_code,
						   &sense_key, 
						   &asc, &ascq);
				if (sense_key != SSD_KEY_ILLEGAL_REQUEST)
					scsi_sense_print(&csio);
			} else {
				xpt_print_path(periph->path);
				kprintf("Synchronize cache failed, status "
				       "== 0x%x, scsi status == 0x%x\n",
				       csio.ccb_h.status, csio.scsi_status);
			}
		}
	}
	return (0);
}

static void
dainit(void)
{
	cam_status status;
	struct cam_path *path;

	/*
	 * Create our extend array for storing the devices we attach to.
	 */
	daperiphs = cam_extend_new();
	SLIST_INIT(&softc_list);
	if (daperiphs == NULL) {
		kprintf("da: Failed to alloc extend array!\n");
		return;
	}

	callout_init(&dasendorderedtag_ch);

	/*
	 * Install a global async callback.  This callback will
	 * receive async callbacks like "new device found".
	 */
	status = xpt_create_path(&path, /*periph*/NULL, CAM_XPT_PATH_ID,
				 CAM_TARGET_WILDCARD, CAM_LUN_WILDCARD);

	if (status == CAM_REQ_CMP) {
		struct ccb_setasync csa;

                xpt_setup_ccb(&csa.ccb_h, path, /*priority*/5);
                csa.ccb_h.func_code = XPT_SASYNC_CB;
                csa.event_enable = AC_FOUND_DEVICE;
                csa.callback = daasync;
                csa.callback_arg = NULL;
                xpt_action((union ccb *)&csa);
		status = csa.ccb_h.status;
                xpt_free_path(path);
        }

	if (status != CAM_REQ_CMP) {
		kprintf("da: Failed to attach master async callback "
		       "due to status 0x%x!\n", status);
	} else {

		/*
		 * Schedule a periodic event to occasionally send an
		 * ordered tag to a device.
		 */
		callout_reset(&dasendorderedtag_ch,
		    (DA_DEFAULT_TIMEOUT * hz) / DA_ORDEREDTAG_INTERVAL,
		    dasendorderedtag, NULL);

		/* Register our shutdown event handler */
		if ((EVENTHANDLER_REGISTER(shutdown_post_sync, dashutdown, 
					   NULL, SHUTDOWN_PRI_DEFAULT)) == NULL)
		    kprintf("dainit: shutdown event registration failed!\n");
	}
}

static void
daoninvalidate(struct cam_periph *periph)
{
	struct da_softc *softc;
	struct bio *q_bio;
	struct buf *q_bp;
	struct ccb_setasync csa;

	softc = (struct da_softc *)periph->softc;

	/*
	 * De-register any async callbacks.
	 */
	xpt_setup_ccb(&csa.ccb_h, periph->path,
		      /* priority */ 5);
	csa.ccb_h.func_code = XPT_SASYNC_CB;
	csa.event_enable = 0;
	csa.callback = daasync;
	csa.callback_arg = periph;
	xpt_action((union ccb *)&csa);

	softc->flags |= DA_FLAG_PACK_INVALID;

	/*
	 * Use a critical section to keep the buffer queue from being
	 * modified while we traverse it.
	 */
	crit_enter();

	/*
	 * Return all queued I/O with ENXIO.
	 * XXX Handle any transactions queued to the card
	 *     with XPT_ABORT_CCB.
	 */
	while ((q_bio = bioq_first(&softc->bio_queue)) != NULL){
		bioq_remove(&softc->bio_queue, q_bio);
		q_bp = q_bio->bio_buf;
		q_bp->b_resid = q_bp->b_bcount;
		q_bp->b_error = ENXIO;
		q_bp->b_flags |= B_ERROR;
		biodone(q_bio);
	}
	crit_exit();

	SLIST_REMOVE(&softc_list, softc, da_softc, links);

	xpt_print_path(periph->path);
	kprintf("lost device\n");
}

static void
dacleanup(struct cam_periph *periph)
{
	struct da_softc *softc;

	softc = (struct da_softc *)periph->softc;

	devstat_remove_entry(&softc->device_stats);
	cam_extend_release(daperiphs, periph->unit_number);
	xpt_print_path(periph->path);
	kprintf("removing device entry\n");
	/*
	 * If we can't free the sysctl tree, oh well...
	 */
	if ((softc->flags & DA_FLAG_SCTX_INIT) != 0
	    && sysctl_ctx_free(&softc->sysctl_ctx) != 0) {
		xpt_print_path(periph->path);
		kprintf("can't remove sysctl context\n");
	}
	if (softc->disk.d_rawdev) {
		disk_destroy(&softc->disk);
	}
	kfree(softc, M_DEVBUF);
}

static void
daasync(void *callback_arg, u_int32_t code,
	struct cam_path *path, void *arg)
{
	struct cam_periph *periph;

	periph = (struct cam_periph *)callback_arg;
	switch (code) {
	case AC_FOUND_DEVICE:
	{
		struct ccb_getdev *cgd;
		cam_status status;
 
		cgd = (struct ccb_getdev *)arg;

		if (SID_TYPE(&cgd->inq_data) != T_DIRECT
		    && SID_TYPE(&cgd->inq_data) != T_RBC
		    && SID_TYPE(&cgd->inq_data) != T_OPTICAL)
			break;

		/*
		 * Allocate a peripheral instance for
		 * this device and start the probe
		 * process.
		 */
		status = cam_periph_alloc(daregister, daoninvalidate,
					  dacleanup, dastart,
					  "da", CAM_PERIPH_BIO,
					  cgd->ccb_h.path, daasync,
					  AC_FOUND_DEVICE, cgd);

		if (status != CAM_REQ_CMP
		 && status != CAM_REQ_INPROG)
			kprintf("daasync: Unable to attach to new device "
				"due to status 0x%x\n", status);
		break;
	}
	case AC_SENT_BDR:
	case AC_BUS_RESET:
	{
		struct da_softc *softc;
		struct ccb_hdr *ccbh;

		softc = (struct da_softc *)periph->softc;
		crit_enter();
		/*
		 * Don't fail on the expected unit attention
		 * that will occur.
		 */
		softc->flags |= DA_FLAG_RETRY_UA;
		for (ccbh = LIST_FIRST(&softc->pending_ccbs);
		     ccbh != NULL; ccbh = LIST_NEXT(ccbh, periph_links.le))
			ccbh->ccb_state |= DA_CCB_RETRY_UA;
		crit_exit();
		/* FALLTHROUGH*/
	}
	default:
		cam_periph_async(periph, code, path, arg);
		break;
	}
}

static void
dasysctlinit(void *context, int pending)
{
	struct cam_periph *periph;
	struct da_softc *softc;
	char tmpstr[80], tmpstr2[80];

	periph = (struct cam_periph *)context;
	softc = (struct da_softc *)periph->softc;

	ksnprintf(tmpstr, sizeof(tmpstr), "CAM DA unit %d", periph->unit_number);
	ksnprintf(tmpstr2, sizeof(tmpstr2), "%d", periph->unit_number);

	sysctl_ctx_init(&softc->sysctl_ctx);
	softc->flags |= DA_FLAG_SCTX_INIT;
	softc->sysctl_tree = SYSCTL_ADD_NODE(&softc->sysctl_ctx,
		SYSCTL_STATIC_CHILDREN(_kern_cam_da), OID_AUTO, tmpstr2,
		CTLFLAG_RD, 0, tmpstr);
	if (softc->sysctl_tree == NULL) {
		kprintf("dasysctlinit: unable to allocate sysctl tree\n");
		return;
	}

	/*
	 * Now register the sysctl handler, so the user can the value on
	 * the fly.
	 */
	SYSCTL_ADD_PROC(&softc->sysctl_ctx,SYSCTL_CHILDREN(softc->sysctl_tree),
		OID_AUTO, "minimum_cmd_size", CTLTYPE_INT | CTLFLAG_RW,
		&softc->minimum_cmd_size, 0, dacmdsizesysctl, "I",
		"Minimum CDB size");
}

static int
dacmdsizesysctl(SYSCTL_HANDLER_ARGS)
{
	int error, value;

	value = *(int *)arg1;

	error = sysctl_handle_int(oidp, &value, 0, req);

	if ((error != 0)
	 || (req->newptr == NULL))
		return (error);

	/*
	 * Acceptable values here are 6, 10 or 12, or 16.
	 */
	if (value < 6)
		value = 6;
	else if ((value > 6)
	      && (value <= 10))
		value = 10;
	else if ((value > 10)
	      && (value <= 12))
		value = 12;
	else if (value > 12)
		value = 16;

	*(int *)arg1 = value;

	return (0);
}

static cam_status
daregister(struct cam_periph *periph, void *arg)
{
	struct da_softc *softc;
	struct ccb_setasync csa;
	struct ccb_pathinq cpi;
	struct ccb_getdev *cgd;
	char tmpstr[80];
	caddr_t match;

	cgd = (struct ccb_getdev *)arg;
	if (periph == NULL) {
		kprintf("daregister: periph was NULL!!\n");
		return(CAM_REQ_CMP_ERR);
	}

	if (cgd == NULL) {
		kprintf("daregister: no getdev CCB, can't register device\n");
		return(CAM_REQ_CMP_ERR);
	}

	softc = kmalloc(sizeof(*softc), M_DEVBUF, M_INTWAIT | M_ZERO);
	LIST_INIT(&softc->pending_ccbs);
	softc->state = DA_STATE_PROBE;
	bioq_init(&softc->bio_queue);
	if (SID_IS_REMOVABLE(&cgd->inq_data))
		softc->flags |= DA_FLAG_PACK_REMOVABLE;
	if ((cgd->inq_data.flags & SID_CmdQue) != 0)
		softc->flags |= DA_FLAG_TAGGED_QUEUING;

	periph->softc = softc;
	
	cam_extend_set(daperiphs, periph->unit_number, periph);

	/*
	 * See if this device has any quirks.
	 */
	match = cam_quirkmatch((caddr_t)&cgd->inq_data,
			       (caddr_t)da_quirk_table,
			       sizeof(da_quirk_table)/sizeof(*da_quirk_table),
			       sizeof(*da_quirk_table), scsi_inquiry_match);

	if (match != NULL)
		softc->quirks = ((struct da_quirk_entry *)match)->quirks;
	else
		softc->quirks = DA_Q_NONE;

	TASK_INIT(&softc->sysctl_task, 0, dasysctlinit, periph);

	/* Check if the SIM does not want 6 byte commands */
	xpt_setup_ccb(&cpi.ccb_h, periph->path, /*priority*/1);
	cpi.ccb_h.func_code = XPT_PATH_INQ;
	xpt_action((union ccb *)&cpi);
	if (cpi.ccb_h.status == CAM_REQ_CMP && (cpi.hba_misc & PIM_NO_6_BYTE))
		softc->quirks |= DA_Q_NO_6_BYTE;

	/*
	 * RBC devices don't have to support READ(6), only READ(10).
	 */
	if (softc->quirks & DA_Q_NO_6_BYTE || SID_TYPE(&cgd->inq_data) == T_RBC)
		softc->minimum_cmd_size = 10;
	else
		softc->minimum_cmd_size = 6;

	/*
	 * Load the user's default, if any.
	 */
	ksnprintf(tmpstr, sizeof(tmpstr), "kern.cam.da.%d.minimum_cmd_size",
		 periph->unit_number);
	TUNABLE_INT_FETCH(tmpstr, &softc->minimum_cmd_size);

	/*
	 * 6, 10, 12, and 16 are the currently permissible values.
	 */
	if (softc->minimum_cmd_size < 6)
		softc->minimum_cmd_size = 6;
	else if ((softc->minimum_cmd_size > 6)
	      && (softc->minimum_cmd_size <= 10))
		softc->minimum_cmd_size = 10;
	else if ((softc->minimum_cmd_size > 10)
	      && (softc->minimum_cmd_size <= 12))
		softc->minimum_cmd_size = 12;
	else if (softc->minimum_cmd_size > 12)
		softc->minimum_cmd_size = 16;

	/*
	 * Block our timeout handler while we
	 * add this softc to the dev list.
	 */
	crit_enter();
	SLIST_INSERT_HEAD(&softc_list, softc, links);
	crit_exit();

	/*
	 * The DA driver supports a blocksize, but
	 * we don't know the blocksize until we do 
	 * a read capacity.  So, set a flag to
	 * indicate that the blocksize is 
	 * unavailable right now.  We'll clear the
	 * flag as soon as we've done a read capacity.
	 */
	devstat_add_entry(&softc->device_stats, "da", 
			  periph->unit_number, 0,
	  		  DEVSTAT_BS_UNAVAILABLE,
			  SID_TYPE(&cgd->inq_data) | DEVSTAT_TYPE_IF_SCSI,
			  DEVSTAT_PRIORITY_DISK);

	/*
	 * Register this media as a disk
	 */
	disk_create(periph->unit_number, &softc->disk, &da_ops);

	/*
	 * Add async callbacks for bus reset and
	 * bus device reset calls.  I don't bother
	 * checking if this fails as, in most cases,
	 * the system will function just fine without
	 * them and the only alternative would be to
	 * not attach the device on failure.
	 */
	xpt_setup_ccb(&csa.ccb_h, periph->path, /*priority*/5);
	csa.ccb_h.func_code = XPT_SASYNC_CB;
	csa.event_enable = AC_SENT_BDR | AC_BUS_RESET | AC_LOST_DEVICE;
	csa.callback = daasync;
	csa.callback_arg = periph;
	xpt_action((union ccb *)&csa);
	/*
	 * Lock this peripheral until we are setup.
	 * This first call can't block
	 */
	cam_periph_lock(periph, 0);
	xpt_schedule(periph, /*priority*/5);

	return(CAM_REQ_CMP);
}

static void
dastart(struct cam_periph *periph, union ccb *start_ccb)
{
	struct da_softc *softc;

	softc = (struct da_softc *)periph->softc;

	
	switch (softc->state) {
	case DA_STATE_NORMAL:
	{
		/* Pull a buffer from the queue and get going on it */		
		struct bio *bio;
		struct buf *bp;

		/*
		 * See if there is a buf with work for us to do..
		 */
		crit_enter();
		bio = bioq_first(&softc->bio_queue);
		if (periph->immediate_priority <= periph->pinfo.priority) {
			CAM_DEBUG_PRINT(CAM_DEBUG_SUBTRACE,
					("queuing for immediate ccb\n"));
			start_ccb->ccb_h.ccb_state = DA_CCB_WAITING;
			SLIST_INSERT_HEAD(&periph->ccb_list, &start_ccb->ccb_h,
					  periph_links.sle);
			periph->immediate_priority = CAM_PRIORITY_NONE;
			crit_exit();
			wakeup(&periph->ccb_list);
		} else if (bio == NULL) {
			crit_exit();
			xpt_release_ccb(start_ccb);
		} else {
			u_int8_t tag_code;

			bioq_remove(&softc->bio_queue, bio);
			bp = bio->bio_buf;

			devstat_start_transaction(&softc->device_stats);

			if ((bp->b_flags & B_ORDERED) != 0
			 || (softc->flags & DA_FLAG_NEED_OTAG) != 0) {
				softc->flags &= ~DA_FLAG_NEED_OTAG;
				softc->ordered_tag_count++;
				tag_code = MSG_ORDERED_Q_TAG;
			} else {
				tag_code = MSG_SIMPLE_Q_TAG;
			}

			KKASSERT(bio->bio_offset % softc->params.secsize == 0);

			scsi_read_write(&start_ccb->csio,
					/*retries*/da_retry_count,
					dadone,
					tag_code,
					(bp->b_cmd == BUF_CMD_READ),
					/*byte2*/0,
					softc->minimum_cmd_size,
					bio->bio_offset / softc->params.secsize,
					bp->b_bcount / softc->params.secsize,
					bp->b_data,
					bp->b_bcount,
					/*sense_len*/SSD_FULL_SIZE,
					da_default_timeout * 1000);
			start_ccb->ccb_h.ccb_state = DA_CCB_BUFFER_IO;

			/*
			 * Block out any asyncronous callbacks
			 * while we touch the pending ccb list.
			 */
			LIST_INSERT_HEAD(&softc->pending_ccbs,
					 &start_ccb->ccb_h, periph_links.le);

			/* We expect a unit attention from this device */
			if ((softc->flags & DA_FLAG_RETRY_UA) != 0) {
				start_ccb->ccb_h.ccb_state |= DA_CCB_RETRY_UA;
				softc->flags &= ~DA_FLAG_RETRY_UA;
			}

			start_ccb->ccb_h.ccb_bio = bio;
			bio = bioq_first(&softc->bio_queue);
			crit_exit();

			xpt_action(start_ccb);
		}
		
		if (bio != NULL) {
			/* Have more work to do, so ensure we stay scheduled */
			xpt_schedule(periph, /* XXX priority */1);
		}
		break;
	}
	case DA_STATE_PROBE:
	{
		struct ccb_scsiio *csio;
		struct scsi_read_capacity_data *rcap;

		rcap = kmalloc(sizeof(*rcap), M_TEMP, M_INTWAIT | M_ZERO);
		csio = &start_ccb->csio;
		scsi_read_capacity(csio,
				   /*retries*/4,
				   dadone,
				   MSG_SIMPLE_Q_TAG,
				   rcap,
				   SSD_FULL_SIZE,
				   /*timeout*/5000);
		start_ccb->ccb_h.ccb_bio = NULL;
		start_ccb->ccb_h.ccb_state = DA_CCB_PROBE;
		xpt_action(start_ccb);
		break;
	}
	case DA_STATE_PROBE2:
	{
		struct ccb_scsiio *csio;
		struct scsi_read_capacity_data_long *rcaplong;

		rcaplong = (struct scsi_read_capacity_data_long *)
		kmalloc(sizeof(*rcaplong), M_TEMP, M_INTWAIT);
		if (rcaplong == NULL) {
			kprintf("dastart: Couldn't allocate read_capacity\n");
			/* da_free_periph??? */
			break;
		}
		csio = &start_ccb->csio;
		scsi_read_capacity_16(csio,
				    /*retries*/ 4,
				    /*cbfcnp*/ dadone,
				    /*tag_action*/ MSG_SIMPLE_Q_TAG,
				    /*lba*/ 0,
				    /*reladr*/ 0,
				    /*pmi*/ 0,
				    rcaplong,
				    /*sense_len*/ SSD_FULL_SIZE,
				    /*timeout*/ 60000);
		start_ccb->ccb_h.ccb_bio = NULL;
		start_ccb->ccb_h.ccb_state = DA_CCB_PROBE2;
		xpt_action(start_ccb);  
		break;
	}
	}
}

static int
cmd6workaround(union ccb *ccb)
{
	struct scsi_rw_6 cmd6;
	struct scsi_rw_10 *cmd10;
	struct da_softc *softc;
	u_int8_t *cdb;
	int frozen;

	cdb = ccb->csio.cdb_io.cdb_bytes;

	/* Translation only possible if CDB is an array and cmd is R/W6 */
	if ((ccb->ccb_h.flags & CAM_CDB_POINTER) != 0 ||
	    (*cdb != READ_6 && *cdb != WRITE_6))
		return 0;

	xpt_print_path(ccb->ccb_h.path);
 	kprintf("READ(6)/WRITE(6) not supported, "
	       "increasing minimum_cmd_size to 10.\n");
 	softc = (struct da_softc *)xpt_path_periph(ccb->ccb_h.path)->softc;
	softc->minimum_cmd_size = 10;

	bcopy(cdb, &cmd6, sizeof(struct scsi_rw_6));
	cmd10 = (struct scsi_rw_10 *)cdb;
	cmd10->opcode = (cmd6.opcode == READ_6) ? READ_10 : WRITE_10;
	cmd10->byte2 = 0;
	scsi_ulto4b(scsi_3btoul(cmd6.addr), cmd10->addr);
	cmd10->reserved = 0;
	scsi_ulto2b(cmd6.length, cmd10->length);
	cmd10->control = cmd6.control;
	ccb->csio.cdb_len = sizeof(*cmd10);

	/* Requeue request, unfreezing queue if necessary */
	frozen = (ccb->ccb_h.status & CAM_DEV_QFRZN) != 0;
 	ccb->ccb_h.status = CAM_REQUEUE_REQ;
	xpt_action(ccb);
	if (frozen) {
		cam_release_devq(ccb->ccb_h.path,
				 /*relsim_flags*/0,
				 /*reduction*/0,
				 /*timeout*/0,
				 /*getcount_only*/0);
	}
	return (ERESTART);
}

static void
dadone(struct cam_periph *periph, union ccb *done_ccb)
{
	struct da_softc *softc;
	struct ccb_scsiio *csio;

	softc = (struct da_softc *)periph->softc;
	csio = &done_ccb->csio;
	switch (csio->ccb_h.ccb_state & DA_CCB_TYPE_MASK) {
	case DA_CCB_BUFFER_IO:
	{
		struct buf *bp;
		struct bio *bio;

		bio = (struct bio *)done_ccb->ccb_h.ccb_bio;
		bp = bio->bio_buf;
		if ((done_ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			int error;
			int sf;
			
			if ((csio->ccb_h.ccb_state & DA_CCB_RETRY_UA) != 0)
				sf = SF_RETRY_UA;
			else
				sf = 0;

			/* Retry selection timeouts */
			sf |= SF_RETRY_SELTO;

			if ((error = daerror(done_ccb, 0, sf)) == ERESTART) {
				/*
				 * A retry was scheuled, so
				 * just return.
				 */
				return;
			}
			if (error != 0) {
				struct bio *q_bio;
				struct buf *q_bp;

				crit_enter();

				if (error == ENXIO) {
					/*
					 * Catastrophic error.  Mark our pack as
					 * invalid.
					 */
					/* XXX See if this is really a media
					 *     change first.
					 */
					xpt_print_path(periph->path);
					kprintf("Invalidating pack\n");
					softc->flags |= DA_FLAG_PACK_INVALID;
				}

				/*
				 * return all queued I/O with EIO, so that
				 * the client can retry these I/Os in the
				 * proper order should it attempt to recover.
				 */
				while ((q_bio = bioq_first(&softc->bio_queue))
					!= NULL) {
					bioq_remove(&softc->bio_queue, q_bio);
					q_bp = q_bio->bio_buf;
					q_bp->b_resid = q_bp->b_bcount;
					q_bp->b_error = EIO;
					q_bp->b_flags |= B_ERROR;
					biodone(q_bio);
				}
				crit_exit();
				bp->b_error = error;
				bp->b_resid = bp->b_bcount;
				bp->b_flags |= B_ERROR;
			} else {
				bp->b_resid = csio->resid;
				bp->b_error = 0;
				if (bp->b_resid != 0)
					bp->b_flags |= B_ERROR;
			}
			if ((done_ccb->ccb_h.status & CAM_DEV_QFRZN) != 0)
				cam_release_devq(done_ccb->ccb_h.path,
						 /*relsim_flags*/0,
						 /*reduction*/0,
						 /*timeout*/0,
						 /*getcount_only*/0);
		} else {
			bp->b_resid = csio->resid;
			if (csio->resid > 0)
				bp->b_flags |= B_ERROR;
		}

		/*
		 * Block out any asyncronous callbacks
		 * while we touch the pending ccb list.
		 */
		crit_enter();
		LIST_REMOVE(&done_ccb->ccb_h, periph_links.le);
		crit_exit();

		if (softc->device_stats.busy_count == 0)
			softc->flags |= DA_FLAG_WENT_IDLE;

		devstat_end_transaction_buf(&softc->device_stats, bp);
		biodone(bio);
		break;
	}
	case DA_CCB_PROBE:
	case DA_CCB_PROBE2:
	{
		struct	   scsi_read_capacity_data *rdcap;
		struct     scsi_read_capacity_data_long *rcaplong;
		char	   announce_buf[80];

		rdcap = NULL;
		rcaplong = NULL;
		if (softc->state == DA_STATE_PROBE)
			rdcap =(struct scsi_read_capacity_data *)csio->data_ptr;
		else
			rcaplong = (struct scsi_read_capacity_data_long *)
				    csio->data_ptr;
		
		if ((csio->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP) {
			struct disk_params *dp;
			uint32_t block_size;
			uint64_t maxsector;

			if (softc->state == DA_STATE_PROBE) {
				block_size = scsi_4btoul(rdcap->length);
				maxsector = scsi_4btoul(rdcap->addr);

				/*
				 * According to SBC-2, if the standard 10
				 * byte READ CAPACITY command returns 2^32,
				 * we should issue the 16 byte version of
				 * the command, since the device in question
				 * has more sectors than can be represented
				 * with the short version of the command.
				 */
				if (maxsector == 0xffffffff) {
					softc->state = DA_STATE_PROBE2;
					kfree(rdcap, M_TEMP);
					xpt_release_ccb(done_ccb);
					xpt_schedule(periph, /*priority*/5);
					return;
				}
			} else {
				block_size = scsi_4btoul(rcaplong->length);
				maxsector = scsi_8btou64(rcaplong->addr);
			}
			dasetgeom(periph, block_size, maxsector);
			dp = &softc->params;
			ksnprintf(announce_buf, sizeof(announce_buf),
				"%juMB (%ju %u byte sectors: %dH %dS/T %dC)",
				(uintmax_t) (((uintmax_t)dp->secsize *
				dp->sectors) / (1024*1024)),
				(uintmax_t)dp->sectors,
				dp->secsize, dp->heads, dp->secs_per_track,
				dp->cylinders);
		} else {
			int	error;

			announce_buf[0] = '\0';

			/*
			 * Retry any UNIT ATTENTION type errors.  They
			 * are expected at boot.
			 */
			error = daerror(done_ccb, 0, SF_RETRY_UA |
					SF_RETRY_SELTO | SF_NO_PRINT);
			if (error == ERESTART) {
				/*
				 * A retry was scheuled, so
				 * just return.
				 */
				return;
			} else if (error != 0) {
				struct scsi_sense_data *sense;
				int asc, ascq;
				int sense_key, error_code;
				int have_sense;
				cam_status status;
				struct ccb_getdev cgd;

				/* Don't wedge this device's queue */
				cam_release_devq(done_ccb->ccb_h.path,
						 /*relsim_flags*/0,
						 /*reduction*/0,
						 /*timeout*/0,
						 /*getcount_only*/0);

				status = done_ccb->ccb_h.status;

				xpt_setup_ccb(&cgd.ccb_h, 
					      done_ccb->ccb_h.path,
					      /* priority */ 1);
				cgd.ccb_h.func_code = XPT_GDEV_TYPE;
				xpt_action((union ccb *)&cgd);

				if (((csio->ccb_h.flags & CAM_SENSE_PHYS) != 0)
				 || ((csio->ccb_h.flags & CAM_SENSE_PTR) != 0)
				 || ((status & CAM_AUTOSNS_VALID) == 0))
					have_sense = FALSE;
				else
					have_sense = TRUE;

				if (have_sense) {
					sense = &csio->sense_data;
					scsi_extract_sense(sense, &error_code,
							   &sense_key, 
							   &asc, &ascq);
				}
				/*
				 * Attach to anything that claims to be a
				 * direct access or optical disk device,
				 * as long as it doesn't return a "Logical
				 * unit not supported" (0x25) error.
				 */
				if ((have_sense) && (asc != 0x25)
				 && (error_code == SSD_CURRENT_ERROR))
					ksnprintf(announce_buf,
					    sizeof(announce_buf),
						"Attempt to query device "
						"size failed: %s, %s",
						scsi_sense_key_text[sense_key],
						scsi_sense_desc(asc,ascq,
								&cgd.inq_data));
				else { 
					if (have_sense)
						scsi_sense_print(
							&done_ccb->csio);
					else {
						xpt_print_path(periph->path);
						kprintf("got CAM status %#x\n",
						       done_ccb->ccb_h.status);
					}

					xpt_print_path(periph->path);
					kprintf("fatal error, failed" 
					       " to attach to device\n");

					/*
					 * Free up resources.
					 */
					cam_periph_invalidate(periph);
				} 
			}
		}
		kfree(csio->data_ptr, M_TEMP);
		if (announce_buf[0] != '\0') {
			xpt_announce_periph(periph, announce_buf);
			/*
			 * Create our sysctl variables, now that we know
			 * we have successfully attached.
			 */
			taskqueue_enqueue(taskqueue_thread[mycpuid],
			    &softc->sysctl_task);
		}
		softc->state = DA_STATE_NORMAL;		
		/*
		 * Since our peripheral may be invalidated by an error
		 * above or an external event, we must release our CCB
		 * before releasing the probe lock on the peripheral.
		 * The peripheral will only go away once the last lock
		 * is removed, and we need it around for the CCB release
		 * operation.
		 */
		xpt_release_ccb(done_ccb);
		cam_periph_unlock(periph);
		return;
	}
	case DA_CCB_WAITING:
	{
		/* Caller will release the CCB */
		wakeup(&done_ccb->ccb_h.cbfcnp);
		return;
	}
	case DA_CCB_DUMP:
		/* No-op.  We're polling */
		return;
	default:
		break;
	}
	xpt_release_ccb(done_ccb);
}

static int
daerror(union ccb *ccb, u_int32_t cam_flags, u_int32_t sense_flags)
{
	struct da_softc	  *softc;
	struct cam_periph *periph;
	int error;

	periph = xpt_path_periph(ccb->ccb_h.path);
	softc = (struct da_softc *)periph->softc;

 	/*
	 * Automatically detect devices that do not support
 	 * READ(6)/WRITE(6) and upgrade to using 10 byte cdbs.
 	 */
	error = 0;
	if ((ccb->ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_INVALID) {
		error = cmd6workaround(ccb);
	} else if (((ccb->ccb_h.status & CAM_STATUS_MASK) ==
		   CAM_SCSI_STATUS_ERROR)
	 && (ccb->ccb_h.status & CAM_AUTOSNS_VALID)
	 && (ccb->csio.scsi_status == SCSI_STATUS_CHECK_COND)
	 && ((ccb->ccb_h.flags & CAM_SENSE_PHYS) == 0)
	 && ((ccb->ccb_h.flags & CAM_SENSE_PTR) == 0)) {
		int sense_key, error_code, asc, ascq;

 		scsi_extract_sense(&ccb->csio.sense_data,
				   &error_code, &sense_key, &asc, &ascq);
		if (sense_key == SSD_KEY_ILLEGAL_REQUEST)
 			error = cmd6workaround(ccb);
	}
	if (error == ERESTART)
		return (ERESTART);

	/*
	 * XXX
	 * Until we have a better way of doing pack validation,
	 * don't treat UAs as errors.
	 */
	sense_flags |= SF_RETRY_UA;
	return(cam_periph_error(ccb, cam_flags, sense_flags,
				&softc->saved_ccb));
}

static void
daprevent(struct cam_periph *periph, int action)
{
	struct	da_softc *softc;
	union	ccb *ccb;		
	int	error;
		
	softc = (struct da_softc *)periph->softc;

	if (((action == PR_ALLOW)
	  && (softc->flags & DA_FLAG_PACK_LOCKED) == 0)
	 || ((action == PR_PREVENT)
	  && (softc->flags & DA_FLAG_PACK_LOCKED) != 0)) {
		return;
	}

	ccb = cam_periph_getccb(periph, /*priority*/1);

	scsi_prevent(&ccb->csio,
		     /*retries*/1,
		     /*cbcfp*/dadone,
		     MSG_SIMPLE_Q_TAG,
		     action,
		     SSD_FULL_SIZE,
		     5000);

	error = cam_periph_runccb(ccb, /*error_routine*/NULL, /*cam_flags*/0,
				  /*sense_flags*/0, &softc->device_stats);

	if (error == 0) {
		if (action == PR_ALLOW)
			softc->flags &= ~DA_FLAG_PACK_LOCKED;
		else
			softc->flags |= DA_FLAG_PACK_LOCKED;
	}

	xpt_release_ccb(ccb);
}

static int
dagetcapacity(struct cam_periph *periph)
{
	struct da_softc *softc;
	union ccb *ccb;
	struct scsi_read_capacity_data *rcap;
	struct scsi_read_capacity_data_long *rcaplong;
	uint32_t block_len;
	uint64_t maxsector;
	int error;
 
	softc = (struct da_softc *)periph->softc;
	block_len = 0;
	maxsector = 0;
	error = 0;
 
	/* Do a read capacity */
	rcap = (void *)kmalloc(sizeof(*rcaplong), M_TEMP, M_INTWAIT);
		
	ccb = cam_periph_getccb(periph, /*priority*/1);
	scsi_read_capacity(&ccb->csio,
			   /*retries*/4,
			   /*cbfncp*/dadone,
			   MSG_SIMPLE_Q_TAG,
			   rcap,
			   SSD_FULL_SIZE,
			   /*timeout*/60000);
	ccb->ccb_h.ccb_bio = NULL;
 
	error = cam_periph_runccb(ccb, daerror,
				  /*cam_flags*/SF_RETRY_SELTO,
				  /*sense_flags*/SF_RETRY_UA,
				  &softc->device_stats);
 
	if ((ccb->ccb_h.status & CAM_DEV_QFRZN) != 0)
		cam_release_devq(ccb->ccb_h.path,
				 /*relsim_flags*/0,
				 /*reduction*/0,
				 /*timeout*/0,
				 /*getcount_only*/0);
 
	if (error == 0) {
		block_len = scsi_4btoul(rcap->length);
		maxsector = scsi_4btoul(rcap->addr);

		if (maxsector != 0xffffffff)
			goto done;
	} else
		goto done;
 
	rcaplong = (struct scsi_read_capacity_data_long *)rcap;
 
	scsi_read_capacity_16(&ccb->csio,
			      /*retries*/ 4,
			      /*cbfcnp*/ dadone,
			      /*tag_action*/ MSG_SIMPLE_Q_TAG,
			      /*lba*/ 0,
			      /*reladr*/ 0,
			      /*pmi*/ 0,
			      rcaplong,
			      /*sense_len*/ SSD_FULL_SIZE,
			      /*timeout*/ 60000);
	ccb->ccb_h.ccb_bio = NULL;
 
	error = cam_periph_runccb(ccb, daerror,
				  /*cam_flags*/SF_RETRY_SELTO,
				  /*sense_flags*/SF_RETRY_UA,
				  &softc->device_stats);
 
	if ((ccb->ccb_h.status & CAM_DEV_QFRZN) != 0)
		cam_release_devq(ccb->ccb_h.path,
				 /*relsim_flags*/0,
				 /*reduction*/0,
				 /*timeout*/0,
				 /*getcount_only*/0);
 
	if (error == 0) {
		block_len = scsi_4btoul(rcaplong->length);
		maxsector = scsi_8btou64(rcaplong->addr);
	}

done:

	if (error == 0)
		dasetgeom(periph, block_len, maxsector);
 
	xpt_release_ccb(ccb);
 
	kfree(rcap, M_TEMP);
 
	return (error);
}

static void
dasetgeom(struct cam_periph *periph, uint32_t block_len, uint64_t maxsector)
{
	struct ccb_calc_geometry ccg;
	struct da_softc *softc;
	struct disk_params *dp;

	softc = (struct da_softc *)periph->softc;

	dp = &softc->params;
	dp->secsize = block_len;
	dp->sectors = maxsector + 1;
	/*
	 * Have the controller provide us with a geometry
	 * for this disk.  The only time the geometry
	 * matters is when we boot and the controller
	 * is the only one knowledgeable enough to come
	 * up with something that will make this a bootable
	 * device.
	 */
	xpt_setup_ccb(&ccg.ccb_h, periph->path, /*priority*/1);
	ccg.ccb_h.func_code = XPT_CALC_GEOMETRY;
	ccg.block_size = dp->secsize;
	ccg.volume_size = dp->sectors;
	ccg.heads = 0;
	ccg.secs_per_track = 0;
	ccg.cylinders = 0;
	xpt_action((union ccb*)&ccg);
	dp->heads = ccg.heads;
	dp->secs_per_track = ccg.secs_per_track;
	dp->cylinders = ccg.cylinders;
}

static void
dasendorderedtag(void *arg)
{
	struct da_softc *softc;

	for (softc = SLIST_FIRST(&softc_list);
	     softc != NULL;
	     softc = SLIST_NEXT(softc, links)) {
		crit_enter();
		if ((softc->ordered_tag_count == 0) 
		 && ((softc->flags & DA_FLAG_WENT_IDLE) == 0)) {
			softc->flags |= DA_FLAG_NEED_OTAG;
		}
		if (softc->device_stats.busy_count > 0)
			softc->flags &= ~DA_FLAG_WENT_IDLE;

		softc->ordered_tag_count = 0;
		crit_exit();
	}
	/* Queue us up again */
	callout_reset(&dasendorderedtag_ch,
	    (da_default_timeout * hz) / DA_ORDEREDTAG_INTERVAL,
	    dasendorderedtag, NULL);
}

/*
 * Step through all DA peripheral drivers, and if the device is still open,
 * sync the disk cache to physical media.
 */
static void
dashutdown(void * arg, int howto)
{
	struct cam_periph *periph;
	struct da_softc *softc;

	for (periph = TAILQ_FIRST(&dadriver.units); periph != NULL;
	     periph = TAILQ_NEXT(periph, unit_links)) {
		union ccb ccb;
		softc = (struct da_softc *)periph->softc;

		/*
		 * We only sync the cache if the drive is still open, and
		 * if the drive is capable of it..
		 */
		if (((softc->flags & DA_FLAG_OPEN) == 0)
		 || (softc->quirks & DA_Q_NO_SYNC_CACHE))
			continue;

		xpt_setup_ccb(&ccb.ccb_h, periph->path, /*priority*/1);

		ccb.ccb_h.ccb_state = DA_CCB_DUMP;
		scsi_synchronize_cache(&ccb.csio,
				       /*retries*/1,
				       /*cbfcnp*/dadone,
				       MSG_SIMPLE_Q_TAG,
				       /*begin_lba*/0, /* whole disk */
				       /*lb_count*/0,
				       SSD_FULL_SIZE,
				       5 * 60 * 1000);

		xpt_polled_action(&ccb);

		if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			if (((ccb.ccb_h.status & CAM_STATUS_MASK) ==
			     CAM_SCSI_STATUS_ERROR)
			 && (ccb.csio.scsi_status == SCSI_STATUS_CHECK_COND)){
				int error_code, sense_key, asc, ascq;

				scsi_extract_sense(&ccb.csio.sense_data,
						   &error_code, &sense_key,
						   &asc, &ascq);

				if (sense_key != SSD_KEY_ILLEGAL_REQUEST)
					scsi_sense_print(&ccb.csio);
			} else {
				xpt_print_path(periph->path);
				kprintf("Synchronize cache failed, status "
				       "== 0x%x, scsi status == 0x%x\n",
				       ccb.ccb_h.status, ccb.csio.scsi_status);
			}
		}

		if ((ccb.ccb_h.status & CAM_DEV_QFRZN) != 0)
			cam_release_devq(ccb.ccb_h.path,
					 /*relsim_flags*/0,
					 /*reduction*/0,
					 /*timeout*/0,
					 /*getcount_only*/0);

	}
}

#else /* !_KERNEL */

/*
 * XXX This is only left out of the kernel build to silence warnings.  If,
 * for some reason this function is used in the kernel, the ifdefs should
 * be moved so it is included both in the kernel and userland.
 */
void
scsi_format_unit(struct ccb_scsiio *csio, u_int32_t retries,
		 void (*cbfcnp)(struct cam_periph *, union ccb *),
		 u_int8_t tag_action, u_int8_t byte2, u_int16_t ileave,
		 u_int8_t *data_ptr, u_int32_t dxfer_len, u_int8_t sense_len,
		 u_int32_t timeout)
{
	struct scsi_format_unit *scsi_cmd;

	scsi_cmd = (struct scsi_format_unit *)&csio->cdb_io.cdb_bytes;
	scsi_cmd->opcode = FORMAT_UNIT;
	scsi_cmd->byte2 = byte2;
	scsi_ulto2b(ileave, scsi_cmd->interleave);

	cam_fill_csio(csio,
		      retries,
		      cbfcnp,
		      /*flags*/ (dxfer_len > 0) ? CAM_DIR_OUT : CAM_DIR_NONE,
		      tag_action,
		      data_ptr,
		      dxfer_len,
		      sense_len,
		      sizeof(*scsi_cmd),
		      timeout);
}

#endif /* _KERNEL */
