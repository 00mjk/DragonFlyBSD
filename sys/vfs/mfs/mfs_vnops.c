/*
 * Copyright (c) 1989, 1993
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
 *	@(#)mfs_vnops.c	8.11 (Berkeley) 5/22/95
 * $FreeBSD: src/sys/ufs/mfs/mfs_vnops.c,v 1.47.2.1 2001/05/22 02:06:43 bp Exp $
 * $DragonFly: src/sys/vfs/mfs/mfs_vnops.c,v 1.22 2006/03/29 18:44:57 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/sysproto.h>
#include <sys/mman.h>
#include <sys/conf.h>

#include <sys/buf2.h>

#include <sys/thread2.h>

#include "mfsnode.h"
#include "mfs_extern.h"

static int	mfs_badop (struct vop_generic_args *);
static int	mfs_bmap (struct vop_bmap_args *);
static int	mfs_close (struct vop_close_args *);
static int	mfs_fsync (struct vop_fsync_args *);
static int	mfs_freeblks (struct vop_freeblks_args *);
static int	mfs_inactive (struct vop_inactive_args *); /* XXX */
static int	mfs_open (struct vop_open_args *);
static int	mfs_reclaim (struct vop_reclaim_args *); /* XXX */
static int	mfs_print (struct vop_print_args *); /* XXX */
static int	mfs_strategy (struct vop_strategy_args *); /* XXX */
static int	mfs_getpages (struct vop_getpages_args *); /* XXX */
/*
 * mfs vnode operations.  Note: the vops here are used for the MFS block
 * device, not for operations on files (MFS calls the ffs mount code for that)
 */
struct vop_ops *mfs_vnode_vops;
static struct vnodeopv_entry_desc mfs_vnodeop_entries[] = {
	{ &vop_default_desc,		(vnodeopv_entry_t) mfs_badop },
	{ &vop_bmap_desc,		(vnodeopv_entry_t) mfs_bmap },
	{ &vop_bwrite_desc,		vop_defaultop },
	{ &vop_close_desc,		(vnodeopv_entry_t) mfs_close },
	{ &vop_freeblks_desc,		(vnodeopv_entry_t) mfs_freeblks },
	{ &vop_fsync_desc,		(vnodeopv_entry_t) mfs_fsync },
	{ &vop_getpages_desc,		(vnodeopv_entry_t) mfs_getpages },
	{ &vop_inactive_desc,		(vnodeopv_entry_t) mfs_inactive },
	{ &vop_ioctl_desc,		vop_enotty },
	{ &vop_islocked_desc,		vop_defaultop },
	{ &vop_lock_desc,		vop_defaultop },
	{ &vop_open_desc,		(vnodeopv_entry_t) mfs_open },
	{ &vop_print_desc,		(vnodeopv_entry_t) mfs_print },
	{ &vop_reclaim_desc,		(vnodeopv_entry_t) mfs_reclaim },
	{ &vop_strategy_desc,		(vnodeopv_entry_t) mfs_strategy },
	{ &vop_unlock_desc,		vop_defaultop },
	{ NULL, NULL }
};
static struct vnodeopv_desc mfs_vnodeop_opv_desc =
	{ &mfs_vnode_vops, mfs_vnodeop_entries, 0 };

VNODEOP_SET(mfs_vnodeop_opv_desc);

/*
 * Vnode Operations.
 *
 * Open called to allow memory filesystem to initialize and
 * validate before actual IO. Record our process identifier
 * so we can tell when we are doing I/O to ourself.
 *
 * NOTE: new device sequencing.  mounts check the device reference count
 * before calling open, so we must associate the device in open and 
 * disassociate it in close rather then faking it when we created the vnode.
 *
 * mfs_open(struct vnode *a_vp, int a_mode, struct ucred *a_cred,
 *	    struct thread *a_td)
 */
/* ARGSUSED */
static int
mfs_open(struct vop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;

	if (vp->v_type != VCHR)
		panic("mfs_open not VCHR");
	v_associate_rdev(vp, udev2dev(vp->v_udev, 0));
	return (0);
}

static int
mfs_fsync(struct vop_fsync_args *ap)
{
	return (VOCALL(spec_vnode_vops, &ap->a_head));
}

