/*
 * Copyright (c) 1989, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)spec_vnops.c	8.14 (Berkeley) 5/21/95
 * $FreeBSD: src/sys/miscfs/specfs/spec_vnops.c,v 1.131.2.4 2001/02/26 04:23:20 jlemon Exp $
 * $DragonFly: src/sys/vfs/specfs/spec_vnops.c,v 1.55 2007/08/13 17:31:56 dillon Exp $
 */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/device.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/vmmeter.h>
#include <sys/bus.h>
#include <sys/tty.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <machine/limits.h>

#include <sys/buf2.h>

#include <sys/thread2.h>

/*
 * Specfs chained debugging (bitmask)
 *
 * 0 - disable debugging
 * 1 - report chained I/Os
 * 2 - force 4K chained I/Os
 */
#define SPEC_CHAIN_DEBUG	0

static int	spec_advlock (struct vop_advlock_args *);  
static int	spec_bmap (struct vop_bmap_args *);
static int	spec_close (struct vop_close_args *);
static int	spec_freeblks (struct vop_freeblks_args *);
static int	spec_fsync (struct  vop_fsync_args *);
static int	spec_getpages (struct vop_getpages_args *);
static int	spec_inactive (struct  vop_inactive_args *);
static int	spec_ioctl (struct vop_ioctl_args *);
static int	spec_open (struct vop_open_args *);
static int	spec_poll (struct vop_poll_args *);
static int	spec_kqfilter (struct vop_kqfilter_args *);
static int	spec_print (struct vop_print_args *);
static int	spec_read (struct vop_read_args *);  
static int	spec_strategy (struct vop_strategy_args *);
static int	spec_write (struct vop_write_args *);
static void	spec_strategy_done(struct bio *nbio);

struct vop_ops spec_vnode_vops = {
	.vop_default =		vop_defaultop,
	.vop_access =		(void *)vop_ebadf,
	.vop_advlock =		spec_advlock,
	.vop_bmap =		spec_bmap,
	.vop_close =		spec_close,
	.vop_old_create =	(void *)vop_panic,
	.vop_freeblks =		spec_freeblks,
	.vop_fsync =		spec_fsync,
	.vop_getpages =		spec_getpages,
	.vop_inactive =		spec_inactive,
	.vop_ioctl =		spec_ioctl,
	.vop_old_link =		(void *)vop_panic,
	.vop_old_mkdir =	(void *)vop_panic,
	.vop_old_mknod =	(void *)vop_panic,
	.vop_open =		spec_open,
	.vop_pathconf =		vop_stdpathconf,
	.vop_poll =		spec_poll,
	.vop_kqfilter =		spec_kqfilter,
	.vop_print =		spec_print,
	.vop_read =		spec_read,
	.vop_readdir =		(void *)vop_panic,
	.vop_readlink =		(void *)vop_panic,
	.vop_reallocblks =	(void *)vop_panic,
	.vop_reclaim =		(void *)vop_null,
	.vop_old_remove =	(void *)vop_panic,
	.vop_old_rename =	(void *)vop_panic,
	.vop_old_rmdir =	(void *)vop_panic,
	.vop_setattr =		(void *)vop_ebadf,
	.vop_strategy =		spec_strategy,
	.vop_old_symlink =	(void *)vop_panic,
	.vop_write =		spec_write
};

struct vop_ops *spec_vnode_vops_p = &spec_vnode_vops;

VNODEOP_SET(spec_vnode_vops);

extern int dev_ref_debug;

/*
 * spec_vnoperate()
 */
int
spec_vnoperate(struct vop_generic_args *ap)
{
	return (VOCALL(&spec_vnode_vops, ap));
}

static void spec_getpages_iodone (struct bio *bio);

/*
 * Open a special file.
 *
 * spec_open(struct vnode *a_vp, int a_mode, struct ucred *a_cred,
 *	     struct file *a_fp)
 */
