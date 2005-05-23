/*-
 * Copyright (c) 1991 The Regents of the University of California.
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
 *	from: @(#)isa.c	7.2 (Berkeley) 5/13/91
 * $FreeBSD: src/sys/i386/isa/intr_machdep.c,v 1.29.2.5 2001/10/14 06:54:27 luigi Exp $
 * $DragonFly: src/sys/platform/pc32/isa/intr_machdep.c,v 1.26 2005/05/23 18:19:53 dillon Exp $
 */
/*
 * This file contains an aggregated module marked:
 * Copyright (c) 1997, Stefan Esser <se@freebsd.org>
 * All rights reserved.
 * See the notice for details.
 */

#include "use_isa.h"
#include "use_mca.h"
#include "opt_auto_eoi.h"

#include <sys/param.h>
#ifndef SMP
#include <machine/lock.h>
#endif
#include <sys/systm.h>
#include <sys/syslog.h>
#include <sys/malloc.h>
#include <sys/errno.h>
#include <sys/interrupt.h>
#include <machine/ipl.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <sys/bus.h> 
#include <machine/globaldata.h>
#include <sys/proc.h>
#include <sys/thread2.h>

#include <machine/smptests.h>			/** FAST_HI */
#include <machine/smp.h>
#include <bus/isa/i386/isa.h>
#include <i386/isa/icu.h>

#if NISA > 0
#include <bus/isa/isavar.h>
#endif
#include <i386/isa/intr_machdep.h>
#include <bus/isa/isavar.h>
#include <sys/interrupt.h>
#ifdef APIC_IO
#include <machine/clock.h>
#endif
#include <machine/cpu.h>

#if NMCA > 0
#include <bus/mca/i386/mca_machdep.h>
#endif

/* XXX should be in suitable include files */
#define	ICU_IMR_OFFSET		1		/* IO_ICU{1,2} + 1 */
#define	ICU_SLAVEID			2

#ifdef APIC_IO
/*
 * This is to accommodate "mixed-mode" programming for 
 * motherboards that don't connect the 8254 to the IO APIC.
 */
#define	AUTO_EOI_1	1
#endif

#define	NR_INTRNAMES	(1 + ICU_LEN + 2 * ICU_LEN)

static inthand2_t isa_strayintr;
#if defined(FAST_HI) && defined(APIC_IO)
static inthand2_t isa_wrongintr;
#endif
static void	init_i8259(void);

void	*intr_unit[ICU_LEN*2];
u_long	*intr_countp[ICU_LEN*2];
inthand2_t *intr_handler[ICU_LEN*2] = {
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
	isa_strayintr, isa_strayintr, isa_strayintr, isa_strayintr,
};

static struct md_intr_info {
    int		irq;
    u_int	mask;
    int		mihandler_installed;
    u_int	*maskp;
} intr_info[ICU_LEN*2];

static inthand_t *fastintr[ICU_LEN] = {
	&IDTVEC(fastintr0), &IDTVEC(fastintr1),
	&IDTVEC(fastintr2), &IDTVEC(fastintr3),
	&IDTVEC(fastintr4), &IDTVEC(fastintr5),
	&IDTVEC(fastintr6), &IDTVEC(fastintr7),
	&IDTVEC(fastintr8), &IDTVEC(fastintr9),
	&IDTVEC(fastintr10), &IDTVEC(fastintr11),
	&IDTVEC(fastintr12), &IDTVEC(fastintr13),
	&IDTVEC(fastintr14), &IDTVEC(fastintr15),
#if defined(APIC_IO)
	&IDTVEC(fastintr16), &IDTVEC(fastintr17),
	&IDTVEC(fastintr18), &IDTVEC(fastintr19),
	&IDTVEC(fastintr20), &IDTVEC(fastintr21),
	&IDTVEC(fastintr22), &IDTVEC(fastintr23),
#endif /* APIC_IO */
};

