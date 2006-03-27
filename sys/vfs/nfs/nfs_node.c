/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
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
 *	@(#)nfs_node.c	8.6 (Berkeley) 5/22/95
 * $FreeBSD: src/sys/nfs/nfs_node.c,v 1.36.2.3 2002/01/05 22:25:04 dillon Exp $
 * $DragonFly: src/sys/vfs/nfs/nfs_node.c,v 1.22 2006/03/27 16:18:39 dillon Exp $
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/fnv_hash.h>

#include <vm/vm_zone.h>

#include "rpcv2.h"
#include "nfsproto.h"
#include "nfs.h"
#include "nfsmount.h"
#include "nfsnode.h"

static vm_zone_t nfsnode_zone;
static LIST_HEAD(nfsnodehashhead, nfsnode) *nfsnodehashtbl;
static u_long nfsnodehash;

#define TRUE	1
#define	FALSE	0

/*
 * Initialize hash links for nfsnodes
 * and build nfsnode free list.
 */
void
nfs_nhinit(void)
{
	nfsnode_zone = zinit("NFSNODE", sizeof(struct nfsnode), 0, 0, 1);
	nfsnodehashtbl = hashinit(desiredvnodes, M_NFSHASH, &nfsnodehash);
}

/*
 * Look up a vnode/nfsnode by file handle.
 * Callers must check for mount points!!
 * In all cases, a pointer to a
 * nfsnode structure is returned.
 */
static int nfs_node_hash_lock;

int
nfs_nget(struct mount *mntp, nfsfh_t *fhp, int fhsize, struct nfsnode **npp)
{
	struct thread *td = curthread;	/* XXX */
	struct nfsnode *np, *np2;
	struct nfsnodehashhead *nhpp;
	struct vnode *vp;
	struct vnode *nvp;
	int error;
	int lkflags;
	struct nfsmount *nmp;

	/*
	 * Calculate nfs mount point and figure out whether the rslock should
	 * be interruptable or not.
	 */
	nmp = VFSTONFS(mntp);
	if (nmp->nm_flag & NFSMNT_INT)
		lkflags = LK_PCATCH;
	else
		lkflags = 0;

retry:
	nhpp = NFSNOHASH(fnv_32_buf(fhp->fh_bytes, fhsize, FNV1_32_INIT));
loop:
	for (np = nhpp->lh_first; np; np = np->n_hash.le_next) {
		if (mntp != NFSTOV(np)->v_mount || np->n_fhsize != fhsize ||
		    bcmp((caddr_t)fhp, (caddr_t)np->n_fhp, fhsize)) {
			continue;
		}
		vp = NFSTOV(np);
		if (vget(vp, LK_EXCLUSIVE, td))
			goto loop;
		for (np = nhpp->lh_first; np; np = np->n_hash.le_next) {
			if (mntp == NFSTOV(np)->v_mount &&
			    np->n_fhsize == fhsize &&
			    bcmp((caddr_t)fhp, (caddr_t)np->n_fhp, fhsize) == 0
			) {
				break;
			}
		}
		if (np == NULL || NFSTOV(np) != vp) {
			vput(vp);
			goto loop;
		}
		*npp = np;
		return(0);
	}
	/*
	 * Obtain a lock to prevent a race condition if the getnewvnode()
	 * or MALLOC() below happens to block.
	 */
	if (nfs_node_hash_lock) {
		while (nfs_node_hash_lock) {
			nfs_node_hash_lock = -1;
			tsleep(&nfs_node_hash_lock, 0, "nfsngt", 0);
		}
		goto loop;
	}
	nfs_node_hash_lock = 1;

	/*
	 * Allocate before getnewvnode since doing so afterward
	 * might cause a bogus v_data pointer to get dereferenced
	 * elsewhere if zalloc should block.
	 */
	np = zalloc(nfsnode_zone);
		
	error = getnewvnode(VT_NFS, mntp, &nvp, 0, LK_NOPAUSE);
	if (error) {
		if (nfs_node_hash_lock < 0)
			wakeup(&nfs_node_hash_lock);
		nfs_node_hash_lock = 0;
		*npp = 0;
		zfree(nfsnode_zone, np);
		return (error);
	}
	vp = nvp;
	bzero((caddr_t)np, sizeof *np);
	np->n_vnode = vp;
	vp->v_data = np;

	/*
	 * Insert the nfsnode in the hash queue for its new file handle
	 */
	for (np2 = nhpp->lh_first; np2 != 0; np2 = np2->n_hash.le_next) {
		if (mntp != NFSTOV(np2)->v_mount || np2->n_fhsize != fhsize ||
		    bcmp((caddr_t)fhp, (caddr_t)np2->n_fhp, fhsize))
			continue;
		vx_put(vp);
		if (nfs_node_hash_lock < 0)
			wakeup(&nfs_node_hash_lock);
		nfs_node_hash_lock = 0;
		zfree(nfsnode_zone, np);
		goto retry;
	}
	LIST_INSERT_HEAD(nhpp, np, n_hash);
	if (fhsize > NFS_SMALLFH) {
		MALLOC(np->n_fhp, nfsfh_t *, fhsize, M_NFSBIGFH, M_WAITOK);
	} else
		np->n_fhp = &np->n_fh;
	bcopy((caddr_t)fhp, (caddr_t)np->n_fhp, fhsize);
	np->n_fhsize = fhsize;
	lockinit(&np->n_rslock, "nfrslk", 0, LK_NOPAUSE | lkflags);

	/*
	 * nvp is locked & refd so effectively so is np.
	 */
	*npp = np;

	if (nfs_node_hash_lock < 0)
		wakeup(&nfs_node_hash_lock);
	nfs_node_hash_lock = 0;

	return (0);
}

/*
 * nfs_inactive(struct vnode *a_vp, struct thread *a_td)
 *
 * NOTE: the passed vnode is locked but not referenced.  On return the
 * vnode must be unlocked and not referenced.
 */
int
nfs_inactive(struct vop_inactive_args *ap)
{
	struct nfsnode *np;
	struct sillyrename *sp;

	np = VTONFS(ap->a_vp);
	if (prtactive && ap->a_vp->v_usecount != 0)
		vprint("nfs_inactive: pushing active", ap->a_vp);
	if (ap->a_vp->v_type != VDIR) {
		sp = np->n_sillyrename;
		np->n_sillyrename = NULL;
	} else {
		sp = NULL;
	}
	if (sp) {
		/*
		 * We need a reference to keep the vnode from being
		 * recycled by getnewvnode while we do the I/O
		 * associated with discarding the buffers.  The vnode
		 * is already locked.
		 */
		nfs_vinvalbuf(ap->a_vp, 0, ap->a_td, 1);

		/*
		 * Either we have the only ref or we were vgone()'d via
		 * revoke and might have more.
		 */
		KKASSERT(ap->a_vp->v_usecount == 1 || 
			(ap->a_vp->v_flag & VRECLAIMED));

		/*
		 * Remove the silly file that was rename'd earlier
		 */
		nfs_removeit(sp);
		crfree(sp->s_cred);
		vrele(sp->s_dvp);
		FREE((caddr_t)sp, M_NFSREQ);
	}

	np->n_flag &= ~(NWRITEERR | NACC | NUPD | NCHG | NLOCKED | NWANTED);

	return (0);
}

/*
 * Reclaim an nfsnode so that it can be used for other purposes.
 *
 * nfs_reclaim(struct vnode *a_vp)
 */
int
nfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct nfsnode *np = VTONFS(vp);
	struct nfsdmap *dp, *dp2;

	if (prtactive && vp->v_usecount != 0)
		vprint("nfs_reclaim: pushing active", vp);

	if (np->n_hash.le_prev != NULL)
		LIST_REMOVE(np, n_hash);

	/*
	 * Free up any directory cookie structures and
	 * large file handle structures that might be associated with
	 * this nfs node.
	 */
	if (vp->v_type == VDIR) {
		dp = np->n_cookies.lh_first;
		while (dp) {
			dp2 = dp;
			dp = dp->ndm_list.le_next;
			FREE((caddr_t)dp2, M_NFSDIROFF);
		}
	}
	if (np->n_fhsize > NFS_SMALLFH) {
		FREE((caddr_t)np->n_fhp, M_NFSBIGFH);
	}
	if (np->n_rucred) {
		crfree(np->n_rucred);
		np->n_rucred = NULL;
	}
	if (np->n_wucred) {
		crfree(np->n_wucred);
		np->n_wucred = NULL;
	}

	vp->v_data = NULL;
	zfree(nfsnode_zone, np);
	return (0);
}

