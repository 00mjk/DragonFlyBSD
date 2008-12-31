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
 * $FreeBSD: src/sys/compat/linux/linux_misc.c,v 1.85.2.9 2002/09/24 08:11:41 mdodd Exp $
 * $DragonFly: src/sys/emulation/linux/linux_misc.c,v 1.39 2007/06/26 19:31:03 dillon Exp $
 */

#include "opt_compat.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/imgact_aout.h>
#include <sys/kernel.h>
#include <sys/kern_syscall.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/nlookup.h>
#include <sys/blist.h>
#include <sys/reboot.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/signal2.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <sys/vmmeter.h>
#include <sys/vnode.h>
#include <sys/wait.h>
#include <sys/thread2.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_zone.h>
#include <vm/swap_pager.h>

#include <machine/frame.h>
#include <machine/limits.h>
#include <machine/psl.h>
#include <machine/sysarch.h>
#ifdef __i386__
#include <machine/segments.h>
#endif

#include <sys/sched.h>

#include <emulation/linux/linux_sysproto.h>
#include <arch_linux/linux.h>
#include <arch_linux/linux_proto.h>
#include "linux_mib.h"
#include "linux_util.h"

#define BSD_TO_LINUX_SIGNAL(sig)	\
	(((sig) <= LINUX_SIGTBLSZ) ? bsd_to_linux_signal[_SIG_IDX(sig)] : sig)

static unsigned int linux_to_bsd_resource[LINUX_RLIM_NLIMITS] = {
	RLIMIT_CPU, RLIMIT_FSIZE, RLIMIT_DATA, RLIMIT_STACK,
	RLIMIT_CORE, RLIMIT_RSS, RLIMIT_NPROC, RLIMIT_NOFILE,
	RLIMIT_MEMLOCK, -1
};

struct l_sysinfo {
	l_long		uptime;		/* Seconds since boot */
	l_ulong		loads[3];	/* 1, 5, and 15 minute load averages */
	l_ulong		totalram;	/* Total usable main memory size */
	l_ulong		freeram;	/* Available memory size */
	l_ulong		sharedram;	/* Amount of shared memory */
	l_ulong		bufferram;	/* Memory used by buffers */
	l_ulong		totalswap;	/* Total swap space size */
	l_ulong		freeswap;	/* swap space still available */
	l_ushort	procs;		/* Number of current processes */
	char		_f[22];		/* Pads structure to 64 bytes */
};

int
sys_linux_sysinfo(struct linux_sysinfo_args *args)
{
	struct l_sysinfo sysinfo;
	vm_object_t object;
	int i;
	struct timespec ts;

	/* Uptime is copied out of print_uptime() in kern_shutdown.c */
	getnanouptime(&ts);
	i = 0;
	if (ts.tv_sec >= 86400) {
		ts.tv_sec %= 86400;
		i = 1;
	}
	if (i || ts.tv_sec >= 3600) {
		ts.tv_sec %= 3600;
		i = 1;
	}
	if (i || ts.tv_sec >= 60) {
		ts.tv_sec %= 60;
		i = 1;
	}
	sysinfo.uptime=ts.tv_sec;

	/* Use the information from the mib to get our load averages */
	for (i = 0; i < 3; i++)
		sysinfo.loads[i] = averunnable.ldavg[i];

	sysinfo.totalram = Maxmem * PAGE_SIZE;
	sysinfo.freeram = sysinfo.totalram - vmstats.v_wire_count * PAGE_SIZE;

	sysinfo.sharedram = 0;
	for (object = TAILQ_FIRST(&vm_object_list); object != NULL;
	     object = TAILQ_NEXT(object, object_list))
		if (object->shadow_count > 1)
			sysinfo.sharedram += object->resident_page_count;

	sysinfo.sharedram *= PAGE_SIZE;
	sysinfo.bufferram = 0;

	if (swapblist == NULL) {
		sysinfo.totalswap= 0;
		sysinfo.freeswap = 0;
	} else {
		sysinfo.totalswap = swapblist->bl_blocks * 1024;
		sysinfo.freeswap = swapblist->bl_root->u.bmu_avail * PAGE_SIZE;
	}

	sysinfo.procs = 20; /* Hack */

	return copyout(&sysinfo, (caddr_t)args->info, sizeof(sysinfo));
}

int
sys_linux_alarm(struct linux_alarm_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct itimerval it, old_it;
	struct timeval tv;

	KKASSERT(p);

#ifdef DEBUG
	if (ldebug(alarm))
		kprintf(ARGS(alarm, "%u"), args->secs);
#endif

	if (args->secs > 100000000)
		return EINVAL;

	it.it_value.tv_sec = (long)args->secs;
	it.it_value.tv_usec = 0;
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 0;
	crit_enter();
	old_it = p->p_realtimer;
	getmicrouptime(&tv);
	if (timevalisset(&old_it.it_value))
		callout_stop(&p->p_ithandle);
	if (it.it_value.tv_sec != 0) {
		callout_reset(&p->p_ithandle, tvtohz_high(&it.it_value),
			     realitexpire, p);
		timevaladd(&it.it_value, &tv);
	}
	p->p_realtimer = it;
	crit_exit();
	if (timevalcmp(&old_it.it_value, &tv, >)) {
		timevalsub(&old_it.it_value, &tv);
		if (old_it.it_value.tv_usec != 0)
			old_it.it_value.tv_sec++;
		args->sysmsg_result = old_it.it_value.tv_sec;
	}
	return 0;
}

