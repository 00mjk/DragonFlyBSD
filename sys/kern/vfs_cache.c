/*
 * Copyright (c) 2003,2004 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * Copyright (c) 1989, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Poul-Henning Kamp of the FreeBSD Project.
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
 *	@(#)vfs_cache.c	8.5 (Berkeley) 3/22/95
 * $FreeBSD: src/sys/kern/vfs_cache.c,v 1.42.2.6 2001/10/05 20:07:03 dillon Exp $
 * $DragonFly: src/sys/kern/vfs_cache.c,v 1.75 2006/08/19 17:27:23 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/sysproto.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/nlookup.h>
#include <sys/filedesc.h>
#include <sys/fnv_hash.h>
#include <sys/globaldata.h>
#include <sys/kern_syscall.h>
#include <sys/dirent.h>
#include <ddb/ddb.h>

/*
 * Random lookups in the cache are accomplished with a hash table using
 * a hash key of (nc_src_vp, name).
 *
 * Negative entries may exist and correspond to structures where nc_vp
 * is NULL.  In a negative entry, NCF_WHITEOUT will be set if the entry
 * corresponds to a whited-out directory entry (verses simply not finding the
 * entry at all).
 *
 * Upon reaching the last segment of a path, if the reference is for DELETE,
 * or NOCACHE is set (rewrite), and the name is located in the cache, it
 * will be dropped.
 */

/*
 * Structures associated with name cacheing.
 */
#define NCHHASH(hash)	(&nchashtbl[(hash) & nchash])
#define MINNEG		1024

MALLOC_DEFINE(M_VFSCACHE, "vfscache", "VFS name cache entries");

static LIST_HEAD(nchashhead, namecache) *nchashtbl;	/* Hash Table */
static struct namecache_list	ncneglist;		/* instead of vnode */

/*
 * ncvp_debug - debug cache_fromvp().  This is used by the NFS server
 * to create the namecache infrastructure leading to a dangling vnode.
 *
 * 0	Only errors are reported
 * 1	Successes are reported
 * 2	Successes + the whole directory scan is reported
 * 3	Force the directory scan code run as if the parent vnode did not
 *	have a namecache record, even if it does have one.
 */
static int	ncvp_debug;
SYSCTL_INT(_debug, OID_AUTO, ncvp_debug, CTLFLAG_RW, &ncvp_debug, 0, "");

static u_long	nchash;			/* size of hash table */
SYSCTL_ULONG(_debug, OID_AUTO, nchash, CTLFLAG_RD, &nchash, 0, "");

static u_long	ncnegfactor = 16;	/* ratio of negative entries */
SYSCTL_ULONG(_debug, OID_AUTO, ncnegfactor, CTLFLAG_RW, &ncnegfactor, 0, "");

static int	nclockwarn;		/* warn on locked entries in ticks */
SYSCTL_INT(_debug, OID_AUTO, nclockwarn, CTLFLAG_RW, &nclockwarn, 0, "");

static u_long	numneg;		/* number of cache entries allocated */
SYSCTL_ULONG(_debug, OID_AUTO, numneg, CTLFLAG_RD, &numneg, 0, "");

static u_long	numcache;		/* number of cache entries allocated */
SYSCTL_ULONG(_debug, OID_AUTO, numcache, CTLFLAG_RD, &numcache, 0, "");

static u_long	numunres;		/* number of unresolved entries */
SYSCTL_ULONG(_debug, OID_AUTO, numunres, CTLFLAG_RD, &numunres, 0, "");

SYSCTL_INT(_debug, OID_AUTO, vnsize, CTLFLAG_RD, 0, sizeof(struct vnode), "");
SYSCTL_INT(_debug, OID_AUTO, ncsize, CTLFLAG_RD, 0, sizeof(struct namecache), "");

static int cache_resolve_mp(struct namecache *ncp);
static void cache_rehash(struct namecache *ncp);

/*
 * The new name cache statistics
 */
SYSCTL_NODE(_vfs, OID_AUTO, cache, CTLFLAG_RW, 0, "Name cache statistics");
#define STATNODE(mode, name, var) \
	SYSCTL_ULONG(_vfs_cache, OID_AUTO, name, mode, var, 0, "");
STATNODE(CTLFLAG_RD, numneg, &numneg);
STATNODE(CTLFLAG_RD, numcache, &numcache);
static u_long numcalls; STATNODE(CTLFLAG_RD, numcalls, &numcalls);
static u_long dothits; STATNODE(CTLFLAG_RD, dothits, &dothits);
static u_long dotdothits; STATNODE(CTLFLAG_RD, dotdothits, &dotdothits);
static u_long numchecks; STATNODE(CTLFLAG_RD, numchecks, &numchecks);
static u_long nummiss; STATNODE(CTLFLAG_RD, nummiss, &nummiss);
static u_long nummisszap; STATNODE(CTLFLAG_RD, nummisszap, &nummisszap);
static u_long numposzaps; STATNODE(CTLFLAG_RD, numposzaps, &numposzaps);
static u_long numposhits; STATNODE(CTLFLAG_RD, numposhits, &numposhits);
static u_long numnegzaps; STATNODE(CTLFLAG_RD, numnegzaps, &numnegzaps);
static u_long numneghits; STATNODE(CTLFLAG_RD, numneghits, &numneghits);

struct nchstats nchstats[SMP_MAXCPU];
/*
 * Export VFS cache effectiveness statistics to user-land.
 *
 * The statistics are left for aggregation to user-land so
 * neat things can be achieved, like observing per-CPU cache
 * distribution.
 */
static int
sysctl_nchstats(SYSCTL_HANDLER_ARGS)
{
	struct globaldata *gd;
	int i, error;

	error = 0;
	for (i = 0; i < ncpus; ++i) {
		gd = globaldata_find(i);
		if ((error = SYSCTL_OUT(req, (void *)&(*gd->gd_nchstats),
			sizeof(struct nchstats))))
			break;
	}

	return (error);
}
SYSCTL_PROC(_vfs_cache, OID_AUTO, nchstats, CTLTYPE_OPAQUE|CTLFLAG_RD,
  0, 0, sysctl_nchstats, "S,nchstats", "VFS cache effectiveness statistics");

static void cache_zap(struct namecache *ncp);

/*
 * cache_hold() and cache_drop() prevent the premature deletion of a
 * namecache entry but do not prevent operations (such as zapping) on
 * that namecache entry.
 *
 * This routine may only be called from outside this source module if
 * nc_refs is already at least 1.
 *
 * This is a rare case where callers are allowed to hold a spinlock,
 * so we can't ourselves.
 */
static __inline
struct namecache *
_cache_hold(struct namecache *ncp)
{
	atomic_add_int(&ncp->nc_refs, 1);
	return(ncp);
}

/*
 * When dropping an entry, if only one ref remains and the entry has not
 * been resolved, zap it.  Since the one reference is being dropped the
 * entry had better not be locked.
 */
static __inline
void
_cache_drop(struct namecache *ncp)
{
	KKASSERT(ncp->nc_refs > 0);
	if (ncp->nc_refs == 1 && 
	    (ncp->nc_flag & NCF_UNRESOLVED) && 
	    TAILQ_EMPTY(&ncp->nc_list)
	) {
		KKASSERT(ncp->nc_exlocks == 0);
		cache_lock(ncp);
		cache_zap(ncp);
	} else {
		atomic_subtract_int(&ncp->nc_refs, 1);
	}
}

/*
 * Link a new namecache entry to its parent.  Be careful to avoid races
 * if vhold() blocks in the future.
 */
static void
cache_link_parent(struct namecache *ncp, struct namecache *par)
{
	KKASSERT(ncp->nc_parent == NULL);
	ncp->nc_parent = par;
	if (TAILQ_EMPTY(&par->nc_list)) {
		TAILQ_INSERT_HEAD(&par->nc_list, ncp, nc_entry);
		/*
		 * Any vp associated with an ncp which has children must
		 * be held to prevent it from being recycled.
		 */
		if (par->nc_vp)
			vhold(par->nc_vp);
	} else {
		TAILQ_INSERT_HEAD(&par->nc_list, ncp, nc_entry);
	}
}

/*
 * Remove the parent association from a namecache structure.  If this is
 * the last child of the parent the cache_drop(par) will attempt to
 * recursively zap the parent.
 */
static void
cache_unlink_parent(struct namecache *ncp)
{
	struct namecache *par;

	if ((par = ncp->nc_parent) != NULL) {
		ncp->nc_parent = NULL;
		par = cache_hold(par);
		TAILQ_REMOVE(&par->nc_list, ncp, nc_entry);
		if (par->nc_vp && TAILQ_EMPTY(&par->nc_list))
			vdrop(par->nc_vp);
		cache_drop(par);
	}
}

/*
 * Allocate a new namecache structure.  Most of the code does not require
 * zero-termination of the string but it makes vop_compat_ncreate() easier.
 */
