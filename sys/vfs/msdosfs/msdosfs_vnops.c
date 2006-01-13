/* $FreeBSD: src/sys/msdosfs/msdosfs_vnops.c,v 1.95.2.4 2003/06/13 15:05:47 trhodes Exp $ */
/* $DragonFly: src/sys/vfs/msdosfs/msdosfs_vnops.c,v 1.28 2006/01/13 21:09:27 swildner Exp $ */
/*	$NetBSD: msdosfs_vnops.c,v 1.68 1998/02/10 14:10:04 mrg Exp $	*/

/*-
 * Copyright (C) 1994, 1995, 1997 Wolfgang Solfrank.
 * Copyright (C) 1994, 1995, 1997 TooLs GmbH.
 * All rights reserved.
 * Original code by Paul Popelka (paulp@uts.amdahl.com) (see below).
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
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Written by Paul Popelka (paulp@uts.amdahl.com)
 *
 * You can do anything you want with this software, just don't say you wrote
 * it, and don't remove this notice.
 *
 * This software is provided "as is".
 *
 * The author supplies this software to be publicly redistributed on the
 * understanding that the author is not responsible for the correct
 * functioning of this software in any circumstances and is not liable for
 * any damages caused by this software.
 *
 * October 1992
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/resourcevar.h>	/* defines plimit structure in proc struct */
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/dirent.h>
#include <sys/signalvar.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_zone.h>
#include <vm/vnode_pager.h>

#include <sys/buf2.h>

#include "bpb.h"
#include "direntry.h"
#include "denode.h"
#include "msdosfsmount.h"
#include "fat.h"

#define	DOS_FILESIZE_MAX	0xffffffff

/*
 * Prototypes for MSDOSFS vnode operations
 */
static int msdosfs_create (struct vop_old_create_args *);
static int msdosfs_mknod (struct vop_old_mknod_args *);
static int msdosfs_close (struct vop_close_args *);
static int msdosfs_access (struct vop_access_args *);
static int msdosfs_getattr (struct vop_getattr_args *);
static int msdosfs_setattr (struct vop_setattr_args *);
static int msdosfs_read (struct vop_read_args *);
static int msdosfs_write (struct vop_write_args *);
static int msdosfs_fsync (struct vop_fsync_args *);
static int msdosfs_remove (struct vop_old_remove_args *);
static int msdosfs_link (struct vop_old_link_args *);
static int msdosfs_rename (struct vop_old_rename_args *);
static int msdosfs_mkdir (struct vop_old_mkdir_args *);
static int msdosfs_rmdir (struct vop_old_rmdir_args *);
static int msdosfs_symlink (struct vop_old_symlink_args *);
static int msdosfs_readdir (struct vop_readdir_args *);
static int msdosfs_bmap (struct vop_bmap_args *);
static int msdosfs_strategy (struct vop_strategy_args *);
static int msdosfs_print (struct vop_print_args *);
static int msdosfs_pathconf (struct vop_pathconf_args *ap);
static int msdosfs_getpages (struct vop_getpages_args *);
static int msdosfs_putpages (struct vop_putpages_args *);

/*
 * Some general notes:
 *
 * In the ufs filesystem the inodes, superblocks, and indirect blocks are
 * read/written using the vnode for the filesystem. Blocks that represent
 * the contents of a file are read/written using the vnode for the file
 * (including directories when they are read/written as files). This
 * presents problems for the dos filesystem because data that should be in
 * an inode (if dos had them) resides in the directory itself.  Since we
 * must update directory entries without the benefit of having the vnode
 * for the directory we must use the vnode for the filesystem.  This means
 * that when a directory is actually read/written (via read, write, or
 * readdir, or seek) we must use the vnode for the filesystem instead of
 * the vnode for the directory as would happen in ufs. This is to insure we
 * retreive the correct block from the buffer cache since the hash value is
 * based upon the vnode address and the desired block number.
 */

/*
 * Create a regular file. On entry the directory to contain the file being
 * created is locked.  We must release before we return. 
 *
 * msdosfs_create(struct vnode *a_dvp, struct vnode **a_vpp,
 *		  struct componentname *a_cnp, struct vattr *a_vap)
 */
static int
msdosfs_create(struct vop_old_create_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	struct denode ndirent;
	struct denode *dep;
	struct denode *pdep = VTODE(ap->a_dvp);
	struct timespec ts;
	int error;

#ifdef MSDOSFS_DEBUG
	printf("msdosfs_create(cnp %p, vap %p\n", cnp, ap->a_vap);
#endif

	/*
	 * If this is the root directory and there is no space left we
	 * can't do anything.  This is because the root directory can not
	 * change size.
	 */
	if (pdep->de_StartCluster == MSDOSFSROOT
	    && pdep->de_fndoffset >= pdep->de_FileSize) {
		error = ENOSPC;
		goto bad;
	}

	/*
	 * Create a directory entry for the file, then call createde() to
	 * have it installed. NOTE: DOS files are always executable.  We
	 * use the absence of the owner write bit to make the file
	 * readonly.
	 */
	bzero(&ndirent, sizeof(ndirent));
	error = uniqdosname(pdep, cnp, ndirent.de_Name);
	if (error)
		goto bad;

	ndirent.de_Attributes = (ap->a_vap->va_mode & VWRITE) ?
				ATTR_ARCHIVE : ATTR_ARCHIVE | ATTR_READONLY;
	ndirent.de_LowerCase = 0;
	ndirent.de_StartCluster = 0;
	ndirent.de_FileSize = 0;
	ndirent.de_dev = pdep->de_dev;
	ndirent.de_devvp = pdep->de_devvp;
	ndirent.de_pmp = pdep->de_pmp;
	ndirent.de_flag = DE_ACCESS | DE_CREATE | DE_UPDATE;
	getnanotime(&ts);
	DETIMES(&ndirent, &ts, &ts, &ts);
	error = createde(&ndirent, pdep, &dep, cnp);
	if (error)
		goto bad;
	*ap->a_vpp = DETOV(dep);
	return (0);

bad:
	return (error);
}

/*
 * msdosfs_mknod(struct vnode *a_dvp, struct vnode **a_vpp,
 *		 struct componentname *a_cnp, struct vattr *a_vap)
 */
static int
msdosfs_mknod(struct vop_old_mknod_args *ap)
{
	switch (ap->a_vap->va_type) {
	case VDIR:
		return (msdosfs_mkdir((struct vop_old_mkdir_args *)ap));
		break;

	case VREG:
		return (msdosfs_create((struct vop_old_create_args *)ap));
		break;

	default:
		return (EINVAL);
	}
	/* NOTREACHED */
}

/*
 * msdosfs_close(struct vnode *a_vp, int a_fflag, struct ucred *a_cred,
 *		 struct thread *a_td)
 */
static int
msdosfs_close(struct vop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);
	struct timespec ts;

	if (vp->v_usecount > 1) {
		getnanotime(&ts);
		if (vp->v_usecount > 1) {
			DETIMES(dep, &ts, &ts, &ts);
		}
	}
	return 0;
}

/*
 * msdosfs_access(struct vnode *a_vp, int a_mode, struct ucred *a_cred,
 *		  struct thread *a_td)
 */
