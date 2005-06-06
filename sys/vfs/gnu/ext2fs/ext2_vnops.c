/*
 *  modified for EXT2FS support in Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *	@(#)ufs_vnops.c 8.27 (Berkeley) 5/27/95
 *	@(#)ext2_vnops.c	8.7 (Berkeley) 2/3/94
 * $FreeBSD: src/sys/gnu/ext2fs/ext2_vnops.c,v 1.51.2.2 2003/01/02 17:26:18 bde Exp $
 * $DragonFly: src/sys/vfs/gnu/ext2fs/ext2_vnops.c,v 1.22 2005/06/06 15:35:06 dillon Exp $
 */

#include "opt_quota.h"
#include "opt_suiddir.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/resourcevar.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/namei.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_zone.h>
#include <vm/vnode_pager.h>
#include <sys/buf2.h>
#include <sys/thread2.h>

#include <sys/signalvar.h>
#include <vfs/ufs/dir.h>
#include <vfs/ufs/quota.h>
#include <vfs/ufs/inode.h>
#include <vfs/ufs/ufsmount.h>
#include <vfs/ufs/ufs_extern.h>

#include "ext2_fs_sb.h"
#include "fs.h"
#include "ext2_extern.h"
#include "ext2_fs.h"

static int ext2_makeinode (int mode, struct vnode *, struct vnode **, struct componentname *);

static int ext2_fsync (struct vop_fsync_args *);
static int ext2_read (struct vop_read_args *);
static int ext2_write (struct vop_write_args *);
static int ext2_remove (struct vop_remove_args *);
static int ext2_link (struct vop_link_args *);
static int ext2_rename (struct vop_rename_args *);
static int ext2_mkdir (struct vop_mkdir_args *);
static int ext2_rmdir (struct vop_rmdir_args *);
static int ext2_create (struct vop_create_args *);
static int ext2_mknod (struct vop_mknod_args *);
static int ext2_symlink (struct vop_symlink_args *);
static int ext2_getpages (struct vop_getpages_args *);
static int ext2_putpages (struct vop_putpages_args *);

/* Global vfs data structures for ufs. */
struct vnodeopv_entry_desc ext2_vnodeop_entries[] = {
	{ &vop_default_desc,		(vnodeopv_entry_t) ufs_vnoperate },
	{ &vop_fsync_desc,		(vnodeopv_entry_t) ext2_fsync },
	{ &vop_inactive_desc,		(vnodeopv_entry_t) ext2_inactive },
	{ &vop_lookup_desc,		(vnodeopv_entry_t) ext2_lookup },
	{ &vop_read_desc,		(vnodeopv_entry_t) ext2_read },
	{ &vop_readdir_desc,		(vnodeopv_entry_t) ext2_readdir },
	{ &vop_reallocblks_desc,	(vnodeopv_entry_t) ext2_reallocblks },
	{ &vop_write_desc,		(vnodeopv_entry_t) ext2_write },
	{ &vop_remove_desc,		(vnodeopv_entry_t) ext2_remove },
	{ &vop_link_desc,		(vnodeopv_entry_t) ext2_link },
	{ &vop_rename_desc,		(vnodeopv_entry_t) ext2_rename },
	{ &vop_mkdir_desc,		(vnodeopv_entry_t) ext2_mkdir },
	{ &vop_rmdir_desc,		(vnodeopv_entry_t) ext2_rmdir },
	{ &vop_create_desc,		(vnodeopv_entry_t) ext2_create },
	{ &vop_mknod_desc,		(vnodeopv_entry_t) ext2_mknod },
	{ &vop_symlink_desc,		(vnodeopv_entry_t) ext2_symlink },
	{ &vop_getpages_desc,		(vnodeopv_entry_t) ext2_getpages },
	{ &vop_putpages_desc,		(vnodeopv_entry_t) ext2_putpages },
	{ NULL, NULL }
};

struct vnodeopv_entry_desc ext2_specop_entries[] = {
	{ &vop_default_desc,		(vnodeopv_entry_t) ufs_vnoperatespec },
	{ &vop_fsync_desc,		(vnodeopv_entry_t) ext2_fsync },
	{ &vop_inactive_desc,		(vnodeopv_entry_t) ext2_inactive },
	{ NULL, NULL }
};

struct vnodeopv_entry_desc ext2_fifoop_entries[] = {
	{ &vop_default_desc,		(vnodeopv_entry_t) ufs_vnoperatefifo },
	{ &vop_fsync_desc,		(vnodeopv_entry_t) ext2_fsync },
	{ &vop_inactive_desc,		(vnodeopv_entry_t) ext2_inactive },
	{ NULL, NULL }
};

#include "ext2_readwrite.c"

/*
 * A virgin directory (no blushing please).
 * Note that the type and namlen fields are reversed relative to ufs.
 * Also, we don't use `struct odirtemplate', since it would just cause
 * endianness problems.
 */