static struct namecache *
cache_alloc(int nlen)
{
	struct namecache *ncp;

	ncp = malloc(sizeof(*ncp), M_VFSCACHE, M_WAITOK|M_ZERO);
	if (nlen)
		ncp->nc_name = malloc(nlen + 1, M_VFSCACHE, M_WAITOK);
	ncp->nc_nlen = nlen;
	ncp->nc_flag = NCF_UNRESOLVED;
	ncp->nc_error = ENOTCONN;	/* needs to be resolved */
	ncp->nc_refs = 1;

	/*
	 * Construct a fake FSMID based on the time of day and a 32 bit
	 * roller for uniqueness.  This is used to generate a useful
	 * FSMID for filesystems which do not support it.
	 */
	ncp->nc_fsmid = cache_getnewfsmid();
	TAILQ_INIT(&ncp->nc_list);
	cache_lock(ncp);
	return(ncp);
}

static void
cache_free(struct namecache *ncp)
{
	KKASSERT(ncp->nc_refs == 1 && ncp->nc_exlocks == 1);
	if (ncp->nc_name)
		free(ncp->nc_name, M_VFSCACHE);
	free(ncp, M_VFSCACHE);
}

/*
 * Ref and deref a namecache structure.
 *
 * Warning: caller may hold an unrelated read spinlock, which means we can't
 * use read spinlocks here.
 */
struct namecache *
cache_hold(struct namecache *ncp)
{
	return(_cache_hold(ncp));
}

void
cache_drop(struct namecache *ncp)
{
	_cache_drop(ncp);
}

/*
 * Namespace locking.  The caller must already hold a reference to the
 * namecache structure in order to lock/unlock it.  This function prevents
 * the namespace from being created or destroyed by accessors other then
 * the lock holder.
 *
 * Note that holding a locked namecache structure prevents other threads
 * from making namespace changes (e.g. deleting or creating), prevents
 * vnode association state changes by other threads, and prevents the
 * namecache entry from being resolved or unresolved by other threads.
 *
 * The lock owner has full authority to associate/disassociate vnodes
 * and resolve/unresolve the locked ncp.
 *
 * WARNING!  Holding a locked ncp will prevent a vnode from being destroyed
 * or recycled, but it does NOT help you if the vnode had already initiated
 * a recyclement.  If this is important, use cache_get() rather then 
 * cache_lock() (and deal with the differences in the way the refs counter
 * is handled).  Or, alternatively, make an unconditional call to 
 * cache_validate() or cache_resolve() after cache_lock() returns.
 */
void
cache_lock(struct namecache *ncp)
{
	thread_t td;
	int didwarn;

	KKASSERT(ncp->nc_refs != 0);
	didwarn = 0;
	td = curthread;

	for (;;) {
		if (ncp->nc_exlocks == 0) {
			ncp->nc_exlocks = 1;
			ncp->nc_locktd = td;
			/* 
			 * The vp associated with a locked ncp must be held
			 * to prevent it from being recycled (which would
			 * cause the ncp to become unresolved).
			 *
			 * WARNING!  If VRECLAIMED is set the vnode could
			 * already be in the middle of a recycle.  Callers
			 * should not assume that nc_vp is usable when
			 * not NULL.  cache_vref() or cache_vget() must be
			 * called.
			 *
			 * XXX loop on race for later MPSAFE work.
			 */
			if (ncp->nc_vp)
				vhold(ncp->nc_vp);
			break;
		}
		if (ncp->nc_locktd == td) {
			++ncp->nc_exlocks;
			break;
		}
		ncp->nc_flag |= NCF_LOCKREQ;
		if (tsleep(ncp, 0, "clock", nclockwarn) == EWOULDBLOCK) {
			if (didwarn)
				continue;
			didwarn = 1;
			printf("[diagnostic] cache_lock: blocked on %p", ncp);
			if ((ncp->nc_flag & NCF_MOUNTPT) && ncp->nc_mount)
			    printf(" [MOUNTFROM %s]\n", ncp->nc_mount->mnt_stat.f_mntfromname);
			else
			    printf(" \"%*.*s\"\n",
				ncp->nc_nlen, ncp->nc_nlen,
				ncp->nc_name);
		}
	}

	if (didwarn == 1) {
		printf("[diagnostic] cache_lock: unblocked %*.*s\n",
			ncp->nc_nlen, ncp->nc_nlen, ncp->nc_name);
	}
}

int
cache_lock_nonblock(struct namecache *ncp)
{
	thread_t td;

	KKASSERT(ncp->nc_refs != 0);
	td = curthread;
	if (ncp->nc_exlocks == 0) {
		ncp->nc_exlocks = 1;
		ncp->nc_locktd = td;
		/* 
		 * The vp associated with a locked ncp must be held
		 * to prevent it from being recycled (which would
		 * cause the ncp to become unresolved).
		 *
		 * WARNING!  If VRECLAIMED is set the vnode could
		 * already be in the middle of a recycle.  Callers
		 * should not assume that nc_vp is usable when
		 * not NULL.  cache_vref() or cache_vget() must be
		 * called.
		 *
		 * XXX loop on race for later MPSAFE work.
		 */
		if (ncp->nc_vp)
			vhold(ncp->nc_vp);
		return(0);
	} else {
		return(EWOULDBLOCK);
	}
}

void
cache_unlock(struct namecache *ncp)
{
	thread_t td = curthread;

	KKASSERT(ncp->nc_refs > 0);
	KKASSERT(ncp->nc_exlocks > 0);
	KKASSERT(ncp->nc_locktd == td);
	if (--ncp->nc_exlocks == 0) {
		if (ncp->nc_vp)
			vdrop(ncp->nc_vp);
		ncp->nc_locktd = NULL;
		if (ncp->nc_flag & NCF_LOCKREQ) {
			ncp->nc_flag &= ~NCF_LOCKREQ;
			wakeup(ncp);
		}
	}
}

/*
 * ref-and-lock, unlock-and-deref functions.
 *
 * This function is primarily used by nlookup.  Even though cache_lock
 * holds the vnode, it is possible that the vnode may have already
 * initiated a recyclement.  We want cache_get() to return a definitively
 * usable vnode or a definitively unresolved ncp.
 */
struct namecache *
cache_get(struct namecache *ncp)
{
	_cache_hold(ncp);
	cache_lock(ncp);
	if (ncp->nc_vp && (ncp->nc_vp->v_flag & VRECLAIMED))
		cache_setunresolved(ncp);
	return(ncp);
}

int
cache_get_nonblock(struct namecache *ncp)
{
	/* XXX MP */
	if (ncp->nc_exlocks == 0 || ncp->nc_locktd == curthread) {
		_cache_hold(ncp);
		cache_lock(ncp);
		if (ncp->nc_vp && (ncp->nc_vp->v_flag & VRECLAIMED))
			cache_setunresolved(ncp);
		return(0);
	}
	return(EWOULDBLOCK);
}

void
cache_put(struct namecache *ncp)
{
	cache_unlock(ncp);
	_cache_drop(ncp);
}

/*
 * Resolve an unresolved ncp by associating a vnode with it.  If the
 * vnode is NULL, a negative cache entry is created.
 *
 * The ncp should be locked on entry and will remain locked on return.
 */
void
cache_setvp(struct namecache *ncp, struct vnode *vp)
{
	KKASSERT(ncp->nc_flag & NCF_UNRESOLVED);
	ncp->nc_vp = vp;
	if (vp != NULL) {
		/*
		 * Any vp associated with an ncp which has children must
		 * be held.  Any vp associated with a locked ncp must be held.
		 */
		if (!TAILQ_EMPTY(&ncp->nc_list))
			vhold(vp);
		TAILQ_INSERT_HEAD(&vp->v_namecache, ncp, nc_vnode);
		if (ncp->nc_exlocks)
			vhold(vp);

		/*
		 * Set auxillary flags
		 */
		switch(vp->v_type) {
		case VDIR:
			ncp->nc_flag |= NCF_ISDIR;
			break;
		case VLNK:
			ncp->nc_flag |= NCF_ISSYMLINK;
			/* XXX cache the contents of the symlink */
			break;
		default:
			break;
		}
		++numcache;
		ncp->nc_error = 0;
	} else {
		TAILQ_INSERT_TAIL(&ncneglist, ncp, nc_vnode);
		++numneg;
		ncp->nc_error = ENOENT;
	}
	ncp->nc_flag &= ~NCF_UNRESOLVED;
}

void
cache_settimeout(struct namecache *ncp, int nticks)
{
	if ((ncp->nc_timeout = ticks + nticks) == 0)
		ncp->nc_timeout = 1;
}

/*
 * Disassociate the vnode or negative-cache association and mark a
 * namecache entry as unresolved again.  Note that the ncp is still
 * left in the hash table and still linked to its parent.
 *
 * The ncp should be locked and refd on entry and will remain locked and refd
 * on return.
 *
 * This routine is normally never called on a directory containing children.
 * However, NFS often does just that in its rename() code as a cop-out to
 * avoid complex namespace operations.  This disconnects a directory vnode
 * from its namecache and can cause the OLDAPI and NEWAPI to get out of
 * sync.
 *
 * NOTE: NCF_FSMID must be cleared so a refurbishment of the ncp, such as
 * in a create, properly propogates flag up the chain.
 */
