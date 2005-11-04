/*
 * Copyright (c) 1996, by Steve Passe
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the developer may NOT be used to endorse or promote products
 *    derived from this software without specific prior written permission.
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
 * $FreeBSD: src/sys/i386/i386/mpapic.c,v 1.37.2.7 2003/01/25 02:31:47 peter Exp $
 * $DragonFly: src/sys/platform/pc32/apic/mpapic.c,v 1.15 2005/11/04 08:57:24 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <machine/globaldata.h>
#include <machine/smp.h>
#include <arch/apic/mpapic.h>
#include <machine/segments.h>
#include <sys/thread2.h>

#include <i386/isa/intr_machdep.h>	/* Xspuriousint() */

/* EISA Edge/Level trigger control registers */
#define ELCR0	0x4d0			/* eisa irq 0-7 */
#define ELCR1	0x4d1			/* eisa irq 8-15 */

/*
 * pointers to pmapped apic hardware.
 */

#if defined(APIC_IO)
volatile ioapic_t	**ioapic;
#endif	/* APIC_IO */

/*
 * Enable APIC, configure interrupts.
 */
void
apic_initialize(void)
{
	u_int   temp;

	/*
	 * setup LVT1 as ExtINT on the BSP.  This is theoretically an
	 * aggregate interrupt input from the 8259.  The INTA cycle
	 * will be routed to the external controller (the 8259) which
	 * is expected to supply the vector.
	 *
	 * Must be setup edge triggered, active high.
	 *
	 * Disable LVT1 on the APs.  It doesn't matter what delivery
	 * mode we use because we leave it masked.
	 */
	temp = lapic.lvt_lint0;
	temp &= ~(APIC_LVT_MASKED | APIC_LVT_TRIG_MASK | 
		  APIC_LVT_POLARITY_MASK | APIC_LVT_DM_MASK);
	if (mycpu->gd_cpuid == 0)
		temp |= APIC_LVT_DM_EXTINT;
	else
		temp |= APIC_LVT_DM_FIXED | APIC_LVT_MASKED;
	lapic.lvt_lint0 = temp;

	/*
	 * setup LVT2 as NMI, masked till later.  Edge trigger, active high.
	 */
	temp = lapic.lvt_lint1;
	temp &= ~(APIC_LVT_MASKED | APIC_LVT_TRIG_MASK | 
		  APIC_LVT_POLARITY_MASK | APIC_LVT_DM_MASK);
	temp |= APIC_LVT_MASKED | APIC_LVT_DM_NMI;
	lapic.lvt_lint1 = temp;

	/*
	 * Set the Task Priority Register as needed.   At the moment allow
	 * interrupts on all cpus (the APs will remain CLId until they are
	 * ready to deal).  We could disable all but IPIs by setting
	 * temp |= TPR_IPI_ONLY for cpu != 0.
	 */
	temp = lapic.tpr;
	temp &= ~APIC_TPR_PRIO;		/* clear priority field */
#ifndef APIC_IO
	/*
	 * If we are NOT running the IO APICs, the LAPIC will only be used
	 * for IPIs.  Set the TPR to prevent any unintentional interrupts.
	 */
	temp |= TPR_IPI_ONLY;
#endif

	lapic.tpr = temp;

	/* 
	 * enable the local APIC 
	 */
	temp = lapic.svr;
	temp |= APIC_SVR_ENABLE;	/* enable the APIC */
	temp &= ~APIC_SVR_FOCUS_DISABLE; /* enable lopri focus processor */

	/*
	 * Set the spurious interrupt vector.  The low 4 bits of the vector
	 * must be 1111.
	 */
	if ((XSPURIOUSINT_OFFSET & 0x0F) != 0x0F)
		panic("bad XSPURIOUSINT_OFFSET: 0x%08x", XSPURIOUSINT_OFFSET);
	temp &= ~APIC_SVR_VECTOR;
	temp |= XSPURIOUSINT_OFFSET;

	lapic.svr = temp;

	if (bootverbose)
		apic_dump("apic_initialize()");
}


/*
 * dump contents of local APIC registers
 */
void
apic_dump(char* str)
{
	printf("SMP: CPU%d %s:\n", mycpu->gd_cpuid, str);
	printf("     lint0: 0x%08x lint1: 0x%08x TPR: 0x%08x SVR: 0x%08x\n",
		lapic.lvt_lint0, lapic.lvt_lint1, lapic.tpr, lapic.svr);
}