static struct dirtemplate mastertemplate = {
	0, 12, 1, EXT2_FT_DIR, ".",
	0, DIRBLKSIZ - 12, 2, EXT2_FT_DIR, ".."
};
static struct dirtemplate omastertemplate = {
	0, 12, 1, EXT2_FT_UNKNOWN, ".",
	0, DIRBLKSIZ - 12, 2, EXT2_FT_UNKNOWN, ".."
};

/*
 * Create a regular file
 *
 * ext2_create(struct vnode *a_dvp, struct vnode **a_vpp,
 *	       struct componentname *a_cnp, struct vattr *a_vap)
 */
static int
ext2_create(struct vop_create_args *ap)
{
	int error;

	error =
	    ext2_makeinode(MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, ap->a_vpp, ap->a_cnp);
	if (error)
		return (error);
	return (0);
}

/*
 * Synch an open file.
 *
 * ext2_fsync(struct vnode *a_vp, struct ucred *a_cred, int a_waitfor,
 *	      struct proc *a_p)
 */
/* ARGSUSED */

static int ext2_fsync_bp(struct buf *bp, void *data);

struct ext2_fsync_bp_info {
	struct vnode *vp;
	int waitfor;
};

static int
ext2_fsync(struct vop_fsync_args *ap)
{
	struct ext2_fsync_bp_info info;
	struct vnode *vp = ap->a_vp;
	int count;

	/* 
	 * XXX why is all this fs specific?
	 */

	/*
	 * Flush all dirty buffers associated with a vnode.
	 */
	ext2_discard_prealloc(VTOI(vp));

	crit_enter();
	info.vp = vp;
loop:
	info.waitfor = ap->a_waitfor;
	count = RB_SCAN(buf_rb_tree, &vp->v_rbdirty_tree, NULL, 
			ext2_fsync_bp, &info);
	if (count)
		goto loop;

	if (ap->a_waitfor == MNT_WAIT) {
		while (vp->v_numoutput) {
			vp->v_flag |= VBWAIT;
			tsleep(&vp->v_numoutput, 0, "e2fsyn", 0);
		}
#if DIAGNOSTIC
		if (!RB_EMPTY(&vp->v_rbdirty_tree)) {
			vprint("ext2_fsync: dirty", vp);
			goto loop;
		}
#endif
	}
	crit_exit();
	return (UFS_UPDATE(ap->a_vp, ap->a_waitfor == MNT_WAIT));
}

static int
ext2_fsync_bp(struct buf *bp, void *data)
{
	struct ext2_fsync_bp_info *info = data;

	if (BUF_LOCK(bp, LK_EXCLUSIVE | LK_NOWAIT))
		return(0);
	if ((bp->b_flags & B_DELWRI) == 0)
		panic("ext2_fsync: not dirty");
	bremfree(bp);
	crit_exit();

	/*
	 * Wait for I/O associated with indirect blocks to complete,
	 * since there is no way to quickly wait for them below.
	 */
	if (bp->b_vp == info->vp || info->waitfor == MNT_NOWAIT)
		(void) bawrite(bp);
	else
		(void) bwrite(bp);
	crit_enter();
	return(1);
}

/*
 * Mknod vnode call
 *
 * ext2_mknod(struct vnode *a_dvp, struct vnode **a_vpp,
 *	      struct componentname *a_cnp, struct vattr *a_vap)
 */
/* ARGSUSED */
static int
ext2_mknod(struct vop_mknod_args *ap)
{
	struct vattr *vap = ap->a_vap;
	struct vnode **vpp = ap->a_vpp;
	struct inode *ip;
	ino_t ino;
	int error;

	error = ext2_makeinode(MAKEIMODE(vap->va_type, vap->va_mode),
	    ap->a_dvp, vpp, ap->a_cnp);
	if (error)
		return (error);
	ip = VTOI(*vpp);
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	if (vap->va_rdev != VNOVAL) {
		/*
		 * Want to be able to use this to make badblock
		 * inodes, so don't truncate the dev number.
		 */
		ip->i_rdev = vap->va_rdev;
	}
	/*
	 * Remove inode, then reload it through VFS_VGET so it is
	 * checked to see if it is an alias of an existing entry in
	 * the inode cache.
	 */
	(*vpp)->v_type = VNON;
	ino = ip->i_number;	/* Save this before vgone() invalidates ip. */
	vgone(*vpp);
	vput(*vpp);
	error = VFS_VGET(ap->a_dvp->v_mount, ino, vpp);
	if (error) {
		*vpp = NULL;
		return (error);
	}
	return (0);
}

/*
 * ext2_remove(struct vnode *a_dvp, struct vnode *a_vp,
 *	       struct componentname *a_cnp)
 */