void
cache_setunresolved(struct namecache *ncp)
{
	struct vnode *vp;

	if ((ncp->nc_flag & NCF_UNRESOLVED) == 0) {
		ncp->nc_flag |= NCF_UNRESOLVED;
		ncp->nc_timeout = 0;
		ncp->nc_error = ENOTCONN;
		++numunres;
		if ((vp = ncp->nc_vp) != NULL) {
			--numcache;
			ncp->nc_vp = NULL;
			TAILQ_REMOVE(&vp->v_namecache, ncp, nc_vnode);

			/*
			 * Any vp associated with an ncp with children is
			 * held by that ncp.  Any vp associated with a locked
			 * ncp is held by that ncp.  These conditions must be
			 * undone when the vp is cleared out from the ncp.
			 */
			if (ncp->nc_flag & NCF_FSMID)
				vupdatefsmid(vp);
			if (!TAILQ_EMPTY(&ncp->nc_list))
				vdrop(vp);
			if (ncp->nc_exlocks)
				vdrop(vp);
		} else {
			TAILQ_REMOVE(&ncneglist, ncp, nc_vnode);
			--numneg;
		}
		ncp->nc_flag &= ~(NCF_WHITEOUT|NCF_ISDIR|NCF_ISSYMLINK|
				  NCF_FSMID);
	}
}

/*
 * Invalidate portions of the namecache topology given a starting entry.
 * The passed ncp is set to an unresolved state and:
 *
 * The passed ncp must be locked.
 *
 * CINV_DESTROY		- Set a flag in the passed ncp entry indicating
 *			  that the physical underlying nodes have been 
 *			  destroyed... as in deleted.  For example, when
 *			  a directory is removed.  This will cause record
 *			  lookups on the name to no longer be able to find
 *			  the record and tells the resolver to return failure
 *			  rather then trying to resolve through the parent.
 *
 *			  The topology itself, including ncp->nc_name,
 *			  remains intact.
 *
 *			  This only applies to the passed ncp, if CINV_CHILDREN
 *			  is specified the children are not flagged.
 *
 * CINV_CHILDREN	- Set all children (recursively) to an unresolved
 *			  state as well.
 *
 *			  Note that this will also have the side effect of
 *			  cleaning out any unreferenced nodes in the topology
 *			  from the leaves up as the recursion backs out.
 *
 * Note that the topology for any referenced nodes remains intact.
 *
 * It is possible for cache_inval() to race a cache_resolve(), meaning that
 * the namecache entry may not actually be invalidated on return if it was
 * revalidated while recursing down into its children.  This code guarentees
 * that the node(s) will go through an invalidation cycle, but does not 
 * guarentee that they will remain in an invalidated state. 
 *
 * Returns non-zero if a revalidation was detected during the invalidation
 * recursion, zero otherwise.  Note that since only the original ncp is
 * locked the revalidation ultimately can only indicate that the original ncp
 * *MIGHT* no have been reresolved.
 */
int
cache_inval(struct namecache *ncp, int flags)
{
	struct namecache *kid;
	struct namecache *nextkid;
	int rcnt = 0;

	KKASSERT(ncp->nc_exlocks);

	cache_setunresolved(ncp);
	if (flags & CINV_DESTROY)
		ncp->nc_flag |= NCF_DESTROYED;

	if ((flags & CINV_CHILDREN) && 
	    (kid = TAILQ_FIRST(&ncp->nc_list)) != NULL
	) {
		cache_hold(kid);
		cache_unlock(ncp);
		while (kid) {
			if ((nextkid = TAILQ_NEXT(kid, nc_entry)) != NULL)
				cache_hold(nextkid);
			if ((kid->nc_flag & NCF_UNRESOLVED) == 0 ||
			    TAILQ_FIRST(&kid->nc_list)
			) {
				cache_lock(kid);
				rcnt += cache_inval(kid, flags & ~CINV_DESTROY);
				cache_unlock(kid);
			}
			cache_drop(kid);
			kid = nextkid;
		}
		cache_lock(ncp);
	}

	/*
	 * Someone could have gotten in there while ncp was unlocked,
	 * retry if so.
	 */
	if ((ncp->nc_flag & NCF_UNRESOLVED) == 0)
		++rcnt;
	return (rcnt);
}

/*
 * Invalidate a vnode's namecache associations.  To avoid races against
 * the resolver we do not invalidate a node which we previously invalidated
 * but which was then re-resolved while we were in the invalidation loop.
 *
 * Returns non-zero if any namecache entries remain after the invalidation
 * loop completed.
 *
 * NOTE: unlike the namecache topology which guarentees that ncp's will not
 * be ripped out of the topology while held, the vnode's v_namecache list
 * has no such restriction.  NCP's can be ripped out of the list at virtually
 * any time if not locked, even if held.
 */
int
cache_inval_vp(struct vnode *vp, int flags)
{
	struct namecache *ncp;
	struct namecache *next;

restart:
	ncp = TAILQ_FIRST(&vp->v_namecache);
	if (ncp)
		cache_hold(ncp);
	while (ncp) {
		/* loop entered with ncp held */
		if ((next = TAILQ_NEXT(ncp, nc_vnode)) != NULL)
			cache_hold(next);
		cache_lock(ncp);
		if (ncp->nc_vp != vp) {
			printf("Warning: cache_inval_vp: race-A detected on "
				"%s\n", ncp->nc_name);
			cache_put(ncp);
			if (next)
				cache_drop(next);
			goto restart;
		}
		cache_inval(ncp, flags);
		cache_put(ncp);		/* also releases reference */
		ncp = next;
		if (ncp && ncp->nc_vp != vp) {
			printf("Warning: cache_inval_vp: race-B detected on "
				"%s\n", ncp->nc_name);
			cache_drop(ncp);
			goto restart;
		}
	}
	return(TAILQ_FIRST(&vp->v_namecache) != NULL);
}

/*
 * The source ncp has been renamed to the target ncp.  Both fncp and tncp
 * must be locked.  Both will be set to unresolved, any children of tncp
 * will be disconnected (the prior contents of the target is assumed to be
 * destroyed by the rename operation, e.g. renaming over an empty directory),
 * and all children of fncp will be moved to tncp.
 *
 * XXX the disconnection could pose a problem, check code paths to make
 * sure any code that blocks can handle the parent being changed out from
 * under it.  Maybe we should lock the children (watch out for deadlocks) ?
 *
 * After we return the caller has the option of calling cache_setvp() if
 * the vnode of the new target ncp is known.
 *
 * Any process CD'd into any of the children will no longer be able to ".."
 * back out.  An rm -rf can cause this situation to occur.
 */
void
cache_rename(struct namecache *fncp, struct namecache *tncp)
{
	struct namecache *scan;
	int didwarn = 0;

	cache_setunresolved(fncp);
	cache_setunresolved(tncp);
	while (cache_inval(tncp, CINV_CHILDREN) != 0) {
		if (didwarn++ % 10 == 0) {
			printf("Warning: cache_rename: race during "
				"rename %s->%s\n",
				fncp->nc_name, tncp->nc_name);
		}
		tsleep(tncp, 0, "mvrace", hz / 10);
		cache_setunresolved(tncp);
	}
	while ((scan = TAILQ_FIRST(&fncp->nc_list)) != NULL) {
		cache_hold(scan);
		cache_unlink_parent(scan);
		cache_link_parent(scan, tncp);
		if (scan->nc_flag & NCF_HASHED)
			cache_rehash(scan);
		cache_drop(scan);
	}
}

/*
 * vget the vnode associated with the namecache entry.  Resolve the namecache
 * entry if necessary and deal with namecache/vp races.  The passed ncp must
 * be referenced and may be locked.  The ncp's ref/locking state is not 
 * effected by this call.
 *
 * lk_type may be LK_SHARED, LK_EXCLUSIVE.  A ref'd, possibly locked
 * (depending on the passed lk_type) will be returned in *vpp with an error
 * of 0, or NULL will be returned in *vpp with a non-0 error code.  The
 * most typical error is ENOENT, meaning that the ncp represents a negative
 * cache hit and there is no vnode to retrieve, but other errors can occur
 * too.
 *
 * The main race we have to deal with are namecache zaps.  The ncp itself
 * will not disappear since it is referenced, and it turns out that the
 * validity of the vp pointer can be checked simply by rechecking the
 * contents of ncp->nc_vp.
 */