unpendhand_t *fastunpend[ICU_LEN] = {
	IDTVEC(fastunpend0), IDTVEC(fastunpend1),
	IDTVEC(fastunpend2), IDTVEC(fastunpend3),
	IDTVEC(fastunpend4), IDTVEC(fastunpend5),
	IDTVEC(fastunpend6), IDTVEC(fastunpend7),
	IDTVEC(fastunpend8), IDTVEC(fastunpend9),
	IDTVEC(fastunpend10), IDTVEC(fastunpend11),
	IDTVEC(fastunpend12), IDTVEC(fastunpend13),
	IDTVEC(fastunpend14), IDTVEC(fastunpend15),
#if defined(APIC_IO)
	IDTVEC(fastunpend16), IDTVEC(fastunpend17),
	IDTVEC(fastunpend18), IDTVEC(fastunpend19),
	IDTVEC(fastunpend20), IDTVEC(fastunpend21),
	IDTVEC(fastunpend22), IDTVEC(fastunpend23),
#endif
};

static inthand_t *slowintr[ICU_LEN] = {
	&IDTVEC(intr0), &IDTVEC(intr1), &IDTVEC(intr2), &IDTVEC(intr3),
	&IDTVEC(intr4), &IDTVEC(intr5), &IDTVEC(intr6), &IDTVEC(intr7),
	&IDTVEC(intr8), &IDTVEC(intr9), &IDTVEC(intr10), &IDTVEC(intr11),
	&IDTVEC(intr12), &IDTVEC(intr13), &IDTVEC(intr14), &IDTVEC(intr15),
#if defined(APIC_IO)
	&IDTVEC(intr16), &IDTVEC(intr17), &IDTVEC(intr18), &IDTVEC(intr19),
	&IDTVEC(intr20), &IDTVEC(intr21), &IDTVEC(intr22), &IDTVEC(intr23),
#endif /* APIC_IO */
};

#define NMI_PARITY (1 << 7)
#define NMI_IOCHAN (1 << 6)
#define ENMI_WATCHDOG (1 << 7)
#define ENMI_BUSTIMER (1 << 6)
#define ENMI_IOSTATUS (1 << 5)

/*
 * Handle a NMI, possibly a machine check.
 * return true to panic system, false to ignore.
 */
int
isa_nmi(cd)
	int cd;
{
	int retval = 0;
	int isa_port = inb(0x61);
	int eisa_port = inb(0x461);

	log(LOG_CRIT, "NMI ISA %x, EISA %x\n", isa_port, eisa_port);
#if NMCA > 0
	if (MCA_system && mca_bus_nmi())
		return(0);
#endif
	
	if (isa_port & NMI_PARITY) {
		log(LOG_CRIT, "RAM parity error, likely hardware failure.");
		retval = 1;
	}

	if (isa_port & NMI_IOCHAN) {
		log(LOG_CRIT, "I/O channel check, likely hardware failure.");
		retval = 1;
	}

	/*
	 * On a real EISA machine, this will never happen.  However it can
	 * happen on ISA machines which implement XT style floating point
	 * error handling (very rare).  Save them from a meaningless panic.
	 */
	if (eisa_port == 0xff)
		return(retval);

	if (eisa_port & ENMI_WATCHDOG) {
		log(LOG_CRIT, "EISA watchdog timer expired, likely hardware failure.");
		retval = 1;
	}

	if (eisa_port & ENMI_BUSTIMER) {
		log(LOG_CRIT, "EISA bus timeout, likely hardware failure.");
		retval = 1;
	}

	if (eisa_port & ENMI_IOSTATUS) {
		log(LOG_CRIT, "EISA I/O port status error.");
		retval = 1;
	}
	return(retval);
}

/*
 *  ICU reinitialize when ICU configuration has lost.
 */
void
icu_reinit()
{
       int i;

       init_i8259();
       for(i=0;i<ICU_LEN;i++)
               if(intr_handler[i] != isa_strayintr)
                       INTREN(1<<i);
}


/*
 * Fill in default interrupt table (in case of spurious interrupt
 * during configuration of kernel, setup interrupt control unit
 */