static int
ext2_remove(struct vop_remove_args *ap)
{
	struct inode *ip;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	int error;

	ip = VTOI(vp);
	if ((ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(dvp)->i_flags & APPEND)) {
		error = EPERM;
		goto out;
	}
	error = ext2_dirremove(dvp, ap->a_cnp);
	if (error == 0) {
		ip->i_nlink--;
		ip->i_flag |= IN_CHANGE;
	}
out:
	return (error);
}

/*
 * link vnode call
 *
 * ext2_link(struct vnode *a_tdvp, struct vnode *a_vp,
 *	     struct componentname *a_cnp)
 */
static int
ext2_link(struct vop_link_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *tdvp = ap->a_tdvp;
	struct componentname *cnp = ap->a_cnp;
	struct thread *td = cnp->cn_td;
	struct inode *ip;
	int error;

	if (tdvp->v_mount != vp->v_mount) {
		error = EXDEV;
		goto out2;
	}
	if (tdvp != vp && (error = vn_lock(vp, LK_EXCLUSIVE, td))) {
		goto out2;
	}
	ip = VTOI(vp);
	if ((nlink_t)ip->i_nlink >= LINK_MAX) {
		error = EMLINK;
		goto out1;
	}
	if (ip->i_flags & (IMMUTABLE | APPEND)) {
		error = EPERM;
		goto out1;
	}
	ip->i_nlink++;
	ip->i_flag |= IN_CHANGE;
	error = UFS_UPDATE(vp, 1);
	if (!error)
		error = ext2_direnter(ip, tdvp, cnp);
	if (error) {
		ip->i_nlink--;
		ip->i_flag |= IN_CHANGE;
	}
out1:
	if (tdvp != vp)
		VOP_UNLOCK(vp, 0, td);
out2:
	return (error);
}

/*
 * Rename system call.  fdvp, fvp are ref'd.  tvp, tdvp are ref'd and locked.
 * all vp's are released and must be in an unlocked state on return.
 *
 * ext2_rename(struct vnode *a_fdvp, struct vnode *a_fvp,
 *		struct componentname *a_fcnp, struct vnode *a_tdvp,
 *		struct vnode *a_tvp, struct componentname *a_tcnp)
 */
