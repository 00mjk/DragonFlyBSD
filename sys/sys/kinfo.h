/*
 * Copyright (c) 2004 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Joerg Sonnenberger <joerg@bec.de>.
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
 * $DragonFly: src/sys/sys/kinfo.h,v 1.8 2006/12/29 18:02:56 victor Exp $
 */

#ifndef _SYS_KINFO_H_
#define _SYS_KINFO_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_PARAM_H_
#include <sys/param.h>
#endif

struct kinfo_file {
	size_t	 f_size;	/* size of struct kinfo_file */
	pid_t	 f_pid;		/* owning process */
	uid_t	 f_uid;		/* effective uid of owning process */
	int	 f_fd;		/* descriptor number */
	void	*f_file;	/* address of struct file */
	short	 f_type;	/* descriptor type */
	int	 f_count;	/* reference count */
	int	 f_msgcount;	/* references from message queue */
	off_t	 f_offset;	/* file offset */
	void	*f_data;	/* file descriptor specific data */
	u_int	 f_flag;	/* flags (see fcntl.h) */
};

/*
 * CPU time statistics
 */
struct kinfo_cputime {
	uint64_t	cp_user;
	uint64_t	cp_nice;
	uint64_t	cp_sys;
	uint64_t	cp_intr;
	uint64_t	cp_idle;
};

/*
 * CPU system/interrupt program counter sampler
 */
#define PCTRACK_ARYSIZE	32	/* must be a power of 2 */
#define PCTRACK_ARYMASK	(PCTRACK_ARYSIZE - 1)

struct kinfo_pcheader {
	int		pc_ntrack;	/* number of tracks per cpu (2) */
	int		pc_arysize;	/* size of storage array (32) */
};

struct kinfo_pctrack {
	int		pc_index;
	void		*pc_array[PCTRACK_ARYSIZE];
};

#define PCTRACK_SYS	0
#define PCTRACK_INT	1
#define PCTRACK_SIZE	2

struct kinfo_clockinfo {
	int	ci_hz;		/* clock frequency */
	int	ci_tick;	/* micro-seconds per hz tick */
	int	ci_tickadj;	/* clock skew rate for adjtime() */
	int	ci_stathz;	/* statistics clock frequency */
	int	ci_profhz;	/* profiling clock frequency */
};

#if defined(_KERNEL)
#ifdef SMP
#define cpu_time	cputime_percpu[mycpuid]
#else
#define cpu_time	cputime_percpu[0]
#endif
#endif

#if defined(_KERNEL)
extern struct kinfo_cputime cputime_percpu[MAXCPU];
#endif

#endif /* !_SYS_KINFO_H_ */