int
sys_linux_brk(struct linux_brk_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct vmspace *vm;
	vm_offset_t new, old;
	struct obreak_args bsd_args;

	KKASSERT(p);
	vm = p->p_vmspace;
#ifdef DEBUG
	if (ldebug(brk))
		kprintf(ARGS(brk, "%p"), (void *)args->dsend);
#endif
	old = (vm_offset_t)vm->vm_daddr + ctob(vm->vm_dsize);
	new = (vm_offset_t)args->dsend;
	bsd_args.sysmsg_result = 0;
	bsd_args.nsize = (char *) new;
	bsd_args.sysmsg_result = 0;
	if (((caddr_t)new > vm->vm_daddr) && !sys_obreak(&bsd_args))
		args->sysmsg_result = (long)new;
	else
		args->sysmsg_result = (long)old;

	return 0;
}

int
sys_linux_uselib(struct linux_uselib_args *args)
{
	struct thread *td = curthread;
	struct proc *p;
	struct nlookupdata nd;
	struct vnode *vp;
	struct exec *a_out;
	struct vattr attr;
	vm_offset_t vmaddr;
	unsigned long file_offset;
	vm_offset_t buffer;
	unsigned long bss_size;
	int error;
	int locked;
	char *path;

	KKASSERT(td->td_proc);
	p = td->td_proc;

	error = linux_copyin_path(args->library, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(uselib))
		kprintf(ARGS(uselib, "%s"), path);
#endif

	a_out = NULL;
	locked = 0;
	vp = NULL;

	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0)
		error = nlookup(&nd);
	if (error == 0)
		error = cache_vget(&nd.nl_nch, nd.nl_cred, LK_EXCLUSIVE, &vp);
	if (error)
		goto cleanup;
	/*
	 * From here on down, we have a locked vnode that must be unlocked.
	 */
	locked = 1;

	/* Writable? */
	if (vp->v_writecount) {
		error = ETXTBSY;
		goto cleanup;
	}

	/* Executable? */
	error = VOP_GETATTR(vp, &attr);
	if (error)
		goto cleanup;

	if ((vp->v_mount->mnt_flag & MNT_NOEXEC) ||
	    ((attr.va_mode & 0111) == 0) || (attr.va_type != VREG)) {
		error = ENOEXEC;
		goto cleanup;
	}

	/* Sensible size? */
	if (attr.va_size == 0) {
		error = ENOEXEC;
		goto cleanup;
	}

	/* Can we access it? */
	error = VOP_ACCESS(vp, VEXEC, p->p_ucred);
	if (error)
		goto cleanup;

	error = VOP_OPEN(vp, FREAD, p->p_ucred, NULL);
	if (error)
		goto cleanup;

	/*
	 * Lock no longer needed
	 */
	vn_unlock(vp);
	locked = 0;

	/* Pull in executable header into kernel_map */
	error = vm_mmap(&kernel_map, (vm_offset_t *)&a_out, PAGE_SIZE,
	    VM_PROT_READ, VM_PROT_READ, 0, (caddr_t)vp, 0);
	if (error)
		goto cleanup;

	/* Is it a Linux binary ? */
	if (((a_out->a_magic >> 16) & 0xff) != 0x64) {
		error = ENOEXEC;
		goto cleanup;
	}

	/*
	 * While we are here, we should REALLY do some more checks
	 */

	/* Set file/virtual offset based on a.out variant. */
	switch ((int)(a_out->a_magic & 0xffff)) {
	case 0413:	/* ZMAGIC */
		file_offset = 1024;
		break;
	case 0314:	/* QMAGIC */
		file_offset = 0;
		break;
	default:
		error = ENOEXEC;
		goto cleanup;
	}

	bss_size = round_page(a_out->a_bss);

	/* Check various fields in header for validity/bounds. */
	if (a_out->a_text & PAGE_MASK || a_out->a_data & PAGE_MASK) {
		error = ENOEXEC;
		goto cleanup;
	}

	/* text + data can't exceed file size */
	if (a_out->a_data + a_out->a_text > attr.va_size) {
		error = EFAULT;
		goto cleanup;
	}

	/*
	 * text/data/bss must not exceed limits
	 * XXX - this is not complete. it should check current usage PLUS
	 * the resources needed by this library.
	 */
	if (a_out->a_text > maxtsiz ||
	    a_out->a_data + bss_size > p->p_rlimit[RLIMIT_DATA].rlim_cur) {
		error = ENOMEM;
		goto cleanup;
	}

	/* prevent more writers */
	vp->v_flag |= VTEXT;

	/*
	 * Check if file_offset page aligned. Currently we cannot handle
	 * misalinged file offsets, and so we read in the entire image
	 * (what a waste).
	 */
	if (file_offset & PAGE_MASK) {
#ifdef DEBUG
		kprintf("uselib: Non page aligned binary %lu\n", file_offset);
#endif
		/* Map text+data read/write/execute */

		/* a_entry is the load address and is page aligned */
		vmaddr = trunc_page(a_out->a_entry);

		/* get anon user mapping, read+write+execute */
		error = vm_map_find(&p->p_vmspace->vm_map, NULL, 0,
				    &vmaddr, a_out->a_text + a_out->a_data,
				    FALSE,
				    VM_MAPTYPE_NORMAL,
				    VM_PROT_ALL, VM_PROT_ALL,
				    0);
		if (error)
			goto cleanup;

		/* map file into kernel_map */
		error = vm_mmap(&kernel_map, &buffer,
		    round_page(a_out->a_text + a_out->a_data + file_offset),
		    VM_PROT_READ, VM_PROT_READ, 0, (caddr_t)vp,
		    trunc_page(file_offset));
		if (error)
			goto cleanup;

		/* copy from kernel VM space to user space */
		error = copyout((caddr_t)(uintptr_t)(buffer + file_offset),
		    (caddr_t)vmaddr, a_out->a_text + a_out->a_data);

		/* release temporary kernel space */
		vm_map_remove(&kernel_map, buffer, buffer +
		    round_page(a_out->a_text + a_out->a_data + file_offset));

		if (error)
			goto cleanup;
	} else {
#ifdef DEBUG
		kprintf("uselib: Page aligned binary %lu\n", file_offset);
#endif
		/*
		 * for QMAGIC, a_entry is 20 bytes beyond the load address
		 * to skip the executable header
		 */
		vmaddr = trunc_page(a_out->a_entry);

		/*
		 * Map it all into the process's space as a single
		 * copy-on-write "data" segment.
		 */
		error = vm_mmap(&p->p_vmspace->vm_map, &vmaddr,
		    a_out->a_text + a_out->a_data, VM_PROT_ALL, VM_PROT_ALL,
		    MAP_PRIVATE | MAP_FIXED, (caddr_t)vp, file_offset);
		if (error)
			goto cleanup;
	}