static int
msdosfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(ap->a_vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct ucred *cred = ap->a_cred;
	mode_t mask, file_mode, mode = ap->a_mode;
	gid_t *gp;
	int i;

	file_mode = (S_IXUSR|S_IXGRP|S_IXOTH) | (S_IRUSR|S_IRGRP|S_IROTH) |
	    ((dep->de_Attributes & ATTR_READONLY) ? 0 : (S_IWUSR|S_IWGRP|S_IWOTH));
	file_mode &= pmp->pm_mask;

	/*
	 * Disallow write attempts on read-only file systems;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the file system.
	 */
	if (mode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}

	/* User id 0 always gets access. */
	if (cred->cr_uid == 0)
		return 0;

	mask = 0;

	/* Otherwise, check the owner. */
	if (cred->cr_uid == pmp->pm_uid) {
		if (mode & VEXEC)
			mask |= S_IXUSR;
		if (mode & VREAD)
			mask |= S_IRUSR;
		if (mode & VWRITE)
			mask |= S_IWUSR;
		return (file_mode & mask) == mask ? 0 : EACCES;
	}

	/* Otherwise, check the groups. */
	for (i = 0, gp = cred->cr_groups; i < cred->cr_ngroups; i++, gp++)
		if (pmp->pm_gid == *gp) {
			if (mode & VEXEC)
				mask |= S_IXGRP;
			if (mode & VREAD)
				mask |= S_IRGRP;
			if (mode & VWRITE)
				mask |= S_IWGRP;
			return (file_mode & mask) == mask ? 0 : EACCES;
		}

	/* Otherwise, check everyone else. */
	if (mode & VEXEC)
		mask |= S_IXOTH;
	if (mode & VREAD)
		mask |= S_IROTH;
	if (mode & VWRITE)
		mask |= S_IWOTH;
	return (file_mode & mask) == mask ? 0 : EACCES;
}

/*
 * msdosfs_getattr(struct vnode *a_vp, struct vattr *a_vap,
 *		   struct ucred *a_cred, struct thread *a_td)
 */
static int
msdosfs_getattr(struct vop_getattr_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct vattr *vap = ap->a_vap;
	mode_t mode;
	struct timespec ts;
	u_long dirsperblk = pmp->pm_BytesPerSec / sizeof(struct direntry);
	u_long fileid;

	getnanotime(&ts);
	DETIMES(dep, &ts, &ts, &ts);
	vap->va_fsid = dev2udev(dep->de_dev);
	/*
	 * The following computation of the fileid must be the same as that
	 * used in msdosfs_readdir() to compute d_fileno. If not, pwd
	 * doesn't work.
	 */
	if (dep->de_Attributes & ATTR_DIRECTORY) {
		fileid = cntobn(pmp, dep->de_StartCluster) * dirsperblk;
		if (dep->de_StartCluster == MSDOSFSROOT)
			fileid = 1;
	} else {
		fileid = cntobn(pmp, dep->de_dirclust) * dirsperblk;
		if (dep->de_dirclust == MSDOSFSROOT)
			fileid = roottobn(pmp, 0) * dirsperblk;
		fileid += dep->de_diroffset / sizeof(struct direntry);
	}
	vap->va_fileid = fileid;
	if ((dep->de_Attributes & ATTR_READONLY) == 0)
		mode = S_IRWXU|S_IRWXG|S_IRWXO;
	else
		mode = S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH;
	vap->va_mode = mode & pmp->pm_mask;
	vap->va_uid = pmp->pm_uid;
	vap->va_gid = pmp->pm_gid;
	vap->va_nlink = 1;
	vap->va_rdev = 0;
	vap->va_size = dep->de_FileSize;
	dos2unixtime(dep->de_MDate, dep->de_MTime, 0, &vap->va_mtime);
	if (pmp->pm_flags & MSDOSFSMNT_LONGNAME) {
		dos2unixtime(dep->de_ADate, 0, 0, &vap->va_atime);
		dos2unixtime(dep->de_CDate, dep->de_CTime, dep->de_CHun, &vap->va_ctime);
	} else {
		vap->va_atime = vap->va_mtime;
		vap->va_ctime = vap->va_mtime;
	}
	vap->va_flags = 0;
	if ((dep->de_Attributes & ATTR_ARCHIVE) == 0)
		vap->va_flags |= SF_ARCHIVED;
	vap->va_gen = 0;
	vap->va_blocksize = pmp->pm_bpcluster;
	vap->va_bytes =
	    (dep->de_FileSize + pmp->pm_crbomask) & ~pmp->pm_crbomask;
	vap->va_type = ap->a_vp->v_type;
	vap->va_filerev = dep->de_modrev;
	return (0);
}

/*
 * msdosfs_setattr(struct vnode *a_vp, struct vattr *a_vap,
 *		   struct ucred *a_cred, struct thread *a_td)
 */
static int
msdosfs_setattr(struct vop_setattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(ap->a_vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct vattr *vap = ap->a_vap;
	struct ucred *cred = ap->a_cred;
	int error = 0;

#ifdef MSDOSFS_DEBUG
	printf("msdosfs_setattr(): vp %p, vap %p, cred %p, p %p\n",
	    ap->a_vp, vap, cred, ap->a_td);
#endif

	/*
	 * Check for unsettable attributes.
	 */
	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    (vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL)) {
#ifdef MSDOSFS_DEBUG
		printf("msdosfs_setattr(): returning EINVAL\n");
		printf("    va_type %d, va_nlink %x, va_fsid %lx, va_fileid %lx\n",
		    vap->va_type, vap->va_nlink, vap->va_fsid, vap->va_fileid);
		printf("    va_blocksize %lx, va_rdev %x, va_bytes %qx, va_gen %lx\n",
		    vap->va_blocksize, vap->va_rdev, vap->va_bytes, vap->va_gen);
		printf("    va_uid %x, va_gid %x\n",
		    vap->va_uid, vap->va_gid);
#endif
		return (EINVAL);
	}
	if (vap->va_flags != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (cred->cr_uid != pmp->pm_uid &&
		    (error = suser_cred(cred, PRISON_ROOT)))
			return (error);
		/*
		 * We are very inconsistent about handling unsupported
		 * attributes.  We ignored the access time and the
		 * read and execute bits.  We were strict for the other
		 * attributes.
		 *
		 * Here we are strict, stricter than ufs in not allowing
		 * users to attempt to set SF_SETTABLE bits or anyone to
		 * set unsupported bits.  However, we ignore attempts to
		 * set ATTR_ARCHIVE for directories `cp -pr' from a more
		 * sensible file system attempts it a lot.
		 */
		if (cred->cr_uid != 0) {
			if (vap->va_flags & SF_SETTABLE)
				return EPERM;
		}
		if (vap->va_flags & ~SF_ARCHIVED)
			return EOPNOTSUPP;
		if (vap->va_flags & SF_ARCHIVED)
			dep->de_Attributes &= ~ATTR_ARCHIVE;
		else if (!(dep->de_Attributes & ATTR_DIRECTORY))
			dep->de_Attributes |= ATTR_ARCHIVE;
		dep->de_flag |= DE_MODIFIED;
	}

	if (vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL) {
		uid_t uid;
		gid_t gid;
		
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		uid = vap->va_uid;
		if (uid == (uid_t)VNOVAL)
			uid = pmp->pm_uid;
		gid = vap->va_gid;
		if (gid == (gid_t)VNOVAL)
			gid = pmp->pm_gid;
		if ((cred->cr_uid != pmp->pm_uid || uid != pmp->pm_uid ||
		    (gid != pmp->pm_gid && !groupmember(gid, cred))) &&
		    (error = suser_cred(cred, PRISON_ROOT)))
			return error;
		if (uid != pmp->pm_uid || gid != pmp->pm_gid)
			return EINVAL;
	}

	if (vap->va_size != VNOVAL) {
		/*
		 * Disallow write attempts on read-only file systems;
		 * unless the file is a socket, fifo, or a block or
		 * character device resident on the file system.
		 */
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
			/* NOT REACHED */
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
		error = detrunc(dep, vap->va_size, 0, ap->a_td);
		if (error)
			return error;
	}
	if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (cred->cr_uid != pmp->pm_uid &&
		    (error = suser_cred(cred, PRISON_ROOT)) &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(ap->a_vp, VWRITE, cred, ap->a_td))))
			return (error);
		if (vp->v_type != VDIR) {
			if ((pmp->pm_flags & MSDOSFSMNT_NOWIN95) == 0 &&
			    vap->va_atime.tv_sec != VNOVAL) {
				dep->de_flag &= ~DE_ACCESS;
				unix2dostime(&vap->va_atime, &dep->de_ADate,
				    NULL, NULL);
			}
			if (vap->va_mtime.tv_sec != VNOVAL) {
				dep->de_flag &= ~DE_UPDATE;
				unix2dostime(&vap->va_mtime, &dep->de_MDate,
				    &dep->de_MTime, NULL);
			}
			dep->de_Attributes |= ATTR_ARCHIVE;
			dep->de_flag |= DE_MODIFIED;
		}
	}
	/*
	 * DOS files only have the ability to have their writability
	 * attribute set, so we use the owner write bit to set the readonly
	 * attribute.
	 */
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (cred->cr_uid != pmp->pm_uid &&
		    (error = suser_cred(cred, PRISON_ROOT)))
			return (error);
		if (vp->v_type != VDIR) {
			/* We ignore the read and execute bits. */
			if (vap->va_mode & VWRITE)
				dep->de_Attributes &= ~ATTR_READONLY;
			else
				dep->de_Attributes |= ATTR_READONLY;
			dep->de_Attributes |= ATTR_ARCHIVE;
			dep->de_flag |= DE_MODIFIED;
		}
	}
	return (deupdat(dep, 1));
}