static int
ext2_rename(struct vop_rename_args *ap)
{
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct thread *td = fcnp->cn_td;
	struct inode *ip, *xp, *dp;
	struct dirtemplate dirbuf;
	int doingdirectory = 0, oldparent = 0, newparent = 0;
	int error = 0;
	u_char namlen;

	/*
	 * Check for cross-device rename.
	 */
	if ((fvp->v_mount != tdvp->v_mount) ||
	    (tvp && (fvp->v_mount != tvp->v_mount)) ||
	    tvp == tdvp) {
		error = EXDEV;
abortit:
		if (tdvp == tvp)
			vrele(tdvp);
		else
			vput(tdvp);
		if (tvp)
			vput(tvp);
		vrele(fdvp);
		vrele(fvp);
		return (error);
	}

	if (tvp && ((VTOI(tvp)->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(tdvp)->i_flags & APPEND))) {
		error = EPERM;
		goto abortit;
	}

	/*
	 * Renaming a file to itself has no effect.  The upper layers should
	 * not call us in that case.  Temporarily just warn if they do.
	 */
	if (fvp == tvp) {
		error = 0;
		goto abortit;
	}

	if ((error = vn_lock(fvp, LK_EXCLUSIVE, td)) != 0)
		goto abortit;

	/*
	 * fvp, tvp, tdvp locked.  fdvp not locked but note that fdvp may
	 * be equal to tdvp.
	 */
	dp = VTOI(fdvp);
	ip = VTOI(fvp);
 	if (ip->i_nlink >= LINK_MAX) {
 		VOP_UNLOCK(fvp, 0, td);
 		error = EMLINK;
 		goto abortit;
 	}
	if ((ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))
	    || (dp->i_flags & APPEND)) {
		VOP_UNLOCK(fvp, 0, td);
		error = EPERM;
		goto abortit;
	}
	if ((ip->i_mode & IFMT) == IFDIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
		    dp == ip || (fcnp->cn_flags | tcnp->cn_flags) & CNP_ISDOTDOT ||
		    (ip->i_flag & IN_RENAME)) {
			VOP_UNLOCK(fvp, 0, td);
			error = EINVAL;
			goto abortit;
		}
		ip->i_flag |= IN_RENAME;
		oldparent = dp->i_number;
		doingdirectory++;
	}

	/*
	 * tvp is non-NULL if the target exists.   fvp is still locked but
	 * we will unlock it soon.  The 'bad' goto target requires dp and
	 * xp to be correctly assigned.
	 */
	dp = VTOI(tdvp);
	if (tvp)
		xp = VTOI(tvp);
	else
		xp = NULL;

	/*
	 * 1) Bump link count while we're moving stuff
	 *    around.  If we crash somewhere before
	 *    completing our work, the link count
	 *    may be wrong, but correctable.
	 */
	ip->i_nlink++;
	ip->i_flag |= IN_CHANGE;
	if ((error = UFS_UPDATE(fvp, 1)) != 0) {
		VOP_UNLOCK(fvp, 0, td);
		goto bad;
	}

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory heirarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..". We must repeat the call
	 * to namei, as the parent directory is unlocked by the
	 * call to checkpath().
	 */
	error = VOP_ACCESS(fvp, VWRITE, tcnp->cn_cred, tcnp->cn_td);
	VOP_UNLOCK(fvp, 0, td);

	/*
	 * tvp (if not NULL) and tdvp are locked.  fvp and fdvp are not.
	 * dp and xp are set according to tdvp and tvp.
	 */
	if (oldparent != dp->i_number)
		newparent = dp->i_number;
	if (doingdirectory && newparent) {
		if (error)	/* write access check above */
			goto bad;

		/*
		 * Prepare for relookup, get rid of xp
		 */
		if (xp != NULL) {
			vput(tvp);
			xp = NULL;
		}

		/*
		 * checkpath vput()'s tdvp (VTOI(dp)) on return no matter what,
		 * get an extra ref so we wind up with just an unlocked, ref'd
		 * tdvp.  The 'out' target skips xp and tdvp cleanups.  Our
		 * tdvp is now unlocked so we have to clean it up ourselves.
		 */
		vref(tdvp);
		error = ext2_checkpath(ip, dp, tcnp->cn_cred);
		tcnp->cn_flags |= CNP_PDIRUNLOCK;
		if (error) {
			vrele(tdvp);
			goto out;
		}
		/*
		 * relookup no longer messes with the ref count.  An unlocked
		 * tdvp must be passed and if no error occurs a locked tdvp
		 * will be returned.  We have to use the out target again.
		 */
		error = relookup(tdvp, &tvp, tcnp);
		if (error) {
			if (tcnp->cn_flags & CNP_PDIRUNLOCK)
				vrele(tdvp);
			else
				vput(tdvp);
			goto out;
		}

		/*
		 * tdvp is locked at this point.  in the RENAME case tvp may
		 * be NULL without an error, assign xp accordingly.  The
		 * 'bad' target can be used again after this.
		 */
		dp = VTOI(tdvp);
		if (tvp)
			xp = VTOI(tvp);
	}
	/*
	 * 2) If target doesn't exist, link the target
	 *    to the source and unlink the source.
	 *    Otherwise, rewrite the target directory
	 *    entry to reference the source inode and
	 *    expunge the original entry's existence.
	 *
	 * tdvp and tvp are cleaned up by this code.  tvp is only good if
	 * xp is not NULL.
	 */
	if (xp == NULL) {
		if (dp->i_dev != ip->i_dev)
			panic("ufs_rename: EXDEV");
		/*
		 * Account for ".." in new directory.
		 * When source and destination have the same
		 * parent we don't fool with the link count.
		 */
		if (doingdirectory && newparent) {
			if ((nlink_t)dp->i_nlink >= LINK_MAX) {
				error = EMLINK;
				goto bad;
			}
			dp->i_nlink++;
			dp->i_flag |= IN_CHANGE;
			error = UFS_UPDATE(tdvp, 1);
			if (error)
				goto bad;
		}
		error = ext2_direnter(ip, tdvp, tcnp);
		if (error) {
			if (doingdirectory && newparent) {
				dp->i_nlink--;
				dp->i_flag |= IN_CHANGE;
				(void)UFS_UPDATE(tdvp, 1);
			}
			goto bad;
		}

		/*
		 * manual cleanup, we can't use the bad or out target after
		 * this.
		 */
		vput(tdvp);
	} else {
		if (xp->i_dev != dp->i_dev || xp->i_dev != ip->i_dev)
			panic("ufs_rename: EXDEV");
		/*
		 * Short circuit rename(foo, foo).
		 */
		if (xp->i_number == ip->i_number)
			panic("ufs_rename: same file");
		/*
		 * If the parent directory is "sticky", then the user must
		 * own the parent directory, or the destination of the rename,
		 * otherwise the destination may not be changed (except by
		 * root). This implements append-only directories.
		 */
		if ((dp->i_mode & S_ISTXT) && tcnp->cn_cred->cr_uid != 0 &&
		    tcnp->cn_cred->cr_uid != dp->i_uid &&
		    xp->i_uid != tcnp->cn_cred->cr_uid) {
			error = EPERM;
			goto bad;
		}
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if ((xp->i_mode&IFMT) == IFDIR) {
			if (! ext2_dirempty(xp, dp->i_number, tcnp->cn_cred) || 
			    xp->i_nlink > 2) {
				error = ENOTEMPTY;
				goto bad;
			}
			if (!doingdirectory) {
				error = ENOTDIR;
				goto bad;
			}
		} else if (doingdirectory) {
			error = EISDIR;
			goto bad;
		}
		error = ext2_dirrewrite(dp, ip, tcnp);
		if (error)
			goto bad;
		/*
		 * If the target directory is in the same
		 * directory as the source directory,
		 * decrement the link count on the parent
		 * of the target directory.
		 */
		 if (doingdirectory && !newparent) {
			dp->i_nlink--;
			dp->i_flag |= IN_CHANGE;
		}

		/*
		 * manual cleanup, we can't use the bad or out target after
		 * this.
		 */
		vput(tdvp);

		/*
		 * Adjust the link count of the target to
		 * reflect the dirrewrite above.  If this is
		 * a directory it is empty and there are
		 * no links to it, so we can squash the inode and
		 * any space associated with it.  We disallowed
		 * renaming over top of a directory with links to
		 * it above, as the remaining link would point to
		 * a directory without "." or ".." entries.
		 */
		xp->i_nlink--;
		if (doingdirectory) {
			if (--xp->i_nlink != 0)
				panic("ufs_rename: linked directory");
			error = UFS_TRUNCATE(tvp, (off_t)0, IO_SYNC,
			    tcnp->cn_cred, tcnp->cn_td);
		}
		xp->i_flag |= IN_CHANGE;
		vput(tvp);
		xp = NULL;
	}

	/*
	 * tvp and tdvp have been cleaned up.  The bad and out targets may
	 * not be used.  fvp and fdvp are ref'd but not locked.  ip
	 * still represents the old fvp and ip->i_flag may still have IN_RENAME
	 * set (if doingdirectory).
	 */

	/*
	 * 3) Unlink the source.
	 *
	 * fdvp is locked and ref'd. ap->a_fvp holds the old lookup unlocked
	 * and ref'd, fvp will hold the new lookup locked and ref'd.
	 *
	 * After the relookup ap->a_fvp must be released as part of our
	 * cleanup, not just fdvp and fvp.  And, on success, fdvp and
	 * fvp will be locked so the bad and out targets cannot be used.
	 */
	fcnp->cn_flags &= ~CNP_MODMASK;
	fcnp->cn_flags |= CNP_LOCKPARENT;
	KKASSERT(fcnp->cn_flags & CNP_PDIRUNLOCK);
	error = relookup(fdvp, &fvp, fcnp);
	if (error) {
		/*
		 * From name has disappeared.
		 */
		if (doingdirectory)
			panic("ufs_rename: lost dir entry");
		/* ip->i_flag only sets IN_RENAME if doingdirectory */
		vrele(ap->a_fvp);
		if (fcnp->cn_flags & CNP_PDIRUNLOCK)
			vrele(fdvp);
		else
			vput(fdvp);
		return (0);
	}
	KKASSERT((fcnp->cn_flags & CNP_PDIRUNLOCK) == 0);

	/*
	 * This case shouldn't occur
	 */
	if (fvp == NULL) {
		/*
		 * From name has disappeared.
		 */
		if (doingdirectory)
			panic("ufs_rename: lost dir entry");
		/* ip->i_flag only sets IN_RENAME if doingdirectory */
		vrele(ap->a_fvp);
		vput(fvp);
		vput(fdvp);
		return (0);
	}

	/*
	 * fvp and fdvp are both ref'd and locked.
	 */
	xp = VTOI(fvp);
	dp = VTOI(fdvp);

	/*
	 * Ensure that the directory entry still exists and has not
	 * changed while the new name has been entered. If the source is
	 * a file then the entry may have been unlinked or renamed. In
	 * either case there is no further work to be done. If the source
	 * is a directory then it cannot have been rmdir'ed; its link
	 * count of three would cause a rmdir to fail with ENOTEMPTY.
	 * The IN_RENAME flag ensures that it cannot be moved by another
	 * rename.
	 */
	if (xp != ip) {
		if (doingdirectory)
			panic("ufs_rename: lost dir entry");
		/* ip->i_flag only sets IN_RENAME if doingdirectory */
	} else {
		/*
		 * If the source is a directory with a
		 * new parent, the link count of the old
		 * parent directory must be decremented
		 * and ".." set to point to the new parent.
		 */
		if (doingdirectory && newparent) {
			dp->i_nlink--;
			dp->i_flag |= IN_CHANGE;
			error = vn_rdwr(UIO_READ, fvp, (caddr_t)&dirbuf,
				sizeof (struct dirtemplate), (off_t)0,
				UIO_SYSSPACE, IO_NODELOCKED,
				tcnp->cn_cred, (int *)0, NULL);
			if (error == 0) {
				/* Like ufs little-endian: */
				namlen = dirbuf.dotdot_type;
				if (namlen != 2 ||
				    dirbuf.dotdot_name[0] != '.' ||
				    dirbuf.dotdot_name[1] != '.') {
					ufs_dirbad(xp, (doff_t)12,
					    "rename: mangled dir");
				} else {
					dirbuf.dotdot_ino = newparent;
					(void) vn_rdwr(UIO_WRITE, fvp,
					    (caddr_t)&dirbuf,
					    sizeof (struct dirtemplate),
					    (off_t)0, UIO_SYSSPACE,
					    IO_NODELOCKED|IO_SYNC,
					    tcnp->cn_cred, (int *)0,
					    NULL);
				}
			}
		}
		error = ext2_dirremove(fdvp, fcnp);
		if (!error) {
			xp->i_nlink--;
			xp->i_flag |= IN_CHANGE;
		}
		xp->i_flag &= ~IN_RENAME;
	}
	vput(fdvp);
	vput(fvp);
	vrele(ap->a_fvp);
	return (error);

