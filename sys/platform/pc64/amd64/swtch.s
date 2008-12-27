/*
 * Copyright (c) 2003,2004,2008 The DragonFly Project.  All rights reserved.
 * Copyright (c) 2008 Jordan Gordeev.
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
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
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
 * $FreeBSD: src/sys/i386/i386/swtch.s,v 1.89.2.10 2003/01/23 03:36:24 ps Exp $
 * $DragonFly: src/sys/platform/pc64/amd64/swtch.s,v 1.3 2008/08/29 17:07:10 dillon Exp $
 */

//#include "use_npx.h"

#include <sys/rtprio.h>

#include <machine/asmacros.h>
#include <machine/segments.h>

#include <machine/pmap.h>
#if JG
#include <machine_base/apic/apicreg.h>
#endif
#include <machine/lock.h>

#define CHECKNZ(expr, scratch_reg) \
	movq expr, scratch_reg; testq scratch_reg, scratch_reg; jnz 7f; int $3; 7:

#include "assym.s"

#if defined(SMP)
#define MPLOCKED        lock ;
#else
#define MPLOCKED
#endif

	.data

	.globl	panic

#if defined(SWTCH_OPTIM_STATS)
	.globl	swtch_optim_stats, tlb_flush_count
swtch_optim_stats:	.long	0		/* number of _swtch_optims */
tlb_flush_count:	.long	0
#endif

	.text


/*
 * cpu_heavy_switch(struct thread *next_thread)
 *
 *	Switch from the current thread to a new thread.  This entry
 *	is normally called via the thread->td_switch function, and will
 *	only be called when the current thread is a heavy weight process.
 *
 *	Some instructions have been reordered to reduce pipeline stalls.
 *
 *	YYY disable interrupts once giant is removed.
 */
ENTRY(cpu_heavy_switch)
	/*
	 * Save RIP, RSP and callee-saved registers (RBX, RBP, R12-R15).
	 */
	movq	PCPU(curthread),%rcx
	/* On top of the stack is the return adress. */
	movq	(%rsp),%rax			/* (reorder optimization) */
	movq	TD_PCB(%rcx),%rdx		/* RDX = PCB */
	movq	%rax,PCB_RIP(%rdx)		/* return PC may be modified */
	movq	%rbx,PCB_RBX(%rdx)
	movq	%rsp,PCB_RSP(%rdx)
	movq	%rbp,PCB_RBP(%rdx)
	movq	%r12,PCB_R12(%rdx)
	movq	%r13,PCB_R13(%rdx)
	movq	%r14,PCB_R14(%rdx)
	movq	%r15,PCB_R15(%rdx)

	movq	%rcx,%rbx			/* RBX = curthread */
	movq	TD_LWP(%rcx),%rcx
	movl	PCPU(cpuid), %eax
	movq	LWP_VMSPACE(%rcx), %rcx		/* RCX = vmspace */
	MPLOCKED btrl	%eax, VM_PMAP+PM_ACTIVE(%rcx)

	/*
	 * Push the LWKT switch restore function, which resumes a heavy
	 * weight process.  Note that the LWKT switcher is based on
	 * TD_SP, while the heavy weight process switcher is based on
	 * PCB_RSP.  TD_SP is usually two ints pushed relative to
	 * PCB_RSP.  We push the flags for later restore by cpu_heavy_restore.
	 */
	pushfq
	movq	$cpu_heavy_restore, %rax
	pushq	%rax
	movq	%rsp,TD_SP(%rbx)

	/*
	 * Save debug regs if necessary
	 */
	movq    PCB_FLAGS(%rdx),%rax
	andq    $PCB_DBREGS,%rax
	jz      1f                              /* no, skip over */
	movq    %dr7,%rax                       /* yes, do the save */
	movq    %rax,PCB_DR7(%rdx)
	/* JG correct value? */
	andq    $0x0000fc00, %rax               /* disable all watchpoints */
	movq    %rax,%dr7
	movq    %dr6,%rax
	movq    %rax,PCB_DR6(%rdx)
	movq    %dr3,%rax
	movq    %rax,PCB_DR3(%rdx)
	movq    %dr2,%rax
	movq    %rax,PCB_DR2(%rdx)
	movq    %dr1,%rax
	movq    %rax,PCB_DR1(%rdx)
	movq    %dr0,%rax
	movq    %rax,PCB_DR0(%rdx)
