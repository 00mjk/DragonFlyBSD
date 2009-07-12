/*
 * Copyright (c) 1996 John S. Dyson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice immediately at the beginning of the file, without modification,
 *    this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Absolutely no warranty of function or purpose is made by the author
 *    John S. Dyson.
 * 4. This work was done expressly for inclusion into FreeBSD.  Other use
 *    is allowed if this notation is included.
 * 5. Modifications may be freely made to this file if the above conditions
 *    are met.
 *
 * $FreeBSD: src/sys/sys/pipe.h,v 1.16 1999/12/29 04:24:45 peter Exp $
 * $DragonFly: src/sys/sys/pipe.h,v 1.10 2006/06/10 20:00:17 dillon Exp $
 */

#ifndef _SYS_PIPE_H_
#define _SYS_PIPE_H_

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_TIME_H_
#include <sys/time.h>			/* for struct timespec */
#endif
#ifndef _SYS_SELINFO_H_
#include <sys/selinfo.h>		/* for struct selinfo */
#endif
#ifndef _SYS_XIO_H_
#include <sys/xio.h>			/* for struct xio */
#endif
#ifndef _MACHINE_PARAM_H_
#include <machine/param.h>		/* for PAGE_SIZE */
#endif

/*
 * Pipe buffer size, keep moderate in value, pipes take kva space.
 * Must be a multiple of PAGE_SIZE.
 */
#ifndef PIPE_SIZE
#define PIPE_SIZE	16384
#endif

#ifndef BIG_PIPE_SIZE
#define BIG_PIPE_SIZE	(64*1024)
#endif

#if PIPE_SIZE < PAGE_SIZE
#error "PIPE_SIZE is too small for this architecture"
#endif

/*
 * Pipe buffer information.
 * Separate in, out, cnt are used to simplify calculations.
 * Buffered write is active when the buffer.cnt field is set.
 */
struct pipebuf {
	u_int	cnt;		/* number of chars currently in buffer */
	u_int	in;		/* in pointer */
	u_int	out;		/* out pointer */
	u_int	size;		/* size of buffer */
	caddr_t	buffer;		/* kva of buffer */
	struct  vm_object *object;	/* VM object containing buffer */
};

/*
 * Bits in pipe_state.
 */
#define PIPE_ASYNC	0x0004	/* Async? I/O */
#define PIPE_WANTR	0x0008	/* Reader wants some characters */
#define PIPE_WANTW	0x0010	/* Writer wants space to put characters */
#define PIPE_WANT	0x0020	/* Pipe is wanted to be run-down */
#define PIPE_SEL	0x0040	/* Pipe has a select active */
#define PIPE_EOF	0x0080	/* Pipe is in EOF condition */
#define PIPE_LOCK	0x0100	/* Process has exclusive access to pts/data */
#define PIPE_LWANT	0x0200	/* Process wants exclusive access to pts/data */

/*
 * Per-pipe data structure.
 * Two of these are linked together to produce bi-directional pipes.
 */
struct pipe {
	struct	pipebuf pipe_buffer;	/* data storage */
	vm_offset_t pipe_kva;		/* kva mapping (testing only) */
	struct	selinfo pipe_sel;	/* for compat with select */
	struct	timespec pipe_atime;	/* time of last access */
	struct	timespec pipe_mtime;	/* time of last modify */
	struct	timespec pipe_ctime;	/* time of status change */
	struct	sigio *pipe_sigio;	/* information for async I/O */
	struct	pipe *pipe_peer;	/* link with other direction */
	u_int	pipe_state;		/* pipe status info */
	int	pipe_busy;		/* busy flag, mostly to handle rundown sanely */
};

#endif	/* _KERNEL || _KERNEL_STRUCTURES */
#endif /* !_SYS_PIPE_H_ */