#if 0
/*
 * Lock an nfsnode
 *
 * nfs_lock(struct vnode *a_vp)
 */
int
nfs_lock(struct vop_lock_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/*
	 * Ugh, another place where interruptible mounts will get hung.
	 * If you make this sleep interruptible, then you have to fix all
	 * the VOP_LOCK() calls to expect interruptibility.
	 */
	while (vp->v_flag & VXLOCK) {
		vp->v_flag |= VXWANT;
		(void) tsleep((caddr_t)vp, 0, "nfslck", 0);
	}
	if (vp->v_tag == VT_NON)
		return (ENOENT);

#if 0
	/*
	 * Only lock regular files.  If a server crashed while we were
	 * holding a directory lock, we could easily end up sleeping
	 * until the server rebooted while holding a lock on the root.
	 * Locks are only needed for protecting critical sections in
	 * VMIO at the moment.
	 * New vnodes will have type VNON but they should be locked
	 * since they may become VREG.  This is checked in loadattrcache
	 * and unwanted locks are released there.
	 */
	if (vp->v_type == VREG || vp->v_type == VNON) {
		while (np->n_flag & NLOCKED) {
			np->n_flag |= NWANTED;
			(void) tsleep((caddr_t) np, 0, "nfslck2", 0);
			/*
			 * If the vnode has transmuted into a VDIR while we
			 * were asleep, then skip the lock.
			 */
			if (vp->v_type != VREG && vp->v_type != VNON)
				return (0);
		}
		np->n_flag |= NLOCKED;
	}
#endif

	return (0);
}

/*
 * Unlock an nfsnode
 *
 * nfs_unlock(struct vnode *a_vp)
 */
int
nfs_unlock(struct vop_unlock_args *ap)
{
#if 0
	struct vnode* vp = ap->a_vp;
        struct nfsnode* np = VTONFS(vp);

	if (vp->v_type == VREG || vp->v_type == VNON) {
		if (!(np->n_flag & NLOCKED))
			panic("nfs_unlock: nfsnode not locked");
		np->n_flag &= ~NLOCKED;
		if (np->n_flag & NWANTED) {
			np->n_flag &= ~NWANTED;
			wakeup((caddr_t) np);
		}
	}
#endif

	return (0);
}

/*
 * Check for a locked nfsnode
 *
 * nfs_islocked(struct vnode *a_vp, struct thread *a_td)
 */
int
nfs_islocked(struct vop_islocked_args *ap)
{
	return VTONFS(ap->a_vp)->n_flag & NLOCKED ? 1 : 0;
}
#endif

