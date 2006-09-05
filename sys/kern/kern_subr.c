/*
 * Copyright (c) 1982, 1986, 1991, 1993
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
 *	@(#)kern_subr.c	8.3 (Berkeley) 1/21/94
 * $FreeBSD: src/sys/kern/kern_subr.c,v 1.31.2.2 2002/04/21 08:09:37 bde Exp $
 * $DragonFly: src/sys/kern/kern_subr.c,v 1.24 2006/09/05 00:55:45 dillon Exp $
 */

#include "opt_ddb.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/resourcevar.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <machine/limits.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>

SYSCTL_INT(_kern, KERN_IOV_MAX, iov_max, CTLFLAG_RD, NULL, UIO_MAXIOV,
	"Maximum number of elements in an I/O vector; sysconf(_SC_IOV_MAX)");

/*
 * UIO_READ:	copy the kernelspace cp to the user or kernelspace UIO
 * UIO_WRITE:	copy the user or kernelspace UIO to the kernelspace cp
 *
 * For userspace UIO's, uio_td must be the current thread.
 */
int
uiomove(caddr_t cp, int n, struct uio *uio)
{
	struct iovec *iov;
	u_int cnt;
	int error = 0;
	int save = 0;
	int baseticks = ticks;

	KASSERT(uio->uio_rw == UIO_READ || uio->uio_rw == UIO_WRITE,
	    ("uiomove: mode"));
	KASSERT(uio->uio_segflg != UIO_USERSPACE || uio->uio_td == curthread,
	    ("uiomove proc"));

	if (curproc) {
		save = curproc->p_flag & P_DEADLKTREAT;
		curproc->p_flag |= P_DEADLKTREAT;
	}

	while (n > 0 && uio->uio_resid) {
		iov = uio->uio_iov;
		cnt = iov->iov_len;
		if (cnt == 0) {
			uio->uio_iov++;
			uio->uio_iovcnt--;
			continue;
		}
		if (cnt > n)
			cnt = n;

		switch (uio->uio_segflg) {

		case UIO_USERSPACE:
			if (ticks - baseticks >= hogticks) {
				uio_yield();
				baseticks = ticks;
			}
			if (uio->uio_rw == UIO_READ)
				error = copyout(cp, iov->iov_base, cnt);
			else
				error = copyin(iov->iov_base, cp, cnt);
			if (error)
				break;
			break;

		case UIO_SYSSPACE:
			if (uio->uio_rw == UIO_READ)
				bcopy((caddr_t)cp, iov->iov_base, cnt);
			else
				bcopy(iov->iov_base, (caddr_t)cp, cnt);
			break;
		case UIO_NOCOPY:
			break;
		}
		iov->iov_base += cnt;
		iov->iov_len -= cnt;
		uio->uio_resid -= cnt;
		uio->uio_offset += cnt;
		cp += cnt;
		n -= cnt;
	}
	if (curproc)
		curproc->p_flag = (curproc->p_flag & ~P_DEADLKTREAT) | save;
	return (error);
}
/*
 * Wrapper for uiomove() that validates the arguments against a known-good
 * kernel buffer.  Currently, uiomove accepts a signed (n) argument, which
 * is almost definitely a bad thing, so we catch that here as well.  We
 * return a runtime failure, but it might be desirable to generate a runtime
 * assertion failure instead.
 */
int
uiomove_frombuf(void *buf, int buflen, struct uio *uio)
{
	unsigned int offset, n;

	if (uio->uio_offset < 0 || uio->uio_resid < 0 ||
	    (offset = uio->uio_offset) != uio->uio_offset)
		return (EINVAL);
	if (buflen <= 0 || offset >= buflen)
		return (0);
	if ((n = buflen - offset) > INT_MAX)
		return (EINVAL);
	return (uiomove((char *)buf + offset, n, uio));
}


int
uiomoveco(cp, n, uio, obj)
	caddr_t cp;
	int n;
	struct uio *uio;
	struct vm_object *obj;
{
	struct iovec *iov;
	u_int cnt;
	int error;
	int baseticks = ticks;

	KASSERT(uio->uio_rw == UIO_READ || uio->uio_rw == UIO_WRITE,
	    ("uiomoveco: mode"));
	KASSERT(uio->uio_segflg != UIO_USERSPACE || uio->uio_td == curthread,
	    ("uiomoveco proc"));