int
cache_vget(struct namecache *ncp, struct ucred *cred,
	   int lk_type, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

again:
	vp = NULL;
	if (ncp->nc_flag & NCF_UNRESOLVED) {
		cache_lock(ncp);
		error = cache_resolve(ncp, cred);
		cache_unlock(ncp);
	} else {
		error = 0;
	}
	if (error == 0 && (vp = ncp->nc_vp) != NULL) {
		/*
		 * Accessing the vnode from the namecache is a bit 
		 * dangerous.  Because there are no refs on the vnode, it
		 * could be in the middle of a reclaim.
		 */
		if (vp->v_flag & VRECLAIMED) {
			printf("Warning: vnode reclaim race detected in cache_vget on %p (%s)\n", vp, ncp->nc_name);
			cache_lock(ncp);
			cache_setunresolved(ncp);
			cache_unlock(ncp);
			goto again;
		}
		error = vget(vp, lk_type);
		if (error) {
			if (vp != ncp->nc_vp)
				goto again;
			vp = NULL;
		} else if (vp != ncp->nc_vp) {
			vput(vp);
			goto again;
		} else if (vp->v_flag & VRECLAIMED) {
			panic("vget succeeded on a VRECLAIMED node! vp %p", vp);
		}
	}
	if (error == 0 && vp == NULL)
		error = ENOENT;
	*vpp = vp;
	return(error);
}

int
cache_vref(struct namecache *ncp, struct ucred *cred, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

again:
	vp = NULL;
	if (ncp->nc_flag & NCF_UNRESOLVED) {
		cache_lock(ncp);
		error = cache_resolve(ncp, cred);
		cache_unlock(ncp);
	} else {
		error = 0;
	}
	if (error == 0 && (vp = ncp->nc_vp) != NULL) {
		/*
		 * Since we did not obtain any locks, a cache zap 
		 * race can occur here if the vnode is in the middle
		 * of being reclaimed and has not yet been able to
		 * clean out its cache node.  If that case occurs,
		 * we must lock and unresolve the cache, then loop
		 * to retry.
		 */
		if (vp->v_flag & VRECLAIMED) {
			printf("Warning: vnode reclaim race detected on cache_vref %p (%s)\n", vp, ncp->nc_name);
			cache_lock(ncp);
			cache_setunresolved(ncp);
			cache_unlock(ncp);
			goto again;
		}
		vref_initial(vp, 1);
	}
	if (error == 0 && vp == NULL)
		error = ENOENT;
	*vpp = vp;
	return(error);
}

/*
 * Recursively set the FSMID update flag for namecache nodes leading
 * to root.  This will cause the next getattr or reclaim to increment the
 * fsmid and mark the inode for lazy updating.
 *
 * Stop recursing when we hit a node whos NCF_FSMID flag is already set.
 * This makes FSMIDs work in an Einsteinian fashion - where the observation
 * effects the result.  In this case a program monitoring a higher level
 * node will have detected some prior change and started its scan (clearing
 * NCF_FSMID in higher level nodes), but since it has not yet observed the
 * node where we find NCF_FSMID still set, we can safely make the related
 * modification without interfering with the theorized program.
 *
 * This also means that FSMIDs cannot represent time-domain quantities
 * in a hierarchical sense.  But the main reason for doing it this way
 * is to reduce the amount of recursion that occurs in the critical path
 * when e.g. a program is writing to a file that sits deep in a directory
 * hierarchy.
 */
void
cache_update_fsmid(struct namecache *ncp)
{
	struct vnode *vp;
	struct namecache *scan;

	/*
	 * Warning: even if we get a non-NULL vp it could still be in the
	 * middle of a recyclement.  Don't do anything fancy, just set
	 * NCF_FSMID.
	 */
	if ((vp = ncp->nc_vp) != NULL) {
		TAILQ_FOREACH(ncp, &vp->v_namecache, nc_vnode) {
			for (scan = ncp; scan; scan = scan->nc_parent) {
				if (scan->nc_flag & NCF_FSMID)
					break;
				scan->nc_flag |= NCF_FSMID;
			}
		}
	} else {
		while (ncp && (ncp->nc_flag & NCF_FSMID) == 0) {
			ncp->nc_flag |= NCF_FSMID;
			ncp = ncp->nc_parent;
		}
	}
}

void
cache_update_fsmid_vp(struct vnode *vp)
{
	struct namecache *ncp;
	struct namecache *scan;

	TAILQ_FOREACH(ncp, &vp->v_namecache, nc_vnode) {
		for (scan = ncp; scan; scan = scan->nc_parent) {
			if (scan->nc_flag & NCF_FSMID)
				break;
			scan->nc_flag |= NCF_FSMID;
		}
	}
}

/*
 * If getattr is called on a vnode (e.g. a stat call), the filesystem
 * may call this routine to determine if the namecache has the hierarchical
 * change flag set, requiring the fsmid to be updated.
 *
 * Since 0 indicates no support, make sure the filesystem fsmid is at least
 * 1.
 */
int
cache_check_fsmid_vp(struct vnode *vp, int64_t *fsmid)
{
	struct namecache *ncp;
	int changed = 0;

	TAILQ_FOREACH(ncp, &vp->v_namecache, nc_vnode) {
		if (ncp->nc_flag & NCF_FSMID) {
			ncp->nc_flag &= ~NCF_FSMID;
			changed = 1;
		}
	}
	if (*fsmid == 0)
		++*fsmid;
	if (changed)
		++*fsmid;
	return(changed);
}

/*
 * Obtain the FSMID for a vnode for filesystems which do not support
 * a built-in FSMID.
 */
int64_t
cache_sync_fsmid_vp(struct vnode *vp)
{
	struct namecache *ncp;

	if ((ncp = TAILQ_FIRST(&vp->v_namecache)) != NULL) {
		if (ncp->nc_flag & NCF_FSMID) {
			ncp->nc_flag &= ~NCF_FSMID;
			++ncp->nc_fsmid;
		}
		return(ncp->nc_fsmid);
	}
	return(VNOVAL);
}

/*
 * Convert a directory vnode to a namecache record without any other 
 * knowledge of the topology.  This ONLY works with directory vnodes and
 * is ONLY used by the NFS server.  dvp must be refd but unlocked, and the
 * returned ncp (if not NULL) will be held and unlocked.
 *
 * If 'makeit' is 0 and dvp has no existing namecache record, NULL is returned.
 * If 'makeit' is 1 we attempt to track-down and create the namecache topology
 * for dvp.  This will fail only if the directory has been deleted out from
 * under the caller.  
 *
 * Callers must always check for a NULL return no matter the value of 'makeit'.
 *
 * To avoid underflowing the kernel stack each recursive call increments
 * the makeit variable.
 */

static int cache_inefficient_scan(struct namecache *ncp, struct ucred *cred,
				  struct vnode *dvp);
static int cache_fromdvp_try(struct vnode *dvp, struct ucred *cred, 
				  struct vnode **saved_dvp);

struct namecache *
cache_fromdvp(struct vnode *dvp, struct ucred *cred, int makeit)
{
	struct namecache *ncp;
	struct vnode *saved_dvp;
	struct vnode *pvp;
	int error;

	ncp = NULL;
	saved_dvp = NULL;

	/*
	 * Temporary debugging code to force the directory scanning code
	 * to be exercised.
	 */
	if (ncvp_debug >= 3 && makeit && TAILQ_FIRST(&dvp->v_namecache)) {
		ncp = TAILQ_FIRST(&dvp->v_namecache);
		printf("cache_fromdvp: forcing %s\n", ncp->nc_name);
		goto force;
	}

	/*
	 * Loop until resolution, inside code will break out on error.
	 */
	while ((ncp = TAILQ_FIRST(&dvp->v_namecache)) == NULL && makeit) {
force:
		/*
		 * If dvp is the root of its filesystem it should already
		 * have a namecache pointer associated with it as a side 
		 * effect of the mount, but it may have been disassociated.
		 */
		if (dvp->v_flag & VROOT) {
			ncp = cache_get(dvp->v_mount->mnt_ncp);
			error = cache_resolve_mp(ncp);
			cache_put(ncp);
			if (ncvp_debug) {
				printf("cache_fromdvp: resolve root of mount %p error %d", 
					dvp->v_mount, error);
			}
			if (error) {
				if (ncvp_debug)
					printf(" failed\n");
				ncp = NULL;
				break;
			}
			if (ncvp_debug)
				printf(" succeeded\n");
			continue;
		}

		/*
		 * If we are recursed too deeply resort to an O(n^2)
		 * algorithm to resolve the namecache topology.  The
		 * resolved pvp is left referenced in saved_dvp to
		 * prevent the tree from being destroyed while we loop.
		 */
		if (makeit > 20) {
			error = cache_fromdvp_try(dvp, cred, &saved_dvp);
			if (error) {
				printf("lookupdotdot(longpath) failed %d "
				       "dvp %p\n", error, dvp);
				break;
			}
			continue;
		}

		/*
		 * Get the parent directory and resolve its ncp.
		 */
		error = vop_nlookupdotdot(*dvp->v_ops, dvp, &pvp, cred);
		if (error) {
			printf("lookupdotdot failed %d dvp %p\n", error, dvp);
			break;
		}
		vn_unlock(pvp);

		/*
		 * Reuse makeit as a recursion depth counter.
		 */
		ncp = cache_fromdvp(pvp, cred, makeit + 1);
		vrele(pvp);
		if (ncp == NULL)
			break;

		/*
		 * Do an inefficient scan of pvp (embodied by ncp) to look
		 * for dvp.  This will create a namecache record for dvp on
		 * success.  We loop up to recheck on success.
		 *
		 * ncp and dvp are both held but not locked.
		 */
		error = cache_inefficient_scan(ncp, cred, dvp);
		cache_drop(ncp);
		if (error) {
			printf("cache_fromdvp: scan %p (%s) failed on dvp=%p\n",
				pvp, ncp->nc_name, dvp);
			ncp = NULL;
			break;
		}
		if (ncvp_debug) {
			printf("cache_fromdvp: scan %p (%s) succeeded\n",
				pvp, ncp->nc_name);
		}
	}
	if (ncp)
		cache_hold(ncp);
	if (saved_dvp)
		vrele(saved_dvp);
	return (ncp);
}