1:
 
#if JG
#if NNPX > 0
	/*
	 * Save the FP state if we have used the FP.  Note that calling
	 * npxsave will NULL out PCPU(npxthread).
	 */
	cmpl	%ebx,PCPU(npxthread)
	jne	1f
	pushl	TD_SAVEFPU(%ebx)
	call	npxsave			/* do it in a big C function */
	addl	$4,%esp			/* EAX, ECX, EDX trashed */
1:
#endif
#endif	/* NNPX > 0 */

	/*
	 * Switch to the next thread, which was passed as an argument
	 * to cpu_heavy_switch().  The argument is in %rdi.
	 * Set the current thread, load the stack pointer,
	 * and 'ret' into the switch-restore function.
	 *
	 * The switch restore function expects the new thread to be in %rax
	 * and the old one to be in %rbx.
	 *
	 * There is a one-instruction window where curthread is the new
	 * thread but %rsp still points to the old thread's stack, but
	 * we are protected by a critical section so it is ok.
	 */
	movq	%rdi,%rax		/* RAX = newtd, RBX = oldtd */
	movq	%rax,PCPU(curthread)
	movq	TD_SP(%rax),%rsp
	CHECKNZ((%rsp), %r9)
	ret

/*
 *  cpu_exit_switch(struct thread *next)
 *
 *	The switch function is changed to this when a thread is going away
 *	for good.  We have to ensure that the MMU state is not cached, and
 *	we don't bother saving the existing thread state before switching.
 *
 *	At this point we are in a critical section and this cpu owns the
 *	thread's token, which serves as an interlock until the switchout is
 *	complete.
 */
ENTRY(cpu_exit_switch)
	/*
	 * Get us out of the vmspace
	 */
#if JG
	movq	%cr3,%rax
	cmpq	%rcx,%rax
	je	1f
	/* JG no increment of statistics counters? see cpu_heavy_restore */
	movq	%rcx,%cr3
1:
#else
	movq	IdlePTD, %rcx
	orq	$(PG_RW|PG_V), %rcx
	movq	link_pdpe,%r12
	movq	%rcx, (%r12)
	movq	%cr3, %rcx
	movq	%rcx, %cr3
#endif
	movq	PCPU(curthread),%rbx

	/*
	 * If this is a process/lwp, deactivate the pmap after we've
	 * switched it out.
	 */
	movq	TD_LWP(%rbx),%rcx
	testq	%rcx,%rcx
	jz	2f
	movl	PCPU(cpuid), %eax
	movq	LWP_VMSPACE(%rcx), %rcx		/* RCX = vmspace */
	MPLOCKED btrl	%eax, VM_PMAP+PM_ACTIVE(%rcx)
2:
	/*
	 * Switch to the next thread.  RET into the restore function, which
	 * expects the new thread in RAX and the old in RBX.
	 *
	 * There is a one-instruction window where curthread is the new
	 * thread but %rsp still points to the old thread's stack, but
	 * we are protected by a critical section so it is ok.
	 */
	movq	%rdi,%rax
	movq	%rax,PCPU(curthread)
	movq	TD_SP(%rax),%rsp
	CHECKNZ((%rsp), %r9)
	ret

/*
 * cpu_heavy_restore()	(current thread in %rax on entry)
 *
 *	Restore the thread after an LWKT switch.  This entry is normally
 *	called via the LWKT switch restore function, which was pulled 
 *	off the thread stack and jumped to.
 *
 *	This entry is only called if the thread was previously saved
 *	using cpu_heavy_switch() (the heavy weight process thread switcher),
 *	or when a new process is initially scheduled.  The first thing we
 *	do is clear the TDF_RUNNING bit in the old thread and set it in the
 *	new thread.
 *
 *	NOTE: The lwp may be in any state, not necessarily LSRUN, because
 *	a preemption switch may interrupt the process and then return via 
 *	cpu_heavy_restore.
 *
 *	YYY theoretically we do not have to restore everything here, a lot
 *	of this junk can wait until we return to usermode.  But for now
 *	we restore everything.
 *
 *	YYY the PCB crap is really crap, it makes startup a bitch because
 *	we can't switch away.
 *
 *	YYY note: spl check is done in mi_switch when it splx()'s.
 */