bad:
	if (xp)
		vput(ITOV(xp));
	if (dp)
		vput(ITOV(dp));
out:
	if (doingdirectory)
		ip->i_flag &= ~IN_RENAME;
	if (vn_lock(fvp, LK_EXCLUSIVE, td) == 0) {
		ip->i_nlink--;
		ip->i_flag |= IN_CHANGE;
		ip->i_flag &= ~IN_RENAME;
		vput(fvp);
	} else {
		vrele(fvp);
	}
	return (error);
}

/*
 * Mkdir system call
 *
 * ext2_mkdir(struct vnode *a_dvp, struct vnode **a_vpp,
 *	      struct componentname *a_cnp, struct vattr *a_vap)
 */
static int
ext2_mkdir(struct vop_mkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vattr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip, *dp;
	struct vnode *tvp;
	struct dirtemplate dirtemplate, *dtp;
	int error, dmode;

	dp = VTOI(dvp);
	if ((nlink_t)dp->i_nlink >= LINK_MAX) {
		error = EMLINK;
		goto out;
	}
	dmode = vap->va_mode & 0777;
	dmode |= IFDIR;
	/*
	 * Must simulate part of ext2_makeinode here to acquire the inode,
	 * but not have it entered in the parent directory. The entry is
	 * made later after writing "." and ".." entries.
	 */
	error = UFS_VALLOC(dvp, dmode, cnp->cn_cred, &tvp);
	if (error)
		goto out;
	ip = VTOI(tvp);
	ip->i_gid = dp->i_gid;
#ifdef SUIDDIR
	{
#ifdef QUOTA
		struct ucred ucred, *ucp;
		ucp = cnp->cn_cred;
#endif
		/*
		 * if we are hacking owners here, (only do this where told to)
		 * and we are not giving it TOO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * The new directory also inherits the SUID bit. 
		 * If user's UID and dir UID are the same,
		 * 'give it away' so that the SUID is still forced on.
		 */
		if ( (dvp->v_mount->mnt_flag & MNT_SUIDDIR) &&
		   (dp->i_mode & ISUID) && dp->i_uid) {
			dmode |= ISUID;
			ip->i_uid = dp->i_uid;
#ifdef QUOTA
			if (dp->i_uid != cnp->cn_cred->cr_uid) {
				/*
				 * make sure the correct user gets charged
				 * for the space.
				 * Make a dummy credential for the victim.
				 * XXX This seems to never be accessed out of
				 * our context so a stack variable is ok.
				 */
				ucred.cr_ref = 1;
				ucred.cr_uid = ip->i_uid;
				ucred.cr_ngroups = 1;
				ucred.cr_groups[0] = dp->i_gid;
				ucp = &ucred;
			}
#endif
		} else {
			ip->i_uid = cnp->cn_cred->cr_uid;
		}
#ifdef QUOTA
		if ((error = getinoquota(ip)) ||
	    	(error = chkiq(ip, 1, ucp, 0))) {
			UFS_VFREE(tvp, ip->i_number, dmode);
			vput(tvp);
			return (error);
		}
#endif
	}
#else
	ip->i_uid = cnp->cn_cred->cr_uid;
#ifdef QUOTA
	if ((error = getinoquota(ip)) ||
	    (error = chkiq(ip, 1, cnp->cn_cred, 0))) {
		UFS_VFREE(tvp, ip->i_number, dmode);
		vput(tvp);
		return (error);
	}
#endif
#endif
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	ip->i_mode = dmode;
	tvp->v_type = VDIR;	/* Rest init'd in getnewvnode(). */
	ip->i_nlink = 2;
	if (cnp->cn_flags & CNP_ISWHITEOUT)
		ip->i_flags |= UF_OPAQUE;
	error = UFS_UPDATE(tvp, 1);

	/*
	 * Bump link count in parent directory
	 * to reflect work done below.  Should
	 * be done before reference is created
	 * so reparation is possible if we crash.
	 */
	dp->i_nlink++;
	dp->i_flag |= IN_CHANGE;
	error = UFS_UPDATE(dvp, 1);
	if (error)
		goto bad;

	/* Initialize directory with "." and ".." from static template. */
	if (EXT2_HAS_INCOMPAT_FEATURE(ip->i_e2fs->s_es,
	    EXT2_FEATURE_INCOMPAT_FILETYPE))
		dtp = &mastertemplate;
	else
		dtp = &omastertemplate;
	dirtemplate = *dtp;
	dirtemplate.dot_ino = ip->i_number;
	dirtemplate.dotdot_ino = dp->i_number;
	/* note that in ext2 DIRBLKSIZ == blocksize, not DEV_BSIZE 
	 * so let's just redefine it - for this function only
	 */
#undef  DIRBLKSIZ 
#define DIRBLKSIZ  VTOI(dvp)->i_e2fs->s_blocksize
	dirtemplate.dotdot_reclen = DIRBLKSIZ - 12;
	error = vn_rdwr(UIO_WRITE, tvp, (caddr_t)&dirtemplate,
	    sizeof (dirtemplate), (off_t)0, UIO_SYSSPACE,
	    IO_NODELOCKED|IO_SYNC, cnp->cn_cred, (int *)0, NULL);
	if (error) {
		dp->i_nlink--;
		dp->i_flag |= IN_CHANGE;
		goto bad;
	}
	if (DIRBLKSIZ > VFSTOUFS(dvp->v_mount)->um_mountp->mnt_stat.f_bsize)
		panic("ufs_mkdir: blksize"); /* XXX should grow with balloc() */
	else {
		ip->i_size = DIRBLKSIZ;
		ip->i_flag |= IN_CHANGE;
	}

	/* Directory set up, now install its entry in the parent directory. */
	error = ext2_direnter(ip, dvp, cnp);
	if (error) {
		dp->i_nlink--;
		dp->i_flag |= IN_CHANGE;
	}
bad:
	/*
	 * No need to do an explicit VOP_TRUNCATE here, vrele will do this
	 * for us because we set the link count to 0.
	 */
	if (error) {
		ip->i_nlink = 0;
		ip->i_flag |= IN_CHANGE;
		vput(tvp);
	} else
		*ap->a_vpp = tvp;
out:
	return (error);
#undef  DIRBLKSIZ
#define DIRBLKSIZ  DEV_BSIZE
}