/*
 * Go up the chain of parent directories until we find something
 * we can resolve into the namecache.  This is very inefficient.
 */
static
int
cache_fromdvp_try(struct vnode *dvp, struct ucred *cred,
		  struct vnode **saved_dvp)
{
	struct namecache *ncp;
	struct vnode *pvp;
	int error;
	static time_t last_fromdvp_report;

	/*
	 * Loop getting the parent directory vnode until we get something we
	 * can resolve in the namecache.
	 */
	vref(dvp);
	for (;;) {
		error = vop_nlookupdotdot(*dvp->v_ops, dvp, &pvp, cred);
		if (error) {
			vrele(dvp);
			return (error);
		}
		vn_unlock(pvp);
		if ((ncp = TAILQ_FIRST(&pvp->v_namecache)) != NULL) {
			cache_hold(ncp);
			vrele(pvp);
			break;
		}
		if (pvp->v_flag & VROOT) {
			ncp = cache_get(pvp->v_mount->mnt_ncp);
			error = cache_resolve_mp(ncp);
			cache_unlock(ncp);
			vrele(pvp);
			if (error) {
				cache_drop(ncp);
				vrele(dvp);
				return (error);
			}
			break;
		}
		vrele(dvp);
		dvp = pvp;
	}
	if (last_fromdvp_report != time_second) {
		last_fromdvp_report = time_second;
		printf("Warning: extremely inefficient path resolution on %s\n",
			ncp->nc_name);
	}
	error = cache_inefficient_scan(ncp, cred, dvp);

	/*
	 * Hopefully dvp now has a namecache record associated with it.
	 * Leave it referenced to prevent the kernel from recycling the
	 * vnode.  Otherwise extremely long directory paths could result
	 * in endless recycling.
	 */
	if (*saved_dvp)
	    vrele(*saved_dvp);
	*saved_dvp = dvp;
	return (error);
}


/*
 * Do an inefficient scan of the directory represented by ncp looking for
 * the directory vnode dvp.  ncp must be held but not locked on entry and
 * will be held on return.  dvp must be refd but not locked on entry and
 * will remain refd on return.
 *
 * Why do this at all?  Well, due to its stateless nature the NFS server
 * converts file handles directly to vnodes without necessarily going through
 * the namecache ops that would otherwise create the namecache topology
 * leading to the vnode.  We could either (1) Change the namecache algorithms
 * to allow disconnect namecache records that are re-merged opportunistically,
 * or (2) Make the NFS server backtrack and scan to recover a connected
 * namecache topology in order to then be able to issue new API lookups.
 *
 * It turns out that (1) is a huge mess.  It takes a nice clean set of 
 * namecache algorithms and introduces a lot of complication in every subsystem
 * that calls into the namecache to deal with the re-merge case, especially
 * since we are using the namecache to placehold negative lookups and the
 * vnode might not be immediately assigned. (2) is certainly far less
 * efficient then (1), but since we are only talking about directories here
 * (which are likely to remain cached), the case does not actually run all
 * that often and has the supreme advantage of not polluting the namecache
 * algorithms.
 */
static int
cache_inefficient_scan(struct namecache *ncp, struct ucred *cred, 
		       struct vnode *dvp)
{
	struct nlcomponent nlc;
	struct namecache *rncp;
	struct dirent *den;
	struct vnode *pvp;
	struct vattr vat;
	struct iovec iov;
	struct uio uio;
	int blksize;
	int eofflag;
	int bytes;
	char *rbuf;
	int error;

	vat.va_blocksize = 0;
	if ((error = VOP_GETATTR(dvp, &vat)) != 0)
		return (error);
	if ((error = cache_vref(ncp, cred, &pvp)) != 0)
		return (error);
	if (ncvp_debug)
		printf("inefficient_scan: directory iosize %ld vattr fileid = %ld\n", vat.va_blocksize, (long)vat.va_fileid);
	if ((blksize = vat.va_blocksize) == 0)
		blksize = DEV_BSIZE;
	rbuf = malloc(blksize, M_TEMP, M_WAITOK);
	rncp = NULL;

	eofflag = 0;
	uio.uio_offset = 0;
again:
	iov.iov_base = rbuf;
	iov.iov_len = blksize;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_resid = blksize;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_READ;
	uio.uio_td = curthread;

	if (ncvp_debug >= 2)
		printf("cache_inefficient_scan: readdir @ %08x\n", (int)uio.uio_offset);
	error = VOP_READDIR(pvp, &uio, cred, &eofflag, NULL, NULL);
	if (error == 0) {
		den = (struct dirent *)rbuf;
		bytes = blksize - uio.uio_resid;

		while (bytes > 0) {
			if (ncvp_debug >= 2) {
				printf("cache_inefficient_scan: %*.*s\n",
					den->d_namlen, den->d_namlen, 
					den->d_name);
			}
			if (den->d_type != DT_WHT &&
			    den->d_ino == vat.va_fileid) {
				if (ncvp_debug) {
					printf("cache_inefficient_scan: "
					       "MATCHED inode %ld path %s/%*.*s\n",
					       vat.va_fileid, ncp->nc_name,
					       den->d_namlen, den->d_namlen,
					       den->d_name);
				}
				nlc.nlc_nameptr = den->d_name;
				nlc.nlc_namelen = den->d_namlen;
				rncp = cache_nlookup(ncp, &nlc);
				KKASSERT(rncp != NULL);
				break;
			}
			bytes -= _DIRENT_DIRSIZ(den);
			den = _DIRENT_NEXT(den);
		}
		if (rncp == NULL && eofflag == 0 && uio.uio_resid != blksize)
			goto again;
	}
	vrele(pvp);
	if (rncp) {
		if (rncp->nc_flag & NCF_UNRESOLVED) {
			cache_setvp(rncp, dvp);
			if (ncvp_debug >= 2) {
				printf("cache_inefficient_scan: setvp %s/%s = %p\n",
					ncp->nc_name, rncp->nc_name, dvp);
			}
		} else {
			if (ncvp_debug >= 2) {
				printf("cache_inefficient_scan: setvp %s/%s already set %p/%p\n", 
					ncp->nc_name, rncp->nc_name, dvp,
					rncp->nc_vp);
			}
		}
		if (rncp->nc_vp == NULL)
			error = rncp->nc_error;
		cache_put(rncp);
	} else {
		printf("cache_inefficient_scan: dvp %p NOT FOUND in %s\n",
			dvp, ncp->nc_name);
		error = ENOENT;
	}
	free(rbuf, M_TEMP);
	return (error);
}

/*
 * Zap a namecache entry.  The ncp is unconditionally set to an unresolved
 * state, which disassociates it from its vnode or ncneglist.
 *
 * Then, if there are no additional references to the ncp and no children,
 * the ncp is removed from the topology and destroyed.  This function will
 * also run through the nc_parent chain and destroy parent ncps if possible.
 * As a side benefit, it turns out the only conditions that allow running
 * up the chain are also the conditions to ensure no deadlock will occur.
 *
 * References and/or children may exist if the ncp is in the middle of the
 * topology, preventing the ncp from being destroyed.
 *
 * This function must be called with the ncp held and locked and will unlock
 * and drop it during zapping.
 */
