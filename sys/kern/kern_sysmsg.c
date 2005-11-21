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
 * $DragonFly: src/sys/kern/Attic/kern_sysmsg.c,v 1.6 2005/11/21 21:59:50 dillon Exp $
 */

/*
 * SYSMSG is our system call message encapsulation and handling subsystem.
 * System calls are now encapsulated in messages.  A system call can execute
 * synchronously or asynchronously.  If a system call wishes to run 
 * asynchronously it returns EASYNC and the process records the pending system
 * call message in p_sysmsgq.
 *
 * SYSMSGs work similarly to LWKT messages in that the originator can request
 * a synchronous or asynchronous operation in isolation from the actual system
 * call which can choose to run the system call synchronous or asynchronously
 * (independant of what was requested).  Like LWKT messages, the synchronous
 * path avoids all queueing operations and is almost as fast as making a 
 * direct procedure call.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/pioctl.h>
#include <sys/kernel.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/uio.h>
#include <sys/vmmeter.h>
#include <sys/malloc.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif
#include <sys/upcall.h>
#include <sys/sysproto.h>
#include <sys/sysunion.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_extern.h>

#include <sys/msgport2.h>
#include <sys/thread2.h>

/*
 * Sysctl to limit max in progress syscall messages per process. 0 for
 * unlimited.
 */
int max_sysmsg = 0;
SYSCTL_INT(_kern, OID_AUTO, max_sysmsg, CTLFLAG_RW, &max_sysmsg, 0,
                "Max sysmsg's a process can have running");

/*
 * Wait for a system call message to be returned.  If NULL is passed we
 * wait for the next ready sysmsg and return it.  We return NULL if there
 * are no pending sysmsgs queued.
 *
 * NOTE: proc must be curproc.
 *
 * MPSAFE
 */
struct sysmsg *
sysmsg_wait(struct lwp *lp, struct sysmsg *sysmsg, int nonblock)
{
	thread_t td = lp->lwp_thread;

	/*
	 * Get the next finished system call or the specified system call,
	 * blocking until it is finished (if requested).
	 */
	if (sysmsg == NULL) {
		if (TAILQ_FIRST(&lp->lwp_sysmsgq) == NULL)
			return(NULL);
		if (nonblock) {
			if ((sysmsg = lwkt_getport(&td->td_msgport)) == NULL)
				return(NULL);
		} else {
			sysmsg = lwkt_waitport(&td->td_msgport, NULL);
		}
	} else {
		if (nonblock && !lwkt_checkmsg(&sysmsg->lmsg))
			return(NULL);
		lwkt_waitport(&td->td_msgport, &sysmsg->lmsg);
	}

	/*
	 * sysmsg is not NULL here
	 */
	TAILQ_REMOVE(&lp->lwp_sysmsgq, sysmsg, msgq);
	lp->lwp_nsysmsg--;
	return(sysmsg);
}

/*
 * Wait for all pending asynchronous system calls to complete, aborting them
 * if requested (XXX).
 */
void
sysmsg_rundown(struct lwp *lp, int doabort)
{
	struct sysmsg *sysmsg;
	thread_t td = lp->lwp_thread;
	globaldata_t gd = td->td_gd;

	while (TAILQ_FIRST(&lp->lwp_sysmsgq) != NULL) {
		printf("WAITSYSMSG\n");
		sysmsg = sysmsg_wait(lp, NULL, 0);
		printf("WAITSYSMSG %p\n", sysmsg);
		KKASSERT(sysmsg != NULL);
		/* XXX don't bother with pending copyouts */
		/* XXX we really should do pending copyouts */
		crit_enter_quick(td);
		sysmsg->lmsg.opaque.ms_sysunnext = gd->gd_freesysun;
		gd->gd_freesysun = (void *)sysmsg;
		crit_exit_quick(td);
	}
}