#ifdef DEBUG
	kprintf("mem=%08lx = %08lx %08lx\n", (long)vmaddr, ((long*)vmaddr)[0],
	    ((long*)vmaddr)[1]);
#endif
	if (bss_size != 0) {
		/* Calculate BSS start address */
		vmaddr = trunc_page(a_out->a_entry) + a_out->a_text +
		    a_out->a_data;

		/* allocate some 'anon' space */
		error = vm_map_find(&p->p_vmspace->vm_map, NULL, 0,
				    &vmaddr, bss_size,
				    FALSE,
				    VM_MAPTYPE_NORMAL,
				    VM_PROT_ALL, VM_PROT_ALL,
				    0);
		if (error)
			goto cleanup;
	}

cleanup:
	/* Unlock/release vnode */
	if (vp) {
		if (locked)
			vn_unlock(vp);
		vrele(vp);
	}
	/* Release the kernel mapping. */
	if (a_out) {
		vm_map_remove(&kernel_map, (vm_offset_t)a_out,
		    (vm_offset_t)a_out + PAGE_SIZE);
	}
	nlookup_done(&nd);
	linux_free_path(&path);
	return (error);
}

int
sys_linux_select(struct linux_select_args *args)
{
	struct select_args bsa;
	struct timeval tv0, tv1, utv, *tvp;
	caddr_t sg;
	int error;

#ifdef DEBUG
	if (ldebug(select))
		kprintf(ARGS(select, "%d, %p, %p, %p, %p"), args->nfds,
		    (void *)args->readfds, (void *)args->writefds,
		    (void *)args->exceptfds, (void *)args->timeout);
#endif

	error = 0;
	bsa.sysmsg_result = 0;
	bsa.nd = args->nfds;
	bsa.in = args->readfds;
	bsa.ou = args->writefds;
	bsa.ex = args->exceptfds;
	bsa.tv = (struct timeval *)args->timeout;

	/*
	 * Store current time for computation of the amount of
	 * time left.
	 */
	if (args->timeout) {
		if ((error = copyin((caddr_t)args->timeout, &utv,
		    sizeof(utv))))
			goto select_out;
#ifdef DEBUG
		if (ldebug(select))
			kprintf(LMSG("incoming timeout (%ld/%ld)"),
			    utv.tv_sec, utv.tv_usec);
#endif

		if (itimerfix(&utv)) {
			/*
			 * The timeval was invalid.  Convert it to something
			 * valid that will act as it does under Linux.
			 */
			sg = stackgap_init();
			tvp = stackgap_alloc(&sg, sizeof(utv));
			utv.tv_sec += utv.tv_usec / 1000000;
			utv.tv_usec %= 1000000;
			if (utv.tv_usec < 0) {
				utv.tv_sec -= 1;
				utv.tv_usec += 1000000;
			}
			if (utv.tv_sec < 0)
				timevalclear(&utv);
			if ((error = copyout(&utv, tvp, sizeof(utv))))
				goto select_out;
			bsa.tv = tvp;
		}
		microtime(&tv0);
	}

	error = sys_select(&bsa);
	args->sysmsg_result = bsa.sysmsg_result;
#ifdef DEBUG
	if (ldebug(select))
		kprintf(LMSG("real select returns %d"), error);
#endif
	if (error) {
		/*
		 * See fs/select.c in the Linux kernel.  Without this,
		 * Maelstrom doesn't work.
		 */
		if (error == ERESTART)
			error = EINTR;
		goto select_out;
	}

	if (args->timeout) {
		if (args->sysmsg_result) {
			/*
			 * Compute how much time was left of the timeout,
			 * by subtracting the current time and the time
			 * before we started the call, and subtracting
			 * that result from the user-supplied value.
			 */
			microtime(&tv1);
			timevalsub(&tv1, &tv0);
			timevalsub(&utv, &tv1);
			if (utv.tv_sec < 0)
				timevalclear(&utv);
		} else
			timevalclear(&utv);
#ifdef DEBUG
		if (ldebug(select))
			kprintf(LMSG("outgoing timeout (%ld/%ld)"),
			    utv.tv_sec, utv.tv_usec);
#endif
		if ((error = copyout(&utv, (caddr_t)args->timeout,
		    sizeof(utv))))
			goto select_out;
	}

select_out:
#ifdef DEBUG
	if (ldebug(select))
		kprintf(LMSG("select_out -> %d"), error);
#endif
	return error;
}