/*
 * msdosfs_read(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *		struct ucred *a_cred)
 */
static int
msdosfs_read(struct vop_read_args *ap)
{
	int error = 0;
	int blsize;
	int isadir;
	int orig_resid;
	u_int n;
	u_long diff;
	u_long on;
	daddr_t lbn;
	daddr_t rablock;
	int rasize;
	int seqcount;
	struct buf *bp;
	struct vnode *vp = ap->a_vp;
	struct denode *dep = VTODE(vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct uio *uio = ap->a_uio;

	if (uio->uio_offset < 0)
		return (EINVAL);

	if ((uoff_t)uio->uio_offset > DOS_FILESIZE_MAX)
                return (0);
	/*
	 * If they didn't ask for any data, then we are done.
	 */
	orig_resid = uio->uio_resid;
	if (orig_resid <= 0)
		return (0);

	seqcount = ap->a_ioflag >> IO_SEQSHIFT;

	isadir = dep->de_Attributes & ATTR_DIRECTORY;
	do {
		if (uio->uio_offset >= dep->de_FileSize)
			break;
		lbn = de_cluster(pmp, uio->uio_offset);
		/*
		 * If we are operating on a directory file then be sure to
		 * do i/o with the vnode for the filesystem instead of the
		 * vnode for the directory.
		 */
		if (isadir) {
			/* convert cluster # to block # */
			error = pcbmap(dep, lbn, &lbn, 0, &blsize);
			if (error == E2BIG) {
				error = EINVAL;
				break;
			} else if (error)
				break;
			error = bread(pmp->pm_devvp, lbn, blsize, &bp);
		} else {
			blsize = pmp->pm_bpcluster;
			rablock = lbn + 1;
			if (seqcount > 1 &&
			    de_cn2off(pmp, rablock) < dep->de_FileSize) {
				rasize = pmp->pm_bpcluster;
				error = breadn(vp, lbn, blsize,
				    &rablock, &rasize, 1, &bp); 
			} else {
				error = bread(vp, lbn, blsize, &bp);
			}
		}
		if (error) {
			brelse(bp);
			break;
		}
		on = uio->uio_offset & pmp->pm_crbomask;
		diff = pmp->pm_bpcluster - on;
		n = diff > uio->uio_resid ? uio->uio_resid : diff;
		diff = dep->de_FileSize - uio->uio_offset;
		if (diff < n)
			n = diff;
		diff = blsize - bp->b_resid;
		if (diff < n)
			n = diff;
		error = uiomove(bp->b_data + on, (int) n, uio);
		brelse(bp);
	} while (error == 0 && uio->uio_resid > 0 && n != 0);
	if (!isadir && (error == 0 || uio->uio_resid != orig_resid) &&
	    (vp->v_mount->mnt_flag & MNT_NOATIME) == 0)
		dep->de_flag |= DE_ACCESS;
	return (error);
}

/*
 * Write data to a file or directory.
 *
 * msdosfs_write(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *		 struct ucred *a_cred)
 */
static int
msdosfs_write(struct vop_write_args *ap)
{
	int n;
	int croffset;
	int resid;
	u_long osize;
	int error = 0;
	u_long count;
	daddr_t bn, lastcn;
	struct buf *bp;
	int ioflag = ap->a_ioflag;
	struct uio *uio = ap->a_uio;
	struct thread *td = uio->uio_td;
	struct vnode *vp = ap->a_vp;
	struct vnode *thisvp;
	struct denode *dep = VTODE(vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct proc *p = (td ? td->td_proc : NULL);

#ifdef MSDOSFS_DEBUG
	printf("msdosfs_write(vp %p, uio %p, ioflag %x, cred %p\n",
	    vp, uio, ioflag, cred);
	printf("msdosfs_write(): diroff %lu, dirclust %lu, startcluster %lu\n",
	    dep->de_diroffset, dep->de_dirclust, dep->de_StartCluster);
#endif

	switch (vp->v_type) {
	case VREG:
		if (ioflag & IO_APPEND)
			uio->uio_offset = dep->de_FileSize;
		thisvp = vp;
		break;
	case VDIR:
		return EISDIR;
	default:
		panic("msdosfs_write(): bad file type");
	}

	if (uio->uio_offset < 0)
		return (EFBIG);

	if (uio->uio_resid == 0)
		return (0);

	/*
	 * If they've exceeded their filesize limit, tell them about it.
	 */
	if (p &&
	    ((uoff_t)uio->uio_offset + uio->uio_resid >
	    p->p_rlimit[RLIMIT_FSIZE].rlim_cur)) {
		psignal(p, SIGXFSZ);
		return (EFBIG);
	}

	if ((uoff_t)uio->uio_offset + uio->uio_resid > DOS_FILESIZE_MAX)
                return (EFBIG);

	/*
	 * If the offset we are starting the write at is beyond the end of
	 * the file, then they've done a seek.  Unix filesystems allow
	 * files with holes in them, DOS doesn't so we must fill the hole
	 * with zeroed blocks.
	 */
	if (uio->uio_offset > dep->de_FileSize) {
		error = deextend(dep, uio->uio_offset);
		if (error)
			return (error);
	}

	/*
	 * Remember some values in case the write fails.
	 */
	resid = uio->uio_resid;
	osize = dep->de_FileSize;

	/*
	 * If we write beyond the end of the file, extend it to its ultimate
	 * size ahead of the time to hopefully get a contiguous area.
	 */
	if (uio->uio_offset + resid > osize) {
		count = de_clcount(pmp, uio->uio_offset + resid) -
			de_clcount(pmp, osize);
		error = extendfile(dep, count, NULL, NULL, 0);
		if (error &&  (error != ENOSPC || (ioflag & IO_UNIT)))
			goto errexit;
		lastcn = dep->de_fc[FC_LASTFC].fc_frcn;
	} else
		lastcn = de_clcount(pmp, osize) - 1;

	do {
		if (de_cluster(pmp, uio->uio_offset) > lastcn) {
			error = ENOSPC;
			break;
		}

		croffset = uio->uio_offset & pmp->pm_crbomask;
		n = min(uio->uio_resid, pmp->pm_bpcluster - croffset);
		if (uio->uio_offset + n > dep->de_FileSize) {
			dep->de_FileSize = uio->uio_offset + n;
			/* The object size needs to be set before buffer is allocated */
			vnode_pager_setsize(vp, dep->de_FileSize);
		}

		bn = de_cluster(pmp, uio->uio_offset);
		if ((uio->uio_offset & pmp->pm_crbomask) == 0
		    && (de_cluster(pmp, uio->uio_offset + uio->uio_resid) 
		        > de_cluster(pmp, uio->uio_offset)
			|| uio->uio_offset + uio->uio_resid >= dep->de_FileSize)) {
			/*
			 * If either the whole cluster gets written,
			 * or we write the cluster from its start beyond EOF,
			 * then no need to read data from disk.
			 */
			bp = getblk(thisvp, bn, pmp->pm_bpcluster, 0, 0);
			clrbuf(bp);
			/*
			 * Do the bmap now, since pcbmap needs buffers
			 * for the fat table. (see msdosfs_strategy)
			 */
			if (bp->b_blkno == bp->b_lblkno) {
				error = pcbmap(dep, bp->b_lblkno, &bp->b_blkno, 
				     0, 0);
				if (error)
					bp->b_blkno = -1;
			}
			if (bp->b_blkno == -1) {
				brelse(bp);
				if (!error)
					error = EIO;		/* XXX */
				break;
			}
		} else {
			/*
			 * The block we need to write into exists, so read it in.
			 */
			error = bread(thisvp, bn, pmp->pm_bpcluster, &bp);
			if (error) {
				brelse(bp);
				break;
			}
		}

		/*
		 * Should these vnode_pager_* functions be done on dir
		 * files?
		 */

		/*
		 * Copy the data from user space into the buf header.
		 */
		error = uiomove(bp->b_data + croffset, n, uio);
		if (error) {
			brelse(bp);
			break;
		}

		/*
		 * If they want this synchronous then write it and wait for
		 * it.  Otherwise, if on a cluster boundary write it
		 * asynchronously so we can move on to the next block
		 * without delay.  Otherwise do a delayed write because we
		 * may want to write somemore into the block later.
		 */
		if (ioflag & IO_SYNC)
			bwrite(bp);
		else if (n + croffset == pmp->pm_bpcluster)
			bawrite(bp);
		else
			bdwrite(bp);
		dep->de_flag |= DE_UPDATE;
	} while (error == 0 && uio->uio_resid > 0);

	/*
	 * If the write failed and they want us to, truncate the file back
	 * to the size it was before the write was attempted.
	 */
errexit:
	if (error) {
		if (ioflag & IO_UNIT) {
			detrunc(dep, osize, ioflag & IO_SYNC, NULL);
			uio->uio_offset -= resid - uio->uio_resid;
			uio->uio_resid = resid;
		} else {
			detrunc(dep, dep->de_FileSize, ioflag & IO_SYNC, NULL);
			if (uio->uio_resid != resid)
				error = 0;
		}
	} else if (ioflag & IO_SYNC)
		error = deupdat(dep, 1);
	return (error);
}

/*
 * Flush the blocks of a file to disk.
 *
 * This function is worthless for vnodes that represent directories. Maybe we
 * could just do a sync if they try an fsync on a directory file.
 *
 * msdosfs_fsync(struct vnode *a_vp, struct ucred *a_cred, int a_waitfor,
 *		 struct thread *a_td)
 */
static int
msdosfs_fsync(struct vop_fsync_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/*
	 * Flush all dirty buffers associated with a vnode.
	 */
#ifdef DIAGNOSTIC
loop:
#endif
	vfsync(vp, ap->a_waitfor, 0, (daddr_t)-1, NULL, NULL);
#ifdef DIAGNOSTIC
	if (ap->a_waitfor == MNT_WAIT && !RB_EMPTY(&vp->v_rbdirty_tree)) {
		vprint("msdosfs_fsync: dirty", vp);
		goto loop;
	}
#endif
	return (deupdat(VTODE(vp), ap->a_waitfor == MNT_WAIT));
}

/*
 * msdosfs_remove(struct vnode *a_dvp, struct vnode *a_vp,
 *		  struct componentname *a_cnp)
 */
static int
msdosfs_remove(struct vop_old_remove_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);
	struct denode *ddep = VTODE(ap->a_dvp);
	int error;

	if (ap->a_vp->v_type == VDIR)
		error = EPERM;
	else
		error = removede(ddep, dep);
#ifdef MSDOSFS_DEBUG
	printf("msdosfs_remove(), dep %p, v_usecount %d\n", dep, ap->a_vp->v_usecount);
#endif
	return (error);
}

/*
 * DOS filesystems don't know what links are. But since we already called
 * msdosfs_lookup() with create and lockparent, the parent is locked so we
 * have to free it before we return the error.
 *
 * msdosfs_link(struct vnode *a_tdvp, struct vnode *a_vp,
 *		struct componentname *a_cnp)
 */
static int
msdosfs_link(struct vop_old_link_args *ap)
{
	return (EOPNOTSUPP);
}

/*
 * Renames on files require moving the denode to a new hash queue since the
 * denode's location is used to compute which hash queue to put the file
 * in. Unless it is a rename in place.  For example "mv a b".
 *
 * What follows is the basic algorithm:
 *
 * if (file move) {
 *	if (dest file exists) {
 *		remove dest file
 *	}
 *	if (dest and src in same directory) {
 *		rewrite name in existing directory slot
 *	} else {
 *		write new entry in dest directory
 *		update offset and dirclust in denode
 *		move denode to new hash chain
 *		clear old directory entry
 *	}
 * } else {
 *	directory move
 *	if (dest directory exists) {
 *		if (dest is not empty) {
 *			return ENOTEMPTY
 *		}
 *		remove dest directory
 *	}
 *	if (dest and src in same directory) {
 *		rewrite name in existing entry
 *	} else {
 *		be sure dest is not a child of src directory
 *		write entry in dest directory
 *		update "." and ".." in moved directory
 *		clear old directory entry for moved directory
 *	}
 * }
 *
 * On entry:
 *	source's parent directory is unlocked
 *	source file or directory is unlocked
 *	destination's parent directory is locked
 *	destination file or directory is locked if it exists
 *
 * On exit:
 *	all denodes should be released
 *
 * Notes:
 * I'm not sure how the memory containing the pathnames pointed at by the
 * componentname structures is freed, there may be some memory bleeding
 * for each rename done.
 *
 * msdosfs_rename(struct vnode *a_fdvp, struct vnode *a_fvp,
 *		  struct componentname *a_fcnp, struct vnode *a_tdvp,
 *		  struct vnode *a_tvp, struct componentname *a_tcnp)
 */
static int
msdosfs_rename(struct vop_old_rename_args *ap)
{
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *tvp = ap->a_tvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct thread *td = fcnp->cn_td;
	struct denode *ip, *xp, *dp, *zp;
	u_char toname[11], oldname[11];
	u_long from_diroffset, to_diroffset;
	u_char to_count;
	int doingdirectory = 0, newparent = 0;
	int error;
	u_long cn;
	daddr_t bn;
	struct denode *fddep;	/* from file's parent directory	 */
	struct msdosfsmount *pmp;
	struct direntry *dotdotp;
	struct buf *bp;

	fddep = VTODE(ap->a_fdvp);
	pmp = fddep->de_pmp;

	pmp = VFSTOMSDOSFS(fdvp->v_mount);

	/*
	 * Check for cross-device rename.
	 */
	if ((fvp->v_mount != tdvp->v_mount) ||
	    (tvp && (fvp->v_mount != tvp->v_mount))) {
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

	/*
	 * If source and dest are the same, do nothing.
	 */
	if (tvp == fvp) {
		error = 0;
		goto abortit;
	}

	/*
	 * fvp, fdvp are unlocked, tvp, tdvp are locked.  Lock fvp and note
	 * that we have to unlock it to use the abortit target.
	 */
	error = vn_lock(fvp, LK_EXCLUSIVE, td);
	if (error)
		goto abortit;
	dp = VTODE(fdvp);
	ip = VTODE(fvp);

	/*
	 * Be sure we are not renaming ".", "..", or an alias of ".". This
	 * leads to a crippled directory tree.  It's pretty tough to do a
	 * "ls" or "pwd" with the "." directory entry missing, and "cd .."
	 * doesn't work if the ".." entry is missing.
	 */
	if (ip->de_Attributes & ATTR_DIRECTORY) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
		    dp == ip ||
		    (fcnp->cn_flags & CNP_ISDOTDOT) ||
		    (tcnp->cn_flags & CNP_ISDOTDOT) ||
		    (ip->de_flag & DE_RENAME)) {
			VOP_UNLOCK(fvp, 0, td);
			error = EINVAL;
			goto abortit;
		}
		ip->de_flag |= DE_RENAME;
		doingdirectory++;
	}

	/*
	 * fvp locked, fdvp unlocked, tvp, tdvp locked.  DE_RENAME only
	 * set if doingdirectory.  We will get fvp unlocked in fairly
	 * short order.  dp and xp must be setup and fvp must be unlocked
	 * for the out and bad targets to work properly.
	 */
	dp = VTODE(tdvp);
	xp = tvp ? VTODE(tvp) : NULL;

	/*
	 * Remember direntry place to use for destination
	 */
	to_diroffset = dp->de_fndoffset;
	to_count = dp->de_fndcnt;

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory heirarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..". We must repeat the call
	 * to namei, as the parent directory is unlocked by the
	 * call to doscheckpath().
	 */
	error = VOP_ACCESS(fvp, VWRITE, tcnp->cn_cred, tcnp->cn_td);
	VOP_UNLOCK(fvp, 0, td);
	if (VTODE(fdvp)->de_StartCluster != VTODE(tdvp)->de_StartCluster)
		newparent = 1;

	/*
	 * ok. fvp, fdvp unlocked, tvp, tdvp locked. tvp may be NULL.
	 * DE_RENAME only set if doingdirectory.
	 */
	if (doingdirectory && newparent) {
		if (error)	/* write access check above */
			goto bad;
		if (xp != NULL) {
			vput(tvp);
			xp = NULL;
		}
		/*
		 * checkpath vput's tdvp (VTOI(dp)) on return no matter what,
		 * get an extra ref so we wind up with just an unlocked, ref'd
		 * tdvp.  The 'out' target skips tvp and tdvp cleanups (tdvp
		 * isn't locked so we can't use the out target).
		 */
		vref(tdvp);
		error = doscheckpath(ip, dp);
		tcnp->cn_flags |= CNP_PDIRUNLOCK;
		if (error) {
			vrele(tdvp);
			goto out;
		}
		/*
		 * relookup no longer messes with the ref count.  tdvp must
		 * be unlocked on entry and on success will be locked on
		 * return.
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
		 * tvp and tdvp are now locked again.
		 */
		dp = VTODE(tdvp);
		xp = tvp ? VTODE(tvp) : NULL;
	}

	/*
	 * tvp and tdvp are now locked again, the 'bad' target can be used
	 * to clean them up again.  Delete an existant target and clean
	 * up tvp.  Set xp to NULL to indicate that tvp has been cleaned up.
	 */
	if (xp != NULL) {
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if (xp->de_Attributes & ATTR_DIRECTORY) {
			if (!dosdirempty(xp)) {
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
		error = removede(dp, xp);
		if (error)
			goto bad;
		vput(tvp);
		xp = NULL;
		tvp = NULL;
	}

	/*
	 * Convert the filename in tcnp into a dos filename. We copy this
	 * into the denode and directory entry for the destination
	 * file/directory.
	 */
	error = uniqdosname(VTODE(tdvp), tcnp, toname);
	if (error)
		goto bad;

	/*
	 * Since from wasn't locked at various places above, we have to do
	 * a relookup here.  If the target and source are the same directory
	 * we have to unlock the target directory in order to safely relookup
	 * the source, because relookup expects its directory to be unlocked.
	 *
	 * Note that ap->a_fvp is still valid and ref'd.  Any cleanup must
	 * now take that into account.
	 *
	 * The tdvp locking issues make this a real mess.
	 */
	fcnp->cn_flags &= ~CNP_MODMASK;
	fcnp->cn_flags |= CNP_LOCKPARENT;
	if (newparent == 0)
		VOP_UNLOCK(tdvp, 0, td);
	error = relookup(fdvp, &fvp, fcnp);
	if (error || fvp == NULL) {
		/*
		 * From name has disappeared.  Note: fdvp might == tdvp.
		 *
		 * DE_RENAME is only set if doingdirectory.
		 */
		if (doingdirectory)
			panic("rename: lost dir entry");
		if (fcnp->cn_flags & CNP_PDIRUNLOCK)
			vrele(fdvp);
		else
			vput(fdvp);
		if (newparent == 0)
			vrele(tdvp);
		else
			vput(tdvp);
		vrele(ap->a_fvp);
		return(0);
	}

	/*
	 * No error occured.  tdvp, fdvp and fvp are all locked.  If 
	 * newparent was 0 be aware that fdvp == tdvp.  tvp has been cleaned
	 * up.  ap->a_fvp is still refd.
	 */
	xp = VTODE(fvp);
	zp = VTODE(fdvp);
	from_diroffset = zp->de_fndoffset;

	/*
	 * Ensure that the directory entry still exists and has not
	 * changed till now. If the source is a file the entry may
	 * have been unlinked or renamed. In either case there is
	 * no further work to be done. If the source is a directory
	 * then it cannot have been rmdir'ed or renamed; this is
	 * prohibited by the DE_RENAME flag.
	 *
	 * DE_RENAME is only set if doingdirectory.
	 */
	if (xp != ip) {
		if (doingdirectory)
			panic("rename: lost dir entry");
		goto done;
	} else {
		u_long new_dirclust;
		u_long new_diroffset;

		/*
		 * First write a new entry in the destination
		 * directory and mark the entry in the source directory
		 * as deleted.  Then move the denode to the correct hash
		 * chain for its new location in the filesystem.  And, if
		 * we moved a directory, then update its .. entry to point
		 * to the new parent directory.
		 */
		bcopy(ip->de_Name, oldname, 11);
		bcopy(toname, ip->de_Name, 11);	/* update denode */
		dp->de_fndoffset = to_diroffset;
		dp->de_fndcnt = to_count;
		error = createde(ip, dp, (struct denode **)0, tcnp);
		if (error) {
			bcopy(oldname, ip->de_Name, 11);
			goto done;
		}
		ip->de_refcnt++;
		zp->de_fndoffset = from_diroffset;
		error = removede(zp, ip);
		if (error) {
			/* XXX should really panic here, fs is corrupt */
			goto done;
		}
		if (!doingdirectory) {
			error = pcbmap(dp, de_cluster(pmp, to_diroffset), 0,
				       &new_dirclust, 0);
			if (error) {
				/* XXX should really panic here, fs is corrupt */
				goto done;
			}
			if (new_dirclust == MSDOSFSROOT)
				new_diroffset = to_diroffset;
			else
				new_diroffset = to_diroffset & pmp->pm_crbomask;
			msdosfs_reinsert(ip, new_dirclust, new_diroffset);
		}
	}

	/*
	 * If we moved a directory to a new parent directory, then we must
	 * fixup the ".." entry in the moved directory.
	 */
	if (doingdirectory && newparent) {
		cn = ip->de_StartCluster;
		if (cn == MSDOSFSROOT) {
			/* this should never happen */
			panic("msdosfs_rename(): updating .. in root directory?");
		} else {
			bn = cntobn(pmp, cn);
		}
		error = bread(pmp->pm_devvp, bn, pmp->pm_bpcluster, &bp);
		if (error) {
			/* XXX should really panic here, fs is corrupt */
			brelse(bp);
			goto done;
		}
		dotdotp = (struct direntry *)bp->b_data + 1;
		putushort(dotdotp->deStartCluster, dp->de_StartCluster);
		if (FAT32(pmp))
			putushort(dotdotp->deHighClust, dp->de_StartCluster >> 16);
		error = bwrite(bp);
		if (error) {
			/* XXX should really panic here, fs is corrupt */
			goto done;
		}
	}

	/*
	 * done case fvp, fdvp, tdvp are locked.  ap->a_fvp is refd
	 */
done:
	if (doingdirectory)
		ip->de_flag &= ~DE_RENAME;	/* XXX fvp not locked */
	vput(fvp);
	if (newparent)
		vput(fdvp);
	else
		vrele(fdvp);
	vput(tdvp);
	vrele(ap->a_fvp);
	return (error);

	/*
	 * 'bad' target: xp governs tvp.  tvp and tdvp arel ocked, fdvp and fvp
	 * are not locked.  ip points to fvp's inode which may have DE_RENAME
	 * set.
	 */
bad:
	if (xp)
		vput(tvp);
	vput(tdvp);
out:
	/*
	 * 'out' target: tvp and tdvp have already been cleaned up.
	 */
	if (doingdirectory)
		ip->de_flag &= ~DE_RENAME;
	vrele(fdvp);
	vrele(fvp);
	return (error);

}

static struct {
	struct direntry dot;
	struct direntry dotdot;
} dosdirtemplate = {
	{	".       ", "   ",			/* the . entry */
		ATTR_DIRECTORY,				/* file attribute */
		0,	 				/* reserved */
		0, { 0, 0 }, { 0, 0 },			/* create time & date */
		{ 0, 0 },				/* access date */
		{ 0, 0 },				/* high bits of start cluster */
		{ 210, 4 }, { 210, 4 },			/* modify time & date */
		{ 0, 0 },				/* startcluster */
		{ 0, 0, 0, 0 } 				/* filesize */
	},
	{	"..      ", "   ",			/* the .. entry */
		ATTR_DIRECTORY,				/* file attribute */
		0,	 				/* reserved */
		0, { 0, 0 }, { 0, 0 },			/* create time & date */
		{ 0, 0 },				/* access date */
		{ 0, 0 },				/* high bits of start cluster */
		{ 210, 4 }, { 210, 4 },			/* modify time & date */
		{ 0, 0 },				/* startcluster */
		{ 0, 0, 0, 0 }				/* filesize */
	}
};

/*
 * msdosfs_mkdir(struct vnode *a_dvp, struct vnode **a_vpp,
 *		 struct componentname *a_cnp, struct vattr *a_vap)
 */
static int
msdosfs_mkdir(struct vop_old_mkdir_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	struct denode *dep;
	struct denode *pdep = VTODE(ap->a_dvp);
	struct direntry *denp;
	struct msdosfsmount *pmp = pdep->de_pmp;
	struct buf *bp;
	u_long newcluster, pcl;
	int bn;
	int error;
	struct denode ndirent;
	struct timespec ts;

	/*
	 * If this is the root directory and there is no space left we
	 * can't do anything.  This is because the root directory can not
	 * change size.
	 */
	if (pdep->de_StartCluster == MSDOSFSROOT
	    && pdep->de_fndoffset >= pdep->de_FileSize) {
		error = ENOSPC;
		goto bad2;
	}

	/*
	 * Allocate a cluster to hold the about to be created directory.
	 */
	error = clusteralloc(pmp, 0, 1, CLUST_EOFE, &newcluster, NULL);
	if (error)
		goto bad2;

	bzero(&ndirent, sizeof(ndirent));
	ndirent.de_pmp = pmp;
	ndirent.de_flag = DE_ACCESS | DE_CREATE | DE_UPDATE;
	getnanotime(&ts);
	DETIMES(&ndirent, &ts, &ts, &ts);

	/*
	 * Now fill the cluster with the "." and ".." entries. And write
	 * the cluster to disk.  This way it is there for the parent
	 * directory to be pointing at if there were a crash.
	 */
	bn = cntobn(pmp, newcluster);
	/* always succeeds */
	bp = getblk(pmp->pm_devvp, bn, pmp->pm_bpcluster, 0, 0);
	bzero(bp->b_data, pmp->pm_bpcluster);
	bcopy(&dosdirtemplate, bp->b_data, sizeof dosdirtemplate);
	denp = (struct direntry *)bp->b_data;
	putushort(denp[0].deStartCluster, newcluster);
	putushort(denp[0].deCDate, ndirent.de_CDate);
	putushort(denp[0].deCTime, ndirent.de_CTime);
	denp[0].deCHundredth = ndirent.de_CHun;
	putushort(denp[0].deADate, ndirent.de_ADate);
	putushort(denp[0].deMDate, ndirent.de_MDate);
	putushort(denp[0].deMTime, ndirent.de_MTime);
	pcl = pdep->de_StartCluster;
	if (FAT32(pmp) && pcl == pmp->pm_rootdirblk)
		pcl = 0;
	putushort(denp[1].deStartCluster, pcl);
	putushort(denp[1].deCDate, ndirent.de_CDate);
	putushort(denp[1].deCTime, ndirent.de_CTime);
	denp[1].deCHundredth = ndirent.de_CHun;
	putushort(denp[1].deADate, ndirent.de_ADate);
	putushort(denp[1].deMDate, ndirent.de_MDate);
	putushort(denp[1].deMTime, ndirent.de_MTime);
	if (FAT32(pmp)) {
		putushort(denp[0].deHighClust, newcluster >> 16);
		putushort(denp[1].deHighClust, pdep->de_StartCluster >> 16);
	}

	error = bwrite(bp);
	if (error)
		goto bad;

	/*
	 * Now build up a directory entry pointing to the newly allocated
	 * cluster.  This will be written to an empty slot in the parent
	 * directory.
	 */
	error = uniqdosname(pdep, cnp, ndirent.de_Name);
	if (error)
		goto bad;

	ndirent.de_Attributes = ATTR_DIRECTORY;
	ndirent.de_LowerCase = 0;
	ndirent.de_StartCluster = newcluster;
	ndirent.de_FileSize = 0;
	ndirent.de_dev = pdep->de_dev;
	ndirent.de_devvp = pdep->de_devvp;
	error = createde(&ndirent, pdep, &dep, cnp);
	if (error)
		goto bad;
	*ap->a_vpp = DETOV(dep);
	return (0);

bad:
	clusterfree(pmp, newcluster, NULL);
bad2:
	return (error);
}

/*
 * msdosfs_rmdir(struct vnode *a_dvp, struct vnode *a_vp,
 *		 struct componentname *a_cnp)
 */
static int
msdosfs_rmdir(struct vop_old_rmdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct denode *ip, *dp;
	struct thread *td = cnp->cn_td;
	int error;
	
	ip = VTODE(vp);
	dp = VTODE(dvp);

	/*
	 * Verify the directory is empty (and valid).
	 * (Rmdir ".." won't be valid since
	 *  ".." will contain a reference to
	 *  the current directory and thus be
	 *  non-empty.)
	 */
	error = 0;
	if (!dosdirempty(ip) || ip->de_flag & DE_RENAME) {
		error = ENOTEMPTY;
		goto out;
	}
	/*
	 * Delete the entry from the directory.  For dos filesystems this
	 * gets rid of the directory entry on disk, the in memory copy
	 * still exists but the de_refcnt is <= 0.  This prevents it from
	 * being found by deget().  When the vput() on dep is done we give
	 * up access and eventually msdosfs_reclaim() will be called which
	 * will remove it from the denode cache.
	 */
	error = removede(dp, ip);
	if (error)
		goto out;
	/*
	 * This is where we decrement the link count in the parent
	 * directory.  Since dos filesystems don't do this we just purge
	 * the name cache.
	 */
	VOP_UNLOCK(dvp, 0, td);
	/*
	 * Truncate the directory that is being deleted.
	 */
	error = detrunc(ip, (u_long)0, IO_SYNC, td);

	vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, td);
out:
	return (error);
}

/*
 * DOS filesystems don't know what symlinks are.
 *
 * msdosfs_symlink(struct vnode *a_dvp, struct vnode **a_vpp,
 *		   struct componentname *a_cnp, struct vattr *a_vap,
 *		   char *a_target)
 */
static int
msdosfs_symlink(struct vop_old_symlink_args *ap)
{
	return (EOPNOTSUPP);
}

/*
 * msdosfs_readdir(struct vnode *a_vp, struct uio *a_uio,
 *		   struct ucred *a_cred, int *a_eofflag, int *a_ncookies,
 *		   u_long **a_cookies)
 */
static int
msdosfs_readdir(struct vop_readdir_args *ap)
{
	int error = 0;
	int diff;
	long n;
	int blsize;
	long on;
	u_long cn;
	u_long dirsperblk;
	long bias = 0;
	daddr_t bn, lbn;
	struct buf *bp;
	struct denode *dep = VTODE(ap->a_vp);
	struct msdosfsmount *pmp = dep->de_pmp;
	struct direntry *dentp;
	struct uio *uio = ap->a_uio;
	u_long *cookies = NULL;
	int ncookies = 0;
	off_t offset, off;
	int chksum = -1;
	ino_t d_ino;
	uint16_t d_namlen;
	uint8_t d_type;
	char *d_name_storage = NULL;
	char *d_name;

#ifdef MSDOSFS_DEBUG
	printf("msdosfs_readdir(): vp %p, uio %p, cred %p, eofflagp %p\n",
	    ap->a_vp, uio, ap->a_cred, ap->a_eofflag);
#endif

	/*
	 * msdosfs_readdir() won't operate properly on regular files since
	 * it does i/o only with the the filesystem vnode, and hence can
	 * retrieve the wrong block from the buffer cache for a plain file.
	 * So, fail attempts to readdir() on a plain file.
	 */
	if ((dep->de_Attributes & ATTR_DIRECTORY) == 0)
		return (ENOTDIR);

	/*
	 * If the user buffer is smaller than the size of one dos directory
	 * entry or the file offset is not a multiple of the size of a
	 * directory entry, then we fail the read.
	 */
	off = offset = uio->uio_offset;
	if (uio->uio_resid < sizeof(struct direntry) ||
	    (offset & (sizeof(struct direntry) - 1)))
		return (EINVAL);

	if (ap->a_ncookies) {
		ncookies = uio->uio_resid / 16;
		MALLOC(cookies, u_long *, ncookies * sizeof(u_long), M_TEMP,
		       M_WAITOK);
		*ap->a_cookies = cookies;
		*ap->a_ncookies = ncookies;
	}

	dirsperblk = pmp->pm_BytesPerSec / sizeof(struct direntry);

	/*
	 * If they are reading from the root directory then, we simulate
	 * the . and .. entries since these don't exist in the root
	 * directory.  We also set the offset bias to make up for having to
	 * simulate these entries. By this I mean that at file offset 64 we
	 * read the first entry in the root directory that lives on disk.
	 */
	if (dep->de_StartCluster == MSDOSFSROOT
	    || (FAT32(pmp) && dep->de_StartCluster == pmp->pm_rootdirblk)) {
#if 0
		printf("msdosfs_readdir(): going after . or .. in root dir, offset %d\n",
		    offset);
#endif
		bias = 2 * sizeof(struct direntry);
		if (offset < bias) {
			for (n = (int)offset / sizeof(struct direntry); n < 2;
			     n++) {
				if (FAT32(pmp))
					d_ino = cntobn(pmp, pmp->pm_rootdirblk)
					    * dirsperblk;
				else
					d_ino = 1;
				d_type = DT_DIR;
				if (n == 0) {
					d_namlen = 1;
					d_name = ".";
				} else /* if (n == 1) */{
					d_namlen = 2;
					d_name = "..";
				}
				if (vop_write_dirent(&error, uio, d_ino, d_type,
				    d_namlen, d_name))
					goto out;
				if (error)
					goto out;
				offset += sizeof(struct direntry);
				off = offset;
				if (cookies) {
					*cookies++ = offset;
					if (--ncookies <= 0)
						goto out;
				}
			}
		}
	}

	d_name_storage = malloc(WIN_MAXLEN, M_TEMP, M_WAITOK);
	off = offset;

	while (uio->uio_resid > 0) {
		lbn = de_cluster(pmp, offset - bias);
		on = (offset - bias) & pmp->pm_crbomask;
		n = min(pmp->pm_bpcluster - on, uio->uio_resid);
		diff = dep->de_FileSize - (offset - bias);
		if (diff <= 0)
			break;
		n = min(n, diff);
		error = pcbmap(dep, lbn, &bn, &cn, &blsize);
		if (error)
			break;
		error = bread(pmp->pm_devvp, bn, blsize, &bp);
		if (error) {
			brelse(bp);
			free(d_name_storage, M_TEMP);
			return (error);
		}
		n = min(n, blsize - bp->b_resid);

		/*
		 * Convert from dos directory entries to fs-independent
		 * directory entries.
		 */
		for (dentp = (struct direntry *)(bp->b_data + on);
		     (char *)dentp < bp->b_data + on + n;
		     dentp++, offset += sizeof(struct direntry)) {
#if 0
			printf("rd: dentp %08x prev %08x crnt %08x deName %02x attr %02x\n",
			    dentp, prev, crnt, dentp->deName[0], dentp->deAttributes);
#endif
			d_name = d_name_storage;
			/*
			 * If this is an unused entry, we can stop.
			 */
			if (dentp->deName[0] == SLOT_EMPTY) {
				brelse(bp);
				goto out;
			}
			/*
			 * Skip deleted entries.
			 */
			if (dentp->deName[0] == SLOT_DELETED) {
				chksum = -1;
				continue;
			}

			/*
			 * Handle Win95 long directory entries
			 */
			if (dentp->deAttributes == ATTR_WIN95) {
				if (pmp->pm_flags & MSDOSFSMNT_SHORTNAME)
					continue;
				chksum = win2unixfn((struct winentry *)dentp,
					d_name, &d_namlen, chksum,
					pmp->pm_flags & MSDOSFSMNT_U2WTABLE,
					pmp->pm_u2w);
				continue;
			}

			/*
			 * Skip volume labels
			 */
			if (dentp->deAttributes & ATTR_VOLUME) {
				chksum = -1;
				continue;
			}
			/*
			 * This computation of d_ino must match
			 * the computation of va_fileid in
			 * msdosfs_getattr.
			 */
			if (dentp->deAttributes & ATTR_DIRECTORY) {
				d_ino = getushort(dentp->deStartCluster);
				if (FAT32(pmp))
					d_ino |= getushort(dentp->deHighClust) << 16;
				/* if this is the root directory */
				if (d_ino != MSDOSFSROOT)
					d_ino = cntobn(pmp, d_ino) * dirsperblk;
				else if (FAT32(pmp))
					d_ino = cntobn(pmp, pmp->pm_rootdirblk)
					    * dirsperblk;
				else
					d_ino = 1;
				d_type = DT_DIR;
			} else {
				d_ino = offset / sizeof(struct direntry);
				d_type = DT_REG;
			}
			if (chksum != winChksum(dentp->deName)) {
				d_namlen = dos2unixfn(dentp->deName,
				    (u_char *)d_name,
				    dentp->deLowerCase |
					((pmp->pm_flags & MSDOSFSMNT_SHORTNAME) ?
					(LCASE_BASE | LCASE_EXT) : 0),
				    pmp->pm_flags & MSDOSFSMNT_U2WTABLE,
				    pmp->pm_d2u,
				    pmp->pm_flags & MSDOSFSMNT_ULTABLE,
				    pmp->pm_ul);
			}
			chksum = -1;
			if (vop_write_dirent(&error, uio, d_ino, d_type,
			    d_namlen, d_name)) {
				brelse(bp);
				goto out;
			}
			if (error) {
				brelse(bp);
				goto out;
			}

			if (cookies) {
				*cookies++ = offset + sizeof(struct direntry);
				if (--ncookies <= 0) {
					brelse(bp);
					goto out;
				}
			}
			off = offset + sizeof(struct direntry);
		}
		brelse(bp);
	}
out:
	if (d_name_storage != NULL)
		free(d_name_storage, M_TEMP);

	/* Subtract unused cookies */
	if (ap->a_ncookies)
		*ap->a_ncookies -= ncookies;

	uio->uio_offset = off;

	/*
	 * Set the eofflag (NFS uses it)
	 */
	if (ap->a_eofflag) {
		if (dep->de_FileSize - (offset - bias) <= 0)
			*ap->a_eofflag = 1;
		else
			*ap->a_eofflag = 0;
	}
	return (error);
}

/*
 * vp  - address of vnode file the file
 * bn  - which cluster we are interested in mapping to a filesystem block number.
 * vpp - returns the vnode for the block special file holding the filesystem
 *	 containing the file of interest
 * bnp - address of where to return the filesystem relative block number
 *
 * msdosfs_bmap(struct vnode *a_vp, daddr_t a_bn, struct vnode **a_vpp,
 *		daddr_t *a_bnp, int *a_runp, int *a_runb)
 */
static int
msdosfs_bmap(struct vop_bmap_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);

	if (ap->a_vpp != NULL)
		*ap->a_vpp = dep->de_devvp;
	if (ap->a_bnp == NULL)
		return (0);
	if (ap->a_runp) {
		/*
		 * Sequential clusters should be counted here.
		 */
		*ap->a_runp = 0;
	}
	if (ap->a_runb) {
		*ap->a_runb = 0;
	}
	return (pcbmap(dep, ap->a_bn, ap->a_bnp, 0, 0));
}