/* ARGSUSED */
static int
spec_open(struct vop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	cdev_t dev;
	int error;
	const char *cp;

	/*
	 * Don't allow open if fs is mounted -nodev.
	 */
	if (vp->v_mount && (vp->v_mount->mnt_flag & MNT_NODEV))
		return (ENXIO);
	if (vp->v_type == VBLK)
		return (ENXIO);

	/*
	 * Resolve the device.  If the vnode is already open v_rdev may
	 * already be resolved.  However, if the device changes out from
	 * under us we report it (and, for now, we allow it).  Since
	 * v_release_rdev() zero's v_opencount, we have to save and restore
	 * it when replacing the rdev reference.
	 */
	if (vp->v_rdev != NULL) {
		dev = get_dev(vp->v_umajor, vp->v_uminor);
		if (dev != vp->v_rdev) {
			int oc = vp->v_opencount;
			kprintf(
			    "Warning: spec_open: dev %s was lost",
			    vp->v_rdev->si_name);
			v_release_rdev(vp);
			error = v_associate_rdev(vp, 
					get_dev(vp->v_umajor, vp->v_uminor));
			if (error) {
				kprintf(", reacquisition failed\n");
			} else {
				vp->v_opencount = oc;
				kprintf(", reacquisition successful\n");
			}
		} else {
			error = 0;
		}
	} else {
		error = v_associate_rdev(vp, get_dev(vp->v_umajor, vp->v_uminor));
	}
	if (error)
		return(error);

	/*
	 * Prevent degenerate open/close sequences from nulling out rdev.
	 */
	dev = vp->v_rdev;
	KKASSERT(dev != NULL);

	/*
	 * Make this field valid before any I/O in ->d_open.  XXX the
	 * device itself should probably be required to initialize
	 * this field in d_open.
	 */
	if (!dev->si_iosize_max)
		dev->si_iosize_max = DFLTPHYS;

	/*
	 * XXX: Disks get special billing here, but it is mostly wrong.
	 * XXX: diskpartitions can overlap and the real checks should
	 * XXX: take this into account, and consequently they need to
	 * XXX: live in the diskslicing code.  Some checks do.
	 */
	if (vn_isdisk(vp, NULL) && ap->a_cred != FSCRED && 
	    (ap->a_mode & FWRITE)) {
		/*
		 * Never allow opens for write if the device is mounted R/W
		 */
		if (vp->v_rdev && vp->v_rdev->si_mountpoint &&
		    !(vp->v_rdev->si_mountpoint->mnt_flag & MNT_RDONLY)) {
				error = EBUSY;
				goto done;
		}

		/*
		 * When running in secure mode, do not allow opens
		 * for writing if the device is mounted
		 */
		if (securelevel >= 1 && vfs_mountedon(vp)) {
			error = EPERM;
			goto done;
		}

		/*
		 * When running in very secure mode, do not allow
		 * opens for writing of any devices.
		 */
		if (securelevel >= 2) {
			error = EPERM;
			goto done;
		}
	}

	/* XXX: Special casing of ttys for deadfs.  Probably redundant */
	if (dev_dflags(dev) & D_TTY)
		vp->v_flag |= VISTTY;

	/*
	 * dev_dopen() is always called for each open.  dev_dclose() is
	 * only called for the last close unless D_TRACKCLOSE is set.
	 */
	vn_unlock(vp);
	error = dev_dopen(dev, ap->a_mode, S_IFCHR, ap->a_cred);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);

	if (error)
		goto done;

	if (dev_dflags(dev) & D_TTY) {
		if (dev->si_tty) {
			struct tty *tp;
			tp = dev->si_tty;
			if (!tp->t_stop) {
				kprintf("Warning:%s: no t_stop, using nottystop\n", devtoname(dev));
				tp->t_stop = nottystop;
			}
		}
	}

	/*
	 * If this is 'disk' or disk-like device, associate a VM object
	 * with it.
	 */
	if (vn_isdisk(vp, NULL)) {
		if (!dev->si_bsize_phys)
			dev->si_bsize_phys = DEV_BSIZE;
		vinitvmio(vp, IDX_TO_OFF(INT_MAX));
	}
	if ((dev_dflags(dev) & D_DISK) == 0) {
		cp = devtoname(dev);
		if (*cp == '#') {
			kprintf("WARNING: driver %s should register devices with make_dev() (cdev_t = \"%s\")\n",
			    dev_dname(dev), cp);
		}
	}

	/*
	 * If we were handed a file pointer we may be able to install a
	 * shortcut which issues device read and write operations directly
	 * from the fileops rather then having to go through spec_read()
	 * and spec_write().
	 */
	if (ap->a_fp)
		vn_setspecops(ap->a_fp);

	if (dev_ref_debug)
		kprintf("spec_open: %s %d\n", dev->si_name, vp->v_opencount);