int     
sys_linux_mremap(struct linux_mremap_args *args)
{
	struct munmap_args bsd_args; 
	int error = 0;

#ifdef DEBUG
	if (ldebug(mremap))
		kprintf(ARGS(mremap, "%p, %08lx, %08lx, %08lx"),
		    (void *)args->addr, 
		    (unsigned long)args->old_len, 
		    (unsigned long)args->new_len,
		    (unsigned long)args->flags);
#endif
	args->new_len = round_page(args->new_len);
	args->old_len = round_page(args->old_len);

	if (args->new_len > args->old_len) {
		args->sysmsg_result = 0;
		return ENOMEM;
	}

	if (args->new_len < args->old_len) {
		bsd_args.sysmsg_result = 0;
		bsd_args.addr = (caddr_t)(args->addr + args->new_len);
		bsd_args.len = args->old_len - args->new_len;
		error = sys_munmap(&bsd_args);
	}

	args->sysmsg_resultp = error ? NULL : (void *)args->addr;
	return error;
}

#define	LINUX_MS_ASYNC		0x0001
#define	LINUX_MS_INVALIDATE	0x0002
#define	LINUX_MS_SYNC		0x0004

int
sys_linux_msync(struct linux_msync_args *args)
{
	struct msync_args bsd_args;
	int error;

	bsd_args.addr = (caddr_t)args->addr;
	bsd_args.len = args->len;
	bsd_args.flags = args->fl & ~LINUX_MS_SYNC;
	bsd_args.sysmsg_result = 0;

	error = sys_msync(&bsd_args);
	args->sysmsg_result = bsd_args.sysmsg_result;
	return(error);
}

int
sys_linux_time(struct linux_time_args *args)
{
	struct timeval tv;
	l_time_t tm;
	int error;

#ifdef DEBUG
	if (ldebug(time))
		kprintf(ARGS(time, "*"));
#endif

	microtime(&tv);
	tm = tv.tv_sec;
	if (args->tm && (error = copyout(&tm, (caddr_t)args->tm, sizeof(tm))))
		return error;
	args->sysmsg_lresult = tm;
	return 0;
}

struct l_times_argv {
	l_long		tms_utime;
	l_long		tms_stime;
	l_long		tms_cutime;
	l_long		tms_cstime;
};

#define CLK_TCK 100	/* Linux uses 100 */

#define CONVTCK(r)	(r.tv_sec * CLK_TCK + r.tv_usec / (1000000 / CLK_TCK))

int
sys_linux_times(struct linux_times_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct timeval tv;
	struct l_times_argv tms;
	struct rusage ru;
	int error;

	KKASSERT(p);
#ifdef DEBUG
	if (ldebug(times))
		kprintf(ARGS(times, "*"));
#endif

	calcru_proc(p, &ru);

	tms.tms_utime = CONVTCK(ru.ru_utime);
	tms.tms_stime = CONVTCK(ru.ru_stime);

	tms.tms_cutime = CONVTCK(p->p_cru.ru_utime);
	tms.tms_cstime = CONVTCK(p->p_cru.ru_stime);

	if ((error = copyout(&tms, (caddr_t)args->buf, sizeof(tms))))
		return error;

	microuptime(&tv);
	args->sysmsg_result = (int)CONVTCK(tv);
	return 0;
}