/*
 * msdosfs_strategy(struct vnode *a_vp, struct buf *a_bp)
 */
static int
msdosfs_strategy(struct vop_strategy_args *ap)
{
	struct buf *bp = ap->a_bp;
	struct denode *dep = VTODE(bp->b_vp);
	struct vnode *vp;
	int error = 0;

	if (bp->b_vp->v_type == VBLK || bp->b_vp->v_type == VCHR)
		panic("msdosfs_strategy: spec");
	/*
	 * If we don't already know the filesystem relative block number
	 * then get it using pcbmap().  If pcbmap() returns the block
	 * number as -1 then we've got a hole in the file.  DOS filesystems
	 * don't allow files with holes, so we shouldn't ever see this.
	 */
	if (bp->b_blkno == bp->b_lblkno) {
		error = pcbmap(dep, bp->b_lblkno, &bp->b_blkno, 0, 0);
		if (error) {
			bp->b_error = error;
			bp->b_flags |= B_ERROR;
			biodone(bp);
			return (error);
		}
		if ((long)bp->b_blkno == -1)
			vfs_bio_clrbuf(bp);
	}
	if (bp->b_blkno == -1) {
		biodone(bp);
		return (0);
	}
	/*
	 * Read/write the block from/to the disk that contains the desired
	 * file block.
	 */
	vp = dep->de_devvp;
	bp->b_dev = vp->v_rdev;
	VOP_STRATEGY(vp, bp);
	return (0);
}