ENTRY(cpu_heavy_restore)
	popfq
	movq	TD_PCB(%rax),%rdx		/* RDX = PCB */
	movq	TD_LWP(%rax),%rcx

#if defined(SWTCH_OPTIM_STATS)
	incl	_swtch_optim_stats
#endif
	/*
	 * Tell the pmap that our cpu is using the VMSPACE now.  We cannot
	 * safely test/reload %cr3 until after we have set the bit in the
	 * pmap (remember, we do not hold the MP lock in the switch code).
	 */
	movq	LWP_VMSPACE(%rcx), %rcx		/* RCX = vmspace */
	movl	PCPU(cpuid), %esi
	MPLOCKED btsl	%esi, VM_PMAP+PM_ACTIVE(%rcx)

	/*
	 * Restore the MMU address space.  If it is the same as the last
	 * thread we don't have to invalidate the tlb (i.e. reload cr3).
	 * YYY which naturally also means that the PM_ACTIVE bit had better
	 * already have been set before we set it above, check? YYY
	 */
#if JG
	movq	%cr3,%rsi
	movq	PCB_CR3(%rdx),%rcx
	cmpq	%rsi,%rcx
	je	4f
#if defined(SWTCH_OPTIM_STATS)
	decl	_swtch_optim_stats
	incl	_tlb_flush_count
#endif
	movq	%rcx,%cr3
4:
#else
	movq	PCB_CR3(%rdx),%rcx
	orq	$(PG_RW|PG_U|PG_V), %rcx
	/*XXX*/
	movq	link_pdpe,%r12
	movq	%rcx, (%r12)
	movq	%cr3, %rcx
	movq	%rcx, %cr3
#endif
	/*
	 * Clear TDF_RUNNING flag in old thread only after cleaning up
	 * %cr3.  The target thread is already protected by being TDF_RUNQ
	 * so setting TDF_RUNNING isn't as big a deal.
	 */
	andl	$~TDF_RUNNING,TD_FLAGS(%rbx)
	orl	$TDF_RUNNING,TD_FLAGS(%rax)

	/*
	 * Deal with the PCB extension, restore the private tss
	 */
	movq	PCB_EXT(%rdx),%rdi	/* check for a PCB extension */
	/* JG cheaper than "movq $1,%rbx", right? */
	/* JG what's that magic value $1? */
	movl	$1,%ebx			/* maybe mark use of a private tss */
	testq	%rdi,%rdi
#if JG
	jnz	2f
#endif

	/* JG
	 * Going back to the common_tss.  We may need to update TSS_ESP0
	 * which sets the top of the supervisor stack when entering from
	 * usermode.  The PCB is at the top of the stack but we need another
	 * 16 bytes to take vm86 into account.
	 */
	leaq	-16(%rdx),%rbx
	movq	%rbx, PCPU(common_tss) + TSS_RSP0
	movq	%rbx, PCPU(rsp0)

#if JG
	cmpl	$0,PCPU(private_tss)	/* don't have to reload if      */
	je	3f			/* already using the common TSS */

	/* JG? */
	subl	%ebx,%ebx		/* unmark use of private tss */

	/*
	 * Get the address of the common TSS descriptor for the ltr.
	 * There is no way to get the address of a segment-accessed variable
	 * so we store a self-referential pointer at the base of the per-cpu
	 * data area and add the appropriate offset.
	 */
	/* JG movl? */
	movq	$gd_common_tssd, %rdi
	/* JG name for "%gs:0"? */
	addq	%gs:0, %rdi

	/*
	 * Move the correct TSS descriptor into the GDT slot, then reload
	 * ltr.
	 */