int
sys_linux_newuname(struct linux_newuname_args *args)
{
	struct thread *td = curthread;
	struct l_new_utsname utsname;
	char *osrelease, *osname;

#ifdef DEBUG
	if (ldebug(newuname))
		kprintf(ARGS(newuname, "*"));
#endif

	osname = linux_get_osname(td);
	osrelease = linux_get_osrelease(td);

	bzero(&utsname, sizeof(utsname));
	strncpy(utsname.sysname, osname, LINUX_MAX_UTSNAME-1);
	strncpy(utsname.nodename, hostname, LINUX_MAX_UTSNAME-1);
	strncpy(utsname.release, osrelease, LINUX_MAX_UTSNAME-1);
	strncpy(utsname.version, version, LINUX_MAX_UTSNAME-1);
	strncpy(utsname.machine, machine, LINUX_MAX_UTSNAME-1);
	strncpy(utsname.domainname, domainname, LINUX_MAX_UTSNAME-1);

	return (copyout(&utsname, (caddr_t)args->buf, sizeof(utsname)));
}

#if defined(__i386__)
struct l_utimbuf {
	l_time_t l_actime;
	l_time_t l_modtime;
};

int
sys_linux_utime(struct linux_utime_args *args)
{
	struct timeval tv[2];
	struct l_utimbuf lut;
	struct nlookupdata nd;
	char *path;
	int error;

	error = linux_copyin_path(args->fname, &path, LINUX_PATH_EXISTS);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(utime))
		kprintf(ARGS(utime, "%s, *"), path);
#endif

	if (args->times) {
		error = copyin(args->times, &lut, sizeof(lut));
		if (error)
			goto cleanup;
		tv[0].tv_sec = lut.l_actime;
		tv[0].tv_usec = 0;
		tv[1].tv_sec = lut.l_modtime;
		tv[1].tv_usec = 0;
	}
	error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
	if (error == 0)
		error = kern_utimes(&nd, args->times ? tv : NULL);
	nlookup_done(&nd);
cleanup:
	linux_free_path(&path);
	return (error);
}
#endif /* __i386__ */

#define __WCLONE 0x80000000

int
sys_linux_waitpid(struct linux_waitpid_args *args)
{
	int error, options, status;

#ifdef DEBUG
	if (ldebug(waitpid))
		kprintf(ARGS(waitpid, "%d, %p, %d"),
		    args->pid, (void *)args->status, args->options);
#endif
	options = args->options & (WNOHANG | WUNTRACED);
	/* WLINUXCLONE should be equal to __WCLONE, but we make sure */
	if (args->options & __WCLONE)
		options |= WLINUXCLONE;

	error = kern_wait(args->pid, args->status ? &status : NULL, options,
	    NULL, &args->sysmsg_result);

	if (error == 0 && args->status) {
		status &= 0xffff;
		if (WIFSIGNALED(status))
			status = (status & 0xffffff80) |
			    BSD_TO_LINUX_SIGNAL(WTERMSIG(status));
		else if (WIFSTOPPED(status))
			status = (status & 0xffff00ff) |
			    (BSD_TO_LINUX_SIGNAL(WSTOPSIG(status)) << 8);
		error = copyout(&status, args->status, sizeof(status));
	}

	return (error);
}

int
sys_linux_wait4(struct linux_wait4_args *args)
{
	struct thread *td = curthread;
	struct lwp *lp = td->td_lwp;
	struct rusage rusage;
	int error, options, status;

	KKASSERT(lp);

#ifdef DEBUG
	if (ldebug(wait4))
		kprintf(ARGS(wait4, "%d, %p, %d, %p"),
		    args->pid, (void *)args->status, args->options,
		    (void *)args->rusage);
#endif
	options = args->options & (WNOHANG | WUNTRACED);
	/* WLINUXCLONE should be equal to __WCLONE, but we make sure */
	if (args->options & __WCLONE)
		options |= WLINUXCLONE;

	error = kern_wait(args->pid, args->status ? &status : NULL, options,
	    args->rusage ? &rusage : NULL, &args->sysmsg_result);

	if (error == 0)
		lwp_delsig(lp, SIGCHLD);

	if (error == 0 && args->status) {
		status &= 0xffff;
		if (WIFSIGNALED(status))
			status = (status & 0xffffff80) |
			    BSD_TO_LINUX_SIGNAL(WTERMSIG(status));
		else if (WIFSTOPPED(status))
			status = (status & 0xffff00ff) |
			    (BSD_TO_LINUX_SIGNAL(WSTOPSIG(status)) << 8);
		error = copyout(&status, args->status, sizeof(status));
	}
	if (error == 0 && args->rusage)
		error = copyout(&rusage, args->rusage, sizeof(rusage));

	return (error);
}

