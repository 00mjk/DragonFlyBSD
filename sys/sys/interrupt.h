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
 * $FreeBSD: src/sys/sys/interrupt.h,v 1.9.2.1 2001/10/14 20:05:50 luigi Exp $
 * $DragonFly: src/sys/sys/interrupt.h,v 1.16 2005/11/21 18:02:46 dillon Exp $
 */

#ifndef _SYS_INTERRUPT_H_
#define _SYS_INTERRUPT_H_

/*
 * System hard and soft interrupt limits.  Note that the architecture may
 * further limit available hardware and software interrupts.
 */
#define MAX_HARDINTS	64
#define MAX_SOFTINTS	64
#define FIRST_SOFTINT	MAX_HARDINTS
#define MAX_INTS	(MAX_HARDINTS + MAX_SOFTINTS)

typedef void inthand2_t (void *, void *);

#ifdef _KERNEL

struct intrframe;
struct thread;
struct lwkt_serialize;
void *register_swi(int intr, inthand2_t *handler, void *arg,
			    const char *name, 
			    struct lwkt_serialize *serializer);
void *register_int(int intr, inthand2_t *handler, void *arg,
			    const char *name, 
			    struct lwkt_serialize *serializer, int flags);
long get_interrupt_counter(int intr);
int count_registered_ints(int intr);
const char *get_registered_name(int intr);

void swi_setpriority(int intr, int pri);
void unregister_swi(void *id);
void unregister_int(void *id);
void register_randintr(int intr);
void unregister_randintr(int intr);
int next_registered_randintr(int intr);
void sched_ithd(int intr);	/* procedure called from MD */
void forward_fastint_remote(void *arg);		/* MD procedure (SMP) */

extern char	eintrnames[];	/* end of intrnames[] */
extern char	intrnames[];	/* string table containing device names */

#endif
#endif