/*
 * Rmdir system call.
 *
 * ext2_rmdir(struct vnode *a_dvp, struct vnode *a_vp,
 *	      struct componentname *a_cnp)
 */
static int
ext2_rmdir(struct vop_rmdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct thread *td = cnp->cn_td;
	struct inode *ip, *dp;
	int error;

	ip = VTOI(vp);
	dp = VTOI(dvp);

	/*
	 * Verify the directory is empty (and valid).
	 * (Rmdir ".." won't be valid since
	 *  ".." will contain a reference to
	 *  the current directory and thus be
	 *  non-empty.)
	 */
	error = 0;
	if (ip->i_nlink != 2 || !ext2_dirempty(ip, dp->i_number, cnp->cn_cred)) {
		error = ENOTEMPTY;
		goto out;
	}
	if ((dp->i_flags & APPEND)
	    || (ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))) {
		error = EPERM;
		goto out;
	}
	/*
	 * Delete reference to directory before purging
	 * inode.  If we crash in between, the directory
	 * will be reattached to lost+found,
	 */
	error = ext2_dirremove(dvp, cnp);
	if (error)
		goto out;
	dp->i_nlink--;
	dp->i_flag |= IN_CHANGE;
	VOP_UNLOCK(dvp, 0, td);
	/*
	 * Truncate inode.  The only stuff left
	 * in the directory is "." and "..".  The
	 * "." reference is inconsequential since
	 * we're quashing it.  The ".." reference
	 * has already been adjusted above.  We've
	 * removed the "." reference and the reference
	 * in the parent directory, but there may be
	 * other hard links so decrement by 2 and
	 * worry about them later.
	 */
	ip->i_nlink -= 2;
	error = UFS_TRUNCATE(vp, (off_t)0, IO_SYNC, cnp->cn_cred, td);
	vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, td);
