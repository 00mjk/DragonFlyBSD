/*
 *	from: vector.s, 386BSD 0.1 unknown origin
 * $FreeBSD: src/sys/i386/isa/apic_vector.s,v 1.47.2.5 2001/09/01 22:33:38 tegge Exp $
 * $DragonFly: src/sys/platform/pc32/apic/apic_vector.s,v 1.24 2005/11/02 08:33:24 dillon Exp $
 */


#include <arch/apic/apicreg.h>
#include <machine/smp.h>
#include "i386/isa/intr_machdep.h"

/* convert an absolute IRQ# into a bitmask */
#define IRQ_LBIT(irq_num)	(1 << (irq_num))

/* make an index into the IO APIC from the IRQ# */
#define REDTBL_IDX(irq_num)	(0x10 + ((irq_num) * 2))

/*
 * Push an interrupt frame in a format acceptable to doreti, reload
 * the segment registers for the kernel.
 */
#define PUSH_FRAME							\
	pushl	$0 ;		/* dummy error code */			\
	pushl	$0 ;		/* dummy trap type */			\
	pushal ;							\
	pushl	%ds ;		/* save data and extra segments ... */	\
	pushl	%es ;							\
	pushl	%fs ;							\
	mov	$KDSEL,%ax ;						\
	mov	%ax,%ds ;						\
	mov	%ax,%es ;						\
	mov	$KPSEL,%ax ;						\
	mov	%ax,%fs ;						\

#define PUSH_DUMMY							\
	pushfl ;		/* phys int frame / flags */		\
	pushl %cs ;		/* phys int frame / cs */		\
	pushl	12(%esp) ;	/* original caller eip */		\
	pushl	$0 ;		/* dummy error code */			\
	pushl	$0 ;		/* dummy trap type */			\
	subl	$12*4,%esp ;	/* pushal + 3 seg regs (dummy) + CPL */	\

/*
 * Warning: POP_FRAME can only be used if there is no chance of a
 * segment register being changed (e.g. by procfs), which is why syscalls
 * have to use doreti.
 */
#define POP_FRAME							\
	popl	%fs ;							\
	popl	%es ;							\
	popl	%ds ;							\
	popal ;								\
	addl	$2*4,%esp ;	/* dummy trap & error codes */		\

#define POP_DUMMY							\
	addl	$17*4,%esp ;						\

#define IOAPICADDR(irq_num) CNAME(int_to_apicintpin) + 16 * (irq_num) + 8
#define REDIRIDX(irq_num) CNAME(int_to_apicintpin) + 16 * (irq_num) + 12

/*
 * Interrupts are expected to already be disabled when using these
 * IMASK_*() macros.
 */
#define IMASK_LOCK							\
	SPIN_LOCK(imen_spinlock) ; 					\

#define IMASK_UNLOCK							\
	SPIN_UNLOCK(imen_spinlock) ;					\
	
#define MASK_IRQ(irq_num)						\
	IMASK_LOCK ;				/* into critical reg */	\
	testl	$IRQ_LBIT(irq_num), apic_imen ;				\
	jne	7f ;			/* masked, don't mask */	\
	orl	$IRQ_LBIT(irq_num), apic_imen ;	/* set the mask bit */	\
	movl	IOAPICADDR(irq_num), %ecx ;	/* ioapic addr */	\
	movl	REDIRIDX(irq_num), %eax ;	/* get the index */	\
	movl	%eax, (%ecx) ;			/* write the index */	\
	movl	IOAPIC_WINDOW(%ecx), %eax ;	/* current value */	\
	orl	$IOART_INTMASK, %eax ;		/* set the mask */	\
	movl	%eax, IOAPIC_WINDOW(%ecx) ;	/* new value */		\
7: ;						/* already masked */	\
	IMASK_UNLOCK ;							\

/*
 * Test to see whether we are handling an edge or level triggered INT.
 *  Level-triggered INTs must still be masked as we don't clear the source,
 *  and the EOI cycle would cause redundant INTs to occur.
 */
#define MASK_LEVEL_IRQ(irq_num)						\
	testl	$IRQ_LBIT(irq_num), apic_pin_trigger ;			\
	jz	9f ;				/* edge, don't mask */	\
	MASK_IRQ(irq_num) ;						\
9: ;									\