2:
	/* JG */
	movl	%ebx,PCPU(private_tss)		/* mark/unmark private tss */
	movq	PCPU(tss_gdt), %rbx		/* entry in GDT */
	movq	0(%rdi), %rax
	movq	%rax, 0(%rbx)
	movl	$GPROC0_SEL*8, %esi		/* GSEL(entry, SEL_KPL) */
	ltr	%si
#endif

3:
	/*
	 * Restore the user %gs and %fs
	 */
	movq	PCB_FSBASE(%rdx),%r9
	cmpq	PCPU(user_fs),%r9
	je	4f
	movq	%rdx,%r10
	movq	%r9,PCPU(user_fs)
	movl	$MSR_FSBASE,%ecx
	movl	PCB_FSBASE(%r10),%eax
	movl	PCB_FSBASE+4(%r10),%edx
	wrmsr
	movq	%r10,%rdx
4:
	movq	PCB_GSBASE(%rdx),%r9
	cmpq	PCPU(user_gs),%r9
	je	5f
	movq	%rdx,%r10
	movq	%r9,PCPU(user_gs)
	movl	$MSR_KGSBASE,%ecx	/* later swapgs moves it to GSBASE */
	movl	PCB_GSBASE(%r10),%eax
	movl	PCB_GSBASE+4(%r10),%edx
	wrmsr
	movq	%r10,%rdx
5:

	/*
	 * Restore general registers.
	 */
	movq	PCB_RBX(%rdx), %rbx
	movq	PCB_RSP(%rdx), %rsp
	movq	PCB_RBP(%rdx), %rbp
	movq	PCB_R12(%rdx), %r12
	movq	PCB_R13(%rdx), %r13
	movq	PCB_R14(%rdx), %r14
	movq	PCB_R15(%rdx), %r15
	movq	PCB_RIP(%rdx), %rax
	movq	%rax, (%rsp)

#if JG
	/*
	 * Restore the user LDT if we have one
	 */
	cmpl	$0, PCB_USERLDT(%edx)
	jnz	1f
	movl	_default_ldt,%eax
	cmpl	PCPU(currentldt),%eax
	je	2f
	lldt	_default_ldt
	movl	%eax,PCPU(currentldt)
	jmp	2f
1:	pushl	%edx
	call	set_user_ldt
	popl	%edx
2:
#endif
#if JG
	/*
	 * Restore the user TLS if we have one
	 */
	pushl	%edx
	call	set_user_TLS
	popl	%edx
#endif

	/*
	 * Restore the DEBUG register state if necessary.
	 */
	movq    PCB_FLAGS(%rdx),%rax
	andq    $PCB_DBREGS,%rax
	jz      1f                              /* no, skip over */
	movq    PCB_DR6(%rdx),%rax              /* yes, do the restore */
	movq    %rax,%dr6
	movq    PCB_DR3(%rdx),%rax
	movq    %rax,%dr3
	movq    PCB_DR2(%rdx),%rax
	movq    %rax,%dr2
	movq    PCB_DR1(%rdx),%rax
	movq    %rax,%dr1
	movq    PCB_DR0(%rdx),%rax
	movq    %rax,%dr0
	movq	%dr7,%rax                /* load dr7 so as not to disturb */
	/* JG correct value? */
	andq    $0x0000fc00,%rax         /*   reserved bits               */
	/* JG we've got more registers on amd64 */
	pushq   %rbx
	movq    PCB_DR7(%rdx),%rbx
	/* JG correct value? */
	andq	$~0x0000fc00,%rbx
	orq     %rbx,%rax
	popq	%rbx
	movq    %rax,%dr7
1:

	CHECKNZ((%rsp), %r9)
	ret

/*
 * savectx(struct pcb *pcb)
 *
 * Update pcb, saving current processor state.
 */
ENTRY(savectx)
	/* fetch PCB */
	/* JG use %rdi instead of %rcx everywhere? */
	movq	%rdi,%rcx

	/* caller's return address - child won't execute this routine */
	movq	(%rsp),%rax
	movq	%rax,PCB_RIP(%rcx)

	movq	%cr3,%rax