done:
	if (error) {
		if (vp->v_opencount == 0)
			v_release_rdev(vp);
	} else {
		vop_stdopen(ap);
	}
	return (error);
}

/*
 * Vnode op for read
 *
 * spec_read(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *	     struct ucred *a_cred)
 */
/* ARGSUSED */
static int
spec_read(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct thread *td;
	struct uio *uio;
	cdev_t dev;
	int error;

	vp = ap->a_vp;
	dev = vp->v_rdev;
	uio = ap->a_uio;
	td = uio->uio_td;

	if (dev == NULL)		/* device was revoked */
		return (EBADF);
	if (uio->uio_resid == 0)
		return (0);

	vn_unlock(vp);
	error = dev_dread(dev, uio, ap->a_ioflag);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	return (error);
}

/*
 * Vnode op for write
 *
 * spec_write(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *	      struct ucred *a_cred)
 */
/* ARGSUSED */
static int
spec_write(struct vop_write_args *ap)
{
	struct vnode *vp;
	struct thread *td;
	struct uio *uio;
	cdev_t dev;
	int error;

	vp = ap->a_vp;
	dev = vp->v_rdev;
	uio = ap->a_uio;
	td = uio->uio_td;

	if (dev == NULL)		/* device was revoked */
		return (EBADF);

	vn_unlock(vp);
	error = dev_dwrite(dev, uio, ap->a_ioflag);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	return (error);
}

/*
 * Device ioctl operation.
 *
 * spec_ioctl(struct vnode *a_vp, int a_command, caddr_t a_data,
 *	      int a_fflag, struct ucred *a_cred)
 */
/* ARGSUSED */
static int
spec_ioctl(struct vop_ioctl_args *ap)
{
	cdev_t dev;

	if ((dev = ap->a_vp->v_rdev) == NULL)
		return (EBADF);		/* device was revoked */

	return (dev_dioctl(dev, ap->a_command, ap->a_data,
		    ap->a_fflag, ap->a_cred));
}

/*
 * spec_poll(struct vnode *a_vp, int a_events, struct ucred *a_cred)
 */
/* ARGSUSED */
static int
spec_poll(struct vop_poll_args *ap)
{
	cdev_t dev;

	if ((dev = ap->a_vp->v_rdev) == NULL)
		return (EBADF);		/* device was revoked */
	return (dev_dpoll(dev, ap->a_events));
}

/*
 * spec_kqfilter(struct vnode *a_vp, struct knote *a_kn)
 */
/* ARGSUSED */
static int
spec_kqfilter(struct vop_kqfilter_args *ap)
{
	cdev_t dev;

	if ((dev = ap->a_vp->v_rdev) == NULL)
		return (EBADF);		/* device was revoked */
	return (dev_dkqfilter(dev, ap->a_kn));
}

/*
 * Synch buffers associated with a block device
 *
 * spec_fsync(struct vnode *a_vp, int a_waitfor)
 */
/* ARGSUSED */
static int
spec_fsync(struct vop_fsync_args *ap)
{
	struct vnode *vp = ap->a_vp;
	int error;

	if (!vn_isdisk(vp, NULL))
		return (0);

	/*
	 * Flush all dirty buffers associated with a block device.
	 */
	error = vfsync(vp, ap->a_waitfor, 10000, NULL, NULL);
	return (error);
}

/*
 * spec_inactive(struct vnode *a_vp)
 */
static int
spec_inactive(struct vop_inactive_args *ap)
{
	return (0);
}

/*
 * Convert a vnode strategy call into a device strategy call.  Vnode strategy
 * calls are not limited to device DMA limits so we have to deal with the
 * case.
 *
 * spec_strategy(struct vnode *a_vp, struct bio *a_bio)
 */