out:
	return (error);
}

/*
 * symlink -- make a symbolic link
 *
 * ext2_symlink(struct vnode *a_dvp, struct vnode **a_vpp,
 *		struct componentname *a_cnp, struct vattr *a_vap,
 *		char *a_target)
 */
static int
ext2_symlink(struct vop_symlink_args *ap)
{
	struct vnode *vp, **vpp = ap->a_vpp;
	struct inode *ip;
	int len, error;

	error = ext2_makeinode(IFLNK | ap->a_vap->va_mode, ap->a_dvp,
	    vpp, ap->a_cnp);
	if (error)
		return (error);
	vp = *vpp;
	len = strlen(ap->a_target);
	if (len < vp->v_mount->mnt_maxsymlinklen) {
		ip = VTOI(vp);
		bcopy(ap->a_target, (char *)ip->i_shortlink, len);
		ip->i_size = len;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	} else
		error = vn_rdwr(UIO_WRITE, vp, ap->a_target, len, (off_t)0,
		    UIO_SYSSPACE, IO_NODELOCKED, ap->a_cnp->cn_cred, (int *)0,
		    NULL);
	if (error)
		vput(vp);
	return (error);
}

/*
 * Allocate a new inode.
 */
static int
ext2_makeinode(int mode, struct vnode *dvp, struct vnode **vpp,
	       struct componentname *cnp)
{
	struct inode *ip, *pdir;
	struct vnode *tvp;
	int error;

