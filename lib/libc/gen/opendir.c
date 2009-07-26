/*
 * Copyright (c) 1983, 1993
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
 * $FreeBSD: src/lib/libc/gen/opendir.c,v 1.10.2.1 2001/06/04 20:59:48 joerg Exp $
 * $DragonFly: src/lib/libc/gen/opendir.c,v 1.6 2005/11/13 00:07:42 swildner Exp $
 *
 * @(#)opendir.c	8.8 (Berkeley) 5/1/95
 */

#include "namespace.h"
#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "un-namespace.h"


#define DEFAULT_FLAGS (DTF_HIDEW | DTF_NODUP)

/*
 * Open a directory given its path.
 */
DIR *
opendir(const char *name)
{
	return (__opendir2(name, DEFAULT_FLAGS));
}

/*
 * Open a directory given a descriptor representing it.
 */
DIR *
fdopendir(int fd)
{
	return (__fdopendir2(fd, DEFAULT_FLAGS));
}

DIR *
__opendir2(const char *name, int flags)
{
	int fd;
	struct stat statb;
	DIR *dirp;
	int saved_errno;

	/*
	 * stat() before _open() because opening of special files may be
	 * harmful.
	 */
	if (stat(name, &statb) != 0)
		return (NULL);
	if (!S_ISDIR(statb.st_mode)) {
		errno = ENOTDIR;
		return (NULL);
	}

	if ((fd = _open(name, O_RDONLY | O_NONBLOCK)) == -1)
		return (NULL);
	dirp = __fdopendir2(fd, flags);
	if (dirp == NULL) {
		saved_errno = errno;
		_close(fd);
		errno = saved_errno;
	}

	return (dirp);
}

DIR *
__fdopendir2(int fd, int flags)
{
	DIR *dirp;
	int incr;
	int saved_errno;
	struct stat statb;
	off_t off;

	dirp = NULL;

	if (_fstat(fd, &statb) != 0)
		goto fail;
	if (!S_ISDIR(statb.st_mode)) {
		errno = ENOTDIR;
		goto fail;
	}
	if (_fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 ||
	    (dirp = malloc(sizeof(DIR))) == NULL)
		goto fail;

	/*
	 * XXX We don't support yet the POSIX requirement that states that the
	 * file offset associated with the fd passed to fdopendir() determines
	 * which directory entry is returned.
	 */
	if ((off = lseek(fd, 0, SEEK_CUR)) != 0) {
		errno = (off != -1) ? ENOTSUP : errno;
		goto fail;
	}

	/*
	 * Use the system page size if that is a multiple of DIRBLKSIZ.
	 * Hopefully this can be a big win someday by allowing page
	 * trades to user space to be done by _getdirentries().
	 */
	incr = getpagesize();
	if ((incr % DIRBLKSIZ) != 0) 
		incr = DIRBLKSIZ;

	dirp->dd_len = incr;
	dirp->dd_buf = malloc(dirp->dd_len);
	if (dirp->dd_buf == NULL)
		goto fail;
	dirp->dd_seek = 0;
	flags &= ~DTF_REWIND;

	dirp->dd_loc = 0;
	dirp->dd_fd = fd;
	dirp->dd_flags = flags;
	dirp->dd_lock = NULL;

	/*
	 * Set up seek point for rewinddir.
	 */
	dirp->dd_rewind = telldir(dirp);

	return (dirp);

fail:
	saved_errno = errno;
	free(dirp);
	errno = saved_errno;
	return (NULL);
}