void
isa_defaultirq()
{
	int i;

	/* icu vectors */
	for (i = 0; i < ICU_LEN; i++)
		icu_unset(i, isa_strayintr);
	init_i8259();
}

static void
init_i8259(void)
{

	/* initialize 8259's */
#if NMCA > 0
	if (MCA_system)
		outb(IO_ICU1, 0x19);		/* reset; program device, four bytes */
	else
#endif
		outb(IO_ICU1, 0x11);		/* reset; program device, four bytes */

	outb(IO_ICU1+ICU_IMR_OFFSET, NRSVIDT);	/* starting at this vector index */
	outb(IO_ICU1+ICU_IMR_OFFSET, IRQ_SLAVE);		/* slave on line 7 */
#ifdef AUTO_EOI_1
	outb(IO_ICU1+ICU_IMR_OFFSET, 2 | 1);		/* auto EOI, 8086 mode */
#else
	outb(IO_ICU1+ICU_IMR_OFFSET, 1);		/* 8086 mode */
#endif
	outb(IO_ICU1+ICU_IMR_OFFSET, 0xff);		/* leave interrupts masked */
	outb(IO_ICU1, 0x0a);		/* default to IRR on read */
	outb(IO_ICU1, 0xc0 | (3 - 1));	/* pri order 3-7, 0-2 (com2 first) */

#if NMCA > 0
	if (MCA_system)
		outb(IO_ICU2, 0x19);		/* reset; program device, four bytes */
	else
#endif
		outb(IO_ICU2, 0x11);		/* reset; program device, four bytes */

	outb(IO_ICU2+ICU_IMR_OFFSET, NRSVIDT+8); /* staring at this vector index */
	outb(IO_ICU2+ICU_IMR_OFFSET, ICU_SLAVEID);         /* my slave id is 7 */
#ifdef AUTO_EOI_2
	outb(IO_ICU2+ICU_IMR_OFFSET, 2 | 1);		/* auto EOI, 8086 mode */
#else
	outb(IO_ICU2+ICU_IMR_OFFSET,1);		/* 8086 mode */
#endif
	outb(IO_ICU2+ICU_IMR_OFFSET, 0xff);          /* leave interrupts masked */
	outb(IO_ICU2, 0x0a);		/* default to IRR on read */
}

/*
 * Caught a stray interrupt, notify
 */
static void
isa_strayintr(void *vcookiep)
{
	int intr = (void **)vcookiep - &intr_unit[0];

	/* DON'T BOTHER FOR NOW! */
	/* for some reason, we get bursts of intr #7, even if not enabled! */
	/*
	 * Well the reason you got bursts of intr #7 is because someone
	 * raised an interrupt line and dropped it before the 8259 could
	 * prioritize it.  This is documented in the intel data book.  This
	 * means you have BAD hardware!  I have changed this so that only
	 * the first 5 get logged, then it quits logging them, and puts
	 * out a special message. rgrimes 3/25/1993
	 */
	/*
	 * XXX TODO print a different message for #7 if it is for a
	 * glitch.  Glitches can be distinguished from real #7's by
	 * testing that the in-service bit is _not_ set.  The test
	 * must be done before sending an EOI so it can't be done if
	 * we are using AUTO_EOI_1.
	 */
	if (intrcnt[1 + intr] <= 5)
		log(LOG_ERR, "stray irq %d\n", intr);
	if (intrcnt[1 + intr] == 5)
		log(LOG_CRIT,
		    "too many stray irq %d's; not logging any more\n", intr);
}

#if defined(FAST_HI) && defined(APIC_IO)

/*
 * This occurs if we mis-programmed the APIC and its vector is still
 * pointing to the slow vector even when we thought we reprogrammed it
 * to the high vector.  This can occur when interrupts are improperly
 * routed by the APIC.  The unit data is opaque so we have to try to
 * find it in the unit array.
 */