#ifdef APIC_INTR_REORDER
#define EOI_IRQ(irq_num)						\
	movl	apic_isrbit_location + 8 * (irq_num), %eax ;		\
	movl	(%eax), %eax ;						\
	testl	apic_isrbit_location + 4 + 8 * (irq_num), %eax ;	\
	jz	9f ;				/* not active */	\
	movl	$0, lapic_eoi ;						\
9:									\

#else

#define EOI_IRQ(irq_num)						\
	testl	$IRQ_LBIT(irq_num), lapic_isr1;				\
	jz	9f	;			/* not active */	\
	movl	$0, lapic_eoi;						\
9:									\

#endif
	
/*
 * Test to see if the source is currntly masked, clear if so.
 */
#define UNMASK_IRQ(irq_num)					\
	cmpl	$0,%eax ;						\
	jnz	8f ;							\
	IMASK_LOCK ;				/* into critical reg */	\
	testl	$IRQ_LBIT(irq_num), apic_imen ;				\
	je	7f ;			/* bit clear, not masked */	\
	andl	$~IRQ_LBIT(irq_num), apic_imen ;/* clear mask bit */	\
	movl	IOAPICADDR(irq_num),%ecx ;	/* ioapic addr */	\
	movl	REDIRIDX(irq_num), %eax ;	/* get the index */	\
	movl	%eax,(%ecx) ;			/* write the index */	\
	movl	IOAPIC_WINDOW(%ecx),%eax ;	/* current value */	\
	andl	$~IOART_INTMASK,%eax ;		/* clear the mask */	\
	movl	%eax,IOAPIC_WINDOW(%ecx) ;	/* new value */		\
7: ;									\
	IMASK_UNLOCK ;							\
8: ;									\

/*
 * Fast interrupt call handlers run in the following sequence:
 *
 *	- Push the trap frame required by doreti
 *	- Mask the interrupt and reenable its source
 *	- If we cannot take the interrupt set its fpending bit and
 *	  doreti.  Note that we cannot mess with mp_lock at all
 *	  if we entered from a critical section!
 *	- If we can take the interrupt clear its fpending bit,
 *	  call the handler, then unmask and doreti.
 *
 * YYY can cache gd base opitner instead of using hidden %fs prefixes.
 */

#define	FAST_INTR(irq_num, vec_name)					\
	.text ;								\
	SUPERALIGN_TEXT ;						\
IDTVEC(vec_name) ;							\
	PUSH_FRAME ;							\
	FAKE_MCOUNT(13*4(%esp)) ;					\
	MASK_LEVEL_IRQ(irq_num) ;					\
	EOI_IRQ(irq_num) ;						\
	movl	PCPU(curthread),%ebx ;					\
	movl	$0,%eax ;	/* CURRENT CPL IN FRAME (REMOVED) */	\
	pushl	%eax ;							\
	cmpl	$TDPRI_CRIT,TD_PRI(%ebx) ;				\
	jl	2f ;							\
1: ;									\
	/* in critical section, make interrupt pending */		\
	/* set the pending bit and return, leave interrupt masked */	\
	orl	$IRQ_LBIT(irq_num),PCPU(fpending) ;			\
	orl	$RQF_INTPEND,PCPU(reqflags) ;				\
	jmp	5f ;							\
2: ;									\
	/* clear pending bit, run handler */				\
	andl	$~IRQ_LBIT(irq_num),PCPU(fpending) ;			\
	pushl	$irq_num ;						\
	call	ithread_fast_handler ;	 /* returns 0 to unmask */	\
	addl	$4, %esp ;						\
	UNMASK_IRQ(irq_num) ;						\
5: ;									\
	MEXITCOUNT ;							\
	jmp	doreti ;						\

/*
 * Restart fast interrupt held up by critical section or cpl.
 *
 *	- Push a dummy trape frame as required by doreti
 *	- The interrupt source is already masked
 *	- Clear the fpending bit
 *	- Run the handler
 *	- Unmask the interrupt
 *	- Pop the dummy frame and do a normal return
 *
 *	The BGL is held on call and left held on return.
 *
 *	YYY can cache gd base pointer instead of using hidden %fs
 *	prefixes.
 */

#define FAST_UNPEND(irq_num, vec_name)					\
	.text ;								\
	SUPERALIGN_TEXT ;						\
