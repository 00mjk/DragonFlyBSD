/*
 * Copyright (c) 2006 The DragonFly Project.  All rights reserved.
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
 * $DragonFly: src/sys/platform/vkernel/i386/locore.s,v 1.4 2007/01/05 22:18:18 dillon Exp $
 */

#include <sys/syscall.h>
#include <machine/asmacros.h>
#include <machine/psl.h>
#include "assym.s"
	
	.globl	kernbase
	.set	kernbase,KERNBASE

	.data
	ALIGN_DATA		/* just to be sure */

	/*
	 * Normally the startup code would begin here, but this is a 
	 * virtual kernel so we just have a main() in platform/init.c
	 */
	
	.text

/*
 * Signal trampoline, copied to top of user stack
 */
NON_GPROF_ENTRY(sigcode)
	call	*SIGF_HANDLER(%esp)		/* call signal handler */
	lea	SIGF_UC(%esp),%eax		/* get ucontext_t */
	pushl	%eax
#if 0
	testl	$PSL_VM,UC_EFLAGS(%eax)
	jne	9f
#endif
	movl	UC_GS(%eax),%gs			/* restore %gs */
#if 0
9:
#endif
	movl	$SYS_sigreturn,%eax
	pushl	%eax				/* junk to fake return addr. */
	int	$0x80				/* enter kernel with args */
0:	jmp	0b

	ALIGN_TEXT
esigcode:

/* void reset_dbregs() */
ENTRY(reset_dbregs)
        movl    $0,%eax
        movl    %eax,%dr7     /* disable all breapoints first */
        movl    %eax,%dr0
        movl    %eax,%dr1
        movl    %eax,%dr2
        movl    %eax,%dr3
        movl    %eax,%dr6
        ret

	.data
	.globl	szsigcode
szsigcode:
	.long	esigcode - sigcode

