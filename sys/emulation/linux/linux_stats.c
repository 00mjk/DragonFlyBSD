/*-
 * Copyright (c) 1994-1995 S�ren Schmidt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer 
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software withough specific prior written permission
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
 * $FreeBSD: src/sys/compat/linux/linux_stats.c,v 1.22.2.3 2001/11/05 19:08:23 marcel Exp $
 * $DragonFly: src/sys/emulation/linux/linux_stats.c,v 1.23 2006/09/10 01:26:38 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/nlookup.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/device.h>
#include <sys/file2.h>
#include <sys/kern_syscall.h>

#include <arch_linux/linux.h>
#include <arch_linux/linux_proto.h>
#include "linux_util.h"

static int
newstat_copyout(struct stat *buf, void *ubuf)
{
	struct l_newstat tbuf;
	cdev_t dev;
	int error;

	tbuf.st_dev = uminor(buf->st_dev) | (umajor(buf->st_dev) << 8);
	tbuf.st_ino = buf->st_ino;
	tbuf.st_mode = buf->st_mode;
	tbuf.st_nlink = buf->st_nlink;
	tbuf.st_uid = buf->st_uid;
	tbuf.st_gid = buf->st_gid;
	tbuf.st_rdev = buf->st_rdev;
	tbuf.st_size = buf->st_size;
	tbuf.st_atime = buf->st_atime;
	tbuf.st_mtime = buf->st_mtime;
	tbuf.st_ctime = buf->st_ctime;
	tbuf.st_blksize = buf->st_blksize;
	tbuf.st_blocks = buf->st_blocks;

	/* Lie about disk drives which are character devices
	 * in FreeBSD but block devices under Linux.
	 */
	if (S_ISCHR(tbuf.st_mode) &&
	    (dev = udev2dev(buf->st_rdev, 0)) != NOCDEV) {
		if (dev_is_good(dev) && (dev_dflags(dev) & D_DISK)) {
			tbuf.st_mode &= ~S_IFMT;
			tbuf.st_mode |= S_IFBLK;

			/* XXX this may not be quite right */
			/* Map major number to 0 */
			tbuf.st_dev = uminor(buf->st_dev) & 0xf;
			tbuf.st_rdev = buf->st_rdev & 0xff;
		}
	}

	error = copyout(&tbuf, ubuf, sizeof(tbuf));
	return (error);
}

int
sys_linux_newstat(struct linux_newstat_args *args)
{
	struct stat buf;
	struct nlookupdata nd;
	char *path;
	int error;

	error = linux_copyin_path(args->path, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(newstat))
		printf(ARGS(newstat, "%s, *"), path);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0) {
		error = kern_stat(&nd, &buf);
		if (error == 0)
			error = newstat_copyout(&buf, args->buf);
		nlookup_done(&nd);
	}
	linux_free_path(&path);
	return (error);
}

int
sys_linux_newlstat(struct linux_newlstat_args *args)
{
	struct stat sb;
	struct nlookupdata nd;
	char *path;
	int error;

	error = linux_copyin_path(args->path, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(newlstat))
		printf(ARGS(newlstat, "%s, *"), path);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, 0);
	if (error == 0) {
		error = kern_stat(&nd, &sb);
		if (error == 0)
			error = newstat_copyout(&sb, args->buf);
		nlookup_done(&nd);
	}
	linux_free_path(&path);
	return (error);
}

int
sys_linux_newfstat(struct linux_newfstat_args *args)
{
	struct stat buf;
	int error;

#ifdef DEBUG
	if (ldebug(newfstat))
		printf(ARGS(newfstat, "%d, *"), args->fd);
#endif
	error = kern_fstat(args->fd, &buf);

	if (error == 0)
		error = newstat_copyout(&buf, args->buf);
	return (error);
}

/* XXX - All fields of type l_int are defined as l_long on i386 */
struct l_statfs {
	l_int		f_type;
	l_int		f_bsize;
	l_int		f_blocks;
	l_int		f_bfree;
	l_int		f_bavail;
	l_int		f_files;
	l_int		f_ffree;
	l_fsid_t	f_fsid;
	l_int		f_namelen;
	l_int		f_spare[6];
};