static int
spec_strategy(struct vop_strategy_args *ap)
{
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct buf *nbp;
	struct vnode *vp;
	struct mount *mp;
	int chunksize;
	int maxiosize;

	if (bp->b_cmd != BUF_CMD_READ &&
	    (LIST_FIRST(&bp->b_dep)) != NULL && bioops.io_start) {
		(*bioops.io_start)(bp);
	}

	/*
	 * Collect statistics on synchronous and asynchronous read
	 * and write counts for disks that have associated filesystems.
	 */
	vp = ap->a_vp;
	KKASSERT(vp->v_rdev != NULL);	/* XXX */
	if (vn_isdisk(vp, NULL) && (mp = vp->v_rdev->si_mountpoint) != NULL) {
		if (bp->b_cmd == BUF_CMD_READ) {
			if (bp->b_flags & B_ASYNC)
				mp->mnt_stat.f_asyncreads++;
			else
				mp->mnt_stat.f_syncreads++;
		} else {
			if (bp->b_flags & B_ASYNC)
				mp->mnt_stat.f_asyncwrites++;
			else
				mp->mnt_stat.f_syncwrites++;
		}
	}

        /*
         * Device iosize limitations only apply to read and write.  Shortcut
         * the I/O if it fits.
         */
	if ((maxiosize = vp->v_rdev->si_iosize_max) == 0) {
		kprintf("%s: si_iosize_max not set!\n", dev_dname(vp->v_rdev));
		maxiosize = MAXPHYS;
	}
#if SPEC_CHAIN_DEBUG & 2
	maxiosize = 4096;
#endif
        if (bp->b_bcount <= maxiosize ||
            (bp->b_cmd != BUF_CMD_READ && bp->b_cmd != BUF_CMD_WRITE)) {
                dev_dstrategy_chain(vp->v_rdev, bio);
                return (0);
        }

	/*
	 * Clone the buffer and set up an I/O chain to chunk up the I/O.
	 */
	nbp = kmalloc(sizeof(*bp), M_DEVBUF, M_INTWAIT|M_ZERO);
	initbufbio(nbp);
	LIST_INIT(&nbp->b_dep);
	BUF_LOCKINIT(nbp);
	BUF_LOCK(nbp, LK_EXCLUSIVE);
	BUF_KERNPROC(nbp);
	nbp->b_vp = vp;
	nbp->b_flags = B_PAGING | (bp->b_flags & B_BNOCLIP);
	nbp->b_data = bp->b_data;
	nbp->b_bio1.bio_done = spec_strategy_done;
	nbp->b_bio1.bio_offset = bio->bio_offset;
	nbp->b_bio1.bio_caller_info1.ptr = bio;

	/*
	 * Start the first transfer
	 */
	if (vn_isdisk(vp, NULL))
		chunksize = vp->v_rdev->si_bsize_phys;
	else
		chunksize = DEV_BSIZE;
	chunksize = maxiosize / chunksize * chunksize;
#if SPEC_CHAIN_DEBUG & 1
	kprintf("spec_strategy chained I/O chunksize=%d\n", chunksize);
#endif
	nbp->b_cmd = bp->b_cmd;
	nbp->b_bcount = chunksize;
	nbp->b_bufsize = chunksize;	/* used to detect a short I/O */
	nbp->b_bio1.bio_caller_info2.index = chunksize;

#if SPEC_CHAIN_DEBUG & 1
	kprintf("spec_strategy: chain %p offset %d/%d bcount %d\n",
		bp, 0, bp->b_bcount, nbp->b_bcount);
#endif

	dev_dstrategy(vp->v_rdev, &nbp->b_bio1);
	return (0);
}

/*
 * Chunked up transfer completion routine - chain transfers until done
 */