static void
isa_wrongintr(void *vcookiep)
{
	int intr;

	for (intr = 0; intr < ICU_LEN*2; ++intr) {
		if (intr_unit[intr] == vcookiep)
			break;
	}
	if (intr == ICU_LEN*2) {
		log(LOG_ERR, "stray unknown irq (APIC misprogrammed)\n");
	} else if (intrcnt[1 + intr] <= 5) {
		log(LOG_ERR, "stray irq ~%d (APIC misprogrammed)\n", intr);
	} else if (intrcnt[1 + intr] == 6) {
		log(LOG_CRIT,
		    "too many stray irq ~%d's; not logging any more\n", intr);
	}
}

#endif

#if NISA > 0
/*
 * Return a bitmap of the current interrupt requests.  This is 8259-specific
 * and is only suitable for use at probe time.
 */
intrmask_t
isa_irq_pending(void)
{
	u_char irr1;
	u_char irr2;

	irr1 = inb(IO_ICU1);
	irr2 = inb(IO_ICU2);
	return ((irr2 << 8) | irr1);
}
#endif

int
update_intr_masks(void)
{
	int intr, n=0;
	u_int mask,*maskptr;

	for (intr=0; intr < ICU_LEN; intr ++) {
#if defined(APIC_IO)
		/* no 8259 SLAVE to ignore */
#else
		if (intr==ICU_SLAVEID) continue;	/* ignore 8259 SLAVE output */
#endif /* APIC_IO */
		maskptr = intr_info[intr].maskp;
		if (!maskptr)
			continue;
		*maskptr |= SWI_CLOCK_MASK | (1 << intr);
		mask = *maskptr;
		if (mask != intr_info[intr].mask) {
#if 0
			printf ("intr_mask[%2d] old=%08x new=%08x ptr=%p.\n",
				intr, intr_info[intr].mask, mask, maskptr);
#endif
			intr_info[intr].mask = mask;
			n++;
		}

	}
	return (n);
}

static void
update_intrname(int intr, char *name)
{
	char buf[32];
	char *cp;
	int name_index, off, strayintr;

	/*
	 * Initialise strings for bitbucket and stray interrupt counters.
	 * These have statically allocated indices 0 and 1 through ICU_LEN.
	 */
	if (intrnames[0] == '\0') {
		off = sprintf(intrnames, "???") + 1;
		for (strayintr = 0; strayintr < ICU_LEN; strayintr++)
			off += sprintf(intrnames + off, "stray irq%d",
			    strayintr) + 1;
	}

	if (name == NULL)
		name = "???";
	if (snprintf(buf, sizeof(buf), "%s irq%d", name, intr) >= sizeof(buf))
		goto use_bitbucket;

	/*
	 * Search for `buf' in `intrnames'.  In the usual case when it is
	 * not found, append it to the end if there is enough space (the \0
	 * terminator for the previous string, if any, becomes a separator).
	 */
	for (cp = intrnames, name_index = 0;
	    cp != eintrnames && name_index < NR_INTRNAMES;
	    cp += strlen(cp) + 1, name_index++) {
		if (*cp == '\0') {
			if (strlen(buf) >= eintrnames - cp)
				break;
			strcpy(cp, buf);
			goto found;
		}
		if (strcmp(cp, buf) == 0)
			goto found;
	}

use_bitbucket:
	printf("update_intrname: counting %s irq%d as %s\n", name, intr,
	    intrnames);
	name_index = 0;
found:
	intr_countp[intr] = &intrcnt[name_index];
}

/*
 * NOTE!  intr_handler[] is only used for FAST interrupts, the *vector.s
 * code ignores it for normal interrupts.
 */