#define	LINUX_CODA_SUPER_MAGIC	0x73757245L
#define	LINUX_EXT2_SUPER_MAGIC	0xEF53L
#define	LINUX_HPFS_SUPER_MAGIC	0xf995e849L
#define	LINUX_ISOFS_SUPER_MAGIC	0x9660L
#define	LINUX_MSDOS_SUPER_MAGIC	0x4d44L
#define	LINUX_NCP_SUPER_MAGIC	0x564cL
#define	LINUX_NFS_SUPER_MAGIC	0x6969L
#define	LINUX_NTFS_SUPER_MAGIC	0x5346544EL
#define	LINUX_PROC_SUPER_MAGIC	0x9fa0L
#define	LINUX_UFS_SUPER_MAGIC	0x00011954L	/* XXX - UFS_MAGIC in Linux */

static long
bsd_to_linux_ftype(const char *fstypename)
{
	int i;
	static struct {const char *bsd_name; long linux_type;} b2l_tbl[] = {
		{"ufs",     LINUX_UFS_SUPER_MAGIC},
		{"cd9660",  LINUX_ISOFS_SUPER_MAGIC},
		{"nfs",     LINUX_NFS_SUPER_MAGIC},
		{"ext2fs",  LINUX_EXT2_SUPER_MAGIC},
		{"procfs",  LINUX_PROC_SUPER_MAGIC},
		{"msdosfs", LINUX_MSDOS_SUPER_MAGIC},
		{"ntfs",    LINUX_NTFS_SUPER_MAGIC},
		{"nwfs",    LINUX_NCP_SUPER_MAGIC},
		{"hpfs",    LINUX_HPFS_SUPER_MAGIC},
		{NULL,      0L}};

	for (i = 0; b2l_tbl[i].bsd_name != NULL; i++)
		if (strcmp(b2l_tbl[i].bsd_name, fstypename) == 0)
			return (b2l_tbl[i].linux_type);

	return (0L);
}

static int
statfs_copyout(struct statfs *statfs, struct l_statfs_buf *buf, l_int namelen)
{
	struct l_statfs linux_statfs;
	int error;

	linux_statfs.f_type = bsd_to_linux_ftype(statfs->f_fstypename);
	linux_statfs.f_bsize = statfs->f_bsize;
	linux_statfs.f_blocks = statfs->f_blocks;
	linux_statfs.f_bfree = statfs->f_bfree;
	linux_statfs.f_bavail = statfs->f_bavail;
  	linux_statfs.f_ffree = statfs->f_ffree;
	linux_statfs.f_files = statfs->f_files;
	linux_statfs.f_fsid.val[0] = statfs->f_fsid.val[0];
	linux_statfs.f_fsid.val[1] = statfs->f_fsid.val[1];
	linux_statfs.f_namelen = namelen;

	error = copyout(&linux_statfs, buf, sizeof(linux_statfs));
	return (error);
}

int
sys_linux_statfs(struct linux_statfs_args *args)
{
	struct statfs statfs;
	struct nlookupdata nd;
	char *path;
	int error, namelen;

	error = linux_copyin_path(args->path, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(statfs))
		printf(ARGS(statfs, "%s, *"), path);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0)
		error = kern_statfs(&nd, &statfs);
	if (error == 0) {
		if (nd.nl_ncp->nc_vp != NULL)
			error = vn_get_namelen(nd.nl_ncp->nc_vp, &namelen);
		else
			error = EINVAL;
	}
	nlookup_done(&nd);
	if (error == 0)
		error = statfs_copyout(&statfs, args->buf, (l_int)namelen);
	linux_free_path(&path);
	return (error);
}

int
sys_linux_fstatfs(struct linux_fstatfs_args *args)
{
	struct proc *p = curthread->td_proc;
	struct file *fp;
	struct statfs statfs;
	int error, namelen;

#ifdef DEBUG
	if (ldebug(fstatfs))
		printf(ARGS(fstatfs, "%d, *"), args->fd);
#endif
	if ((error = kern_fstatfs(args->fd, &statfs)) != 0)
		return (error);
	if ((error = holdvnode(p->p_fd, args->fd, &fp)) != 0)
		return (error);
	error = vn_get_namelen((struct vnode *)fp->f_data, &namelen);
	fdrop(fp);
	if (error == 0)
		error = statfs_copyout(&statfs, args->buf, (l_int)namelen);
	return (error);
}

struct l_ustat 
{
	l_daddr_t	f_tfree;
	l_ino_t		f_tinode;
	char		f_fname[6];
	char		f_fpack[6];
};