static void
cache_zap(struct namecache *ncp)
{
	struct namecache *par;

	/*
	 * Disassociate the vnode or negative cache ref and set NCF_UNRESOLVED.
	 */
	cache_setunresolved(ncp);

	/*
	 * Try to scrap the entry and possibly tail-recurse on its parent.
	 * We only scrap unref'd (other then our ref) unresolved entries,
	 * we do not scrap 'live' entries.
	 */
	while (ncp->nc_flag & NCF_UNRESOLVED) {
		/*
		 * Someone other then us has a ref, stop.
		 */
		if (ncp->nc_refs > 1)
			goto done;

		/*
		 * We have children, stop.
		 */
		if (!TAILQ_EMPTY(&ncp->nc_list))
			goto done;

		/*
		 * Remove ncp from the topology: hash table and parent linkage.
		 */
		if (ncp->nc_flag & NCF_HASHED) {
			ncp->nc_flag &= ~NCF_HASHED;
			LIST_REMOVE(ncp, nc_hash);
		}
		if ((par = ncp->nc_parent) != NULL) {
			par = cache_hold(par);
			TAILQ_REMOVE(&par->nc_list, ncp, nc_entry);
			ncp->nc_parent = NULL;
			if (par->nc_vp && TAILQ_EMPTY(&par->nc_list))
				vdrop(par->nc_vp);
		}

		/*
		 * ncp should not have picked up any refs.  Physically
		 * destroy the ncp.
		 */
		KKASSERT(ncp->nc_refs == 1);
		--numunres;
		/* cache_unlock(ncp) not required */
		ncp->nc_refs = -1;	/* safety */
		if (ncp->nc_name)
			free(ncp->nc_name, M_VFSCACHE);
		free(ncp, M_VFSCACHE);

		/*
		 * Loop on the parent (it may be NULL).  Only bother looping
		 * if the parent has a single ref (ours), which also means
		 * we can lock it trivially.
		 */
		ncp = par;
		if (ncp == NULL)
			return;
		if (ncp->nc_refs != 1) {
			cache_drop(ncp);
			return;
		}
		KKASSERT(par->nc_exlocks == 0);
		cache_lock(ncp);
	}
done:
	cache_unlock(ncp);
	atomic_subtract_int(&ncp->nc_refs, 1);
}

static enum { CHI_LOW, CHI_HIGH } cache_hysteresis_state = CHI_LOW;

static __inline
void
cache_hysteresis(void)
{
	/*
	 * Don't cache too many negative hits.  We use hysteresis to reduce
	 * the impact on the critical path.
	 */
	switch(cache_hysteresis_state) {
	case CHI_LOW:
		if (numneg > MINNEG && numneg * ncnegfactor > numcache) {
			cache_cleanneg(10);
			cache_hysteresis_state = CHI_HIGH;
		}
		break;
	case CHI_HIGH:
		if (numneg > MINNEG * 9 / 10 && 
		    numneg * ncnegfactor * 9 / 10 > numcache
		) {
			cache_cleanneg(10);
		} else {
			cache_hysteresis_state = CHI_LOW;
		}
		break;
	}
}

/*
 * NEW NAMECACHE LOOKUP API
 *
 * Lookup an entry in the cache.  A locked, referenced, non-NULL 
 * entry is *always* returned, even if the supplied component is illegal.
 * The resulting namecache entry should be returned to the system with
 * cache_put() or cache_unlock() + cache_drop().
 *
 * namecache locks are recursive but care must be taken to avoid lock order
 * reversals.
 *
 * Nobody else will be able to manipulate the associated namespace (e.g.
 * create, delete, rename, rename-target) until the caller unlocks the
 * entry.
 *
 * The returned entry will be in one of three states:  positive hit (non-null
 * vnode), negative hit (null vnode), or unresolved (NCF_UNRESOLVED is set).
 * Unresolved entries must be resolved through the filesystem to associate the
 * vnode and/or determine whether a positive or negative hit has occured.
 *
 * It is not necessary to lock a directory in order to lock namespace under
 * that directory.  In fact, it is explicitly not allowed to do that.  A
 * directory is typically only locked when being created, renamed, or
 * destroyed.
 *
 * The directory (par) may be unresolved, in which case any returned child
 * will likely also be marked unresolved.  Likely but not guarenteed.  Since
 * the filesystem lookup requires a resolved directory vnode the caller is
 * responsible for resolving the namecache chain top-down.  This API 
 * specifically allows whole chains to be created in an unresolved state.
 */
struct namecache *
cache_nlookup(struct namecache *par, struct nlcomponent *nlc)
{
	struct namecache *ncp;
	struct namecache *new_ncp;
	struct nchashhead *nchpp;
	u_int32_t hash;
	globaldata_t gd;

	numcalls++;
	gd = mycpu;

	/*
	 * Try to locate an existing entry
	 */
	hash = fnv_32_buf(nlc->nlc_nameptr, nlc->nlc_namelen, FNV1_32_INIT);
	hash = fnv_32_buf(&par, sizeof(par), hash);
	new_ncp = NULL;
restart:
	LIST_FOREACH(ncp, (NCHHASH(hash)), nc_hash) {
		numchecks++;

		/*
		 * Zap entries that have timed out.
		 */
		if (ncp->nc_timeout && 
		    (int)(ncp->nc_timeout - ticks) < 0 &&
		    (ncp->nc_flag & NCF_UNRESOLVED) == 0 &&
		    ncp->nc_exlocks == 0
		) {
			cache_zap(cache_get(ncp));
			goto restart;
		}

		/*
		 * Break out if we find a matching entry.  Note that
		 * UNRESOLVED entries may match, but DESTROYED entries
		 * do not.
		 */
		if (ncp->nc_parent == par &&
		    ncp->nc_nlen == nlc->nlc_namelen &&
		    bcmp(ncp->nc_name, nlc->nlc_nameptr, ncp->nc_nlen) == 0 &&
		    (ncp->nc_flag & NCF_DESTROYED) == 0
		) {
			if (cache_get_nonblock(ncp) == 0) {
				if (new_ncp)
					cache_free(new_ncp);
				goto found;
			}
			cache_get(ncp);
			cache_put(ncp);
			goto restart;
		}
	}

	/*
	 * We failed to locate an entry, create a new entry and add it to
	 * the cache.  We have to relookup after possibly blocking in
	 * malloc.
	 */
	if (new_ncp == NULL) {
		new_ncp = cache_alloc(nlc->nlc_namelen);
		goto restart;
	}

	ncp = new_ncp;

	/*
	 * Initialize as a new UNRESOLVED entry, lock (non-blocking),
	 * and link to the parent.  The mount point is usually inherited
	 * from the parent unless this is a special case such as a mount
	 * point where nlc_namelen is 0.  The caller is responsible for
	 * setting nc_mount in that case.  If nlc_namelen is 0 nc_name will
	 * be NULL.
	 */
	if (nlc->nlc_namelen) {
		bcopy(nlc->nlc_nameptr, ncp->nc_name, nlc->nlc_namelen);
		ncp->nc_name[nlc->nlc_namelen] = 0;
		ncp->nc_mount = par->nc_mount;
	}
	nchpp = NCHHASH(hash);
	LIST_INSERT_HEAD(nchpp, ncp, nc_hash);
	ncp->nc_flag |= NCF_HASHED;
	cache_link_parent(ncp, par);
found:
	/*
	 * stats and namecache size management
	 */
	if (ncp->nc_flag & NCF_UNRESOLVED)
		++gd->gd_nchstats->ncs_miss;
	else if (ncp->nc_vp)
		++gd->gd_nchstats->ncs_goodhits;
	else
		++gd->gd_nchstats->ncs_neghits;
	cache_hysteresis();
	return(ncp);
}

/*
 * Given a locked ncp, validate that the vnode, if present, is actually
 * usable.  If it is not usable set the ncp to an unresolved state.
 */
void
cache_validate(struct namecache *ncp)
{
	if ((ncp->nc_flag & NCF_UNRESOLVED) == 0) {
		if (ncp->nc_vp && (ncp->nc_vp->v_flag & VRECLAIMED))
			cache_setunresolved(ncp);
	}
}

/*
 * Resolve an unresolved namecache entry, generally by looking it up.
 * The passed ncp must be locked and refd. 
 *
 * Theoretically since a vnode cannot be recycled while held, and since
 * the nc_parent chain holds its vnode as long as children exist, the
 * direct parent of the cache entry we are trying to resolve should
 * have a valid vnode.  If not then generate an error that we can 
 * determine is related to a resolver bug.
 *
 * However, if a vnode was in the middle of a recyclement when the NCP
 * got locked, ncp->nc_vp might point to a vnode that is about to become
 * invalid.  cache_resolve() handles this case by unresolving the entry
 * and then re-resolving it.
 *
 * Note that successful resolution does not necessarily return an error
 * code of 0.  If the ncp resolves to a negative cache hit then ENOENT
 * will be returned.
 */
