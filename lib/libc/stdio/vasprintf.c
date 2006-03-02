/*	$OpenBSD: vasprintf.c,v 1.4 1998/06/21 22:13:47 millert Exp $	*/

/*
 * Copyright (c) 1997 Todd C. Miller <Todd.Miller@courtesan.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/lib/libc/stdio/vasprintf.c,v 1.11 1999/08/28 00:01:19 peter Exp $
 * $DragonFly: src/lib/libc/stdio/vasprintf.c,v 1.8 2006/03/02 18:05:30 joerg Exp $
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>

#include "local.h"
#include "priv_stdio.h"

int
vasprintf(char **str, const char *fmt, va_list ap)
{
	int ret;
	FILE f;

	f.pub._fileno = -1;
	f.pub._flags = __SWR | __SSTR | __SALC;
	f._bf._base = f.pub._p = (unsigned char *)malloc(128);
	if (f._bf._base == NULL) {
		*str = NULL;
		errno = ENOMEM;
		return (-1);
	}
	f._bf._size = f.pub._w = 127;		/* Leave room for the NULL */
	f._up = NULL;
	f.fl_mutex = PTHREAD_MUTEX_INITIALIZER;
	f.fl_owner = NULL;
	f.fl_count = 0;
	memset(WCIO_GET(&f), 0, sizeof(struct wchar_io_data));
	ret = __vfprintf(&f, fmt, ap);
	*f.pub._p = '\0';
	f._bf._base = reallocf(f._bf._base, f._bf._size + 1);
	if (f._bf._base == NULL) {
		errno = ENOMEM;
		ret = -1;
	}
	*str = (char *)f._bf._base;
	return (ret);
}