IDTVEC(vec_name) ;							\
	pushl	%ebp ;							\
	movl	%esp,%ebp ;						\
	PUSH_DUMMY ;							\
	pushl	$irq_num ;						\
	call	ithread_fast_handler ;  /* returns 0 to unmask */	\
	addl	$4, %esp ;						\
	UNMASK_IRQ(irq_num) ;						\
	POP_DUMMY ;							\
	popl %ebp ;							\
	ret ;								\

/*
 * Slow interrupt call handlers run in the following sequence:
 *
 *	- Push the trap frame required by doreti.
 *	- Mask the interrupt and reenable its source.
 *	- If we cannot take the interrupt set its ipending bit and
 *	  doreti.  In addition to checking for a critical section
 *	  and cpl mask we also check to see if the thread is still
 *	  running.  Note that we cannot mess with mp_lock at all
 *	  if we entered from a critical section!
 *	- If we can take the interrupt clear its ipending bit
 *	  and schedule the thread.  Leave interrupts masked and doreti.
 *
 *	Note that calls to sched_ithd() are made with interrupts enabled
 *	and outside a critical section.  YYY sched_ithd may preempt us
 *	synchronously (fix interrupt stacking).
 *
 *	YYY can cache gd base pointer instead of using hidden %fs
 *	prefixes.
 */

#define INTR(irq_num, vec_name, maybe_extra_ipending)			\
	.text ;								\
	SUPERALIGN_TEXT ;						\
IDTVEC(vec_name) ;							\
	PUSH_FRAME ;							\
	maybe_extra_ipending ;						\
;									\
	MASK_LEVEL_IRQ(irq_num) ;					\
	EOI_IRQ(irq_num) ;						\
	movl	PCPU(curthread),%ebx ;					\
	movl	$0,%eax ;	/* CURRENT CPL IN FRAME (REMOVED) */	\
	pushl	%eax ;		/* cpl do restore */			\
	cmpl	$TDPRI_CRIT,TD_PRI(%ebx) ;				\
	jl	2f ;							\
1: ;									\
	/* set the pending bit and return, leave the interrupt masked */ \
	orl	$IRQ_LBIT(irq_num), PCPU(ipending) ;			\
	orl	$RQF_INTPEND,PCPU(reqflags) ;				\
	jmp	5f ;							\
2: ;									\
	/* set running bit, clear pending bit, run handler */		\
	andl	$~IRQ_LBIT(irq_num), PCPU(ipending) ;			\
	sti ;								\
	pushl	$irq_num ;						\
	call	sched_ithd ;						\
	addl	$4,%esp ;						\
5: ;									\
	MEXITCOUNT ;							\
	jmp	doreti ;						\

/*
 * Wrong interrupt call handlers.  We program these into APIC vectors
 * that should otherwise never occur.  For example, we program the SLOW
 * vector for irq N with this when we program the FAST vector with the
 * real interrupt.
 *
 * XXX for now all we can do is EOI it.  We can't call do_wrongintr
 * (yet) because we could be in a critical section.
 */
#define WRONGINTR(irq_num,vec_name)					\
	.text ;								\
	SUPERALIGN_TEXT	 ;						\
IDTVEC(vec_name) ;							\
	PUSH_FRAME ;							\
	movl	$0, lapic_eoi ;	/* End Of Interrupt to APIC */		\
	/*pushl	$irq_num ;*/						\
	/*call	do_wrongintr ;*/					\
	/*addl	$4,%esp ;*/						\
	POP_FRAME ;							\
	iret  ;								\

/*
 * Handle "spurious INTerrupts".
 * Notes:
 *  This is different than the "spurious INTerrupt" generated by an
 *   8259 PIC for missing INTs.  See the APIC documentation for details.
 *  This routine should NOT do an 'EOI' cycle.
 */
	.text
	SUPERALIGN_TEXT
	.globl Xspuriousint
Xspuriousint:

	/* No EOI cycle used here */

	iret


/*
 * Handle TLB shootdowns.
 */
	.text
	SUPERALIGN_TEXT
	.globl	Xinvltlb
Xinvltlb:
	pushl	%eax

#ifdef COUNT_XINVLTLB_HITS
	pushl	%fs
	movl	$KPSEL, %eax
	mov	%ax, %fs
	movl	PCPU(cpuid), %eax
	popl	%fs
	ss
	incl	_xhits(,%eax,4)
