/*
 * Copyright (c) 2003 Daniel Eischen <deischen@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libpthread/arch/amd64/amd64/pthread_md.c,v 1.4 2004/11/06 03:33:19 peter Exp $
 * $DragonFly: src/lib/libthread_xu/arch/amd64/amd64/pthread_md.c,v 1.2 2005/03/28 03:33:13 dillon Exp $
 */

#include <stdlib.h>
#include <strings.h>
#include "rtld_tls.h"
#include "pthread_md.h"

/*
 * The constructors.
 */
struct tcb *
_tcb_ctor(struct pthread *thread, int initial)
{
	struct tcb *old_tcb;
	struct tcb *tcb;
	int flags;

	old_tcb = NULL;
	flags = 0;

	if (initial) {
		/* 
		 * We may have to replace a TLS already created by the low
		 * level libc startup code
		 */
		struct tls_info info;
		if (sys_get_tls_area(0, &info, sizeof(info)) == 0) {
			old_tcb = info.base;
			flags = RTLD_ALLOC_TLS_FREE_OLD;
		}
	}
	tcb = _rtld_allocate_tls(old_tcb, sizeof(struct tcb), flags);
	if (tcb) {
		tcb->tcb_thread = thread;
	}

	return (tcb);
}

void
_tcb_dtor(struct tcb *tcb)
{
	_rtld_free_tls(tcb, sizeof(struct tcb), 16);
}