/*
 * mfs_freeblks() - hook to allow us to free physical memory.
 *
 *	We implement the B_FREEBUF strategy.  We can't just madvise()
 *	here because we have to do it in the correct order vs other bio
 *	requests, so we queue it.
 *
 *	Note: geteblk() sets B_INVAL.  We leave it set to guarentee buffer
 *	throw-away on brelse()? XXX
 *
 * mfs_freeblks(struct vnode *a_vp, daddr_t a_addr, daddr_t a_length)
 */
static int
mfs_freeblks(struct vop_freeblks_args *ap)
{       
	struct buf *bp;
	struct vnode *vp = ap->a_vp;

	bp = geteblk(ap->a_length);
	bp->b_flags |= B_FREEBUF | B_ASYNC;
	bp->b_bio1.bio_offset = ap->a_offset;
	bp->b_bcount = ap->a_length;
	BUF_KERNPROC(bp);
	vn_strategy(vp, &bp->b_bio1);
	return(0);
}

/*
 * Pass I/O requests to the memory filesystem process.
 *
 * mfs_strategy(struct vnode *a_vp, struct bio *a_bio)
 */
static int
mfs_strategy(struct vop_strategy_args *ap)
{
	struct bio *bio = ap->a_bio;
	struct buf *bp = bio->bio_buf;
	struct mfsnode *mfsp;
	struct thread *td = curthread;		/* XXX */

	mfsp = ap->a_vp->v_rdev->si_drv1;
	if (mfsp == NULL) {
		bp->b_error = ENXIO;
		bp->b_flags |= B_ERROR;
		biodone(bio);
		return(0);
	}

	/*
	 * splbio required for queueing/dequeueing, in case of forwarded
	 * BPs from bio interrupts (?).  It may not be necessary.
	 */

	crit_enter();

	if (mfsp->mfs_td == NULL) {
		/*
		 * mini-root.  Note: B_FREEBUF not supported at the moment,
		 * I'm not sure what kind of dataspace b_data is in.
		 */
		caddr_t base;

		base = mfsp->mfs_baseoff + bio->bio_offset;
		if (bp->b_flags & B_FREEBUF)
			;
		else if (bp->b_flags & B_READ)
			bcopy(base, bp->b_data, bp->b_bcount);
		else
			bcopy(bp->b_data, base, bp->b_bcount);
		biodone(bio);
	} else if (mfsp->mfs_td == td) {
		/*
		 * VOP to self
		 */
		crit_exit();
		mfs_doio(bio, mfsp);
		crit_enter();
	} else {
		/*
		 * VOP from some other process, queue to MFS process and
		 * wake it up.
		 */
		bioq_insert_tail(&mfsp->bio_queue, bio);
		wakeup((caddr_t)mfsp);
	}
	crit_exit();
	return (0);
}

/*
 * Memory file system I/O.
 *
 * Trivial on the HP since buffer has already been mapping into KVA space.
 *
 * Read and Write are handled with a simple copyin and copyout.    
 *
 * We also partially support VOP_FREEBLKS() via B_FREEBUF.  We can't implement
 * completely -- for example, on fragments or inode metadata, but we can
 * implement it for page-aligned requests.
 */
void
mfs_doio(struct bio *bio, struct mfsnode *mfsp)
{
	struct buf *bp = bio->bio_buf;
	caddr_t base = mfsp->mfs_baseoff + bio->bio_offset;

	if (bp->b_flags & B_FREEBUF) {
		/*
		 * Implement B_FREEBUF, which allows the filesystem to tell
		 * a block device when blocks are no longer needed (like when
		 * a file is deleted).  We use the hook to MADV_FREE the VM.
		 * This makes an MFS filesystem work as well or better then
		 * a sun-style swap-mounted filesystem.
		 */
		int bytes = bp->b_bcount;

		if ((vm_offset_t)base & PAGE_MASK) {
			int n = PAGE_SIZE - ((vm_offset_t)base & PAGE_MASK);
			bytes -= n;
			base += n;
		}
                if (bytes > 0) {
                        struct madvise_args uap;

			bytes &= ~PAGE_MASK;
			if (bytes != 0) {
				bzero(&uap, sizeof(uap));
				uap.addr  = base;
				uap.len   = bytes;
				uap.behav = MADV_FREE;
				madvise(&uap);
			}
                }
		bp->b_error = 0;
	} else if (bp->b_flags & B_READ) {
		/*
		 * Read data from our 'memory' disk
		 */
		bp->b_error = copyin(base, bp->b_data, bp->b_bcount);
	} else {
		/*
		 * Write data to our 'memory' disk
		 */
		bp->b_error = copyout(bp->b_data, base, bp->b_bcount);
	}
	if (bp->b_error)
		bp->b_flags |= B_ERROR;
	biodone(bio);
}