#if defined(APIC_IO)

/*
 * IO APIC code,
 */

#define IOAPIC_ISA_INTS		16
#define REDIRCNT_IOAPIC(A) \
	    ((int)((io_apic_versions[(A)] & IOART_VER_MAXREDIR) >> MAXREDIRSHIFT) + 1)

static int trigger (int apic, int pin, u_int32_t * flags);
static void polarity (int apic, int pin, u_int32_t * flags, int level);

#define DEFAULT_FLAGS		\
	((u_int32_t)		\
	 (IOART_INTMSET |	\
	  IOART_DESTPHY |	\
	  IOART_DELLOPRI))

#define DEFAULT_ISA_FLAGS	\
	((u_int32_t)		\
	 (IOART_INTMSET |	\
	  IOART_TRGREDG |	\
	  IOART_INTAHI |	\
	  IOART_DESTPHY |	\
	  IOART_DELLOPRI))

void
io_apic_set_id(int apic, int id)
{
	u_int32_t ux;
	
	ux = io_apic_read(apic, IOAPIC_ID);	/* get current contents */
	if (((ux & APIC_ID_MASK) >> 24) != id) {
		printf("Changing APIC ID for IO APIC #%d"
		       " from %d to %d on chip\n",
		       apic, ((ux & APIC_ID_MASK) >> 24), id);
		ux &= ~APIC_ID_MASK;	/* clear the ID field */
		ux |= (id << 24);
		io_apic_write(apic, IOAPIC_ID, ux);	/* write new value */
		ux = io_apic_read(apic, IOAPIC_ID);	/* re-read && test */
		if (((ux & APIC_ID_MASK) >> 24) != id)
			panic("can't control IO APIC #%d ID, reg: 0x%08x",
			      apic, ux);
	}
}


int
io_apic_get_id(int apic)
{
  return (io_apic_read(apic, IOAPIC_ID) & APIC_ID_MASK) >> 24;
}
  


/*
 * Setup the IO APIC.
 */

extern int	apic_pin_trigger;	/* 'opaque' */

void
io_apic_setup_intpin(int apic, int pin)
{
	int bus, bustype, irq;
	u_char		select;		/* the select register is 8 bits */
	u_int32_t	flags;		/* the window register is 32 bits */
	u_int32_t	target;		/* the window register is 32 bits */
	u_int32_t	vector;		/* the window register is 32 bits */
	int		level;

	select = pin * 2 + IOAPIC_REDTBL0;	/* register */

	/*
	 * Always clear an IO APIC pin before [re]programming it.  This is
	 * particularly important if the pin is set up for a level interrupt
	 * as the IOART_REM_IRR bit might be set.   When we reprogram the
	 * vector any EOI from pending ints on this pin could be lost and
	 * IRR might never get reset.
	 *
	 * To fix this problem, clear the vector and make sure it is 
	 * programmed as an edge interrupt.  This should theoretically
	 * clear IRR so we can later, safely program it as a level 
	 * interrupt.
	 */
	imen_lock();

	flags = io_apic_read(apic, select) & IOART_RESV;
	flags |= IOART_INTMSET | IOART_TRGREDG | IOART_INTAHI;
	flags |= IOART_DESTPHY | IOART_DELFIXED;

	target = io_apic_read(apic, select + 1) & IOART_HI_DEST_RESV;
	target |= 0;	/* fixed mode cpu mask of 0 - don't deliver anywhere */

	vector = 0;

	io_apic_write(apic, select, flags | vector);
	io_apic_write(apic, select + 1, target);

	imen_unlock();

	/*
	 * We only deal with vectored interrupts here.  ? documentation is
	 * lacking, I'm guessing an interrupt type of 0 is the 'INT' type,
	 * vs ExTINT, etc.
	 *
	 * This test also catches unconfigured pins.
	 */
	if (apic_int_type(apic, pin) != 0)
		return;

	/*
	 * Leave the pin unprogrammed if it does not correspond to
	 * an IRQ.
	 */
	irq = apic_irq(apic, pin);
	if (irq < 0)
		return;
	
	/* determine the bus type for this pin */
	bus = apic_src_bus_id(apic, pin);
	if (bus < 0)
		return;
	bustype = apic_bus_type(bus);
	
	if ((bustype == ISA) &&
	    (pin < IOAPIC_ISA_INTS) && 
	    (irq == pin) &&
	    (apic_polarity(apic, pin) == 0x1) &&
	    (apic_trigger(apic, pin) == 0x3)) {
		/* 
		 * A broken BIOS might describe some ISA 
		 * interrupts as active-high level-triggered.
		 * Use default ISA flags for those interrupts.
		 */
		flags = DEFAULT_ISA_FLAGS;
	} else {
		/* 
		 * Program polarity and trigger mode according to 
		 * interrupt entry.
		 */
		flags = DEFAULT_FLAGS;
		level = trigger(apic, pin, &flags);
		if (level == 1)
			apic_pin_trigger |= (1 << irq);
		polarity(apic, pin, &flags, level);
	}
	
	if (bootverbose) {
		printf("IOAPIC #%d intpin %d -> irq %d\n",
		       apic, pin, irq);
	}

	/*
	 * Program the appropriate registers.  This routing may be 
	 * overridden when an interrupt handler for a device is
	 * actually added (see inthand_add(), which calls through
	 * the MACHINTR ABI to set up an interrupt handler/vector).
	 *
	 * The order in which we must program the two registers for
	 * safety is unclear! XXX
	 */
	imen_lock();

	vector = IDT_OFFSET + irq;			/* IDT vec */
	target = io_apic_read(apic, select + 1) & IOART_HI_DEST_RESV;
	target |= IOART_HI_DEST_BROADCAST;
	flags |= io_apic_read(apic, select) & IOART_RESV;
	io_apic_write(apic, select, flags | vector);
	io_apic_write(apic, select + 1, target);

	imen_unlock();
}