#endif /* COUNT_XINVLTLB_HITS */

	movl	%cr3, %eax		/* invalidate the TLB */
	movl	%eax, %cr3

	ss				/* stack segment, avoid %ds load */
	movl	$0, lapic_eoi		/* End Of Interrupt to APIC */

	popl	%eax
	iret


/*
 * Executed by a CPU when it receives an Xcpustop IPI from another CPU,
 *
 *  - Signals its receipt.
 *  - Waits for permission to restart.
 *  - Processing pending IPIQ events while waiting.
 *  - Signals its restart.
 */

	.text
	SUPERALIGN_TEXT
	.globl Xcpustop
Xcpustop:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%eax
	pushl	%ecx
	pushl	%edx
	pushl	%ds			/* save current data segment */
	pushl	%fs

	movl	$KDSEL, %eax
	mov	%ax, %ds		/* use KERNEL data segment */
	movl	$KPSEL, %eax
	mov	%ax, %fs

	movl	$0, lapic_eoi		/* End Of Interrupt to APIC */

	movl	PCPU(cpuid), %eax
	imull	$PCB_SIZE, %eax
	leal	CNAME(stoppcbs)(%eax), %eax
	pushl	%eax
	call	CNAME(savectx)		/* Save process context */
	addl	$4, %esp
	
		
	movl	PCPU(cpuid), %eax

	/*
	 * Indicate that we have stopped and loop waiting for permission
	 * to start again.  We must still process IPI events while in a
	 * stopped state.
	 */
	lock
	btsl	%eax, stopped_cpus	/* stopped_cpus |= (1<<id) */
1:
	andl	$~RQF_IPIQ,PCPU(reqflags)
	pushl	%eax
	call	lwkt_smp_stopped
	popl	%eax
	btl	%eax, started_cpus	/* while (!(started_cpus & (1<<id))) */
	jnc	1b

	lock
	btrl	%eax, started_cpus	/* started_cpus &= ~(1<<id) */
	lock
	btrl	%eax, stopped_cpus	/* stopped_cpus &= ~(1<<id) */

	test	%eax, %eax
	jnz	2f

	movl	CNAME(cpustop_restartfunc), %eax
	test	%eax, %eax
	jz	2f
	movl	$0, CNAME(cpustop_restartfunc)	/* One-shot */

	call	*%eax
2:
	popl	%fs
	popl	%ds			/* restore previous data segment */
	popl	%edx
	popl	%ecx
	popl	%eax
	movl	%ebp, %esp
	popl	%ebp
	iret

	/*
	 * For now just have one ipiq IPI, but what we really want is
	 * to have one for each source cpu to the APICs don't get stalled
	 * backlogging the requests.
	 */
	.text
	SUPERALIGN_TEXT
	.globl Xipiq
Xipiq:
	PUSH_FRAME
	movl	$0, lapic_eoi		/* End Of Interrupt to APIC */
	FAKE_MCOUNT(13*4(%esp))

	movl	PCPU(curthread),%ebx
	cmpl	$TDPRI_CRIT,TD_PRI(%ebx)
	jge	1f
	subl	$8,%esp			/* make same as interrupt frame */
	incl	PCPU(intr_nesting_level)
	addl	$TDPRI_CRIT,TD_PRI(%ebx)
	call	lwkt_process_ipiq_frame
	subl	$TDPRI_CRIT,TD_PRI(%ebx)
	decl	PCPU(intr_nesting_level)
	addl	$8,%esp
	pushl	$0			/* CPL for frame (REMOVED) */
	MEXITCOUNT
	jmp	doreti
1:
	orl	$RQF_IPIQ,PCPU(reqflags)
	MEXITCOUNT
	POP_FRAME
	iret

