/*-
 * Copyright (c) 2002 Daniel Eischen <deischen@freebsd.org>.
 * Copyright (c) 2005 David Xu <davidxu@freebsd.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libpthread/arch/i386/include/pthread_md.h,v 1.13 2004/11/06 03:35:51 peter Exp $
 * $DragonFly: src/lib/libthread_xu/arch/i386/include/pthread_md.h,v 1.5 2005/03/29 19:26:20 joerg Exp $
 */

/*
 * Machine-dependent thread prototypes/definitions for the thread kernel.
 */
#ifndef _PTHREAD_MD_H_
#define	_PTHREAD_MD_H_

#include <stddef.h>
#include <sys/types.h>
#include <machine/tls.h>

struct pthread;

static __inline int
atomic_cmpset_int(volatile int *dst, int exp, int src)
{
	int res = exp;

	__asm __volatile (
	"	lock cmpxchgl %1,%2 ;	"
	"       setz	%%al ;		"
	"	movzbl	%%al,%0 ;	"
	"1:				"
	"# atomic_cmpset_int"
	: "+a" (res)			/* 0 (result) */
	: "r" (src),			/* 1 */
	  "m" (*(dst))			/* 2 */
	: "memory");				 

	return (res);
}

#define atomic_cmpset_acq_int	atomic_cmpset_int

/*
 * The constructors.
 */
struct tls_tcb	*_tcb_ctor(struct pthread *, int);
void		_tcb_dtor(struct tls_tcb *tcb);

#endif