int
io_apic_setup(int apic)
{
	int		maxpin;
	int		pin;

	if (apic == 0)
		apic_pin_trigger = 0;	/* default to edge-triggered */

	maxpin = REDIRCNT_IOAPIC(apic);		/* pins in APIC */
	printf("Programming %d pins in IOAPIC #%d\n", maxpin, apic);
	
	for (pin = 0; pin < maxpin; ++pin) {
		io_apic_setup_intpin(apic, pin);
	}
	while (pin < 32) {
		if (apic_int_type(apic, pin) >= 0) {
			printf("Warning: IOAPIC #%d pin %d does not exist,"
				" cannot program!\n", apic, pin);
		}
		++pin;
	}

	/* return GOOD status */
	return 0;
}
#undef DEFAULT_ISA_FLAGS
#undef DEFAULT_FLAGS


#define DEFAULT_EXTINT_FLAGS	\
	((u_int32_t)		\
	 (IOART_INTMSET |	\
	  IOART_TRGREDG |	\
	  IOART_INTAHI |	\
	  IOART_DESTPHY |	\
	  IOART_DELLOPRI))

/*
 * Setup the source of External INTerrupts.
 */
int
ext_int_setup(int apic, int intr)
{
	u_char  select;		/* the select register is 8 bits */
	u_int32_t flags;	/* the window register is 32 bits */
	u_int32_t target;	/* the window register is 32 bits */
	u_int32_t vector;	/* the window register is 32 bits */

	if (apic_int_type(apic, intr) != 3)
		return -1;

	target = IOART_HI_DEST_BROADCAST;
	select = IOAPIC_REDTBL0 + (2 * intr);
	vector = IDT_OFFSET + intr;
	flags = DEFAULT_EXTINT_FLAGS;

	io_apic_write(apic, select, flags | vector);
	io_apic_write(apic, select + 1, target);

	return 0;
}
#undef DEFAULT_EXTINT_FLAGS


/*
 * Set the trigger level for an IO APIC pin.
 */
static int
trigger(int apic, int pin, u_int32_t * flags)
{
	int     id;
	int     eirq;
	int     level;
	static int intcontrol = -1;

	switch (apic_trigger(apic, pin)) {

	case 0x00:
		break;

	case 0x01:
		*flags &= ~IOART_TRGRLVL;	/* *flags |= IOART_TRGREDG */
		return 0;

	case 0x03:
		*flags |= IOART_TRGRLVL;
		return 1;

	case -1:
	default:
		goto bad;
	}

	if ((id = apic_src_bus_id(apic, pin)) == -1)
		goto bad;

	switch (apic_bus_type(id)) {
	case ISA:
		*flags &= ~IOART_TRGRLVL;	/* *flags |= IOART_TRGREDG; */
		return 0;

	case EISA:
		eirq = apic_src_bus_irq(apic, pin);

		if (eirq < 0 || eirq > 15) {
			printf("EISA IRQ %d?!?!\n", eirq);
			goto bad;
		}

		if (intcontrol == -1) {
			intcontrol = inb(ELCR1) << 8;
			intcontrol |= inb(ELCR0);
			printf("EISA INTCONTROL = %08x\n", intcontrol);
		}

		/* Use ELCR settings to determine level or edge mode */
		level = (intcontrol >> eirq) & 1;

		/*
		 * Note that on older Neptune chipset based systems, any
		 * pci interrupts often show up here and in the ELCR as well
		 * as level sensitive interrupts attributed to the EISA bus.
		 */

		if (level)
			*flags |= IOART_TRGRLVL;
		else
			*flags &= ~IOART_TRGRLVL;

		return level;

	case PCI:
		*flags |= IOART_TRGRLVL;
		return 1;

	case -1:
	default:
		goto bad;
	}

bad:
	panic("bad APIC IO INT flags");
}


