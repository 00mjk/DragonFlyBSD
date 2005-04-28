/*	$OpenBSD: dirname.c,v 1.4 1999/05/30 17:10:30 espie Exp $	*/
/*	$FreeBSD: src/lib/libc/gen/dirname.c,v 1.1.2.2 2001/07/23 10:13:04 dd Exp $	*/
/*	$DragonFly: src/lib/libc/gen/dirname.c,v 1.6 2005/04/28 13:45:42 joerg Exp $	*/

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
 * $OpenBSD: dirname.c,v 1.4 1999/05/30 17:10:30 espie Exp $
 */

#include <errno.h>
#include <libgen.h>
#include <string.h>
#include <sys/param.h>

char *
dirname(const char *path)
{
	static char bname[MAXPATHLEN];
	const char *endp;

	/* Empty or NULL string gets treated as "." */
	if (path == NULL || *path == '\0') {
		strlcpy(bname, ".", sizeof(bname));
		return(bname);
	}

	/* Strip trailing slashes */
	endp = path + strlen(path) - 1;
	while (endp > path && *endp == '/')
		endp--;

	/* Find the start of the dir */
	while (endp > path && *endp != '/')
		endp--;

	/* Either the dir is "/" or there are no slashes */
	if (endp == path) {
		strlcpy(bname, *endp == '/' ? "/" : ".", sizeof(bname));
		return(bname);
	}

	do {
		endp--;
	} while (endp > path && *endp == '/');

	if (endp + 2 > path + sizeof(bname)) {
		errno = ENAMETOOLONG;
		return(NULL);
	}
	memcpy(bname, path, endp - path + 1);
	bname[endp - path + 1] = '\0';
	return(bname);
}