static
void
spec_strategy_done(struct bio *nbio)
{
	struct buf *nbp = nbio->bio_buf;
	struct bio *bio = nbio->bio_caller_info1.ptr;	/* original bio */
	struct buf *bp = bio->bio_buf;			/* original bp */
	int chunksize = nbio->bio_caller_info2.index;	/* chunking */
	int boffset = nbp->b_data - bp->b_data;

	if (nbp->b_flags & B_ERROR) {
		/*
		 * An error terminates the chain, propogate the error back
		 * to the original bp
		 */
		bp->b_flags |= B_ERROR;
		bp->b_error = nbp->b_error;
		bp->b_resid = bp->b_bcount - boffset +
			      (nbp->b_bcount - nbp->b_resid);
#if SPEC_CHAIN_DEBUG & 1
		kprintf("spec_strategy: chain %p error %d bcount %d/%d\n",
			bp, bp->b_error, bp->b_bcount,
			bp->b_bcount - bp->b_resid);
#endif
		kfree(nbp, M_DEVBUF);
		biodone(bio);
	} else if (nbp->b_resid) {
		/*
		 * A short read or write terminates the chain
		 */
		bp->b_error = nbp->b_error;
		bp->b_resid = bp->b_bcount - boffset +
			      (nbp->b_bcount - nbp->b_resid);
#if SPEC_CHAIN_DEBUG & 1
		kprintf("spec_strategy: chain %p short read(1) bcount %d/%d\n",
			bp, bp->b_bcount - bp->b_resid, bp->b_bcount);
#endif
		kfree(nbp, M_DEVBUF);
		biodone(bio);
	} else if (nbp->b_bcount != nbp->b_bufsize) {
		/*
		 * A short read or write can also occur by truncating b_bcount
		 */
#if SPEC_CHAIN_DEBUG & 1
		kprintf("spec_strategy: chain %p short read(2) bcount %d/%d\n",
			bp, nbp->b_bcount + boffset, bp->b_bcount);
#endif
		bp->b_error = 0;
		bp->b_bcount = nbp->b_bcount + boffset; 
		bp->b_resid = nbp->b_resid;
		kfree(nbp, M_DEVBUF);
		biodone(bio);
	} else if (nbp->b_bcount + boffset == bp->b_bcount) {
		/*
		 * No more data terminates the chain
		 */
#if SPEC_CHAIN_DEBUG & 1
		kprintf("spec_strategy: chain %p finished bcount %d\n",
			bp, bp->b_bcount);
#endif
		bp->b_error = 0;
		bp->b_resid = 0;
		kfree(nbp, M_DEVBUF);
		biodone(bio);
	} else {
		/*
		 * Continue the chain
		 */
		boffset += nbp->b_bcount;
		nbp->b_data = bp->b_data + boffset;
		nbp->b_bcount = bp->b_bcount - boffset;
		if (nbp->b_bcount > chunksize)
			nbp->b_bcount = chunksize;
		nbp->b_bio1.bio_done = spec_strategy_done;
		nbp->b_bio1.bio_offset = bio->bio_offset + boffset;

#if SPEC_CHAIN_DEBUG & 1
		kprintf("spec_strategy: chain %p offset %d/%d bcount %d\n",
			bp, boffset, bp->b_bcount, nbp->b_bcount);
#endif

		dev_dstrategy(nbp->b_vp->v_rdev, &nbp->b_bio1);
	}
}

/*
 * spec_freeblks(struct vnode *a_vp, daddr_t a_addr, daddr_t a_length)
 */
static int
spec_freeblks(struct vop_freeblks_args *ap)
{
	struct buf *bp;

	/*
	 * XXX: This assumes that strategy does the deed right away.
	 * XXX: this may not be TRTTD.
	 */
	KKASSERT(ap->a_vp->v_rdev != NULL);
	if ((dev_dflags(ap->a_vp->v_rdev) & D_CANFREE) == 0)
		return (0);
	bp = geteblk(ap->a_length);
	bp->b_cmd = BUF_CMD_FREEBLKS;
	bp->b_bio1.bio_offset = ap->a_offset;
	bp->b_bcount = ap->a_length;
	dev_dstrategy(ap->a_vp->v_rdev, &bp->b_bio1);
	return (0);
}

/*
 * Implement degenerate case where the block requested is the block
 * returned, and assume that the entire device is contiguous in regards
 * to the contiguous block range (runp and runb).
 *
 * spec_bmap(struct vnode *a_vp, off_t a_loffset,
 *	     off_t *a_doffsetp, int *a_runp, int *a_runb)
 */
static int
spec_bmap(struct vop_bmap_args *ap)
{
	if (ap->a_doffsetp != NULL)
		*ap->a_doffsetp = ap->a_loffset;
	if (ap->a_runp != NULL)
		*ap->a_runp = MAXBSIZE;
	if (ap->a_runb != NULL) {
		if (ap->a_loffset < MAXBSIZE)
			*ap->a_runb = (int)ap->a_loffset;
		else
			*ap->a_runb = MAXBSIZE;
	}
	return (0);
}

/*
 * Device close routine
 *
 * spec_close(struct vnode *a_vp, int a_fflag)
 *
 * NOTE: the vnode may or may not be locked on call.
 */