int
sys_linux_ustat(struct linux_ustat_args *args)
{
	struct l_ustat lu;
	cdev_t dev;
	struct vnode *vp;
	struct statfs *stat;
	int error;

#ifdef DEBUG
	if (ldebug(ustat))
		printf(ARGS(ustat, "%d, *"), args->dev);
#endif

	/*
	 * lu.f_fname and lu.f_fpack are not used. They are always zeroed.
	 * lu.f_tinode and lu.f_tfree are set from the device's super block.
	 */
	bzero(&lu, sizeof(lu));

	/*
	 * XXX - Don't return an error if we can't find a vnode for the
	 * device. Our cdev_t is 32-bits whereas Linux only has a 16-bits
	 * cdev_t. The dev_t that is used now may as well be a truncated
	 * cdev_t returned from previous syscalls. Just return a bzeroed
	 * ustat in that case.
	 */
	dev = udev2dev(makeudev(args->dev >> 8, args->dev & 0xFF), 0);
	if (dev != NOCDEV && vfinddev(dev, VCHR, &vp)) {
		if (vp->v_mount == NULL) {
			return (EINVAL);
		}
		stat = &(vp->v_mount->mnt_stat);
		error = VFS_STATFS(vp->v_mount, stat, curproc->p_ucred);
		if (error) {
			return (error);
		}
		lu.f_tfree = stat->f_bfree;
		lu.f_tinode = stat->f_ffree;
	}
	return (copyout(&lu, args->ubuf, sizeof(lu)));
}

#if defined(__i386__)

static int
stat64_copyout(struct stat *buf, void *ubuf)
{
	struct l_stat64 lbuf;
	int error;

	bzero(&lbuf, sizeof(lbuf));
	lbuf.st_dev = uminor(buf->st_dev) | (umajor(buf->st_dev) << 8);
	lbuf.st_ino = buf->st_ino;
	lbuf.st_mode = buf->st_mode;
	lbuf.st_nlink = buf->st_nlink;
	lbuf.st_uid = buf->st_uid;
	lbuf.st_gid = buf->st_gid;
	lbuf.st_rdev = buf->st_rdev;
	lbuf.st_size = buf->st_size;
	lbuf.st_atime = buf->st_atime;
	lbuf.st_mtime = buf->st_mtime;
	lbuf.st_ctime = buf->st_ctime;
	lbuf.st_blksize = buf->st_blksize;
	lbuf.st_blocks = buf->st_blocks;

	/*
	 * The __st_ino field makes all the difference. In the Linux kernel
	 * it is conditionally compiled based on STAT64_HAS_BROKEN_ST_INO,
	 * but without the assignment to __st_ino the runtime linker refuses
	 * to mmap(2) any shared libraries. I guess it's broken alright :-)
	 */
	lbuf.__st_ino = buf->st_ino;

	error = copyout(&lbuf, ubuf, sizeof(lbuf));
	return (error);
}

int
sys_linux_stat64(struct linux_stat64_args *args)
{
	struct nlookupdata nd;
	struct stat buf;
	char *path;
	int error;

	error = linux_copyin_path(args->filename, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(stat64))
		printf(ARGS(stat64, "%s, *"), path);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0) {
		error = kern_stat(&nd, &buf);
		if (error == 0)
			error = stat64_copyout(&buf, args->statbuf);
		nlookup_done(&nd);
	}
	linux_free_path(&path);
	return (error);
}

int
sys_linux_lstat64(struct linux_lstat64_args *args)
{
	struct nlookupdata nd;
	struct stat sb;
	char *path;
	int error;

	error = linux_copyin_path(args->filename, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(lstat64))
		printf(ARGS(lstat64, "%s, *"), path);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, 0);
	if (error == 0) {
		error = kern_stat(&nd, &sb);
		if (error == 0)
			error = stat64_copyout(&sb, args->statbuf);
		nlookup_done(&nd);
	}
	linux_free_path(&path);
	return (error);
}

int
sys_linux_fstat64(struct linux_fstat64_args *args)
{
	struct stat buf;
	int error;

#ifdef DEBUG
	if (ldebug(fstat64))
		printf(ARGS(fstat64, "%d, *"), args->fd);
#endif
	error = kern_fstat(args->fd, &buf);

	if (error == 0)
		error = stat64_copyout(&buf, args->statbuf);
	return (error);
}

#endif /* __i386__ */