/*
 * msdosfs_print(struct vnode *vp)
 */
static int
msdosfs_print(struct vop_print_args *ap)
{
	struct denode *dep = VTODE(ap->a_vp);

	printf(
	    "tag VT_MSDOSFS, startcluster %lu, dircluster %lu, diroffset %lu ",
	       dep->de_StartCluster, dep->de_dirclust, dep->de_diroffset);
	printf(" dev %d, %d", major(dep->de_dev), minor(dep->de_dev));
	lockmgr_printinfo(&ap->a_vp->v_lock);
	printf("\n");
	return (0);
}

/*
 * msdosfs_pathconf(struct vnode *a_vp, int a_name, int *a_retval)
 */
static int
msdosfs_pathconf(struct vop_pathconf_args *ap)
{
	struct msdosfsmount *pmp = VTODE(ap->a_vp)->de_pmp;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 1;
		return (0);
	case _PC_NAME_MAX:
		*ap->a_retval = pmp->pm_flags & MSDOSFSMNT_LONGNAME ? WIN_MAXLEN : 12;
		return (0);
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return (0);
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return (0);
	case _PC_NO_TRUNC:
		*ap->a_retval = 0;
		return (0);
	default:
		return (EINVAL);
	}
	/* NOTREACHED */
}

/*
 * get page routine
 *
 * XXX By default, wimp out... note that a_offset is ignored (and always
 * XXX has been).
 */
