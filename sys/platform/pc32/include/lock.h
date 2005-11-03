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
 * $FreeBSD: src/sys/i386/include/lock.h,v 1.11.2.2 2000/09/30 02:49:34 ps Exp $
 * $DragonFly: src/sys/platform/pc32/include/lock.h,v 1.13 2005/11/03 04:53:57 dillon Exp $
 */

#ifndef _MACHINE_LOCK_H_
#define _MACHINE_LOCK_H_

#ifndef _MACHINE_PSL_H_
#include "psl.h"
#endif

/*
 * MP_FREE_LOCK is used by both assembly and C under SMP.
 */
#ifdef SMP
#define MP_FREE_LOCK		0xffffffff	/* value of lock when free */
#endif

#ifdef LOCORE

/*
 * Spinlock assembly support.  Note: eax and ecx can be tromped.  No
 * other register will be.   Note that these routines are sometimes
 * called with (%edx) as the mem argument.
 *
 * Under UP the spinlock routines still serve to disable/restore 
 * interrupts.
 */


#ifdef SMP

#define SPIN_INIT(mem)						\
	movl	$0,mem ;					\

#define SPIN_INIT_NOREG(mem)					\
	SPIN_INIT(mem) ;					\

#define SPIN_LOCK(mem)						\
	pushfl ;						\
	popl	%ecx ;		/* flags */			\
	cli ;							\
	orl	$PSL_C,%ecx ;	/* make sure non-zero */	\
7: ;								\
	movl	$0,%eax ;	/* expected contents of lock */	\
	lock cmpxchgl %ecx,mem ; /* Z=1 (jz) on success */	\
	pause ;							\
	jnz	7b ; 						\

#define SPIN_LOCK_PUSH_REGS					\
	subl	$8,%esp ;					\
	movl	%ecx,(%esp) ;					\
	movl	%eax,4(%esp) ;					\

#define SPIN_LOCK_POP_REGS					\
	movl	(%esp),%ecx ;					\
	movl	4(%esp),%eax ;					\
	addl	$8,%esp ;					\

#define SPIN_LOCK_FRAME_SIZE	8

#define SPIN_LOCK_NOREG(mem)					\
	SPIN_LOCK_PUSH_REGS ;					\
	SPIN_LOCK(mem) ;					\
	SPIN_LOCK_POP_REGS ;					\

#define SPIN_UNLOCK(mem)					\
	pushl	mem ;						\
	movl	$0,mem ;					\
	popfl ;							\

#define SPIN_UNLOCK_PUSH_REGS
#define SPIN_UNLOCK_POP_REGS
#define SPIN_UNLOCK_FRAME_SIZE	0

#define SPIN_UNLOCK_NOREG(mem)					\
	SPIN_UNLOCK(mem) ;					\

#else

#define SPIN_LOCK(mem)						\
	pushfl ;						\
	cli ;							\
	orl	$PSL_C,(%esp) ;					\
	popl	mem ;						\

#define SPIN_LOCK_PUSH_RESG
#define SPIN_LOCK_POP_REGS
#define SPIN_LOCK_FRAME_SIZE	0

#define SPIN_UNLOCK(mem)					\
	pushl	mem ;						\
	movl	$0,mem ;					\
	popfl ;							\

#define SPIN_UNLOCK_PUSH_REGS
#define SPIN_UNLOCK_POP_REGS
#define SPIN_UNLOCK_FRAME_SIZE	0

#endif	/* SMP */

#else	/* !LOCORE */

#ifdef _KERNEL

/*
 * Spinlock functions (UP and SMP).  Under UP a spinlock still serves
 * to disable/restore interrupts even if it doesn't spin.
 */
struct spinlock_deprecated {
	volatile int	opaque;
};

typedef struct spinlock_deprecated *spinlock_t;

void	mpintr_lock(void);	/* disables int / spinlock combo */
void	mpintr_unlock(void);
void	com_lock(void);		/* disables int / spinlock combo */
void	com_unlock(void);
void	imen_lock(void);	/* disables int / spinlock combo */
void	imen_unlock(void);
void	clock_lock(void);	/* disables int / spinlock combo */
void	clock_unlock(void);
void	cons_lock(void);	/* disables int / spinlock combo */
void	cons_unlock(void);

extern struct spinlock_deprecated smp_rv_spinlock;

void	spin_lock_deprecated(spinlock_t lock);
void	spin_unlock_deprecated(spinlock_t lock);

/*
 * Inline version of spinlock routines -- overrides assembly.  Only unlock
 * and init here please.
 */
static __inline void
spin_lock_init(spinlock_t lock)
{
	lock->opaque = 0;
}

#endif  /* _KERNEL */

#if defined(_KERNEL) || defined(_UTHREAD)

/*
 * MP LOCK functions for SMP and UP.  Under UP the MP lock does not exist
 * but we leave a few functions intact as macros for convenience.
 */
#ifdef SMP

void	get_mplock(void);
int	try_mplock(void);
void	rel_mplock(void);
int	cpu_try_mplock(void);
void	cpu_get_initial_mplock(void);

extern u_int	mp_lock;

#define MP_LOCK_HELD()   (mp_lock == mycpu->gd_cpuid)
#define ASSERT_MP_LOCK_HELD(td)   KASSERT(MP_LOCK_HELD(), ("MP_LOCK_HELD(): not held thread %p", td))

static __inline void
cpu_rel_mplock(void)
{
	mp_lock = MP_FREE_LOCK;
}

static __inline int
owner_mplock(void)
{
	return (mp_lock);
}

#else

#define get_mplock()
#define try_mplock()	1
#define rel_mplock()
#define owner_mplock()	0	/* always cpu 0 */
#define ASSERT_MP_LOCK_HELD(td)

#endif	/* SMP */
#endif  /* _KERNEL || _UTHREAD */
#endif	/* LOCORE */
#endif	/* !_MACHINE_LOCK_H_ */