/* ARGSUSED */
static int
spec_close(struct vop_close_args *ap)
{
	struct proc *p = curproc;
	struct vnode *vp = ap->a_vp;
	cdev_t dev = vp->v_rdev;
	int error;
	int needrelock;

	/*
	 * Hack: a tty device that is a controlling terminal
	 * has a reference from the session structure.
	 * We cannot easily tell that a character device is
	 * a controlling terminal, unless it is the closing
	 * process' controlling terminal.  In that case,
	 * if the reference count is 2 (this last descriptor
	 * plus the session), release the reference from the session.
	 *
	 * It is possible for v_opencount to be 0 or 1 in this case, 0
	 * because the tty might have been revoked.
	 */
	if (dev)
		reference_dev(dev);
	if (vcount(vp) == 2 && vp->v_opencount <= 1 && 
	    p && vp == p->p_session->s_ttyvp) {
		p->p_session->s_ttyvp = NULL;
		vrele(vp);
	}

	/*
	 * Vnodes can be opened and close multiple times.  Do not really
	 * close the device unless (1) it is being closed forcibly,
	 * (2) the device wants to track closes, or (3) this is the last
	 * vnode doing its last close on the device.
	 *
	 * XXX the VXLOCK (force close) case can leave vnodes referencing
	 * a closed device.
	 */
	if (dev && ((vp->v_flag & VRECLAIMED) ||
	    (dev_dflags(dev) & D_TRACKCLOSE) ||
	    (vcount(vp) <= 1 && vp->v_opencount == 1))) {
		needrelock = 0;
		if (vn_islocked(vp)) {
			needrelock = 1;
			vn_unlock(vp);
		}
		error = dev_dclose(dev, ap->a_fflag, S_IFCHR);
		if (needrelock)
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	} else {
		error = 0;
	}

	/*
	 * Track the actual opens and closes on the vnode.  The last close
	 * disassociates the rdev.  If the rdev is already disassociated 
	 * the vnode might have been revoked and no further opencount
	 * tracking occurs.
	 */
	if (dev) {
		/*KKASSERT(vp->v_opencount > 0);*/
		if (dev_ref_debug) {
			kprintf("spec_close: %s %d\n",
				dev->si_name, vp->v_opencount - 1);
		}
		if (vp->v_opencount == 1)
			v_release_rdev(vp);
		release_dev(dev);
	}
	vop_stdclose(ap);
	return(error);
}

/*
 * Print out the contents of a special device vnode.
 *
 * spec_print(struct vnode *a_vp)
 */
static int
spec_print(struct vop_print_args *ap)
{
	kprintf("tag VT_NON, dev %s\n", devtoname(ap->a_vp->v_rdev));
	return (0);
}

/*
 * Special device advisory byte-level locks.
 *
 * spec_advlock(struct vnode *a_vp, caddr_t a_id, int a_op,
 *		struct flock *a_fl, int a_flags)
 */
/* ARGSUSED */
static int
spec_advlock(struct vop_advlock_args *ap)
{
	return ((ap->a_flags & F_POSIX) ? EINVAL : EOPNOTSUPP);
}

static void
spec_getpages_iodone(struct bio *bio)
{
	bio->bio_buf->b_cmd = BUF_CMD_DONE;
	wakeup(bio->bio_buf);
}

/*
 * spec_getpages() - get pages associated with device vnode.
 *
 * Note that spec_read and spec_write do not use the buffer cache, so we
 * must fully implement getpages here.
 */
