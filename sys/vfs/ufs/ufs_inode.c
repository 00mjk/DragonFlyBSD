/*
 * Copyright (c) 1991, 1993, 1995
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
 *	@(#)ufs_inode.c	8.9 (Berkeley) 5/14/95
 * $FreeBSD: src/sys/ufs/ufs/ufs_inode.c,v 1.25.2.3 2002/07/05 22:42:31 dillon Exp $
 * $DragonFly: src/sys/vfs/ufs/ufs_inode.c,v 1.11 2004/10/12 19:21:12 dillon Exp $
 */

#include "opt_quota.h"
#include "opt_ufs.h"

#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/malloc.h>

#include "quota.h"
#include "inode.h"
#include "ufsmount.h"
#include "ufs_extern.h"
#ifdef UFS_DIRHASH
#include "dir.h"
#include "dirhash.h"
#endif

int	prtactive = 0;		/* 1 => print out reclaim of active vnodes */

/*
 * Last reference to an inode.  If necessary, write or delete it.
 *
 * ufs_inactive(struct vnode *a_vp, struct thread *a_td)
 */
int
ufs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct thread *td = ap->a_td;
	int mode, error = 0;

	if (prtactive && vp->v_usecount != 0)
		vprint("ufs_inactive: pushing active", vp);

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip == NULL || ip->i_mode == 0)
		goto out;
	if (ip->i_nlink <= 0) {
#ifdef QUOTA
		if (!getinoquota(ip))
			(void)chkiq(ip, -1, NOCRED, FORCE);
#endif
		error = UFS_TRUNCATE(vp, (off_t)0, 0, NOCRED, td);
		ip->i_rdev = 0;
		mode = ip->i_mode;
		ip->i_mode = 0;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		UFS_VFREE(vp, ip->i_number, mode);
	}
	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE))
		UFS_UPDATE(vp, 0);
out:
	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (ip == NULL || ip->i_mode == 0)
		vrecycle(vp, td);
	return (error);
}

/*
 * Reclaim an inode so that it can be used for other purposes.
 *
 * ufs_reclaim(struct vnode *a_vp, struct thread *a_td)
 */
int
ufs_reclaim(struct vop_reclaim_args *ap)
{
	struct inode *ip;
	struct vnode *vp = ap->a_vp;
#ifdef QUOTA
	int i;
#endif

	if (prtactive && vp->v_usecount != 0)
		vprint("ufs_reclaim: pushing active", vp);
	ip = VTOI(vp);
	if (ip && (ip->i_flag & IN_LAZYMOD)) {
		ip->i_flag |= IN_MODIFIED;
		UFS_UPDATE(vp, 0);
	}
	/*
	 * Remove the inode from its hash chain and purge namecache
	 * data associated with the vnode.
	 */
	vp->v_data = NULL;
	if (ip) {
		ufs_ihashrem(ip);
		if (ip->i_devvp) {
			vrele(ip->i_devvp);
			ip->i_devvp = 0;
		}
#ifdef QUOTA
		for (i = 0; i < MAXQUOTAS; i++) {
			if (ip->i_dquot[i] != NODQUOT) {
				dqrele(vp, ip->i_dquot[i]);
				ip->i_dquot[i] = NODQUOT;
			}
		}
#endif
#ifdef UFS_DIRHASH
		if (ip->i_dirhash != NULL)
			ufsdirhash_free(ip);
#endif
		free(ip, VFSTOUFS(vp->v_mount)->um_malloctype);
	}
	return (0);
}