	while (n > 0 && uio->uio_resid) {
		iov = uio->uio_iov;
		cnt = iov->iov_len;
		if (cnt == 0) {
			uio->uio_iov++;
			uio->uio_iovcnt--;
			continue;
		}
		if (cnt > n)
			cnt = n;

		switch (uio->uio_segflg) {

		case UIO_USERSPACE:
			if (ticks - baseticks >= hogticks) {
				uio_yield();
				baseticks = ticks;
			}
			if (uio->uio_rw == UIO_READ) {
				error = copyout(cp, iov->iov_base, cnt);
			} else {
				error = copyin(iov->iov_base, cp, cnt);
			}
			if (error)
				return (error);
			break;

		case UIO_SYSSPACE:
			if (uio->uio_rw == UIO_READ)
				bcopy((caddr_t)cp, iov->iov_base, cnt);
			else
				bcopy(iov->iov_base, (caddr_t)cp, cnt);
			break;
		case UIO_NOCOPY:
			break;
		}
		iov->iov_base += cnt;
		iov->iov_len -= cnt;
		uio->uio_resid -= cnt;
		uio->uio_offset += cnt;
		cp += cnt;
		n -= cnt;
	}
	return (0);
}

/*
 * Give next character to user as result of read.
 */
int
ureadc(c, uio)
	int c;
	struct uio *uio;
{
	struct iovec *iov;

again:
	if (uio->uio_iovcnt == 0 || uio->uio_resid == 0)
		panic("ureadc");
	iov = uio->uio_iov;
	if (iov->iov_len == 0) {
		uio->uio_iovcnt--;
		uio->uio_iov++;
		goto again;
	}
	switch (uio->uio_segflg) {

	case UIO_USERSPACE:
		if (subyte(iov->iov_base, c) < 0)
			return (EFAULT);
		break;

	case UIO_SYSSPACE:
		*iov->iov_base = c;
		break;

	case UIO_NOCOPY:
		break;
	}
	iov->iov_base++;
	iov->iov_len--;
	uio->uio_resid--;
	uio->uio_offset++;
	return (0);
}

/*
 * General routine to allocate a hash table.  Make the hash table size a
 * power of 2 greater or equal to the number of elements requested, and 
 * store the masking value in *hashmask.
 */
void *
hashinit(elements, type, hashmask)
	int elements;
	struct malloc_type *type;
	u_long *hashmask;
{
	long hashsize;
	LIST_HEAD(generic, generic) *hashtbl;
	int i;

	if (elements <= 0)
		panic("hashinit: bad elements");
	for (hashsize = 2; hashsize < elements; hashsize <<= 1)
		continue;
	hashtbl = kmalloc((u_long)hashsize * sizeof(*hashtbl), type, M_WAITOK);
	for (i = 0; i < hashsize; i++)
		LIST_INIT(&hashtbl[i]);
	*hashmask = hashsize - 1;
	return (hashtbl);
}

static int primes[] = { 1, 13, 31, 61, 127, 251, 509, 761, 1021, 1531, 2039,
			2557, 3067, 3583, 4093, 4603, 5119, 5623, 6143, 6653,
			7159, 7673, 8191, 12281, 16381, 24571, 32749 };
#define NPRIMES (sizeof(primes) / sizeof(primes[0]))

/*
 * General routine to allocate a prime number sized hash table.
 */
void *
phashinit(elements, type, nentries)
	int elements;
	struct malloc_type *type;
	u_long *nentries;
{
	long hashsize;
	LIST_HEAD(generic, generic) *hashtbl;
	int i;

	if (elements <= 0)
		panic("phashinit: bad elements");
	for (i = 1, hashsize = primes[1]; hashsize <= elements;) {
		i++;
		if (i == NPRIMES)
			break;
		hashsize = primes[i];
	}
	hashsize = primes[i - 1];
	hashtbl = kmalloc((u_long)hashsize * sizeof(*hashtbl), type, M_WAITOK);
	for (i = 0; i < hashsize; i++)
		LIST_INIT(&hashtbl[i]);
	*nentries = hashsize;
	return (hashtbl);
}

/*
 * Copyin an iovec.  If the iovec array fits, use the preallocated small
 * iovec structure.  If it is too big, dynamically allocate an iovec array
 * of sufficient size.
 *
 * MPSAFE
 */
int
iovec_copyin(struct iovec *uiov, struct iovec **kiov, struct iovec *siov,
	     size_t iov_cnt, int *iov_len)
{
	struct iovec *iovp;
	int error, i;

	if (iov_cnt >= UIO_MAXIOV)
		return EMSGSIZE;
	if (iov_cnt >= UIO_SMALLIOV) {
		MALLOC(*kiov, struct iovec *, sizeof(struct iovec) * iov_cnt,
		    M_IOV, M_WAITOK);
	} else {
		*kiov = siov;
	}
	error = copyin(uiov, *kiov, iov_cnt * sizeof(struct iovec));
	if (error == 0) {
		*iov_len = 0;
		for (i = 0, iovp = *kiov; i < iov_cnt; i++, iovp++) {
			/*
			 * Check for both *iov_len overflows and out of
			 * range iovp->iov_len's.  We limit to the
			 * capabilities of signed integers.
			 */
			if (*iov_len + (int)iovp->iov_len < *iov_len)
				error = EINVAL;
			*iov_len += (int)iovp->iov_len;
		}
	}
	if (error)
		iovec_free(kiov, siov);
	return (error);
}