/*
 * Set the polarity value for an IO APIC pin.
 */
static void
polarity(int apic, int pin, u_int32_t * flags, int level)
{
	int     id;

	switch (apic_polarity(apic, pin)) {

	case 0x00:
		break;

	case 0x01:
		*flags &= ~IOART_INTALO;	/* *flags |= IOART_INTAHI */
		return;

	case 0x03:
		*flags |= IOART_INTALO;
		return;

	case -1:
	default:
		goto bad;
	}

	if ((id = apic_src_bus_id(apic, pin)) == -1)
		goto bad;

	switch (apic_bus_type(id)) {
	case ISA:
		*flags &= ~IOART_INTALO;	/* *flags |= IOART_INTAHI */
		return;

	case EISA:
		/* polarity converter always gives active high */
		*flags &= ~IOART_INTALO;
		return;

	case PCI:
		*flags |= IOART_INTALO;
		return;

	case -1:
	default:
		goto bad;
	}

bad:
	panic("bad APIC IO INT flags");
}


/*
 * Print contents of apic_imen.
 */
extern	u_int apic_imen;		/* keep apic_imen 'opaque' */
void
imen_dump(void)
{
	int x;

	printf("SMP: enabled INTs: ");
	for (x = 0; x < 24; ++x)
		if ((apic_imen & (1 << x)) == 0)
        		printf("%d, ", x);
	printf("apic_imen: 0x%08x\n", apic_imen);
}


/*
 * Inter Processor Interrupt functions.
 */

#endif	/* APIC_IO */

/*
 * Send APIC IPI 'vector' to 'destType' via 'deliveryMode'.
 *
 *  destType is 1 of: APIC_DEST_SELF, APIC_DEST_ALLISELF, APIC_DEST_ALLESELF
 *  vector is any valid SYSTEM INT vector
 *  delivery_mode is 1 of: APIC_DELMODE_FIXED, APIC_DELMODE_LOWPRIO
 *
 * A backlog of requests can create a deadlock between cpus.  To avoid this
 * we have to be able to accept IPIs at the same time we are trying to send
 * them.  The critical section prevents us from attempting to send additional
 * IPIs reentrantly, but also prevents IPIQ processing so we have to call
 * lwkt_process_ipiq() manually.  It's rather messy and expensive for this
 * to occur but fortunately it does not happen too often.
 */
int
apic_ipi(int dest_type, int vector, int delivery_mode)
{
	u_long  icr_lo;

	crit_enter();
	if ((lapic.icr_lo & APIC_DELSTAT_MASK) != 0) {
	    unsigned int eflags = read_eflags();
	    cpu_enable_intr();
	    while ((lapic.icr_lo & APIC_DELSTAT_MASK) != 0) {
		lwkt_process_ipiq();
	    }
	    write_eflags(eflags);
	}

	icr_lo = (lapic.icr_lo & APIC_ICRLO_RESV_MASK) | dest_type | 
		delivery_mode | vector;
	lapic.icr_lo = icr_lo;
	crit_exit();
	return 0;
}

void
single_apic_ipi(int cpu, int vector, int delivery_mode)
{
	u_long  icr_lo;
	u_long  icr_hi;

	crit_enter();
	if ((lapic.icr_lo & APIC_DELSTAT_MASK) != 0) {
	    unsigned int eflags = read_eflags();
	    cpu_enable_intr();
	    while ((lapic.icr_lo & APIC_DELSTAT_MASK) != 0) {
		lwkt_process_ipiq();
	    }
	    write_eflags(eflags);
	}
	icr_hi = lapic.icr_hi & ~APIC_ID_MASK;
	icr_hi |= (CPU_TO_ID(cpu) << 24);
	lapic.icr_hi = icr_hi;

	/* build IRC_LOW */
	icr_lo = (lapic.icr_lo & APIC_ICRLO_RESV_MASK)
	    | APIC_DEST_DESTFLD | delivery_mode | vector;

	/* write APIC ICR */
	lapic.icr_lo = icr_lo;
	crit_exit();
}