MCOUNT_LABEL(bintr)
	FAST_INTR(0,fastintr0)
	FAST_INTR(1,fastintr1)
	FAST_INTR(2,fastintr2)
	FAST_INTR(3,fastintr3)
	FAST_INTR(4,fastintr4)
	FAST_INTR(5,fastintr5)
	FAST_INTR(6,fastintr6)
	FAST_INTR(7,fastintr7)
	FAST_INTR(8,fastintr8)
	FAST_INTR(9,fastintr9)
	FAST_INTR(10,fastintr10)
	FAST_INTR(11,fastintr11)
	FAST_INTR(12,fastintr12)
	FAST_INTR(13,fastintr13)
	FAST_INTR(14,fastintr14)
	FAST_INTR(15,fastintr15)
	FAST_INTR(16,fastintr16)
	FAST_INTR(17,fastintr17)
	FAST_INTR(18,fastintr18)
	FAST_INTR(19,fastintr19)
	FAST_INTR(20,fastintr20)
	FAST_INTR(21,fastintr21)
	FAST_INTR(22,fastintr22)
	FAST_INTR(23,fastintr23)
	
	/* YYY what is this garbage? */

	INTR(0,intr0,)
	INTR(1,intr1,)
	INTR(2,intr2,)
	INTR(3,intr3,)
	INTR(4,intr4,)
	INTR(5,intr5,)
	INTR(6,intr6,)
	INTR(7,intr7,)
	INTR(8,intr8,)
	INTR(9,intr9,)
	INTR(10,intr10,)
	INTR(11,intr11,)
	INTR(12,intr12,)
	INTR(13,intr13,)
	INTR(14,intr14,)
	INTR(15,intr15,)
	INTR(16,intr16,)
	INTR(17,intr17,)
	INTR(18,intr18,)
	INTR(19,intr19,)
	INTR(20,intr20,)
	INTR(21,intr21,)
	INTR(22,intr22,)
	INTR(23,intr23,)

	FAST_UNPEND(0,fastunpend0)
	FAST_UNPEND(1,fastunpend1)
	FAST_UNPEND(2,fastunpend2)
	FAST_UNPEND(3,fastunpend3)
	FAST_UNPEND(4,fastunpend4)
	FAST_UNPEND(5,fastunpend5)
	FAST_UNPEND(6,fastunpend6)
	FAST_UNPEND(7,fastunpend7)
	FAST_UNPEND(8,fastunpend8)
	FAST_UNPEND(9,fastunpend9)
	FAST_UNPEND(10,fastunpend10)
	FAST_UNPEND(11,fastunpend11)
	FAST_UNPEND(12,fastunpend12)
	FAST_UNPEND(13,fastunpend13)
	FAST_UNPEND(14,fastunpend14)
	FAST_UNPEND(15,fastunpend15)
	FAST_UNPEND(16,fastunpend16)
	FAST_UNPEND(17,fastunpend17)
	FAST_UNPEND(18,fastunpend18)
	FAST_UNPEND(19,fastunpend19)
	FAST_UNPEND(20,fastunpend20)
	FAST_UNPEND(21,fastunpend21)
	FAST_UNPEND(22,fastunpend22)
	FAST_UNPEND(23,fastunpend23)

	WRONGINTR(0,wrongintr0)
	WRONGINTR(1,wrongintr1)
	WRONGINTR(2,wrongintr2)
	WRONGINTR(3,wrongintr3)
	WRONGINTR(4,wrongintr4)
	WRONGINTR(5,wrongintr5)
	WRONGINTR(6,wrongintr6)
	WRONGINTR(7,wrongintr7)
	WRONGINTR(8,wrongintr8)
	WRONGINTR(9,wrongintr9)
	WRONGINTR(10,wrongintr10)
	WRONGINTR(11,wrongintr11)
	WRONGINTR(12,wrongintr12)
	WRONGINTR(13,wrongintr13)
	WRONGINTR(14,wrongintr14)
	WRONGINTR(15,wrongintr15)
	WRONGINTR(16,wrongintr16)
	WRONGINTR(17,wrongintr17)
	WRONGINTR(18,wrongintr18)
	WRONGINTR(19,wrongintr19)
	WRONGINTR(20,wrongintr20)
	WRONGINTR(21,wrongintr21)
	WRONGINTR(22,wrongintr22)
	WRONGINTR(23,wrongintr23)
MCOUNT_LABEL(eintr)

	.data

#ifdef COUNT_XINVLTLB_HITS
	.globl	xhits
xhits:
	.space	(NCPU * 4), 0
#endif /* COUNT_XINVLTLB_HITS */

/* variables used by stop_cpus()/restart_cpus()/Xcpustop */
	.globl stopped_cpus, started_cpus
stopped_cpus:
	.long	0
started_cpus:
	.long	0

	.globl CNAME(cpustop_restartfunc)
CNAME(cpustop_restartfunc):
	.long 0
		
	.globl	apic_pin_trigger
apic_pin_trigger:
	.long	0

	.text
