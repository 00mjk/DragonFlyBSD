/*
 *  modified for Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
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
 *	@(#)ext2_subr.c	8.2 (Berkeley) 9/21/93
 * $FreeBSD: src/sys/gnu/ext2fs/ext2_subr.c,v 1.13.2.2 2000/08/03 18:48:27 peter Exp $
 * $DragonFly: src/sys/vfs/gnu/ext2fs/ext2_subr.c,v 1.10 2006/03/24 18:35:33 dillon Exp $
 */

#include <sys/param.h>
#include "ext2_fs_sb.h"
#include "fs.h"

#include <sys/lock.h>
#include <sys/systm.h>
#include <sys/ucred.h>
#include <sys/vnode.h>
#include "ext2_extern.h"
#include <sys/buf.h>
#include <vfs/ufs/quota.h>
#include <vfs/ufs/inode.h>

#include "opt_ddb.h"

#ifdef DDB
void	ext2_checkoverlap (struct buf *, struct inode *);
#endif

/*
 * Return buffer with the contents of block "offset" from the beginning of
 * directory "ip".  If "res" is non-zero, fill it in with a pointer to the
 * remaining space in the directory.
 */
int
ext2_blkatoff(struct vnode *vp, off_t offset, char **res, struct buf **bpp)
{
	struct inode *ip;
	struct ext2_sb_info *fs;
	struct buf *bp;
	daddr_t lbn;
	int bsize, error;

	ip = VTOI(vp);
	fs = ip->i_e2fs;
	lbn = lblkno(fs, offset);
	bsize = blksize(fs, ip, lbn);

	*bpp = NULL;
	if ((error = bread(vp, lblktodoff(fs, lbn), bsize, &bp)) != 0) {
		brelse(bp);
		return (error);
	}
	if (res)
		*res = (char *)bp->b_data + blkoff(fs, offset);
	*bpp = bp;
	return (0);
}

#ifdef DDB
void
ext2_checkoverlap(struct buf *bp, struct inode *ip)
{
	struct buf *ebp, *ep;
	off_t start;
	off_t last;
	struct vnode *vp;

	ebp = &buf[nbuf];
	start = bp->b_bio2.bio_offset;
	last = start + bp->b_bcount - 1;
	for (ep = buf; ep < ebp; ep++) {
		if (ep == bp || (ep->b_flags & B_INVAL) ||
		    ep->b_vp == NULLVP || ep->b_bio2.bio_offset == NOOFFSET)
			continue;
		if (VOP_BMAP(ep->b_vp, (off_t)0, &vp, NULL, NULL, NULL))
			continue;
		if (vp != ip->i_devvp)
			continue;
		/* look for overlap */
		if (ep->b_bcount == 0 || ep->b_bio2.bio_offset > last ||
		    ep->b_bio2.bio_offset + ep->b_bcount <= start)
			continue;
		vprint("Disk overlap", vp);
		printf("\tstart %lld, end %lld overlap start %lld, end %lld\n",
			start, last, ep->b_bio2.bio_offset,
			ep->b_bio2.bio_offset + ep->b_bcount - 1);
		panic("Disk buffer overlap");
	}
}
#endif /* DDB */
