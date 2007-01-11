/*
 * Copyright (c) 2003,2004 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com> and David Xu <davidxu@freebsd.org>
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
 * $DragonFly: src/sys/kern/kern_umtx.c,v 1.6 2007/01/11 20:53:41 dillon Exp $
 */

/*
 * This module implements userland mutex helper functions.  umtx_sleep()
 * handling blocking and umtx_wakeup() handles wakeups.  The sleep/wakeup
 * functions operate on user addresses.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysproto.h>
#include <sys/sysunion.h>
#include <sys/sysent.h>
#include <sys/syscall.h>
#include <sys/sfbuf.h>
#include <sys/module.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_pageout.h>
#include <vm/vm_extern.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

/*
 * If the contents of the userland-supplied pointer matches the specified
 * value enter an interruptable sleep for up to <timeout> microseconds.
 * If the contents does not match then return immediately.
 *
 * The specified timeout may not exceed 1 second.
 *
 * Returns 0 if we slept and were woken up, -1 and ETIMEDOUT if we slept
 * and timed out, and EBUSY if the contents of the pointer did not match
 * the specified value.  A timeout of 0 indicates an unlimited sleep.
 * EINTR is returned if the call was interrupted by a signal.
 *
 * This function interlocks against call to umtx_wakeup.  It does NOT interlock
 * against changes in *ptr.  However, it does not have to.  The standard use
 * of *ptr is to differentiate between an uncontested and a contested mutex
 * and call umtx_wakeup when releasing a contested mutex.  Therefore we can
 * safely race against changes in *ptr as long as we are properly interlocked
 * against the umtx_wakeup() call.
 *
 * The VM page associated with the mutex is held to prevent reuse in order
 * to guarentee that the physical address remains consistent.
 *
 * umtx_sleep { const int *ptr, int value, int timeout }
 */
int
sys_umtx_sleep(struct umtx_sleep_args *uap)
{
    int error = EBUSY;
    struct sf_buf *sf;
    vm_page_t m;
    void *waddr;
    int offset;
    int timeout;

    if ((unsigned int)uap->timeout > 1000000)
	return (EINVAL);
    if ((vm_offset_t)uap->ptr & (sizeof(int) - 1))
	return (EFAULT);
    m = vm_fault_page_quick((vm_offset_t)uap->ptr, VM_PROT_READ, &error);
    if (m == NULL)
	return (EFAULT);
    sf = sf_buf_alloc(m, SFB_CPUPRIVATE);
    offset = (vm_offset_t)uap->ptr & PAGE_MASK;

    if (*(int *)(sf_buf_kva(sf) + offset) == uap->value) {
	if ((timeout = uap->timeout) != 0)
	    timeout = (timeout * hz + 999999) / 1000000;
	waddr = (void *)((intptr_t)VM_PAGE_TO_PHYS(m) + offset);
	error = tsleep(waddr, PCATCH|PDOMAIN_UMTX, "umtxsl", timeout);
	/* Can not restart timeout wait. */
	if (timeout != 0 && error == ERESTART)
		error = EINTR;
    } else {
	error = EBUSY;
    }

    sf_buf_free(sf);
    vm_page_unhold(m);
    return(error);
}

/*
 * umtx_wakeup { const int *ptr, int count }
 *
 * Wakeup the specified number of processes held in umtx_sleep() on the
 * specified user address.  A count of 0 wakes up all waiting processes.
 *
 * XXX assumes that the physical address space does not exceed the virtual
 * address space.
 */
int
sys_umtx_wakeup(struct umtx_wakeup_args *uap)
{
    vm_page_t m;
    int offset;
    int error;
    void *waddr;

    cpu_mfence();
    if ((vm_offset_t)uap->ptr & (sizeof(int) - 1))
	return (EFAULT);
    m = vm_fault_page_quick((vm_offset_t)uap->ptr, VM_PROT_READ, &error);
    if (m == NULL)
	return (EFAULT);
    offset = (vm_offset_t)uap->ptr & PAGE_MASK;
    waddr = (void *)((intptr_t)VM_PAGE_TO_PHYS(m) + offset);

    if (uap->count == 1) {
	wakeup_domain_one(waddr, PDOMAIN_UMTX);
    } else {
	/* XXX wakes them all up for now */
	wakeup_domain(waddr, PDOMAIN_UMTX);
    }
    vm_page_unhold(m);
    return(0);
}