static int
spec_getpages(struct vop_getpages_args *ap)
{
	vm_offset_t kva;
	int error;
	int i, pcount, size;
	struct buf *bp;
	vm_page_t m;
	vm_ooffset_t offset;
	int toff, nextoff, nread;
	struct vnode *vp = ap->a_vp;
	int blksiz;
	int gotreqpage;

	error = 0;
	pcount = round_page(ap->a_count) / PAGE_SIZE;

	/*
	 * Calculate the offset of the transfer and do sanity check.
	 */
	offset = IDX_TO_OFF(ap->a_m[0]->pindex) + ap->a_offset;

	/*
	 * Round up physical size for real devices.  We cannot round using
	 * v_mount's block size data because v_mount has nothing to do with
	 * the device.  i.e. it's usually '/dev'.  We need the physical block
	 * size for the device itself.
	 *
	 * We can't use v_rdev->si_mountpoint because it only exists when the
	 * block device is mounted.  However, we can use v_rdev.
	 */

	if (vn_isdisk(vp, NULL))
		blksiz = vp->v_rdev->si_bsize_phys;
	else
		blksiz = DEV_BSIZE;

	size = (ap->a_count + blksiz - 1) & ~(blksiz - 1);

	bp = getpbuf(NULL);
	kva = (vm_offset_t)bp->b_data;

	/*
	 * Map the pages to be read into the kva.
	 */
	pmap_qenter(kva, ap->a_m, pcount);

	/* Build a minimal buffer header. */
	bp->b_cmd = BUF_CMD_READ;
	bp->b_bcount = size;
	bp->b_resid = 0;
	bp->b_runningbufspace = size;
	runningbufspace += bp->b_runningbufspace;

	bp->b_bio1.bio_offset = offset;
	bp->b_bio1.bio_done = spec_getpages_iodone;

	mycpu->gd_cnt.v_vnodein++;
	mycpu->gd_cnt.v_vnodepgsin += pcount;

	/* Do the input. */
	vn_strategy(ap->a_vp, &bp->b_bio1);

	crit_enter();

	/* We definitely need to be at splbio here. */
	while (bp->b_cmd != BUF_CMD_DONE)
		tsleep(bp, 0, "spread", 0);

	crit_exit();

	if (bp->b_flags & B_ERROR) {
		if (bp->b_error)
			error = bp->b_error;
		else
			error = EIO;
	}

	/*
	 * If EOF is encountered we must zero-extend the result in order
	 * to ensure that the page does not contain garabge.  When no
	 * error occurs, an early EOF is indicated if b_bcount got truncated.
	 * b_resid is relative to b_bcount and should be 0, but some devices
	 * might indicate an EOF with b_resid instead of truncating b_bcount.
	 */
	nread = bp->b_bcount - bp->b_resid;
	if (nread < ap->a_count)
		bzero((caddr_t)kva + nread, ap->a_count - nread);
	pmap_qremove(kva, pcount);

	gotreqpage = 0;
	for (i = 0, toff = 0; i < pcount; i++, toff = nextoff) {
		nextoff = toff + PAGE_SIZE;
		m = ap->a_m[i];

		m->flags &= ~PG_ZERO;

		if (nextoff <= nread) {
			m->valid = VM_PAGE_BITS_ALL;
			vm_page_undirty(m);
		} else if (toff < nread) {
			/*
			 * Since this is a VM request, we have to supply the
			 * unaligned offset to allow vm_page_set_validclean()
			 * to zero sub-DEV_BSIZE'd portions of the page.
			 */
			vm_page_set_validclean(m, 0, nread - toff);
		} else {
			m->valid = 0;
			vm_page_undirty(m);
		}

		if (i != ap->a_reqpage) {
			/*
			 * Just in case someone was asking for this page we
			 * now tell them that it is ok to use.
			 */
			if (!error || (m->valid == VM_PAGE_BITS_ALL)) {
				if (m->valid) {
					if (m->flags & PG_WANTED) {
						vm_page_activate(m);
					} else {
						vm_page_deactivate(m);
					}
					vm_page_wakeup(m);
				} else {
					vm_page_free(m);
				}
			} else {
				vm_page_free(m);
			}
		} else if (m->valid) {
			gotreqpage = 1;
			/*
			 * Since this is a VM request, we need to make the
			 * entire page presentable by zeroing invalid sections.
			 */
			if (m->valid != VM_PAGE_BITS_ALL)
			    vm_page_zero_invalid(m, FALSE);
		}
	}
	if (!gotreqpage) {
		m = ap->a_m[ap->a_reqpage];
		kprintf(
	    "spec_getpages:(%s) I/O read failure: (error=%d) bp %p vp %p\n",
			devtoname(vp->v_rdev), error, bp, bp->b_vp);
		kprintf(
	    "               size: %d, resid: %d, a_count: %d, valid: 0x%x\n",
		    size, bp->b_resid, ap->a_count, m->valid);
		kprintf(
	    "               nread: %d, reqpage: %d, pindex: %lu, pcount: %d\n",
		    nread, ap->a_reqpage, (u_long)m->pindex, pcount);
		/*
		 * Free the buffer header back to the swap buffer pool.
		 */
		relpbuf(bp, NULL);
		return VM_PAGER_ERROR;
	}
	/*
	 * Free the buffer header back to the swap buffer pool.
	 */
	relpbuf(bp, NULL);
	return VM_PAGER_OK;
}