int
cache_resolve(struct namecache *ncp, struct ucred *cred)
{
	struct namecache *par;
	int error;

restart:
	/*
	 * If the ncp is already resolved we have nothing to do.  However,
	 * we do want to guarentee that a usable vnode is returned when
	 * a vnode is present, so make sure it hasn't been reclaimed.
	 */
	if ((ncp->nc_flag & NCF_UNRESOLVED) == 0) {
		if (ncp->nc_vp && (ncp->nc_vp->v_flag & VRECLAIMED))
			cache_setunresolved(ncp);
		if ((ncp->nc_flag & NCF_UNRESOLVED) == 0)
			return (ncp->nc_error);
	}

	/*
	 * Mount points need special handling because the parent does not
	 * belong to the same filesystem as the ncp.
	 */
	if (ncp->nc_flag & NCF_MOUNTPT)
		return (cache_resolve_mp(ncp));

	/*
	 * We expect an unbroken chain of ncps to at least the mount point,
	 * and even all the way to root (but this code doesn't have to go
	 * past the mount point).
	 */
	if (ncp->nc_parent == NULL) {
		printf("EXDEV case 1 %p %*.*s\n", ncp,
			ncp->nc_nlen, ncp->nc_nlen, ncp->nc_name);
		ncp->nc_error = EXDEV;
		return(ncp->nc_error);
	}

	/*
	 * The vp's of the parent directories in the chain are held via vhold()
	 * due to the existance of the child, and should not disappear. 
	 * However, there are cases where they can disappear:
	 *
	 *	- due to filesystem I/O errors.
	 *	- due to NFS being stupid about tracking the namespace and
	 *	  destroys the namespace for entire directories quite often.
	 *	- due to forced unmounts.
	 *	- due to an rmdir (parent will be marked DESTROYED)
	 *
	 * When this occurs we have to track the chain backwards and resolve
	 * it, looping until the resolver catches up to the current node.  We
	 * could recurse here but we might run ourselves out of kernel stack
	 * so we do it in a more painful manner.  This situation really should
	 * not occur all that often, or if it does not have to go back too
	 * many nodes to resolve the ncp.
	 */
	while (ncp->nc_parent->nc_vp == NULL) {
		/*
		 * This case can occur if a process is CD'd into a
		 * directory which is then rmdir'd.  If the parent is marked
		 * destroyed there is no point trying to resolve it.
		 */
		if (ncp->nc_parent->nc_flag & NCF_DESTROYED)
			return(ENOENT);

		par = ncp->nc_parent;
		while (par->nc_parent && par->nc_parent->nc_vp == NULL)
			par = par->nc_parent;
		if (par->nc_parent == NULL) {
			printf("EXDEV case 2 %*.*s\n",
				par->nc_nlen, par->nc_nlen, par->nc_name);
			return (EXDEV);
		}
		printf("[diagnostic] cache_resolve: had to recurse on %*.*s\n",
			par->nc_nlen, par->nc_nlen, par->nc_name);
		/*
		 * The parent is not set in stone, ref and lock it to prevent
		 * it from disappearing.  Also note that due to renames it
		 * is possible for our ncp to move and for par to no longer
		 * be one of its parents.  We resolve it anyway, the loop 
		 * will handle any moves.
		 */
		cache_get(par);
		if (par->nc_flag & NCF_MOUNTPT) {
			cache_resolve_mp(par);
		} else if (par->nc_parent->nc_vp == NULL) {
			printf("[diagnostic] cache_resolve: raced on %*.*s\n", par->nc_nlen, par->nc_nlen, par->nc_name);
			cache_put(par);
			continue;
		} else if (par->nc_flag & NCF_UNRESOLVED) {
			par->nc_error = VOP_NRESOLVE(par, cred);
		}
		if ((error = par->nc_error) != 0) {
			if (par->nc_error != EAGAIN) {
				printf("EXDEV case 3 %*.*s error %d\n",
				    par->nc_nlen, par->nc_nlen, par->nc_name,
				    par->nc_error);
				cache_put(par);
				return(error);
			}
			printf("[diagnostic] cache_resolve: EAGAIN par %p %*.*s\n",
				par, par->nc_nlen, par->nc_nlen, par->nc_name);
		}
		cache_put(par);
		/* loop */
	}

	/*
	 * Call VOP_NRESOLVE() to get the vp, then scan for any disconnected
	 * ncp's and reattach them.  If this occurs the original ncp is marked
	 * EAGAIN to force a relookup.
	 *
	 * NOTE: in order to call VOP_NRESOLVE(), the parent of the passed
	 * ncp must already be resolved.
	 */
	KKASSERT((ncp->nc_flag & NCF_MOUNTPT) == 0);
	ncp->nc_error = VOP_NRESOLVE(ncp, cred);
	/*vop_nresolve(*ncp->nc_parent->nc_vp->v_ops, ncp, cred);*/
	if (ncp->nc_error == EAGAIN) {
		printf("[diagnostic] cache_resolve: EAGAIN ncp %p %*.*s\n",
			ncp, ncp->nc_nlen, ncp->nc_nlen, ncp->nc_name);
		goto restart;
	}
	return(ncp->nc_error);
}

/*
 * Resolve the ncp associated with a mount point.  Such ncp's almost always
 * remain resolved and this routine is rarely called.  NFS MPs tends to force
 * re-resolution more often due to its mac-truck-smash-the-namecache
 * method of tracking namespace changes.
 *
 * The semantics for this call is that the passed ncp must be locked on
 * entry and will be locked on return.  However, if we actually have to
 * resolve the mount point we temporarily unlock the entry in order to
 * avoid race-to-root deadlocks due to e.g. dead NFS mounts.  Because of
 * the unlock we have to recheck the flags after we relock.
 */
static int
cache_resolve_mp(struct namecache *ncp)
{
	struct vnode *vp;
	struct mount *mp = ncp->nc_mount;
	int error;

	KKASSERT(mp != NULL);

	/*
	 * If the ncp is already resolved we have nothing to do.  However,
	 * we do want to guarentee that a usable vnode is returned when
	 * a vnode is present, so make sure it hasn't been reclaimed.
	 */
	if ((ncp->nc_flag & NCF_UNRESOLVED) == 0) {
		if (ncp->nc_vp && (ncp->nc_vp->v_flag & VRECLAIMED))
			cache_setunresolved(ncp);
	}

	if (ncp->nc_flag & NCF_UNRESOLVED) {
		cache_unlock(ncp);
		while (vfs_busy(mp, 0))
			;
		error = VFS_ROOT(mp, &vp);
		cache_lock(ncp);

		/*
		 * recheck the ncp state after relocking.
		 */
		if (ncp->nc_flag & NCF_UNRESOLVED) {
			ncp->nc_error = error;
			if (error == 0) {
				cache_setvp(ncp, vp);
				vput(vp);
			} else {
				printf("[diagnostic] cache_resolve_mp: failed to resolve mount %p\n", mp);
				cache_setvp(ncp, NULL);
			}
		} else if (error == 0) {
			vput(vp);
		}
		vfs_unbusy(mp);
	}
	return(ncp->nc_error);
}

void
cache_cleanneg(int count)
{
	struct namecache *ncp;

	/*
	 * Automode from the vnlru proc - clean out 10% of the negative cache
	 * entries.
	 */
	if (count == 0)
		count = numneg / 10 + 1;

	/*
	 * Attempt to clean out the specified number of negative cache
	 * entries.
	 */
	while (count) {
		ncp = TAILQ_FIRST(&ncneglist);
		if (ncp == NULL) {
			KKASSERT(numneg == 0);
			break;
		}
		TAILQ_REMOVE(&ncneglist, ncp, nc_vnode);
		TAILQ_INSERT_TAIL(&ncneglist, ncp, nc_vnode);
		if (cache_get_nonblock(ncp) == 0)
			cache_zap(ncp);
		--count;
	}
}

/*
 * Rehash a ncp.  Rehashing is typically required if the name changes (should
 * not generally occur) or the parent link changes.  This function will
 * unhash the ncp if the ncp is no longer hashable.
 */
static void
cache_rehash(struct namecache *ncp)
{
	struct nchashhead *nchpp;
	u_int32_t hash;

	if (ncp->nc_flag & NCF_HASHED) {
		ncp->nc_flag &= ~NCF_HASHED;
		LIST_REMOVE(ncp, nc_hash);
	}
	if (ncp->nc_nlen && ncp->nc_parent) {
		hash = fnv_32_buf(ncp->nc_name, ncp->nc_nlen, FNV1_32_INIT);
		hash = fnv_32_buf(&ncp->nc_parent, 
					sizeof(ncp->nc_parent), hash);
		nchpp = NCHHASH(hash);
		LIST_INSERT_HEAD(nchpp, ncp, nc_hash);
		ncp->nc_flag |= NCF_HASHED;
	}
}

/*
 * Name cache initialization, from vfsinit() when we are booting
 */
void
nchinit(void)
{
	int i;
	globaldata_t gd;

	/* initialise per-cpu namecache effectiveness statistics. */
	for (i = 0; i < ncpus; ++i) {
		gd = globaldata_find(i);
		gd->gd_nchstats = &nchstats[i];
	}
	TAILQ_INIT(&ncneglist);
	nchashtbl = hashinit(desiredvnodes*2, M_VFSCACHE, &nchash);
	nclockwarn = 1 * hz;
}

/*
 * Called from start_init() to bootstrap the root filesystem.  Returns
 * a referenced, unlocked namecache record.
 */
struct namecache *
cache_allocroot(struct mount *mp, struct vnode *vp)
{
	struct namecache *ncp = cache_alloc(0);

	ncp->nc_flag |= NCF_MOUNTPT | NCF_ROOT;
	ncp->nc_mount = mp;
	cache_setvp(ncp, vp);
	return(ncp);
}

/*
 * vfs_cache_setroot()
 *
 *	Create an association between the root of our namecache and
 *	the root vnode.  This routine may be called several times during
 *	booting.
 *
 *	If the caller intends to save the returned namecache pointer somewhere
 *	it must cache_hold() it.
 */