int
sys_linux_mknod(struct linux_mknod_args *args)
{
	struct nlookupdata nd;
	char *path;
	int error;

	error = linux_copyin_path(args->path, &path, LINUX_PATH_CREATE);
	if (error)
		return (error);
#ifdef DEBUG
	if (ldebug(mknod))
		kprintf(ARGS(mknod, "%s, %d, %d"),
		    path, args->mode, args->dev);
#endif
	error = nlookup_init(&nd, path, UIO_SYSSPACE, 0);
	if (error == 0) {
		if (args->mode & S_IFIFO) {
			error = kern_mkfifo(&nd, args->mode);
		} else {
			error = kern_mknod(&nd, args->mode,
					   umajor(args->dev),
					   uminor(args->dev));
		}
	}
	nlookup_done(&nd);

	linux_free_path(&path);
	return(error);
}

/*
 * UGH! This is just about the dumbest idea I've ever heard!!
 */
int
sys_linux_personality(struct linux_personality_args *args)
{
#ifdef DEBUG
	if (ldebug(personality))
		kprintf(ARGS(personality, "%d"), args->per);
#endif
	if (args->per != 0)
		return EINVAL;

	/* Yes Jim, it's still a Linux... */
	args->sysmsg_result = 0;
	return 0;
}

/*
 * Wrappers for get/setitimer for debugging..
 */
int
sys_linux_setitimer(struct linux_setitimer_args *args)
{
	struct setitimer_args bsa;
	struct itimerval foo;
	int error;

#ifdef DEBUG
	if (ldebug(setitimer))
		kprintf(ARGS(setitimer, "%p, %p"),
		    (void *)args->itv, (void *)args->oitv);
#endif
	bsa.which = args->which;
	bsa.itv = (struct itimerval *)args->itv;
	bsa.oitv = (struct itimerval *)args->oitv;
	bsa.sysmsg_result = 0;
	if (args->itv) {
	    if ((error = copyin((caddr_t)args->itv, &foo, sizeof(foo))))
		return error;
#ifdef DEBUG
	    if (ldebug(setitimer)) {
	        kprintf("setitimer: value: sec: %ld, usec: %ld\n",
		    foo.it_value.tv_sec, foo.it_value.tv_usec);
	        kprintf("setitimer: interval: sec: %ld, usec: %ld\n",
		    foo.it_interval.tv_sec, foo.it_interval.tv_usec);
	    }
#endif
	}
	error = sys_setitimer(&bsa);
	args->sysmsg_result = bsa.sysmsg_result;
	return(error);
}

int
sys_linux_getitimer(struct linux_getitimer_args *args)
{
	struct getitimer_args bsa;
	int error;
#ifdef DEBUG
	if (ldebug(getitimer))
		kprintf(ARGS(getitimer, "%p"), (void *)args->itv);
#endif
	bsa.which = args->which;
	bsa.itv = (struct itimerval *)args->itv;
	bsa.sysmsg_result = 0;
	error = sys_getitimer(&bsa);
	args->sysmsg_result = bsa.sysmsg_result;
	return(error);
}

int
sys_linux_nice(struct linux_nice_args *args)
{
	struct setpriority_args	bsd_args;
	int error;

	bsd_args.which = PRIO_PROCESS;
	bsd_args.who = 0;	/* current process */
	bsd_args.prio = args->inc;
	bsd_args.sysmsg_result = 0;
	error = sys_setpriority(&bsd_args);
	args->sysmsg_result = bsd_args.sysmsg_result;
	return(error);
}

int
sys_linux_setgroups(struct linux_setgroups_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct ucred *newcred, *oldcred;
	l_gid_t linux_gidset[NGROUPS];
	gid_t *bsd_gidset;
	int ngrp, error;

	KKASSERT(p);

	ngrp = args->gidsetsize;
	oldcred = p->p_ucred;

	/*
	 * cr_groups[0] holds egid. Setting the whole set from
	 * the supplied set will cause egid to be changed too.
	 * Keep cr_groups[0] unchanged to prevent that.
	 */

	if ((error = priv_check_cred(oldcred, PRIV_ROOT, PRISON_ROOT)) != 0)
		return (error);

	if (ngrp >= NGROUPS)
		return (EINVAL);

	newcred = crdup(oldcred);
	if (ngrp > 0) {
		error = copyin((caddr_t)args->grouplist, linux_gidset,
			       ngrp * sizeof(l_gid_t));
		if (error)
			return (error);

		newcred->cr_ngroups = ngrp + 1;

		bsd_gidset = newcred->cr_groups;
		ngrp--;
		while (ngrp >= 0) {
			bsd_gidset[ngrp + 1] = linux_gidset[ngrp];
			ngrp--;
		}
	} else {
		newcred->cr_ngroups = 1;
	}

	setsugid();
	p->p_ucred = newcred;
	crfree(oldcred);
	return (0);
}