int
msdosfs_getpages(struct vop_getpages_args *ap)
{
	return vnode_pager_generic_getpages(ap->a_vp, ap->a_m, ap->a_count,
		ap->a_reqpage);
}

/*
 * put page routine
 *
 * XXX By default, wimp out... note that a_offset is ignored (and always
 * XXX has been).
 */
int
msdosfs_putpages(struct vop_putpages_args *ap)
{
	return vnode_pager_generic_putpages(ap->a_vp, ap->a_m, ap->a_count,
		ap->a_sync, ap->a_rtvals);
}

/* Global vfs data structures for msdosfs */
struct vnodeopv_entry_desc msdosfs_vnodeop_entries[] = {
	{ &vop_default_desc,		vop_defaultop },
	{ &vop_access_desc,		(vnodeopv_entry_t) msdosfs_access },
	{ &vop_bmap_desc,		(vnodeopv_entry_t) msdosfs_bmap },
	{ &vop_old_lookup_desc,		(vnodeopv_entry_t) msdosfs_lookup },
	{ &vop_close_desc,		(vnodeopv_entry_t) msdosfs_close },
	{ &vop_old_create_desc,		(vnodeopv_entry_t) msdosfs_create },
	{ &vop_fsync_desc,		(vnodeopv_entry_t) msdosfs_fsync },
	{ &vop_getattr_desc,		(vnodeopv_entry_t) msdosfs_getattr },
	{ &vop_inactive_desc,		(vnodeopv_entry_t) msdosfs_inactive },
	{ &vop_islocked_desc,		(vnodeopv_entry_t) vop_stdislocked },
	{ &vop_old_link_desc,		(vnodeopv_entry_t) msdosfs_link },
	{ &vop_lock_desc,		(vnodeopv_entry_t) vop_stdlock },
	{ &vop_old_mkdir_desc,		(vnodeopv_entry_t) msdosfs_mkdir },
	{ &vop_old_mknod_desc,		(vnodeopv_entry_t) msdosfs_mknod },
	{ &vop_pathconf_desc,		(vnodeopv_entry_t) msdosfs_pathconf },
	{ &vop_print_desc,		(vnodeopv_entry_t) msdosfs_print },
	{ &vop_read_desc,		(vnodeopv_entry_t) msdosfs_read },
	{ &vop_readdir_desc,		(vnodeopv_entry_t) msdosfs_readdir },
	{ &vop_reclaim_desc,		(vnodeopv_entry_t) msdosfs_reclaim },
	{ &vop_old_remove_desc,		(vnodeopv_entry_t) msdosfs_remove },
	{ &vop_old_rename_desc,		(vnodeopv_entry_t) msdosfs_rename },
	{ &vop_old_rmdir_desc,		(vnodeopv_entry_t) msdosfs_rmdir },
	{ &vop_setattr_desc,		(vnodeopv_entry_t) msdosfs_setattr },
	{ &vop_strategy_desc,		(vnodeopv_entry_t) msdosfs_strategy },
	{ &vop_old_symlink_desc,	(vnodeopv_entry_t) msdosfs_symlink },
	{ &vop_unlock_desc,		(vnodeopv_entry_t) vop_stdunlock },
	{ &vop_write_desc,		(vnodeopv_entry_t) msdosfs_write },
	{ &vop_getpages_desc,		(vnodeopv_entry_t) msdosfs_getpages },
	{ &vop_putpages_desc,		(vnodeopv_entry_t) msdosfs_putpages },
	{ NULL, NULL }
};
