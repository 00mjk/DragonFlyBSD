/*-
 * Copyright (c) 1994 Bruce D. Evans.
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
 * $FreeBSD: src/sys/sys/diskslice.h,v 1.36.2.1 2001/01/29 01:50:50 ken Exp $
 * $DragonFly: src/sys/sys/diskslice.h,v 1.10 2007/05/15 05:37:39 dillon Exp $
 */

#ifndef	_SYS_DISKSLICE_H_
#define	_SYS_DISKSLICE_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_IOCCOM_H_
#include <sys/ioccom.h>
#endif

#define	BASE_SLICE		2
#define	COMPATIBILITY_SLICE	0
#define	DIOCGSLICEINFO		_IOR('d', 111, struct diskslices)
#define	DIOCSYNCSLICEINFO	_IOW('d', 112, int)
#define	MAX_SLICES		16
#define	WHOLE_DISK_SLICE	1

struct	diskslice {
	u_long	ds_offset;		/* starting sector */
	u_long	ds_size;		/* number of sectors */
	int	ds_type;		/* (foreign) slice type */
	struct disklabel *ds_label;	/* BSD label, if any */
	void	*ds_dev;		/* devfs token for raw whole slice */
#ifdef MAXPARTITIONS			/* XXX don't depend on disklabel.h */
#if MAXPARTITIONS !=	16		/* but check consistency if possible */
#error "inconsistent MAXPARTITIONS"
#endif
#else
#define	MAXPARTITIONS	16
#endif
	void	*ds_devs[MAXPARTITIONS];	/* XXX s.b. in label */
	u_char	ds_openmask;		/* devs open */
	u_char	ds_wlabel;		/* nonzero if label is writable */
};

struct diskslices {
	struct cdevsw *dss_cdevsw;	/* for containing device */
	int	dss_first_bsd_slice;	/* COMPATIBILITY_SLICE is mapped here */
	u_int	dss_nslices;		/* actual dimension of dss_slices[] */
	u_int	dss_oflags;		/* copy of flags for "first" open */
	int	dss_secmult;		/* block to sector multiplier */
	int	dss_secshift;		/* block to sector shift (or -1) */
	int	dss_secsize;		/* sector size */
	struct diskslice
		dss_slices[MAX_SLICES];	/* actually usually less */
};

#ifdef _KERNEL

#define	dsgetlabel(dev, ssp)	(ssp->dss_slices[dkslice(dev)].ds_label)

struct buf;
struct bio;
struct disklabel;
struct disk_info;

int	mbrinit (cdev_t dev, struct disk_info *info,
		 struct diskslices **sspp);
struct bio *dscheck (cdev_t dev, struct bio *bio, struct diskslices *ssp);
void	dsclose (cdev_t dev, int mode, struct diskslices *ssp);
void	dsgone (struct diskslices **sspp);
int	dsioctl (cdev_t dev, u_long cmd, caddr_t data, int flags,
		     struct diskslices **sspp, struct disk_info *info);
int	dsisopen (struct diskslices *ssp);
struct diskslices *dsmakeslicestruct (int nslices, struct disk_info *info);
char	*dsname (cdev_t dev, int unit, int slice, int part,
		     char *partname);
int	dsopen (cdev_t dev, int mode, u_int flags,
		    struct diskslices **sspp, struct disk_info *info);
int	dssize (cdev_t dev, struct diskslices **sspp);

#endif /* _KERNEL */

#endif /* !_SYS_DISKSLICE_H_ */