int
sys_linux_getgroups(struct linux_getgroups_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct ucred *cred;
	l_gid_t linux_gidset[NGROUPS];
	gid_t *bsd_gidset;
	int bsd_gidsetsz, ngrp, error;

	KKASSERT(p);

	cred = p->p_ucred;
	bsd_gidset = cred->cr_groups;
	bsd_gidsetsz = cred->cr_ngroups - 1;

	/*
	 * cr_groups[0] holds egid. Returning the whole set
	 * here will cause a duplicate. Exclude cr_groups[0]
	 * to prevent that.
	 */

	if ((ngrp = args->gidsetsize) == 0) {
		args->sysmsg_result = bsd_gidsetsz;
		return (0);
	}

	if (ngrp < bsd_gidsetsz)
		return (EINVAL);

	ngrp = 0;
	while (ngrp < bsd_gidsetsz) {
		linux_gidset[ngrp] = bsd_gidset[ngrp + 1];
		ngrp++;
	}

	if ((error = copyout(linux_gidset, (caddr_t)args->grouplist,
	    ngrp * sizeof(l_gid_t))))
		return (error);

	args->sysmsg_result = ngrp;
	return (0);
}

int
sys_linux_setrlimit(struct linux_setrlimit_args *args)
{
	struct l_rlimit linux_rlim;
	struct rlimit rlim;
	u_int which;
	int error;

#ifdef DEBUG
	if (ldebug(setrlimit))
		kprintf(ARGS(setrlimit, "%d, %p"),
		    args->resource, (void *)args->rlim);
#endif
	if (args->resource >= LINUX_RLIM_NLIMITS)
		return (EINVAL);
	which = linux_to_bsd_resource[args->resource];
	if (which == -1)
		return (EINVAL);

	error = copyin(args->rlim, &linux_rlim, sizeof(linux_rlim));
	if (error)
		return (error);
	rlim.rlim_cur = (rlim_t)linux_rlim.rlim_cur;
	rlim.rlim_max = (rlim_t)linux_rlim.rlim_max;

	error = kern_setrlimit(which, &rlim);

	return(error);
}

int
sys_linux_old_getrlimit(struct linux_old_getrlimit_args *args)
{
	struct l_rlimit linux_rlim;
	struct rlimit rlim;
	u_int which;
	int error;

#ifdef DEBUG
	if (ldebug(old_getrlimit))
		kprintf(ARGS(old_getrlimit, "%d, %p"),
		    args->resource, (void *)args->rlim);
#endif
	if (args->resource >= LINUX_RLIM_NLIMITS)
		return (EINVAL);
	which = linux_to_bsd_resource[args->resource];
	if (which == -1)
		return (EINVAL);

	error = kern_getrlimit(which, &rlim);

	if (error == 0) {
		linux_rlim.rlim_cur = (l_ulong)rlim.rlim_cur;
		if (linux_rlim.rlim_cur == ULONG_MAX)
			linux_rlim.rlim_cur = LONG_MAX;
		linux_rlim.rlim_max = (l_ulong)rlim.rlim_max;
		if (linux_rlim.rlim_max == ULONG_MAX)
			linux_rlim.rlim_max = LONG_MAX;
		error = copyout(&linux_rlim, args->rlim, sizeof(linux_rlim));
	}
	return (error);
}

int
sys_linux_getrlimit(struct linux_getrlimit_args *args)
{
	struct l_rlimit linux_rlim;
	struct rlimit rlim;
	u_int which;
	int error;

#ifdef DEBUG
	if (ldebug(getrlimit))
		kprintf(ARGS(getrlimit, "%d, %p"),
		    args->resource, (void *)args->rlim);
#endif
	if (args->resource >= LINUX_RLIM_NLIMITS)
		return (EINVAL);
	which = linux_to_bsd_resource[args->resource];
	if (which == -1)
		return (EINVAL);

	error = kern_getrlimit(which, &rlim);

	if (error == 0) {
		linux_rlim.rlim_cur = (l_ulong)rlim.rlim_cur;
		linux_rlim.rlim_max = (l_ulong)rlim.rlim_max;
		error = copyout(&linux_rlim, args->rlim, sizeof(linux_rlim));
	}
	return (error);
}

int
sys_linux_sched_setscheduler(struct linux_sched_setscheduler_args *args)
{
	struct sched_setscheduler_args bsd;
	int error;

#ifdef DEBUG
	if (ldebug(sched_setscheduler))
		kprintf(ARGS(sched_setscheduler, "%d, %d, %p"),
		    args->pid, args->policy, (const void *)args->param);
#endif

	switch (args->policy) {
	case LINUX_SCHED_OTHER:
		bsd.policy = SCHED_OTHER;
		break;
	case LINUX_SCHED_FIFO:
		bsd.policy = SCHED_FIFO;
		break;
	case LINUX_SCHED_RR:
		bsd.policy = SCHED_RR;
		break;
	default:
		return EINVAL;
	}

	bsd.pid = args->pid;
	bsd.param = (struct sched_param *)args->param;
	bsd.sysmsg_result = 0;

	error = sys_sched_setscheduler(&bsd);
	args->sysmsg_result = bsd.sysmsg_result;
	return(error);
}