/*
 * This is a noop, simply returning what one has been given.
 *
 * mfs_bmap(struct vnode *a_vp, off_t a_loffset, struct vnode **a_vpp,
 *	    off_t *a_doffsetp, int *a_runp, int *a_runb)
 */
static int
mfs_bmap(struct vop_bmap_args *ap)
{
	if (ap->a_vpp != NULL)
		*ap->a_vpp = ap->a_vp;
	if (ap->a_doffsetp != NULL)
		*ap->a_doffsetp = ap->a_loffset;
	if (ap->a_runp != NULL)
		*ap->a_runp = 0;
	if (ap->a_runb != NULL)
		*ap->a_runb = 0;
	return (0);
}

/*
 * Memory filesystem close routine
 *
 * mfs_close(struct vnode *a_vp, int a_fflag, struct ucred *a_cred,
 *	     struct thread *a_td)
 */
/* ARGSUSED */
static int
mfs_close(struct vop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct mfsnode *mfsp = VTOMFS(vp);
	struct bio *bio;
	int error;

	/*
	 * Finish any pending I/O requests.
	 */
	while ((bio = bioq_first(&mfsp->bio_queue)) != NULL) {
		bioq_remove(&mfsp->bio_queue, bio);
		mfs_doio(bio, mfsp);
		wakeup((caddr_t)bio->bio_buf);
	}
	/*
	 * On last close of a memory filesystem
	 * we must invalidate any in core blocks, so that
	 * we can, free up its vnode.
	 */
	if ((error = vinvalbuf(vp, 1, ap->a_td, 0, 0)) != 0)
		return (error);
	/*
	 * There should be no way to have any more uses of this
	 * vnode, so if we find any other uses, it is a panic.
	 */
	if (vp->v_usecount > 1)
		printf("mfs_close: ref count %d > 1\n", vp->v_usecount);
	if (vp->v_usecount > 1 || (bioq_first(&mfsp->bio_queue) != NULL))
		panic("mfs_close");
	/*
	 * Send a request to the filesystem server to exit.
	 */
	mfsp->mfs_active = 0;
	v_release_rdev(vp);
	if (mfsp->mfs_dev) {
		destroy_dev(mfsp->mfs_dev);
		mfsp->mfs_dev = NULL;
	}
	wakeup((caddr_t)mfsp);
	return (0);
}

/*
 * Memory filesystem inactive routine
 *
 * mfs_inactive(struct vnode *a_vp, struct thread *a_td)
 */
/* ARGSUSED */
static int
mfs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct mfsnode *mfsp = VTOMFS(vp);

	if (bioq_first(&mfsp->bio_queue) != NULL)
		panic("mfs_inactive: not inactive (next buffer %p)",
			bioq_first(&mfsp->bio_queue));
	return (0);
}

/*
 * Reclaim a memory filesystem devvp so that it can be reused.
 *
 * mfs_reclaim(struct vnode *a_vp)
 */
static int
mfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;

	FREE(vp->v_data, M_MFSNODE);
	vp->v_data = NULL;
	return (0);
}

/*
 * Print out the contents of an mfsnode.
 *
 * mfs_print(struct vnode *a_vp)
 */
static int
mfs_print(struct vop_print_args *ap)
{
	struct mfsnode *mfsp = VTOMFS(ap->a_vp);

	printf("tag VT_MFS, td %p, base %p, size %ld\n",
	    mfsp->mfs_td, (void *)mfsp->mfs_baseoff, mfsp->mfs_size);
	return (0);
}

/*
 * Block device bad operation
 */
static int
mfs_badop(struct vop_generic_args *ap)
{
	int i;

	printf("mfs_badop[%s]\n", ap->a_desc->vdesc_name);
	i = vop_defaultop(ap);
	printf("mfs_badop[%s] = %d\n", ap->a_desc->vdesc_name, i);
	return (i);
}

static int
mfs_getpages(struct vop_getpages_args *ap)
{
	return (VOCALL(spec_vnode_vops, &ap->a_head));
}