#ifndef JG
	movq	(%rax), %rax
	movq	$0x000ffffffffff000, %rcx
	andq	%rcx, %rax
	movq	(%rax), %rax
	andq	%rcx, %rax
#endif
	movq	%rax,PCB_CR3(%rcx)

	movq	%rbx,PCB_RBX(%rcx)
	movq	%rsp,PCB_RSP(%rcx)
	movq	%rbp,PCB_RBP(%rcx)
	movq	%r12,PCB_R12(%rcx)
	movq	%r13,PCB_R13(%rcx)
	movq	%r14,PCB_R14(%rcx)
	movq	%r15,PCB_R15(%rcx)

#if JG
#if NNPX > 0
	/*
	 * If npxthread == NULL, then the npx h/w state is irrelevant and the
	 * state had better already be in the pcb.  This is true for forks
	 * but not for dumps (the old book-keeping with FP flags in the pcb
	 * always lost for dumps because the dump pcb has 0 flags).
	 *
	 * If npxthread != NULL, then we have to save the npx h/w state to
	 * npxthread's pcb and copy it to the requested pcb, or save to the
	 * requested pcb and reload.  Copying is easier because we would
	 * have to handle h/w bugs for reloading.  We used to lose the
	 * parent's npx state for forks by forgetting to reload.
	 */
	movl	PCPU(npxthread),%eax
	testl	%eax,%eax
	je	1f

	pushl	%ecx			/* target pcb */
	movl	TD_SAVEFPU(%eax),%eax	/* originating savefpu area */
	pushl	%eax

	pushl	%eax
	call	npxsave
	addl	$4,%esp

	popl	%eax
	popl	%ecx

	pushl	$PCB_SAVEFPU_SIZE
	leal    PCB_SAVEFPU(%ecx),%ecx
	pushl	%ecx
	pushl	%eax
	call	bcopy
	addl	$12,%esp
#endif	/* NNPX > 0 */

1:
#endif
	CHECKNZ((%rsp), %r9)
	ret

/*
 * cpu_idle_restore()	(current thread in %rax on entry) (one-time execution)
 *
 *	Don't bother setting up any regs other than %rbp so backtraces
 *	don't die.  This restore function is used to bootstrap into the
 *	cpu_idle() LWKT only, after that cpu_lwkt_*() will be used for
 *	switching.
 *
 *	Clear TDF_RUNNING in old thread only after we've cleaned up %cr3.
 *
 *	If we are an AP we have to call ap_init() before jumping to
 *	cpu_idle().  ap_init() will synchronize with the BP and finish
 *	setting up various ncpu-dependant globaldata fields.  This may
 *	happen on UP as well as SMP if we happen to be simulating multiple
 *	cpus.
 */
ENTRY(cpu_idle_restore)
	/* cli */
	movq	IdlePTD,%rcx
	/* JG xor? */
	movl	$0,%ebp
	/* JG push RBP? */
	pushq	$0
	orq	$(PG_RW|PG_V), %rcx
	movq	link_pdpe,%r12
	movq	%rcx, (%r12)
	movq	%cr3, %rcx
	movq	%rcx,%cr3
	andl	$~TDF_RUNNING,TD_FLAGS(%rbx)
	orl	$TDF_RUNNING,TD_FLAGS(%rax)
#ifdef SMP
	cmpl	$0,PCPU(cpuid)
	je	1f
	call	ap_init
1:
#endif
	/*
	 * ap_init can decide to enable interrupts early, but otherwise, or if
	 * we are UP, do it here.
	 */
	sti
	jmp	cpu_idle

/*
 * cpu_kthread_restore() (current thread is %rax on entry) (one-time execution)
 *
 *	Don't bother setting up any regs other then %rbp so backtraces
 *	don't die.  This restore function is used to bootstrap into an
 *	LWKT based kernel thread only.  cpu_lwkt_switch() will be used
 *	after this.
 *
 *	Since all of our context is on the stack we are reentrant and
 *	we can release our critical section and enable interrupts early.
 */