void
vfs_cache_setroot(struct vnode *nvp, struct namecache *ncp)
{
	struct vnode *ovp;
	struct namecache *oncp;

	ovp = rootvnode;
	oncp = rootncp;
	rootvnode = nvp;
	rootncp = ncp;

	if (ovp)
		vrele(ovp);
	if (oncp)
		cache_drop(oncp);
}

/*
 * XXX OLD API COMPAT FUNCTION.  This really messes up the new namecache
 * topology and is being removed as quickly as possible.  The new VOP_N*()
 * API calls are required to make specific adjustments using the supplied
 * ncp pointers rather then just bogusly purging random vnodes.
 *
 * Invalidate all namecache entries to a particular vnode as well as 
 * any direct children of that vnode in the namecache.  This is a 
 * 'catch all' purge used by filesystems that do not know any better.
 *
 * Note that the linkage between the vnode and its namecache entries will
 * be removed, but the namecache entries themselves might stay put due to
 * active references from elsewhere in the system or due to the existance of
 * the children.   The namecache topology is left intact even if we do not
 * know what the vnode association is.  Such entries will be marked
 * NCF_UNRESOLVED.
 */
void
cache_purge(struct vnode *vp)
{
	cache_inval_vp(vp, CINV_DESTROY | CINV_CHILDREN);
}

/*
 * Flush all entries referencing a particular filesystem.
 *
 * Since we need to check it anyway, we will flush all the invalid
 * entries at the same time.
 */
void
cache_purgevfs(struct mount *mp)
{
	struct nchashhead *nchpp;
	struct namecache *ncp, *nnp;

	/*
	 * Scan hash tables for applicable entries.
	 */
	for (nchpp = &nchashtbl[nchash]; nchpp >= nchashtbl; nchpp--) {
		ncp = LIST_FIRST(nchpp);
		if (ncp)
			cache_hold(ncp);
		while (ncp) {
			nnp = LIST_NEXT(ncp, nc_hash);
			if (nnp)
				cache_hold(nnp);
			if (ncp->nc_mount == mp) {
				cache_lock(ncp);
				cache_zap(ncp);
			} else {
				cache_drop(ncp);
			}
			ncp = nnp;
		}
	}
}

/*
 * Create a new (theoretically) unique fsmid
 */
int64_t
cache_getnewfsmid(void)
{
	static int fsmid_roller;
	int64_t fsmid;

	++fsmid_roller;
	fsmid = ((int64_t)time_second << 32) |
			(fsmid_roller & 0x7FFFFFFF);
	return (fsmid);
}


static int disablecwd;
SYSCTL_INT(_debug, OID_AUTO, disablecwd, CTLFLAG_RW, &disablecwd, 0, "");

static u_long numcwdcalls; STATNODE(CTLFLAG_RD, numcwdcalls, &numcwdcalls);
static u_long numcwdfail1; STATNODE(CTLFLAG_RD, numcwdfail1, &numcwdfail1);
static u_long numcwdfail2; STATNODE(CTLFLAG_RD, numcwdfail2, &numcwdfail2);
static u_long numcwdfail3; STATNODE(CTLFLAG_RD, numcwdfail3, &numcwdfail3);
static u_long numcwdfail4; STATNODE(CTLFLAG_RD, numcwdfail4, &numcwdfail4);
static u_long numcwdfound; STATNODE(CTLFLAG_RD, numcwdfound, &numcwdfound);

int
sys___getcwd(struct __getcwd_args *uap)
{
	int buflen;
	int error;
	char *buf;
	char *bp;

	if (disablecwd)
		return (ENODEV);

	buflen = uap->buflen;
	if (buflen < 2)
		return (EINVAL);
	if (buflen > MAXPATHLEN)
		buflen = MAXPATHLEN;

	buf = malloc(buflen, M_TEMP, M_WAITOK);
	bp = kern_getcwd(buf, buflen, &error);
	if (error == 0)
		error = copyout(bp, uap->buf, strlen(bp) + 1);
	free(buf, M_TEMP);
	return (error);
}

char *
kern_getcwd(char *buf, size_t buflen, int *error)
{
	struct proc *p = curproc;
	char *bp;
	int i, slash_prefixed;
	struct filedesc *fdp;
	struct namecache *ncp;

	numcwdcalls++;
	bp = buf;
	bp += buflen - 1;
	*bp = '\0';
	fdp = p->p_fd;
	slash_prefixed = 0;

	ncp = fdp->fd_ncdir;
	while (ncp && ncp != fdp->fd_nrdir && (ncp->nc_flag & NCF_ROOT) == 0) {
		if (ncp->nc_flag & NCF_MOUNTPT) {
			if (ncp->nc_mount == NULL) {
				*error = EBADF;		/* forced unmount? */
				return(NULL);
			}
			ncp = ncp->nc_parent;
			continue;
		}
		for (i = ncp->nc_nlen - 1; i >= 0; i--) {
			if (bp == buf) {
				numcwdfail4++;
				*error = ENOMEM;
				return(NULL);
			}
			*--bp = ncp->nc_name[i];
		}
		if (bp == buf) {
			numcwdfail4++;
			*error = ENOMEM;
			return(NULL);
		}
		*--bp = '/';
		slash_prefixed = 1;
		ncp = ncp->nc_parent;
	}
	if (ncp == NULL) {
		numcwdfail2++;
		*error = ENOENT;
		return(NULL);
	}
	if (!slash_prefixed) {
		if (bp == buf) {
			numcwdfail4++;
			*error = ENOMEM;
			return(NULL);
		}
		*--bp = '/';
	}
	numcwdfound++;
	*error = 0;
	return (bp);
}

/*
 * Thus begins the fullpath magic.
 */

#undef STATNODE
#define STATNODE(name)							\
	static u_int name;						\
	SYSCTL_UINT(_vfs_cache, OID_AUTO, name, CTLFLAG_RD, &name, 0, "")

static int disablefullpath;
SYSCTL_INT(_debug, OID_AUTO, disablefullpath, CTLFLAG_RW,
    &disablefullpath, 0, "");

STATNODE(numfullpathcalls);
STATNODE(numfullpathfail1);
STATNODE(numfullpathfail2);
STATNODE(numfullpathfail3);
STATNODE(numfullpathfail4);
STATNODE(numfullpathfound);

int
cache_fullpath(struct proc *p, struct namecache *ncp, char **retbuf, char **freebuf)
{
	char *bp, *buf;
	int i, slash_prefixed;
	struct namecache *fd_nrdir;

	numfullpathcalls--;

	buf = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);
	bp = buf + MAXPATHLEN - 1;
	*bp = '\0';
	if (p != NULL)
		fd_nrdir = p->p_fd->fd_nrdir;
	else
		fd_nrdir = NULL;
	slash_prefixed = 0;
	while (ncp && ncp != fd_nrdir && (ncp->nc_flag & NCF_ROOT) == 0) {
		if (ncp->nc_flag & NCF_MOUNTPT) {
			if (ncp->nc_mount == NULL) {
				free(buf, M_TEMP);
				return(EBADF);
			}
			ncp = ncp->nc_parent;
			continue;
		}
		for (i = ncp->nc_nlen - 1; i >= 0; i--) {
			if (bp == buf) {
				numfullpathfail4++;
				free(buf, M_TEMP);
				return(ENOMEM);
			}
			*--bp = ncp->nc_name[i];
		}
		if (bp == buf) {
			numfullpathfail4++;
			free(buf, M_TEMP);
			return(ENOMEM);
		}
		*--bp = '/';
		slash_prefixed = 1;
		ncp = ncp->nc_parent;
	}
	if (ncp == NULL) {
		numfullpathfail2++;
		free(buf, M_TEMP);
		return(ENOENT);
	}
	if (p != NULL && (ncp->nc_flag & NCF_ROOT) && ncp != fd_nrdir) {
		bp = buf + MAXPATHLEN - 1;
		*bp = '\0';
		slash_prefixed = 0;
	}
	if (!slash_prefixed) {
		if (bp == buf) {
			numfullpathfail4++;
			free(buf, M_TEMP);
			return(ENOMEM);
		}
		*--bp = '/';
	}
	numfullpathfound++;
	*retbuf = bp; 
	*freebuf = buf;

	return(0);
}

int
vn_fullpath(struct proc *p, struct vnode *vn, char **retbuf, char **freebuf) 
{
	struct namecache *ncp;

	numfullpathcalls++;
	if (disablefullpath)
		return (ENODEV);

	if (p == NULL)
		return (EINVAL);

	/* vn is NULL, client wants us to use p->p_textvp */
	if (vn == NULL) {
		if ((vn = p->p_textvp) == NULL)
			return (EINVAL);
	}
	TAILQ_FOREACH(ncp, &vn->v_namecache, nc_vnode) {
		if (ncp->nc_nlen)
			break;
	}
	if (ncp == NULL)
		return (EINVAL);

	numfullpathcalls--;
	return(cache_fullpath(p, ncp, retbuf, freebuf));
}