	pdir = VTOI(dvp);
	*vpp = NULL;
	if ((mode & IFMT) == 0)
		mode |= IFREG;

	error = UFS_VALLOC(dvp, mode, cnp->cn_cred, &tvp);
	if (error) {
		return (error);
	}
	ip = VTOI(tvp);
	ip->i_gid = pdir->i_gid;
#ifdef SUIDDIR
	{
#ifdef QUOTA
		struct ucred ucred, *ucp;
		ucp = cnp->cn_cred;
#endif
		/*
		 * if we are
		 * not the owner of the directory,
		 * and we are hacking owners here, (only do this where told to)
		 * and we are not giving it TOO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * Note that this drops off the execute bits for security.
		 */
		if ( (dvp->v_mount->mnt_flag & MNT_SUIDDIR) &&
		     (pdir->i_mode & ISUID) &&
		     (pdir->i_uid != cnp->cn_cred->cr_uid) && pdir->i_uid) {
			ip->i_uid = pdir->i_uid;
			mode &= ~07111;
#ifdef QUOTA
			/*
			 * make sure the correct user gets charged
			 * for the space.
			 * Quickly knock up a dummy credential for the victim.
			 * XXX This seems to never be accessed out of our
			 * context so a stack variable is ok.
			 */
			ucred.cr_ref = 1;
			ucred.cr_uid = ip->i_uid;
			ucred.cr_ngroups = 1;
			ucred.cr_groups[0] = pdir->i_gid;
			ucp = &ucred;
#endif
		} else {
			ip->i_uid = cnp->cn_cred->cr_uid;
		}
	
#ifdef QUOTA
		if ((error = getinoquota(ip)) ||
	    	(error = chkiq(ip, 1, ucp, 0))) {
			UFS_VFREE(tvp, ip->i_number, mode);
			vput(tvp);
			return (error);
		}
#endif
	}
#else
	ip->i_uid = cnp->cn_cred->cr_uid;
#ifdef QUOTA
	if ((error = getinoquota(ip)) ||
	    (error = chkiq(ip, 1, cnp->cn_cred, 0))) {
		UFS_VFREE(tvp, ip->i_number, mode);
		vput(tvp);
		return (error);
	}
#endif
#endif
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	ip->i_mode = mode;
	tvp->v_type = IFTOVT(mode);	/* Rest init'd in getnewvnode(). */
	ip->i_nlink = 1;
	if ((ip->i_mode & ISGID) && !groupmember(ip->i_gid, cnp->cn_cred) &&
	    suser_cred(cnp->cn_cred, PRISON_ROOT))
		ip->i_mode &= ~ISGID;

	if (cnp->cn_flags & CNP_ISWHITEOUT)
		ip->i_flags |= UF_OPAQUE;

	/*
	 * Make sure inode goes to disk before directory entry.
	 */
	error = UFS_UPDATE(tvp, 1);
	if (error)
		goto bad;
	error = ext2_direnter(ip, dvp, cnp);
	if (error)
		goto bad;

	*vpp = tvp;
	return (0);

bad:
	/*
	 * Write error occurred trying to update the inode
	 * or the directory so must deallocate the inode.
	 */
	ip->i_nlink = 0;
	ip->i_flag |= IN_CHANGE;
	vput(tvp);
	return (error);
}

/*
 * get page routine
 *
 * XXX By default, wimp out... note that a_offset is ignored (and always
 * XXX has been).
 */
static int
ext2_getpages(struct vop_getpages_args *ap)
{
	return (vnode_pager_generic_getpages(ap->a_vp, ap->a_m, ap->a_count,
		ap->a_reqpage));
}

/*
 * put page routine
 *
 * XXX By default, wimp out... note that a_offset is ignored (and always
 * XXX has been).
 */
static int
ext2_putpages(struct vop_putpages_args *ap)
{
	return (vnode_pager_generic_putpages(ap->a_vp, ap->a_m, ap->a_count,
		ap->a_sync, ap->a_rtvals));
}