int
sys_linux_sched_getscheduler(struct linux_sched_getscheduler_args *args)
{
	struct sched_getscheduler_args bsd;
	int error;

#ifdef DEBUG
	if (ldebug(sched_getscheduler))
		kprintf(ARGS(sched_getscheduler, "%d"), args->pid);
#endif

	bsd.sysmsg_result = 0;
	bsd.pid = args->pid;
	error = sys_sched_getscheduler(&bsd);
	args->sysmsg_result = bsd.sysmsg_result;

	switch (args->sysmsg_result) {
	case SCHED_OTHER:
		args->sysmsg_result = LINUX_SCHED_OTHER;
		break;
	case SCHED_FIFO:
		args->sysmsg_result = LINUX_SCHED_FIFO;
		break;
	case SCHED_RR:
		args->sysmsg_result = LINUX_SCHED_RR;
		break;
	}
	return error;
}

int
sys_linux_sched_get_priority_max(struct linux_sched_get_priority_max_args *args)
{
	struct sched_get_priority_max_args bsd;
	int error;

#ifdef DEBUG
	if (ldebug(sched_get_priority_max))
		kprintf(ARGS(sched_get_priority_max, "%d"), args->policy);
#endif

	switch (args->policy) {
	case LINUX_SCHED_OTHER:
		bsd.policy = SCHED_OTHER;
		break;
	case LINUX_SCHED_FIFO:
		bsd.policy = SCHED_FIFO;
		break;
	case LINUX_SCHED_RR:
		bsd.policy = SCHED_RR;
		break;
	default:
		return EINVAL;
	}
	bsd.sysmsg_result = 0;

	error = sys_sched_get_priority_max(&bsd);
	args->sysmsg_result = bsd.sysmsg_result;
	return(error);
}

int
sys_linux_sched_get_priority_min(struct linux_sched_get_priority_min_args *args)
{
	struct sched_get_priority_min_args bsd;
	int error;

#ifdef DEBUG
	if (ldebug(sched_get_priority_min))
		kprintf(ARGS(sched_get_priority_min, "%d"), args->policy);
#endif

	switch (args->policy) {
	case LINUX_SCHED_OTHER:
		bsd.policy = SCHED_OTHER;
		break;
	case LINUX_SCHED_FIFO:
		bsd.policy = SCHED_FIFO;
		break;
	case LINUX_SCHED_RR:
		bsd.policy = SCHED_RR;
		break;
	default:
		return EINVAL;
	}
	bsd.sysmsg_result = 0;

	error = sys_sched_get_priority_min(&bsd);
	args->sysmsg_result = bsd.sysmsg_result;
	return(error);
}

#define REBOOT_CAD_ON	0x89abcdef
#define REBOOT_CAD_OFF	0
#define REBOOT_HALT	0xcdef0123

int
sys_linux_reboot(struct linux_reboot_args *args)
{
	struct reboot_args bsd_args;
	int error;

#ifdef DEBUG
	if (ldebug(reboot))
		kprintf(ARGS(reboot, "0x%x"), args->cmd);
#endif
	if (args->cmd == REBOOT_CAD_ON || args->cmd == REBOOT_CAD_OFF)
		return (0);
	bsd_args.opt = (args->cmd == REBOOT_HALT) ? RB_HALT : 0;
	bsd_args.sysmsg_result = 0;

	error = sys_reboot(&bsd_args);
	args->sysmsg_result = bsd_args.sysmsg_result;
	return(error);
}

/*
 * The FreeBSD native getpid(2), getgid(2) and getuid(2) also modify
 * p->p_retval[1] when COMPAT_43 or COMPAT_SUNOS is defined. This
 * globbers registers that are assumed to be preserved. The following
 * lightweight syscalls fixes this. See also linux_getgid16() and
 * linux_getuid16() in linux_uid16.c.
 *
 * linux_getpid() - MP SAFE
 * linux_getgid() - MP SAFE
 * linux_getuid() - MP SAFE
 */

int
sys_linux_getpid(struct linux_getpid_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	KKASSERT(p);

	args->sysmsg_result = p->p_pid;
	return (0);
}

int
sys_linux_getgid(struct linux_getgid_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	KKASSERT(p);

	args->sysmsg_result = p->p_ucred->cr_rgid;
	return (0);
}

int
sys_linux_getuid(struct linux_getuid_args *args)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;

	KKASSERT(p);

	args->sysmsg_result = p->p_ucred->cr_ruid;
	return (0);
}

int
sys_linux_getsid(struct linux_getsid_args *args)
{
	struct getsid_args bsd;
	int error;

	bsd.sysmsg_result = 0;
	bsd.pid = args->pid;
	error = sys_getsid(&bsd);
	args->sysmsg_result = bsd.sysmsg_result;
	return(error);
}

int
linux_nosys(struct nosys_args *args)
{
	/* XXX */
	return (ENOSYS);
}