ENTRY(cpu_kthread_restore)
	sti
	movq	IdlePTD,%rcx
	movq	TD_PCB(%rax),%rdx
	/* JG "movq $0, %rbp"? "xorq %rbp, %rbp"? */
	movl	$0,%ebp
	orq	$(PG_RW|PG_V), %rcx
	movq	link_pdpe,%r12
	movq	%rcx, (%r12)
	movq	%cr3, %rcx
	movq	%rcx,%cr3
	/* rax and rbx come from the switchout code */
	andl	$~TDF_RUNNING,TD_FLAGS(%rbx)
	orl	$TDF_RUNNING,TD_FLAGS(%rax)
	subl	$TDPRI_CRIT,TD_PRI(%rax)
	movq	PCB_R12(%rdx),%rdi	/* argument to RBX function */
	movq	PCB_RBX(%rdx),%rax	/* thread function */
	/* note: top of stack return address inherited by function */
	CHECKNZ(%rax, %r9)
	jmp	*%rax

/*
 * cpu_lwkt_switch(struct thread *)
 *
 *	Standard LWKT switching function.  Only non-scratch registers are
 *	saved and we don't bother with the MMU state or anything else.
 *
 *	This function is always called while in a critical section.
 *
 *	There is a one-instruction window where curthread is the new
 *	thread but %rsp still points to the old thread's stack, but
 *	we are protected by a critical section so it is ok.
 *
 *	YYY BGL, SPL
 */
ENTRY(cpu_lwkt_switch)
	pushq	%rbp	/* JG note: GDB hacked to locate ebp relative to td_sp */
	/* JG we've got more registers on AMD64 */
	pushq	%rbx
	movq	PCPU(curthread),%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushfq

#if JG
#if NNPX > 0
	/*
	 * Save the FP state if we have used the FP.  Note that calling
	 * npxsave will NULL out PCPU(npxthread).
	 *
	 * We have to deal with the FP state for LWKT threads in case they
	 * happen to get preempted or block while doing an optimized
	 * bzero/bcopy/memcpy.
	 */
	cmpl	%ebx,PCPU(npxthread)
	jne	1f
	pushl	TD_SAVEFPU(%ebx)
	call	npxsave			/* do it in a big C function */
	addl	$4,%esp			/* EAX, ECX, EDX trashed */
1:
#endif	/* NNPX > 0 */
#endif

	movq	%rdi,%rax		/* switch to this thread */
	pushq	$cpu_lwkt_restore
	movq	%rsp,TD_SP(%rbx)
	movq	%rax,PCPU(curthread)
	movq	TD_SP(%rax),%rsp

	/*
	 * %rax contains new thread, %rbx contains old thread.
	 */
	CHECKNZ((%rsp), %r9)
	ret

/*
 * cpu_lwkt_restore()	(current thread in %rax on entry)
 *
 *	Standard LWKT restore function.  This function is always called
 *	while in a critical section.
 *	
 *	Warning: due to preemption the restore function can be used to 
 *	'return' to the original thread.  Interrupt disablement must be
 *	protected through the switch so we cannot run splz here.
 *
 *	YYY we theoretically do not need to load KPML4phys into cr3, but if
 *	so we need a way to detect when the PTD we are using is being 
 *	deleted due to a process exiting.
 */
ENTRY(cpu_lwkt_restore)
#if JG
	movq	common_lvl4_phys,%rcx	/* YYY borrow but beware desched/cpuchg/exit */
#endif
	movq	IdlePTD, %rcx
	orq	$(PG_RW|PG_V), %rcx
	movq	link_pdpe,%r12
	movq	%rcx, (%r12)
	movq	%cr3, %rcx
	movq	%rcx, %cr3
#if JG
	movq	%cr3,%rdx
	cmpq	%rcx,%rdx
	je	1f
	movq	%rcx,%cr3
1:
#endif
	andl	$~TDF_RUNNING,TD_FLAGS(%rbx)
	orl	$TDF_RUNNING,TD_FLAGS(%rax)
	popfq
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
