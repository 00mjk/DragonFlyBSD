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
 *	@(#)nfs_bio.c	8.9 (Berkeley) 3/30/95
 * $FreeBSD: /repoman/r/ncvs/src/sys/nfsclient/nfs_bio.c,v 1.130 2004/04/14 23:23:55 peadar Exp $
 * $DragonFly: src/sys/vfs/nfs/nfs_bio.c,v 1.45 2008/07/18 00:09:39 dillon Exp $
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/kernel.h>
#include <sys/buf2.h>
#include <sys/msfbuf.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_page.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>

#include <sys/thread2.h>

#include "rpcv2.h"
#include "nfsproto.h"
#include "nfs.h"
#include "nfsmount.h"
#include "nfsnode.h"

static struct buf *nfs_getcacheblk(struct vnode *vp, off_t loffset,
				   int size, struct thread *td);
static int nfs_check_dirent(struct nfs_dirent *dp, int maxlen);

extern int nfs_numasync;
extern int nfs_pbuf_freecnt;
extern struct nfsstats nfsstats;

/*
 * Vnode op for VM getpages.
 *
 * nfs_getpages(struct vnode *a_vp, vm_page_t *a_m, int a_count,
 *		int a_reqpage, vm_ooffset_t a_offset)
 */
int
nfs_getpages(struct vop_getpages_args *ap)
{
	struct thread *td = curthread;		/* XXX */
	int i, error, nextoff, size, toff, count, npages;
	struct uio uio;
	struct iovec iov;
	char *kva;
	struct vnode *vp;
	struct nfsmount *nmp;
	vm_page_t *pages;
	vm_page_t m;
	struct msf_buf *msf;

	vp = ap->a_vp;
	nmp = VFSTONFS(vp->v_mount);
	pages = ap->a_m;
	count = ap->a_count;

	if (vp->v_object == NULL) {
		kprintf("nfs_getpages: called with non-merged cache vnode??\n");
		return VM_PAGER_ERROR;
	}

	if ((nmp->nm_flag & NFSMNT_NFSV3) != 0 &&
	    (nmp->nm_state & NFSSTA_GOTFSINFO) == 0)
		(void)nfs_fsinfo(nmp, vp, td);

	npages = btoc(count);

	/*
	 * NOTE that partially valid pages may occur in cases other
	 * then file EOF, such as when a file is partially written and
	 * ftruncate()-extended to a larger size.   It is also possible
	 * for the valid bits to be set on garbage beyond the file EOF and
	 * clear in the area before EOF (e.g. m->valid == 0xfc), which can
	 * occur due to vtruncbuf() and the buffer cache's handling of
	 * pages which 'straddle' buffers or when b_bufsize is not a 
	 * multiple of PAGE_SIZE.... the buffer cache cannot normally
	 * clear the extra bits.  This kind of situation occurs when you
	 * make a small write() (m->valid == 0x03) and then mmap() and
	 * fault in the buffer(m->valid = 0xFF).  When NFS flushes the
	 * buffer (vinvalbuf() m->valid = 0xFC) we are left with a mess.
	 *
	 * This is combined with the possibility that the pages are partially
	 * dirty or that there is a buffer backing the pages that is dirty
	 * (even if m->dirty is 0).
	 *
	 * To solve this problem several hacks have been made:  (1) NFS
	 * guarentees that the IO block size is a multiple of PAGE_SIZE and
	 * (2) The buffer cache, when invalidating an NFS buffer, will
	 * disregard the buffer's fragmentory b_bufsize and invalidate
	 * the whole page rather then just the piece the buffer owns.
	 *
	 * This allows us to assume that a partially valid page found here
	 * is fully valid (vm_fault will zero'd out areas of the page not
	 * marked as valid).
	 */
	m = pages[ap->a_reqpage];
	if (m->valid != 0) {
		for (i = 0; i < npages; ++i) {
			if (i != ap->a_reqpage)
				vnode_pager_freepage(pages[i]);
		}
		return(0);
	}

	/*
	 * Use an MSF_BUF as a medium to retrieve data from the pages.
	 */
	msf_map_pagelist(&msf, pages, npages, 0);
	KKASSERT(msf);
	kva = msf_buf_kva(msf);

	iov.iov_base = kva;
	iov.iov_len = count;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = IDX_TO_OFF(pages[0]->pindex);
	uio.uio_resid = count;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_READ;
	uio.uio_td = td;

	error = nfs_readrpc(vp, &uio);
	msf_buf_free(msf);

	if (error && (uio.uio_resid == count)) {
		kprintf("nfs_getpages: error %d\n", error);
		for (i = 0; i < npages; ++i) {
			if (i != ap->a_reqpage)
				vnode_pager_freepage(pages[i]);
		}
		return VM_PAGER_ERROR;
	}

	/*
	 * Calculate the number of bytes read and validate only that number
	 * of bytes.  Note that due to pending writes, size may be 0.  This
	 * does not mean that the remaining data is invalid!
	 */

	size = count - uio.uio_resid;

	for (i = 0, toff = 0; i < npages; i++, toff = nextoff) {
		nextoff = toff + PAGE_SIZE;
		m = pages[i];

		m->flags &= ~PG_ZERO;

		if (nextoff <= size) {
			/*
			 * Read operation filled an entire page
			 */
			m->valid = VM_PAGE_BITS_ALL;
			vm_page_undirty(m);
		} else if (size > toff) {
			/*
			 * Read operation filled a partial page.
			 */
			m->valid = 0;
			vm_page_set_validclean(m, 0, size - toff);
			/* handled by vm_fault now	  */
			/* vm_page_zero_invalid(m, TRUE); */
		} else {
			/*
			 * Read operation was short.  If no error occured
			 * we may have hit a zero-fill section.   We simply
			 * leave valid set to 0.
			 */
			;
		}
		if (i != ap->a_reqpage) {
			/*
			 * Whether or not to leave the page activated is up in
			 * the air, but we should put the page on a page queue
			 * somewhere (it already is in the object).  Result:
			 * It appears that emperical results show that
			 * deactivating pages is best.
			 */

			/*
			 * Just in case someone was asking for this page we
			 * now tell them that it is ok to use.
			 */
			if (!error) {
				if (m->flags & PG_WANTED)
					vm_page_activate(m);
				else
					vm_page_deactivate(m);
				vm_page_wakeup(m);
			} else {
				vnode_pager_freepage(m);
			}
		}
	}
	return 0;
}

/*
 * Vnode op for VM putpages.
 *
 * nfs_putpages(struct vnode *a_vp, vm_page_t *a_m, int a_count, int a_sync,
 *		int *a_rtvals, vm_ooffset_t a_offset)
 */
int
nfs_putpages(struct vop_putpages_args *ap)
{
	struct thread *td = curthread;
	struct uio uio;
	struct iovec iov;
	char *kva;
	int iomode, must_commit, i, error, npages, count;
	off_t offset;
	int *rtvals;
	struct vnode *vp;
	struct nfsmount *nmp;
	struct nfsnode *np;
	vm_page_t *pages;
	struct msf_buf *msf;

	vp = ap->a_vp;
	np = VTONFS(vp);
	nmp = VFSTONFS(vp->v_mount);
	pages = ap->a_m;
	count = ap->a_count;
	rtvals = ap->a_rtvals;
	npages = btoc(count);
	offset = IDX_TO_OFF(pages[0]->pindex);

	if ((nmp->nm_flag & NFSMNT_NFSV3) != 0 &&
	    (nmp->nm_state & NFSSTA_GOTFSINFO) == 0)
		(void)nfs_fsinfo(nmp, vp, td);

	for (i = 0; i < npages; i++) {
		rtvals[i] = VM_PAGER_AGAIN;
	}

	/*
	 * When putting pages, do not extend file past EOF.
	 */

	if (offset + count > np->n_size) {
		count = np->n_size - offset;
		if (count < 0)
			count = 0;
	}

	/*
	 * Use an MSF_BUF as a medium to retrieve data from the pages.
	 */
	msf_map_pagelist(&msf, pages, npages, 0);
	KKASSERT(msf);
	kva = msf_buf_kva(msf);

	iov.iov_base = kva;
	iov.iov_len = count;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = offset;
	uio.uio_resid = count;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = td;

	if ((ap->a_sync & VM_PAGER_PUT_SYNC) == 0)
	    iomode = NFSV3WRITE_UNSTABLE;
	else
	    iomode = NFSV3WRITE_FILESYNC;

	error = nfs_writerpc(vp, &uio, &iomode, &must_commit);

	msf_buf_free(msf);

	if (!error) {
		int nwritten = round_page(count - uio.uio_resid) / PAGE_SIZE;
		for (i = 0; i < nwritten; i++) {
			rtvals[i] = VM_PAGER_OK;
			vm_page_undirty(pages[i]);
		}
		if (must_commit)
			nfs_clearcommit(vp->v_mount);
	}
	return rtvals[0];
}

/*
 * Vnode op for read using bio
 */
int
nfs_bioread(struct vnode *vp, struct uio *uio, int ioflag)
{
	struct nfsnode *np = VTONFS(vp);
	int biosize, i;
	struct buf *bp = 0, *rabp;
	struct vattr vattr;
	struct thread *td;
	struct nfsmount *nmp = VFSTONFS(vp->v_mount);
	daddr_t lbn, rabn;
	off_t raoffset;
	off_t loffset;
	int bcount;
	int seqcount;
	int nra, error = 0, n = 0, on = 0;

#ifdef DIAGNOSTIC
	if (uio->uio_rw != UIO_READ)
		panic("nfs_read mode");
#endif
	if (uio->uio_resid == 0)
		return (0);
	if (uio->uio_offset < 0)	/* XXX VDIR cookies can be negative */
		return (EINVAL);
	td = uio->uio_td;

	if ((nmp->nm_flag & NFSMNT_NFSV3) != 0 &&
	    (nmp->nm_state & NFSSTA_GOTFSINFO) == 0)
		(void)nfs_fsinfo(nmp, vp, td);
	if (vp->v_type != VDIR &&
	    (uio->uio_offset + uio->uio_resid) > nmp->nm_maxfilesize)
		return (EFBIG);
	biosize = vp->v_mount->mnt_stat.f_iosize;
	seqcount = (int)((off_t)(ioflag >> IO_SEQSHIFT) * biosize / BKVASIZE);

	/*
	 * For nfs, cache consistency can only be maintained approximately.
	 * Although RFC1094 does not specify the criteria, the following is
	 * believed to be compatible with the reference port.
	 *
	 * NFS:		If local changes have been made and this is a
	 *		directory, the directory must be invalidated and
	 *		the attribute cache must be cleared.
	 *
	 *		GETATTR is called to synchronize the file size.
	 *
	 *		If remote changes are detected local data is flushed
	 *		and the cache is invalidated.
	 *
	 *		NOTE: In the normal case the attribute cache is not
	 *		cleared which means GETATTR may use cached data and
	 *		not immediately detect changes made on the server.
	 */
	if ((np->n_flag & NLMODIFIED) && vp->v_type == VDIR) {
		nfs_invaldir(vp);
		error = nfs_vinvalbuf(vp, V_SAVE, 1);
		if (error)
			return (error);
		np->n_attrstamp = 0;
	}
	error = VOP_GETATTR(vp, &vattr);
	if (error)
		return (error);
	if (np->n_flag & NRMODIFIED) {
		if (vp->v_type == VDIR)
			nfs_invaldir(vp);
		error = nfs_vinvalbuf(vp, V_SAVE, 1);
		if (error)
			return (error);
		np->n_flag &= ~NRMODIFIED;
	}
	do {
	    if (np->n_flag & NDONTCACHE) {
		switch (vp->v_type) {
		case VREG:
			return (nfs_readrpc(vp, uio));
		case VLNK:
			return (nfs_readlinkrpc(vp, uio));
		case VDIR:
			break;
		default:
			kprintf(" NDONTCACHE: type %x unexpected\n", vp->v_type);
			break;
		};
	    }
	    switch (vp->v_type) {
	    case VREG:
		nfsstats.biocache_reads++;
		lbn = uio->uio_offset / biosize;
		on = uio->uio_offset & (biosize - 1);
		loffset = (off_t)lbn * biosize;

		/*
		 * Start the read ahead(s), as required.
		 */
		if (nfs_numasync > 0 && nmp->nm_readahead > 0) {
		    for (nra = 0; nra < nmp->nm_readahead && nra < seqcount &&
			(off_t)(lbn + 1 + nra) * biosize < np->n_size; nra++) {
			rabn = lbn + 1 + nra;
			raoffset = (off_t)rabn * biosize;
			if (!findblk(vp, raoffset)) {
			    rabp = nfs_getcacheblk(vp, raoffset, biosize, td);
			    if (!rabp)
				return (EINTR);
			    if ((rabp->b_flags & (B_CACHE|B_DELWRI)) == 0) {
				rabp->b_flags |= B_ASYNC;
				rabp->b_cmd = BUF_CMD_READ;
				vfs_busy_pages(vp, rabp);
				if (nfs_asyncio(vp, &rabp->b_bio2, td)) {
				    rabp->b_flags |= B_INVAL|B_ERROR;
				    vfs_unbusy_pages(rabp);
				    brelse(rabp);
				    break;
				}
			    } else {
				brelse(rabp);
			    }
			}
		    }
		}

		/*
		 * Obtain the buffer cache block.  Figure out the buffer size
		 * when we are at EOF.  If we are modifying the size of the
		 * buffer based on an EOF condition we need to hold 
		 * nfs_rslock() through obtaining the buffer to prevent
		 * a potential writer-appender from messing with n_size.
		 * Otherwise we may accidently truncate the buffer and
		 * lose dirty data.
		 *
		 * Note that bcount is *not* DEV_BSIZE aligned.
		 */

again:
		bcount = biosize;
		if (loffset >= np->n_size) {
			bcount = 0;
		} else if (loffset + biosize > np->n_size) {
			bcount = np->n_size - loffset;
		}
		if (bcount != biosize) {
			switch(nfs_rslock(np)) {
			case ENOLCK:
				goto again;
				/* not reached */
			case EINTR:
			case ERESTART:
				return(EINTR);
				/* not reached */
			default:
				break;
			}
		}

		bp = nfs_getcacheblk(vp, loffset, bcount, td);

		if (bcount != biosize)
			nfs_rsunlock(np);
		if (!bp)
			return (EINTR);

		/*
		 * If B_CACHE is not set, we must issue the read.  If this
		 * fails, we return an error.
		 */

		if ((bp->b_flags & B_CACHE) == 0) {
		    bp->b_cmd = BUF_CMD_READ;
		    vfs_busy_pages(vp, bp);
		    error = nfs_doio(vp, &bp->b_bio2, td);
		    if (error) {
			brelse(bp);
			return (error);
		    }
		}

		/*
		 * on is the offset into the current bp.  Figure out how many
		 * bytes we can copy out of the bp.  Note that bcount is
		 * NOT DEV_BSIZE aligned.
		 *
		 * Then figure out how many bytes we can copy into the uio.
		 */

		n = 0;
		if (on < bcount)
			n = min((unsigned)(bcount - on), uio->uio_resid);
		break;
	    case VLNK:
		biosize = min(NFS_MAXPATHLEN, np->n_size);
		nfsstats.biocache_readlinks++;
		bp = nfs_getcacheblk(vp, (off_t)0, biosize, td);
		if (bp == NULL)
			return (EINTR);
		if ((bp->b_flags & B_CACHE) == 0) {
		    bp->b_cmd = BUF_CMD_READ;
		    vfs_busy_pages(vp, bp);
		    error = nfs_doio(vp, &bp->b_bio2, td);
		    if (error) {
			bp->b_flags |= B_ERROR | B_INVAL;
			brelse(bp);
			return (error);
		    }
		}
		n = min(uio->uio_resid, bp->b_bcount - bp->b_resid);
		on = 0;
		break;
	    case VDIR:
		nfsstats.biocache_readdirs++;
		if (np->n_direofoffset
		    && uio->uio_offset >= np->n_direofoffset) {
		    return (0);
		}
		lbn = (uoff_t)uio->uio_offset / NFS_DIRBLKSIZ;
		on = uio->uio_offset & (NFS_DIRBLKSIZ - 1);
		loffset = uio->uio_offset - on;
		bp = nfs_getcacheblk(vp, loffset, NFS_DIRBLKSIZ, td);
		if (bp == NULL)
		    return (EINTR);

		if ((bp->b_flags & B_CACHE) == 0) {
		    bp->b_cmd = BUF_CMD_READ;
		    vfs_busy_pages(vp, bp);
		    error = nfs_doio(vp, &bp->b_bio2, td);
		    if (error) {
			    brelse(bp);
		    }
		    while (error == NFSERR_BAD_COOKIE) {
			kprintf("got bad cookie vp %p bp %p\n", vp, bp);
			nfs_invaldir(vp);
			error = nfs_vinvalbuf(vp, 0, 1);
			/*
			 * Yuck! The directory has been modified on the
			 * server. The only way to get the block is by
			 * reading from the beginning to get all the
			 * offset cookies.
			 *
			 * Leave the last bp intact unless there is an error.
			 * Loop back up to the while if the error is another
			 * NFSERR_BAD_COOKIE (double yuch!).
			 */
			for (i = 0; i <= lbn && !error; i++) {
			    if (np->n_direofoffset
				&& (i * NFS_DIRBLKSIZ) >= np->n_direofoffset)
				    return (0);
			    bp = nfs_getcacheblk(vp, (off_t)i * NFS_DIRBLKSIZ,
						 NFS_DIRBLKSIZ, td);
			    if (!bp)
				return (EINTR);
			    if ((bp->b_flags & B_CACHE) == 0) {
				    bp->b_cmd = BUF_CMD_READ;
				    vfs_busy_pages(vp, bp);
				    error = nfs_doio(vp, &bp->b_bio2, td);
				    /*
				     * no error + B_INVAL == directory EOF,
				     * use the block.
				     */
				    if (error == 0 && (bp->b_flags & B_INVAL))
					    break;
			    }
			    /*
			     * An error will throw away the block and the
			     * for loop will break out.  If no error and this
			     * is not the block we want, we throw away the
			     * block and go for the next one via the for loop.
			     */
			    if (error || i < lbn)
				    brelse(bp);
			}
		    }
		    /*
		     * The above while is repeated if we hit another cookie
		     * error.  If we hit an error and it wasn't a cookie error,
		     * we give up.
		     */
		    if (error)
			    return (error);
		}

		/*
		 * If not eof and read aheads are enabled, start one.
		 * (You need the current block first, so that you have the
		 *  directory offset cookie of the next block.)
		 */
		if (nfs_numasync > 0 && nmp->nm_readahead > 0 &&
		    (bp->b_flags & B_INVAL) == 0 &&
		    (np->n_direofoffset == 0 ||
		    loffset + NFS_DIRBLKSIZ < np->n_direofoffset) &&
		    (np->n_flag & NDONTCACHE) == 0 &&
		    !findblk(vp, loffset + NFS_DIRBLKSIZ)) {
			rabp = nfs_getcacheblk(vp, loffset + NFS_DIRBLKSIZ,
					       NFS_DIRBLKSIZ, td);
			if (rabp) {
			    if ((rabp->b_flags & (B_CACHE|B_DELWRI)) == 0) {
				rabp->b_flags |= B_ASYNC;
				rabp->b_cmd = BUF_CMD_READ;
				vfs_busy_pages(vp, rabp);
				if (nfs_asyncio(vp, &rabp->b_bio2, td)) {
				    rabp->b_flags |= B_INVAL|B_ERROR;
				    vfs_unbusy_pages(rabp);
				    brelse(rabp);
				}
			    } else {
				brelse(rabp);
			    }
			}
		}
		/*
		 * Unlike VREG files, whos buffer size ( bp->b_bcount ) is
		 * chopped for the EOF condition, we cannot tell how large
		 * NFS directories are going to be until we hit EOF.  So
		 * an NFS directory buffer is *not* chopped to its EOF.  Now,
		 * it just so happens that b_resid will effectively chop it
		 * to EOF.  *BUT* this information is lost if the buffer goes
		 * away and is reconstituted into a B_CACHE state ( due to
		 * being VMIO ) later.  So we keep track of the directory eof
		 * in np->n_direofoffset and chop it off as an extra step 
		 * right here.
		 */
		n = lmin(uio->uio_resid, NFS_DIRBLKSIZ - bp->b_resid - on);
		if (np->n_direofoffset && n > np->n_direofoffset - uio->uio_offset)
			n = np->n_direofoffset - uio->uio_offset;
		break;
	    default:
		kprintf(" nfs_bioread: type %x unexpected\n",vp->v_type);
		break;
	    };

	    switch (vp->v_type) {
	    case VREG:
		if (n > 0)
		    error = uiomove(bp->b_data + on, (int)n, uio);
		break;
	    case VLNK:
		if (n > 0)
		    error = uiomove(bp->b_data + on, (int)n, uio);
		n = 0;
		break;
	    case VDIR:
		if (n > 0) {
		    off_t old_off = uio->uio_offset;
		    caddr_t cpos, epos;
		    struct nfs_dirent *dp;

		    /*
		     * We are casting cpos to nfs_dirent, it must be
		     * int-aligned.
		     */
		    if (on & 3) {
			error = EINVAL;
			break;
		    }

		    cpos = bp->b_data + on;
		    epos = bp->b_data + on + n;
		    while (cpos < epos && error == 0 && uio->uio_resid > 0) {
			    dp = (struct nfs_dirent *)cpos;
			    error = nfs_check_dirent(dp, (int)(epos - cpos));
			    if (error)
				    break;
			    if (vop_write_dirent(&error, uio, dp->nfs_ino,
				dp->nfs_type, dp->nfs_namlen, dp->nfs_name)) {
				    break;
			    }
			    cpos += dp->nfs_reclen;
		    }
		    n = 0;
		    if (error == 0)
			    uio->uio_offset = old_off + cpos - bp->b_data - on;
		}
		/*
		 * Invalidate buffer if caching is disabled, forcing a
		 * re-read from the remote later.
		 */
		if (np->n_flag & NDONTCACHE)
			bp->b_flags |= B_INVAL;
		break;
	    default:
		kprintf(" nfs_bioread: type %x unexpected\n",vp->v_type);
	    }
	    brelse(bp);
	} while (error == 0 && uio->uio_resid > 0 && n > 0);
	return (error);
}

/*
 * Userland can supply any 'seek' offset when reading a NFS directory.
 * Validate the structure so we don't panic the kernel.  Note that
 * the element name is nul terminated and the nul is not included
 * in nfs_namlen.
 */
static
int
nfs_check_dirent(struct nfs_dirent *dp, int maxlen)
{
	int nfs_name_off = offsetof(struct nfs_dirent, nfs_name[0]);

	if (nfs_name_off >= maxlen)
		return (EINVAL);
	if (dp->nfs_reclen < nfs_name_off || dp->nfs_reclen > maxlen)
		return (EINVAL);
	if (nfs_name_off + dp->nfs_namlen >= dp->nfs_reclen)
		return (EINVAL);
	if (dp->nfs_reclen & 3)
		return (EINVAL);
	return (0);
}

/*
 * Vnode op for write using bio
 *
 * nfs_write(struct vnode *a_vp, struct uio *a_uio, int a_ioflag,
 *	     struct ucred *a_cred)
 */
int
nfs_write(struct vop_write_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct thread *td = uio->uio_td;
	struct vnode *vp = ap->a_vp;
	struct nfsnode *np = VTONFS(vp);
	int ioflag = ap->a_ioflag;
	struct buf *bp;
	struct vattr vattr;
	struct nfsmount *nmp = VFSTONFS(vp->v_mount);
	daddr_t lbn;
	off_t loffset;
	int n, on, error = 0, iomode, must_commit;
	int haverslock = 0;
	int bcount;
	int biosize;

#ifdef DIAGNOSTIC
	if (uio->uio_rw != UIO_WRITE)
		panic("nfs_write mode");
	if (uio->uio_segflg == UIO_USERSPACE && uio->uio_td != curthread)
		panic("nfs_write proc");
#endif
	if (vp->v_type != VREG)
		return (EIO);
	if (np->n_flag & NWRITEERR) {
		np->n_flag &= ~NWRITEERR;
		return (np->n_error);
	}
	if ((nmp->nm_flag & NFSMNT_NFSV3) != 0 &&
	    (nmp->nm_state & NFSSTA_GOTFSINFO) == 0)
		(void)nfs_fsinfo(nmp, vp, td);

	/*
	 * Synchronously flush pending buffers if we are in synchronous
	 * mode or if we are appending.
	 */
	if (ioflag & (IO_APPEND | IO_SYNC)) {
		if (np->n_flag & NLMODIFIED) {
			np->n_attrstamp = 0;
			error = nfs_flush(vp, MNT_WAIT, td, 0);
			/* error = nfs_vinvalbuf(vp, V_SAVE, 1); */
			if (error)
				return (error);
		}
	}

	/*
	 * If IO_APPEND then load uio_offset.  We restart here if we cannot
	 * get the append lock.
	 */
restart:
	if (ioflag & IO_APPEND) {
		np->n_attrstamp = 0;
		error = VOP_GETATTR(vp, &vattr);
		if (error)
			return (error);
		uio->uio_offset = np->n_size;
	}

	if (uio->uio_offset < 0)
		return (EINVAL);
	if ((uio->uio_offset + uio->uio_resid) > nmp->nm_maxfilesize)
		return (EFBIG);
	if (uio->uio_resid == 0)
		return (0);

	/*
	 * We need to obtain the rslock if we intend to modify np->n_size
	 * in order to guarentee the append point with multiple contending
	 * writers, to guarentee that no other appenders modify n_size
	 * while we are trying to obtain a truncated buffer (i.e. to avoid
	 * accidently truncating data written by another appender due to
	 * the race), and to ensure that the buffer is populated prior to
	 * our extending of the file.  We hold rslock through the entire
	 * operation.
	 *
	 * Note that we do not synchronize the case where someone truncates
	 * the file while we are appending to it because attempting to lock
	 * this case may deadlock other parts of the system unexpectedly.
	 */
	if ((ioflag & IO_APPEND) ||
	    uio->uio_offset + uio->uio_resid > np->n_size) {
		switch(nfs_rslock(np)) {
		case ENOLCK:
			goto restart;
			/* not reached */
		case EINTR:
		case ERESTART:
			return(EINTR);
			/* not reached */
		default:
			break;
		}
		haverslock = 1;
	}

	/*
	 * Maybe this should be above the vnode op call, but so long as
	 * file servers have no limits, i don't think it matters
	 */
	if (td->td_proc && uio->uio_offset + uio->uio_resid >
	      td->td_proc->p_rlimit[RLIMIT_FSIZE].rlim_cur) {
		lwpsignal(td->td_proc, td->td_lwp, SIGXFSZ);
		if (haverslock)
			nfs_rsunlock(np);
		return (EFBIG);
	}

	biosize = vp->v_mount->mnt_stat.f_iosize;

	do {
		if ((np->n_flag & NDONTCACHE) && uio->uio_iovcnt == 1) {
		    iomode = NFSV3WRITE_FILESYNC;
		    error = nfs_writerpc(vp, uio, &iomode, &must_commit);
		    if (must_commit)
			    nfs_clearcommit(vp->v_mount);
		    break;
		}
		nfsstats.biocache_writes++;
		lbn = uio->uio_offset / biosize;
		on = uio->uio_offset & (biosize-1);
		loffset = uio->uio_offset - on;
		n = min((unsigned)(biosize - on), uio->uio_resid);
again:
		/*
		 * Handle direct append and file extension cases, calculate
		 * unaligned buffer size.
		 */

		if (uio->uio_offset == np->n_size && n) {
			/*
			 * Get the buffer (in its pre-append state to maintain
			 * B_CACHE if it was previously set).  Resize the
			 * nfsnode after we have locked the buffer to prevent
			 * readers from reading garbage.
			 */
			bcount = on;
			bp = nfs_getcacheblk(vp, loffset, bcount, td);

			if (bp != NULL) {
				long save;

				np->n_size = uio->uio_offset + n;
				np->n_flag |= NLMODIFIED;
				vnode_pager_setsize(vp, np->n_size);

				save = bp->b_flags & B_CACHE;
				bcount += n;
				allocbuf(bp, bcount);
				bp->b_flags |= save;
			}
		} else {
			/*
			 * Obtain the locked cache block first, and then 
			 * adjust the file's size as appropriate.
			 */
			bcount = on + n;
			if (loffset + bcount < np->n_size) {
				if (loffset + biosize < np->n_size)
					bcount = biosize;
				else
					bcount = np->n_size - loffset;
			}
			bp = nfs_getcacheblk(vp, loffset, bcount, td);
			if (uio->uio_offset + n > np->n_size) {
				np->n_size = uio->uio_offset + n;
				np->n_flag |= NLMODIFIED;
				vnode_pager_setsize(vp, np->n_size);
			}
		}

		if (bp == NULL) {
			error = EINTR;
			break;
		}

		/*
		 * Issue a READ if B_CACHE is not set.  In special-append
		 * mode, B_CACHE is based on the buffer prior to the write
		 * op and is typically set, avoiding the read.  If a read
		 * is required in special append mode, the server will
		 * probably send us a short-read since we extended the file
		 * on our end, resulting in b_resid == 0 and, thusly, 
		 * B_CACHE getting set.
		 *
		 * We can also avoid issuing the read if the write covers
		 * the entire buffer.  We have to make sure the buffer state
		 * is reasonable in this case since we will not be initiating
		 * I/O.  See the comments in kern/vfs_bio.c's getblk() for
		 * more information.
		 *
		 * B_CACHE may also be set due to the buffer being cached
		 * normally.
		 *
		 * When doing a UIO_NOCOPY write the buffer is not
		 * overwritten and we cannot just set B_CACHE unconditionally
		 * for full-block writes.
		 */

		if (on == 0 && n == bcount && uio->uio_segflg != UIO_NOCOPY) {
			bp->b_flags |= B_CACHE;
			bp->b_flags &= ~(B_ERROR | B_INVAL);
		}

		if ((bp->b_flags & B_CACHE) == 0) {
			bp->b_cmd = BUF_CMD_READ;
			vfs_busy_pages(vp, bp);
			error = nfs_doio(vp, &bp->b_bio2, td);
			if (error) {
				brelse(bp);
				break;
			}
		}
		if (!bp) {
			error = EINTR;
			break;
		}
		np->n_flag |= NLMODIFIED;

		/*
		 * If dirtyend exceeds file size, chop it down.  This should
		 * not normally occur but there is an append race where it
		 * might occur XXX, so we log it. 
		 *
		 * If the chopping creates a reverse-indexed or degenerate
		 * situation with dirtyoff/end, we 0 both of them.
		 */

		if (bp->b_dirtyend > bcount) {
			kprintf("NFS append race @%08llx:%d\n", 
			    (long long)bp->b_bio2.bio_offset,
			    bp->b_dirtyend - bcount);
			bp->b_dirtyend = bcount;
		}

		if (bp->b_dirtyoff >= bp->b_dirtyend)
			bp->b_dirtyoff = bp->b_dirtyend = 0;

		/*
		 * If the new write will leave a contiguous dirty
		 * area, just update the b_dirtyoff and b_dirtyend,
		 * otherwise force a write rpc of the old dirty area.
		 *
		 * While it is possible to merge discontiguous writes due to 
		 * our having a B_CACHE buffer ( and thus valid read data
		 * for the hole), we don't because it could lead to 
		 * significant cache coherency problems with multiple clients,
		 * especially if locking is implemented later on.
		 *
		 * as an optimization we could theoretically maintain
		 * a linked list of discontinuous areas, but we would still
		 * have to commit them separately so there isn't much
		 * advantage to it except perhaps a bit of asynchronization.
		 */

		if (bp->b_dirtyend > 0 &&
		    (on > bp->b_dirtyend || (on + n) < bp->b_dirtyoff)) {
			if (bwrite(bp) == EINTR) {
				error = EINTR;
				break;
			}
			goto again;
		}

		error = uiomove((char *)bp->b_data + on, n, uio);

		/*
		 * Since this block is being modified, it must be written
		 * again and not just committed.  Since write clustering does
		 * not work for the stage 1 data write, only the stage 2
		 * commit rpc, we have to clear B_CLUSTEROK as well.
		 */
		bp->b_flags &= ~(B_NEEDCOMMIT | B_CLUSTEROK);

		if (error) {
			bp->b_flags |= B_ERROR;
			brelse(bp);
			break;
		}

		/*
		 * Only update dirtyoff/dirtyend if not a degenerate 
		 * condition.
		 */
		if (n) {
			if (bp->b_dirtyend > 0) {
				bp->b_dirtyoff = min(on, bp->b_dirtyoff);
				bp->b_dirtyend = max((on + n), bp->b_dirtyend);
			} else {
				bp->b_dirtyoff = on;
				bp->b_dirtyend = on + n;
			}
			vfs_bio_set_validclean(bp, on, n);
		}

		/*
		 * If the lease is non-cachable or IO_SYNC do bwrite().
		 *
		 * IO_INVAL appears to be unused.  The idea appears to be
		 * to turn off caching in this case.  Very odd.  XXX
		 *
		 * If nfs_async is set bawrite() will use an unstable write
		 * (build dirty bufs on the server), so we might as well
		 * push it out with bawrite().  If nfs_async is not set we
		 * use bdwrite() to cache dirty bufs on the client.
		 */
		if ((np->n_flag & NDONTCACHE) || (ioflag & IO_SYNC)) {
			if (ioflag & IO_INVAL)
				bp->b_flags |= B_NOCACHE;
			error = bwrite(bp);
			if (error)
				break;
			if (np->n_flag & NDONTCACHE) {
				error = nfs_vinvalbuf(vp, V_SAVE, 1);
				if (error)
					break;
			}
		} else if ((n + on) == biosize && nfs_async) {
			bawrite(bp);
		} else {
			bdwrite(bp);
		}
	} while (uio->uio_resid > 0 && n > 0);

	if (haverslock)
		nfs_rsunlock(np);

	return (error);
}

/*
 * Get an nfs cache block.
 *
 * Allocate a new one if the block isn't currently in the cache
 * and return the block marked busy. If the calling process is
 * interrupted by a signal for an interruptible mount point, return
 * NULL.
 *
 * The caller must carefully deal with the possible B_INVAL state of
 * the buffer.  nfs_doio() clears B_INVAL (and nfs_asyncio() clears it
 * indirectly), so synchronous reads can be issued without worrying about
 * the B_INVAL state.  We have to be a little more careful when dealing
 * with writes (see comments in nfs_write()) when extending a file past
 * its EOF.
 */
static struct buf *
nfs_getcacheblk(struct vnode *vp, off_t loffset, int size, struct thread *td)
{
	struct buf *bp;
	struct mount *mp;
	struct nfsmount *nmp;

	mp = vp->v_mount;
	nmp = VFSTONFS(mp);

	if (nmp->nm_flag & NFSMNT_INT) {
		bp = getblk(vp, loffset, size, GETBLK_PCATCH, 0);
		while (bp == NULL) {
			if (nfs_sigintr(nmp, NULL, td))
				return (NULL);
			bp = getblk(vp, loffset, size, 0, 2 * hz);
		}
	} else {
		bp = getblk(vp, loffset, size, 0, 0);
	}

	/*
	 * bio2, the 'device' layer.  Since BIOs use 64 bit byte offsets
	 * now, no translation is necessary.
	 */
	bp->b_bio2.bio_offset = loffset;
	return (bp);
}

/*
 * Flush and invalidate all dirty buffers. If another process is already
 * doing the flush, just wait for completion.
 */
int
nfs_vinvalbuf(struct vnode *vp, int flags, int intrflg)
{
	struct nfsnode *np = VTONFS(vp);
	struct nfsmount *nmp = VFSTONFS(vp->v_mount);
	int error = 0, slpflag, slptimeo;
	thread_t td = curthread;

	if (vp->v_flag & VRECLAIMED)
		return (0);

	if ((nmp->nm_flag & NFSMNT_INT) == 0)
		intrflg = 0;
	if (intrflg) {
		slpflag = PCATCH;
		slptimeo = 2 * hz;
	} else {
		slpflag = 0;
		slptimeo = 0;
	}
	/*
	 * First wait for any other process doing a flush to complete.
	 */
	while (np->n_flag & NFLUSHINPROG) {
		np->n_flag |= NFLUSHWANT;
		error = tsleep((caddr_t)&np->n_flag, 0, "nfsvinval", slptimeo);
		if (error && intrflg && nfs_sigintr(nmp, NULL, td))
			return (EINTR);
	}

	/*
	 * Now, flush as required.
	 */
	np->n_flag |= NFLUSHINPROG;
	error = vinvalbuf(vp, flags, slpflag, 0);
	while (error) {
		if (intrflg && nfs_sigintr(nmp, NULL, td)) {
			np->n_flag &= ~NFLUSHINPROG;
			if (np->n_flag & NFLUSHWANT) {
				np->n_flag &= ~NFLUSHWANT;
				wakeup((caddr_t)&np->n_flag);
			}
			return (EINTR);
		}
		error = vinvalbuf(vp, flags, 0, slptimeo);
	}
	np->n_flag &= ~(NLMODIFIED | NFLUSHINPROG);
	if (np->n_flag & NFLUSHWANT) {
		np->n_flag &= ~NFLUSHWANT;
		wakeup((caddr_t)&np->n_flag);
	}
	return (0);
}

/*
 * Initiate asynchronous I/O. Return an error if no nfsiods are available.
 * This is mainly to avoid queueing async I/O requests when the nfsiods
 * are all hung on a dead server.
 *
 * Note: nfs_asyncio() does not clear (B_ERROR|B_INVAL) but when the bp
 * is eventually dequeued by the async daemon, nfs_doio() *will*.
 */
int
nfs_asyncio(struct vnode *vp, struct bio *bio, struct thread *td)
{
	struct buf *bp = bio->bio_buf;
	struct nfsmount *nmp;
	int i;
	int gotiod;
	int slpflag = 0;
	int slptimeo = 0;
	int error;

	/*
	 * If no async daemons then return EIO to force caller to run the rpc
	 * synchronously.
	 */
	if (nfs_numasync == 0)
		return (EIO);

	KKASSERT(vp->v_tag == VT_NFS);
	nmp = VFSTONFS(vp->v_mount);

	/*
	 * Commits are usually short and sweet so lets save some cpu and 
	 * leave the async daemons for more important rpc's (such as reads
	 * and writes).
	 */
	if (bp->b_cmd == BUF_CMD_WRITE && (bp->b_flags & B_NEEDCOMMIT) &&
	    (nmp->nm_bioqiods > nfs_numasync / 2)) {
		return(EIO);
	}

again:
	if (nmp->nm_flag & NFSMNT_INT)
		slpflag = PCATCH;
	gotiod = FALSE;

	/*
	 * Find a free iod to process this request.
	 */
	for (i = 0; i < NFS_MAXASYNCDAEMON; i++)
		if (nfs_iodwant[i]) {
			/*
			 * Found one, so wake it up and tell it which
			 * mount to process.
			 */
			NFS_DPF(ASYNCIO,
				("nfs_asyncio: waking iod %d for mount %p\n",
				 i, nmp));
			nfs_iodwant[i] = NULL;
			nfs_iodmount[i] = nmp;
			nmp->nm_bioqiods++;
			wakeup((caddr_t)&nfs_iodwant[i]);
			gotiod = TRUE;
			break;
		}

	/*
	 * If none are free, we may already have an iod working on this mount
	 * point.  If so, it will process our request.
	 */
	if (!gotiod) {
		if (nmp->nm_bioqiods > 0) {
			NFS_DPF(ASYNCIO,
				("nfs_asyncio: %d iods are already processing mount %p\n",
				 nmp->nm_bioqiods, nmp));
			gotiod = TRUE;
		}
	}

	/*
	 * If we have an iod which can process the request, then queue
	 * the buffer.
	 */
	if (gotiod) {
		/*
		 * Ensure that the queue never grows too large.  We still want
		 * to asynchronize so we block rather then return EIO.
		 */
		while (nmp->nm_bioqlen >= 2*nfs_numasync) {
			NFS_DPF(ASYNCIO,
				("nfs_asyncio: waiting for mount %p queue to drain\n", nmp));
			nmp->nm_bioqwant = TRUE;
			error = tsleep(&nmp->nm_bioq, slpflag,
				       "nfsaio", slptimeo);
			if (error) {
				if (nfs_sigintr(nmp, NULL, td))
					return (EINTR);
				if (slpflag == PCATCH) {
					slpflag = 0;
					slptimeo = 2 * hz;
				}
			}
			/*
			 * We might have lost our iod while sleeping,
			 * so check and loop if nescessary.
			 */
			if (nmp->nm_bioqiods == 0) {
				NFS_DPF(ASYNCIO,
					("nfs_asyncio: no iods after mount %p queue was drained, looping\n", nmp));
				goto again;
			}
		}
		BUF_KERNPROC(bp);

		/*
		 * The passed bio's buffer is not necessary associated with
		 * the NFS vnode it is being written to.  Store the NFS vnode
		 * in the BIO driver info.
		 */
		bio->bio_driver_info = vp;
		TAILQ_INSERT_TAIL(&nmp->nm_bioq, bio, bio_act);
		nmp->nm_bioqlen++;
		return (0);
	}

	/*
	 * All the iods are busy on other mounts, so return EIO to
	 * force the caller to process the i/o synchronously.
	 */
	NFS_DPF(ASYNCIO, ("nfs_asyncio: no iods available, i/o is synchronous\n"));
	return (EIO);
}

/*
 * Do an I/O operation to/from a cache block. This may be called
 * synchronously or from an nfsiod.  The BIO is normalized for DEV_BSIZE.
 *
 * NOTE! TD MIGHT BE NULL
 */
int
nfs_doio(struct vnode *vp, struct bio *bio, struct thread *td)
{
	struct buf *bp = bio->bio_buf;
	struct uio *uiop;
	struct nfsnode *np;
	struct nfsmount *nmp;
	int error = 0, iomode, must_commit = 0;
	struct uio uio;
	struct iovec io;

	KKASSERT(vp->v_tag == VT_NFS);
	np = VTONFS(vp);
	nmp = VFSTONFS(vp->v_mount);
	uiop = &uio;
	uiop->uio_iov = &io;
	uiop->uio_iovcnt = 1;
	uiop->uio_segflg = UIO_SYSSPACE;
	uiop->uio_td = td;

	/*
	 * clear B_ERROR and B_INVAL state prior to initiating the I/O.  We
	 * do this here so we do not have to do it in all the code that
	 * calls us.
	 */
	bp->b_flags &= ~(B_ERROR | B_INVAL);


	KASSERT(bp->b_cmd != BUF_CMD_DONE, 
		("nfs_doio: bp %p already marked done!", bp));

	if (bp->b_cmd == BUF_CMD_READ) {
	    io.iov_len = uiop->uio_resid = bp->b_bcount;
	    io.iov_base = bp->b_data;
	    uiop->uio_rw = UIO_READ;

	    switch (vp->v_type) {
	    case VREG:
		uiop->uio_offset = bio->bio_offset;
		nfsstats.read_bios++;
		error = nfs_readrpc(vp, uiop);

		if (!error) {
		    if (uiop->uio_resid) {
			/*
			 * If we had a short read with no error, we must have
			 * hit a file hole.  We should zero-fill the remainder.
			 * This can also occur if the server hits the file EOF.
			 *
			 * Holes used to be able to occur due to pending 
			 * writes, but that is not possible any longer.
			 */
			int nread = bp->b_bcount - uiop->uio_resid;
			int left  = uiop->uio_resid;

			if (left > 0)
				bzero((char *)bp->b_data + nread, left);
			uiop->uio_resid = 0;
		    }
		}
		if (td && td->td_proc && (vp->v_flag & VTEXT) &&
		    np->n_mtime != np->n_vattr.va_mtime.tv_sec) {
			uprintf("Process killed due to text file modification\n");
			ksignal(td->td_proc, SIGKILL);
		}
		break;
	    case VLNK:
		uiop->uio_offset = 0;
		nfsstats.readlink_bios++;
		error = nfs_readlinkrpc(vp, uiop);
		break;
	    case VDIR:
		nfsstats.readdir_bios++;
		uiop->uio_offset = bio->bio_offset;
		if (nmp->nm_flag & NFSMNT_RDIRPLUS) {
			error = nfs_readdirplusrpc(vp, uiop);
			if (error == NFSERR_NOTSUPP)
				nmp->nm_flag &= ~NFSMNT_RDIRPLUS;
		}
		if ((nmp->nm_flag & NFSMNT_RDIRPLUS) == 0)
			error = nfs_readdirrpc(vp, uiop);
		/*
		 * end-of-directory sets B_INVAL but does not generate an
		 * error.
		 */
		if (error == 0 && uiop->uio_resid == bp->b_bcount)
			bp->b_flags |= B_INVAL;
		break;
	    default:
		kprintf("nfs_doio:  type %x unexpected\n",vp->v_type);
		break;
	    };
	    if (error) {
		bp->b_flags |= B_ERROR;
		bp->b_error = error;
	    }
	} else {
	    /* 
	     * If we only need to commit, try to commit
	     */
	    KKASSERT(bp->b_cmd == BUF_CMD_WRITE);
	    if (bp->b_flags & B_NEEDCOMMIT) {
		    int retv;
		    off_t off;

		    off = bio->bio_offset + bp->b_dirtyoff;
		    retv = nfs_commit(vp, off, 
				bp->b_dirtyend - bp->b_dirtyoff, td);
		    if (retv == 0) {
			    bp->b_dirtyoff = bp->b_dirtyend = 0;
			    bp->b_flags &= ~(B_NEEDCOMMIT | B_CLUSTEROK);
			    bp->b_resid = 0;
			    biodone(bio);
			    return (0);
		    }
		    if (retv == NFSERR_STALEWRITEVERF) {
			    nfs_clearcommit(vp->v_mount);
		    }
	    }

	    /*
	     * Setup for actual write
	     */

	    if (bio->bio_offset + bp->b_dirtyend > np->n_size)
		bp->b_dirtyend = np->n_size - bio->bio_offset;

	    if (bp->b_dirtyend > bp->b_dirtyoff) {
		io.iov_len = uiop->uio_resid = bp->b_dirtyend
		    - bp->b_dirtyoff;
		uiop->uio_offset = bio->bio_offset + bp->b_dirtyoff;
		io.iov_base = (char *)bp->b_data + bp->b_dirtyoff;
		uiop->uio_rw = UIO_WRITE;
		nfsstats.write_bios++;

		if ((bp->b_flags & (B_ASYNC | B_NEEDCOMMIT | B_NOCACHE | B_CLUSTER)) == B_ASYNC)
		    iomode = NFSV3WRITE_UNSTABLE;
		else
		    iomode = NFSV3WRITE_FILESYNC;

		error = nfs_writerpc(vp, uiop, &iomode, &must_commit);

		/*
		 * When setting B_NEEDCOMMIT also set B_CLUSTEROK to try
		 * to cluster the buffers needing commit.  This will allow
		 * the system to submit a single commit rpc for the whole
		 * cluster.  We can do this even if the buffer is not 100% 
		 * dirty (relative to the NFS blocksize), so we optimize the
		 * append-to-file-case.
		 *
		 * (when clearing B_NEEDCOMMIT, B_CLUSTEROK must also be
		 * cleared because write clustering only works for commit
		 * rpc's, not for the data portion of the write).
		 */

		if (!error && iomode == NFSV3WRITE_UNSTABLE) {
		    bp->b_flags |= B_NEEDCOMMIT;
		    if (bp->b_dirtyoff == 0
			&& bp->b_dirtyend == bp->b_bcount)
			bp->b_flags |= B_CLUSTEROK;
		} else {
		    bp->b_flags &= ~(B_NEEDCOMMIT | B_CLUSTEROK);
		}

		/*
		 * For an interrupted write, the buffer is still valid
		 * and the write hasn't been pushed to the server yet,
		 * so we can't set B_ERROR and report the interruption
		 * by setting B_EINTR. For the B_ASYNC case, B_EINTR
		 * is not relevant, so the rpc attempt is essentially
		 * a noop.  For the case of a V3 write rpc not being
		 * committed to stable storage, the block is still
		 * dirty and requires either a commit rpc or another
		 * write rpc with iomode == NFSV3WRITE_FILESYNC before
		 * the block is reused. This is indicated by setting
		 * the B_DELWRI and B_NEEDCOMMIT flags.
		 *
		 * If the buffer is marked B_PAGING, it does not reside on
		 * the vp's paging queues so we cannot call bdirty().  The
		 * bp in this case is not an NFS cache block so we should
		 * be safe. XXX
		 */
    		if (error == EINTR
		    || (!error && (bp->b_flags & B_NEEDCOMMIT))) {
			crit_enter();
			bp->b_flags &= ~(B_INVAL|B_NOCACHE);
			if ((bp->b_flags & B_PAGING) == 0)
			    bdirty(bp);
			if (error && (bp->b_flags & B_ASYNC) == 0)
			    bp->b_flags |= B_EINTR;
			crit_exit();
	    	} else {
		    if (error) {
			bp->b_flags |= B_ERROR;
			bp->b_error = np->n_error = error;
			np->n_flag |= NWRITEERR;
		    }
		    bp->b_dirtyoff = bp->b_dirtyend = 0;
		}
	    } else {
		bp->b_resid = 0;
		biodone(bio);
		return (0);
	    }
	}
	bp->b_resid = uiop->uio_resid;
	if (must_commit)
	    nfs_clearcommit(vp->v_mount);
	biodone(bio);
	return (error);
}

/*
 * Used to aid in handling ftruncate() operations on the NFS client side.
 * Truncation creates a number of special problems for NFS.  We have to
 * throw away VM pages and buffer cache buffers that are beyond EOF, and
 * we have to properly handle VM pages or (potentially dirty) buffers
 * that straddle the truncation point.
 */

int
nfs_meta_setsize(struct vnode *vp, struct thread *td, u_quad_t nsize)
{
	struct nfsnode *np = VTONFS(vp);
	u_quad_t tsize = np->n_size;
	int biosize = vp->v_mount->mnt_stat.f_iosize;
	int error = 0;

	np->n_size = nsize;

	if (np->n_size < tsize) {
		struct buf *bp;
		daddr_t lbn;
		off_t loffset;
		int bufsize;

		/*
		 * vtruncbuf() doesn't get the buffer overlapping the 
		 * truncation point.  We may have a B_DELWRI and/or B_CACHE
		 * buffer that now needs to be truncated.
		 */
		error = vtruncbuf(vp, nsize, biosize);
		lbn = nsize / biosize;
		bufsize = nsize & (biosize - 1);
		loffset = nsize - bufsize;
		bp = nfs_getcacheblk(vp, loffset, bufsize, td);
		if (bp->b_dirtyoff > bp->b_bcount)
			bp->b_dirtyoff = bp->b_bcount;
		if (bp->b_dirtyend > bp->b_bcount)
			bp->b_dirtyend = bp->b_bcount;
		bp->b_flags |= B_RELBUF;  /* don't leave garbage around */
		brelse(bp);
	} else {
		vnode_pager_setsize(vp, nsize);
	}
	return(error);
}