int
icu_setup(int intr, inthand2_t *handler, void *arg, u_int *maskptr, int flags)
{
#if defined(FAST_HI) && defined(APIC_IO)
	int		select;		/* the select register is 8 bits */
	int		vector;
	u_int32_t	value;		/* the window register is 32 bits */
#endif /* FAST_HI */
	u_long	ef;
	u_int	mask = (maskptr ? *maskptr : 0);

#if defined(APIC_IO)
	if ((u_int)intr >= ICU_LEN)	/* no 8259 SLAVE to ignore */
#else
	if ((u_int)intr >= ICU_LEN || intr == ICU_SLAVEID)
#endif /* APIC_IO */
		return (EINVAL);
	if (intr_handler[intr] != isa_strayintr)
		return (EBUSY);

	ef = read_eflags();
	cpu_disable_intr();	/* YYY */
	intr_handler[intr] = handler;
	intr_unit[intr] = arg;
	intr_info[intr].maskp = maskptr;
	intr_info[intr].mask = mask | SWI_CLOCK_MASK | (1 << intr);
#if 0
	/* YYY  fast ints supported and mp protected but ... */
	flags &= ~INTR_FAST;
#endif
#if defined(FAST_HI) && defined(APIC_IO)
	if (flags & INTR_FAST) {
		/*
		 * Install a spurious interrupt in the low space in case
		 * the IO apic is not properly reprogrammed.
		 */
		vector = TPR_SLOW_INTS + intr;
		setidt(vector, isa_wrongintr,
		       SDT_SYS386IGT, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
		vector = TPR_FAST_INTS + intr;
		setidt(vector, fastintr[intr],
		       SDT_SYS386IGT, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
	} else {
		vector = TPR_SLOW_INTS + intr;
#ifdef APIC_INTR_REORDER
#ifdef APIC_INTR_HIGHPRI_CLOCK
		/* XXX: Hack (kludge?) for more accurate clock. */
		if (intr == apic_8254_intr || intr == 8) {
			vector = TPR_FAST_INTS + intr;
		}
#endif
#endif
		setidt(vector, slowintr[intr],
		       SDT_SYS386IGT, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
	}
#ifdef APIC_INTR_REORDER
	set_lapic_isrloc(intr, vector);
#endif
	/*
	 * Reprogram the vector in the IO APIC.
	 *
	 * XXX EOI/mask a pending (stray) interrupt on the old vector?
	 */
	if (int_to_apicintpin[intr].ioapic >= 0) {
		select = int_to_apicintpin[intr].redirindex;
		value = io_apic_read(int_to_apicintpin[intr].ioapic, 
				     select) & ~IOART_INTVEC;
		io_apic_write(int_to_apicintpin[intr].ioapic, 
			      select, value | vector);
	}
#else
	setidt(ICU_OFFSET + intr,
	       flags & INTR_FAST ? fastintr[intr] : slowintr[intr],
	       SDT_SYS386IGT, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
#endif /* FAST_HI && APIC_IO */
	INTREN(1 << intr);
	write_eflags(ef);
	return (0);
}

int
icu_unset(intr, handler)
	int	intr;
	inthand2_t *handler;
{
	u_long	ef;

	if ((u_int)intr >= ICU_LEN || handler != intr_handler[intr]) {
		printf("icu_unset: invalid handler %d %p/%p\n", intr, handler, 
		    (((u_int)intr >= ICU_LEN) ? (void *)-1 : intr_handler[intr]));
		return (EINVAL);
	}

	INTRDIS(1 << intr);
	ef = read_eflags();
	cpu_disable_intr();	/* YYY */
	intr_countp[intr] = &intrcnt[1 + intr];
	intr_handler[intr] = isa_strayintr;
	intr_info[intr].maskp = NULL;
	intr_info[intr].mask = HWI_MASK | SWI_MASK;
	intr_unit[intr] = &intr_unit[intr];
#ifdef FAST_HI_XXX
	/* XXX how do I re-create dvp here? */
	setidt(flags & INTR_FAST ? TPR_FAST_INTS + intr : TPR_SLOW_INTS + intr,
	    slowintr[intr], SDT_SYS386IGT, SEL_KPL, GSEL(GCODE_SEL, SEL_KPL));
#else /* FAST_HI */
#ifdef APIC_INTR_REORDER
	set_lapic_isrloc(intr, ICU_OFFSET + intr);
#endif
	setidt(ICU_OFFSET + intr, slowintr[intr], SDT_SYS386IGT, SEL_KPL,
	    GSEL(GCODE_SEL, SEL_KPL));
#endif /* FAST_HI */
	write_eflags(ef);
	return (0);
}


/* The following notice applies beyond this point in the file */

/*
 * Copyright (c) 1997, Stefan Esser <se@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 * $FreeBSD: src/sys/i386/isa/intr_machdep.c,v 1.29.2.5 2001/10/14 06:54:27 luigi Exp $
 *
 */

typedef struct intrec {
	intrmask_t      mask;
	inthand2_t      *handler;
	void            *argument;
	struct intrec   *next;
	char            *name;
	int             intr;
	intrmask_t      *maskptr;
	int             flags;
	int		enabled;
} intrec;

static intrec *intreclist_head[ICU_LEN];

/*
 * The interrupt multiplexer calls each of the handlers in turn.  The
 * ipl is initially quite low.  It is raised as necessary for each call
 * and lowered after the call.  Thus out of order handling is possible
 * even for interrupts of the same type.  This is probably no more
 * harmful than out of order handling in general (not harmful except
 * for real time response which we don't support anyway).
 */
static void
intr_mux(void *arg)
{
	intrec **pp;
	intrec *p;
	intrmask_t oldspl;

	for (pp = arg; (p = *pp) != NULL; pp = &p->next) {
		oldspl = splq(p->mask);
		if (p->enabled)
			p->handler(p->argument);
		splx(oldspl);
	}
}

static intrec*
find_idesc(unsigned *maskptr, int irq)
{
	intrec *p = intreclist_head[irq];

	while (p && p->maskptr != maskptr)
		p = p->next;

	return (p);
}

/*
 * Both the low level handler and the shared interrupt multiplexer
 * block out further interrupts as set in the handlers "mask", while
 * the handler is running. In fact *maskptr should be used for this
 * purpose, but since this requires one more pointer dereference on
 * each interrupt, we rather bother update "mask" whenever *maskptr
 * changes. The function "update_masks" should be called **after**
 * all manipulation of the linked list of interrupt handlers hung
 * off of intrdec_head[irq] is complete, since the chain of handlers
 * will both determine the *maskptr values and the instances of mask
 * that are fixed. This function should be called with the irq for
 * which a new handler has been add blocked, since the masks may not
 * yet know about the use of this irq for a device of a certain class.
 */

static void
update_mux_masks(void)
{
	int irq;
	for (irq = 0; irq < ICU_LEN; irq++) {
		intrec *idesc = intreclist_head[irq];
		while (idesc != NULL) {
			if (idesc->maskptr != NULL) {
				/* our copy of *maskptr may be stale, refresh */
				idesc->mask = *idesc->maskptr;
			}
			idesc = idesc->next;
		}
	}
}

static void
update_masks(intrmask_t *maskptr, int irq)
{
	intrmask_t mask = 1 << irq;

	if (maskptr == NULL)
		return;

	if (find_idesc(maskptr, irq) == NULL) {
		/* no reference to this maskptr was found in this irq's chain */
		*maskptr &= ~mask;
	} else {
		/* a reference to this maskptr was found in this irq's chain */
		*maskptr |= mask;
	}
	/* we need to update all values in the intr_mask[irq] array */
	update_intr_masks();
	/* update mask in chains of the interrupt multiplex handler as well */
	update_mux_masks();
}

/*
 * Add an interrupt handler to the linked list hung off of intreclist_head[irq]
 * and install a shared interrupt multiplex handler.  Install an interrupt
 * thread for each interrupt (though FAST interrupts will not use it).
 * The preemption procedure checks the CPL.  lwkt_preempt() will check
 * relative thread priorities for us as long as we properly pass through
 * critpri.
 *
 * The interrupt thread has already been put on the run queue, so if we cannot
 * preempt we should force a reschedule.
 */
static void
cpu_intr_preempt(struct thread *td, int critpri)
{
	struct md_intr_info *info = td->td_info.intdata;

	if ((curthread->td_cpl & (1 << info->irq)) == 0)
		lwkt_preempt(td, critpri);
	else
		need_lwkt_resched(); /* XXX may not be required */
}

static int
add_intrdesc(intrec *idesc)
{
	int irq = idesc->intr;
	intrec *head;
	intrec **headp;

	/*
	 * There are two ways to enter intr_mux().  (1) via the scheduled
	 * interrupt thread or (2) directly.   The thread mechanism is the
	 * normal mechanism used by SLOW interrupts, while the direct method
	 * is used by FAST interrupts.
	 *
	 * We need to create an interrupt thread if none exists.
	 */
	if (intr_info[irq].mihandler_installed == 0) {
		struct thread *td;

		intr_info[irq].mihandler_installed = 1;
		intr_info[irq].irq = irq;
		td = register_int(irq, intr_mux, &intreclist_head[irq], idesc->name, idesc->maskptr);
		td->td_info.intdata = &intr_info[irq];
		td->td_preemptable = cpu_intr_preempt;
		printf("installed MI handler for int %d\n", irq);
	}

	headp = &intreclist_head[irq];
	head = *headp;

	/*
	 * Check exclusion
	 */
	if (head) {
		if ((idesc->flags & INTR_EXCL) || (head->flags & INTR_EXCL)) {
			printf("\tdevice combination doesn't support "
			       "shared irq%d\n", irq);
			return (-1);
		}
	}

	/*
	 * Always install intr_mux as the hard handler so it can deal with
	 * individual enablement on handlers.
	 */
	if (head == NULL) {
		if (icu_setup(irq, intr_mux, &intreclist_head[irq], 0, 0) != 0)
			return (-1);
		update_intrname(irq, idesc->name);
	} else {
		if (bootverbose && head->next == NULL)
			printf("\tusing shared irq%d.\n", irq);
		update_intrname(irq, "mux");
	}

	/*
	 * Append to the end of the chain and update our SPL masks.
	 */
	while (*headp != NULL)
		headp = &(*headp)->next;
	*headp = idesc;

	update_masks(idesc->maskptr, irq);
	return (0);
}

/*
 * Create and activate an interrupt handler descriptor data structure.
 *
 * The dev_instance pointer is required for resource management, and will
 * only be passed through to resource_claim().
 *
 * There will be functions that derive a driver and unit name from a
 * dev_instance variable, and those functions will be used to maintain the
 * interrupt counter label array referenced by systat and vmstat to report
 * device interrupt rates (->update_intrlabels).
 *
 * Add the interrupt handler descriptor data structure created by an
 * earlier call of create_intr() to the linked list for its irq and
 * adjust the interrupt masks if necessary.
 *
 * WARNING: This is an internal function and not to be used by device
 * drivers.  It is subject to change without notice.
 */

intrec *
inthand_add(const char *name, int irq, inthand2_t handler, void *arg,
	     intrmask_t *maskptr, int flags)
{
	intrec *idesc;
	int errcode = -1;
	intrmask_t oldspl;

	if (ICU_LEN > 8 * sizeof *maskptr) {
		printf("create_intr: ICU_LEN of %d too high for %d bit intrmask\n",
		       ICU_LEN, 8 * sizeof *maskptr);
		return (NULL);
	}
	if ((unsigned)irq >= ICU_LEN) {
		printf("create_intr: requested irq%d too high, limit is %d\n",
		       irq, ICU_LEN -1);
		return (NULL);
	}

	idesc = malloc(sizeof *idesc, M_DEVBUF, M_WAITOK | M_ZERO);
	if (idesc == NULL)
		return NULL;

	if (name == NULL)
		name = "???";
	idesc->name     = malloc(strlen(name) + 1, M_DEVBUF, M_WAITOK);
	if (idesc->name == NULL) {
		free(idesc, M_DEVBUF);
		return NULL;
	}
	strcpy(idesc->name, name);

	idesc->handler  = handler;
	idesc->argument = arg;
	idesc->maskptr  = maskptr;
	idesc->intr     = irq;
	idesc->flags    = flags;
	idesc->enabled  = 1;

	/* block this irq */
	oldspl = splq(1 << irq);

	/* add irq to class selected by maskptr */
	errcode = add_intrdesc(idesc);
	splx(oldspl);

	if (errcode != 0) {
		if (bootverbose)
			printf("\tintr_connect(irq%d) failed, result=%d\n", 
			       irq, errcode);
		free(idesc->name, M_DEVBUF);
		free(idesc, M_DEVBUF);
		idesc = NULL;
	}

	return (idesc);
}

/*
 * Deactivate and remove the interrupt handler descriptor data connected
 * created by an earlier call of intr_connect() from the linked list and
 * adjust theinterrupt masks if necessary.
 *
 * Return the memory held by the interrupt handler descriptor data structure
 * to the system. Make sure, the handler is not actively used anymore, before.
 */
int
inthand_remove(intrec *idesc)
{
	intrec **hook, *head;
	intrmask_t oldspl;
	int irq;

	if (idesc == NULL)
		return (-1);

	irq = idesc->intr;
	oldspl = splq(1 << irq);

	/*
	 * Find and remove the interrupt descriptor.
	 */
	hook = &intreclist_head[irq];
	while (*hook != idesc) {
		if (*hook == NULL) {
			splx(oldspl);
			return(-1);
		}
		hook = &(*hook)->next;
	}
	*hook = idesc->next;

	/*
	 * If the list is now empty, revert the hard vector to the spurious
	 * interrupt.
	 */
	head = intreclist_head[irq];
	if (head == NULL) {
		/*
		 * No more interrupts on this irq
		 */
		icu_unset(irq, intr_mux);
		update_intrname(irq, NULL);
	} else if (head->next) {
		/*
		 * This irq is still shared (has at least two handlers)
		 * (the name should already be set to "mux").
		 */
	} else {
		/*
		 * This irq is no longer shared
		 */
		update_intrname(irq, head->name);
	}
	update_masks(idesc->maskptr, irq);
	splx(oldspl);
	free(idesc, M_DEVBUF);

	return (0);
}

/*
 * These functions are used in tandem with the device disabling its
 * interrupt in the device hardware to prevent the handler from being
 * run.  Otherwise it is possible for a device interrupt to occur,
 * schedule the handler, for the device to disable the hard interrupt,
 * and for the handler to then run because it has already been scheduled.
 */
void
inthand_disabled(intrec *idesc)
{
	idesc->enabled = 0;
}

void
inthand_enabled(intrec *idesc)
{
	idesc->enabled = 1;
}

/*
 * ithread_done()
 *
 *	This function is called by an interrupt thread when it has completed
 *	processing a loop.  We re-enable interrupts and interlock with
 *	ipending.
 *
 *	See kern/kern_intr.c for more information.
 */
void
ithread_done(int irq)
{
    struct mdglobaldata *gd = mdcpu;
    int mask = 1 << irq;
    thread_t td;

    td = gd->mi.gd_curthread;

    KKASSERT(td->td_pri >= TDPRI_CRIT);
    lwkt_deschedule_self(td);
    INTREN(mask);
    if (gd->gd_ipending & mask) {
	atomic_clear_int_nonlocked(&gd->gd_ipending, mask);
	INTRDIS(mask);
	lwkt_schedule_self(td);
    } else {
	lwkt_switch();
    }
}

#ifdef SMP
/*
 * forward_fast_remote()
 *
 *	This function is called from the receiving end of an IPIQ when a
 *	remote cpu wishes to forward a fast interrupt to us.  All we have to
 *	do is set the interrupt pending and let the IPI's doreti deal with it.
 */
void
forward_fastint_remote(void *arg)
{
    int irq = (int)arg;
    struct mdglobaldata *gd = mdcpu;

    atomic_set_int_nonlocked(&gd->gd_fpending, 1 << irq);
    atomic_set_int_nonlocked(&gd->mi.gd_reqflags, RQF_INTPEND);
}

#endif