#if 0	

/*
 * Returns 0 if the apic is busy, 1 if we were able to queue the request.
 *
 * NOT WORKING YET!  The code as-is may end up not queueing an IPI at all
 * to the target, and the scheduler does not 'poll' for IPI messages.
 */
int
single_apic_ipi_passive(int cpu, int vector, int delivery_mode)
{
	u_long  icr_lo;
	u_long  icr_hi;

	crit_enter();
	if ((lapic.icr_lo & APIC_DELSTAT_MASK) != 0) {
	    crit_exit();
	    return(0);
	}
	icr_hi = lapic.icr_hi & ~APIC_ID_MASK;
	icr_hi |= (CPU_TO_ID(cpu) << 24);
	lapic.icr_hi = icr_hi;

	/* build IRC_LOW */
	icr_lo = (lapic.icr_lo & APIC_RESV2_MASK)
	    | APIC_DEST_DESTFLD | delivery_mode | vector;

	/* write APIC ICR */
	lapic.icr_lo = icr_lo;
	crit_exit();
	return(1);
}

#endif

/*
 * Send APIC IPI 'vector' to 'target's via 'delivery_mode'.
 *
 * target is a bitmask of destination cpus.  Vector is any
 * valid system INT vector.  Delivery mode may be either
 * APIC_DELMODE_FIXED or APIC_DELMODE_LOWPRIO.
 */
void
selected_apic_ipi(u_int target, int vector, int delivery_mode)
{
	crit_enter();
	while (target) {
		int n = bsfl(target);
		target &= ~(1 << n);
		single_apic_ipi(n, vector, delivery_mode);
	}
	crit_exit();
}

/*
 * Timer code, in development...
 *  - suggested by rgrimes@gndrsh.aac.dev.com
 */

/** XXX FIXME: temp hack till we can determin bus clock */
#ifndef BUS_CLOCK
#define BUS_CLOCK	66000000
#define bus_clock()	66000000
#endif

#if defined(READY)
int acquire_apic_timer (void);
int release_apic_timer (void);

/*
 * Acquire the APIC timer for exclusive use.
 */
int
acquire_apic_timer(void)
{
#if 1
	return 0;
#else
	/** XXX FIXME: make this really do something */
	panic("APIC timer in use when attempting to aquire");
#endif
}


/*
 * Return the APIC timer.
 */
int
release_apic_timer(void)
{
#if 1
	return 0;
#else
	/** XXX FIXME: make this really do something */
	panic("APIC timer was already released");
#endif
}
#endif	/* READY */


/*
 * Load a 'downcount time' in uSeconds.
 */
void
set_apic_timer(int value)
{
	u_long  lvtt;
	long    ticks_per_microsec;

	/*
	 * Calculate divisor and count from value:
	 * 
	 *  timeBase == CPU bus clock divisor == [1,2,4,8,16,32,64,128]
	 *  value == time in uS
	 */
	lapic.dcr_timer = APIC_TDCR_1;
	ticks_per_microsec = bus_clock() / 1000000;

	/* configure timer as one-shot */
	lvtt = lapic.lvt_timer;
	lvtt &= ~(APIC_LVTT_VECTOR | APIC_LVTT_DS);
	lvtt &= ~(APIC_LVTT_PERIODIC);
	lvtt |= APIC_LVTT_MASKED;		/* no INT, one-shot */
	lapic.lvt_timer = lvtt;

	/* */
	lapic.icr_timer = value * ticks_per_microsec;
}


/*
 * Read remaining time in timer.
 */
int
read_apic_timer(void)
{
#if 0
	/** XXX FIXME: we need to return the actual remaining time,
         *         for now we just return the remaining count.
         */
#else
	return lapic.ccr_timer;
#endif
}


/*
 * Spin-style delay, set delay time in uS, spin till it drains.
 */
void
u_sleep(int count)
{
	set_apic_timer(count);
	while (read_apic_timer())
		 /* spin */ ;
}
