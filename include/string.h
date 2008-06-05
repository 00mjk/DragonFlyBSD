/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)string.h	8.1 (Berkeley) 6/2/93
 * $FreeBSD: src/include/string.h,v 1.6.2.3 2001/12/25 00:36:57 ache Exp $
 * $DragonFly: src/include/string.h,v 1.9 2008/06/05 17:53:10 swildner Exp $
 */

#ifndef _STRING_H_
#define	_STRING_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif

#ifndef _MACHINE_STDINT_H_
#include <machine/stdint.h>
#endif

#ifndef _SIZE_T_DECLARED
#define _SIZE_T_DECLARED
typedef __size_t        size_t;		/* open group */
#endif

#include <sys/_null.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
void	*memchr (const void *, int, size_t);
void	*memmove (void *, const void *, size_t);
char	*strchr (const char *, int);
int	 strcoll (const char *, const char *);
size_t strcspn (const char *, const char *);
char	*strerror (int);
char	*strpbrk (const char *, const char *);
char	*strrchr (const char *, int);
size_t strspn (const char *, const char *);
char	*strstr (const char *, const char *);
char	*strtok (char *, const char *);
size_t strxfrm (char *, const char *, size_t);

#if !defined(_KERNEL_VIRTUAL)
int	memcmp (const void *, const void *, size_t);
void	*memcpy (void *, const void *, size_t);
void	*memset (void *, int, size_t);
char	*strcat (char *, const char *);
int	strcmp (const char *, const char *);
char	*strcpy (char *, const char *);
size_t	strlen (const char *);
char	*strncat (char *, const char *, size_t);
int	strncmp (const char *, const char *, size_t);
char	*strncpy (char *, const char *, size_t);
#endif

/* Nonstandard routines */
#if !defined(_ANSI_SOURCE) && !defined(_POSIX_SOURCE)
void	*memccpy (void *, const void *, int, size_t);
int	 strcasecmp (const char *, const char *);
char	*strcasestr (const char *, const char *);
char	*strdup (const char *);
int	 strerror_r (int, char *, size_t);
void	 strmode (mode_t, char *);
int	 strncasecmp (const char *, const char *, size_t);
char	*strnstr (const char *, const char *, size_t);
char	*strsep (char **, const char *);
char	*strsignal (int);
char	*strtok_r (char *, const char *, char **);
void	 swab (const void *, void *, size_t);

#if !defined(_KERNEL_VIRTUAL)
void	bcopy (const void *, void *, size_t);
void	bzero (void *, size_t);
int	ffs (int);
int	bcmp (const void *, const void *, size_t);
char	*index (const char *, int);
char	*rindex (const char *, int);
size_t	strlcat (char *, const char *, size_t);
size_t	strlcpy (char *, const char *, size_t);
#endif

#endif

__END_DECLS

#endif /* _STRING_H_ */
