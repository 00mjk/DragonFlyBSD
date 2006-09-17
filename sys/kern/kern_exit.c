/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *	@(#)kern_exit.c	8.7 (Berkeley) 2/12/94
 * $FreeBSD: src/sys/kern/kern_exit.c,v 1.92.2.11 2003/01/13 22:51:16 dillon Exp $
 * $DragonFly: src/sys/kern/kern_exit.c,v 1.62 2006/09/17 21:07:32 dillon Exp $
 */

#include "opt_compat.h"
#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/ktrace.h>
#include <sys/pioctl.h>
#include <sys/tty.h>
#include <sys/wait.h>
#include <sys/vnode.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/ptrace.h>
#include <sys/acct.h>		/* for acct_process() function prototype */
#include <sys/filedesc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/aio.h>
#include <sys/jail.h>
#include <sys/kern_syscall.h>
#include <sys/upcall.h>
#include <sys/caps.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_zone.h>
#include <vm/vm_extern.h>
#include <sys/user.h>

#include <sys/thread2.h>

static MALLOC_DEFINE(M_ATEXIT, "atexit", "atexit callback");
static MALLOC_DEFINE(M_ZOMBIE, "zombie", "zombie proc status");

/*
 * callout list for things to do at exit time
 */
struct exitlist {
	exitlist_fn function;
	TAILQ_ENTRY(exitlist) next;
};

TAILQ_HEAD(exit_list_head, exitlist);
static struct exit_list_head exit_list = TAILQ_HEAD_INITIALIZER(exit_list);

/*
 * exit --
 *	Death of process.
 *
 * SYS_EXIT_ARGS(int rval)
 */
int
sys_exit(struct exit_args *uap)
{
	exit1(W_EXITCODE(uap->rval, 0));
	/* NOTREACHED */
}

/*
 * Exit: deallocate address space and other resources, change proc state
 * to zombie, and unlink proc from allproc and parent's lists.  Save exit
 * status and rusage for wait().  Check for child processes and orphan them.
 */
void
exit1(int rv)
{
	struct proc *p = curproc;
	struct lwp *lp;
	struct proc *q, *nq;
	struct vmspace *vm;
	struct vnode *vtmp;
	struct exitlist *ep;

	if (p->p_pid == 1) {
		printf("init died (signal %d, exit %d)\n",
		    WTERMSIG(rv), WEXITSTATUS(rv));
		panic("Going nowhere without my init!");
	}

	lp = &p->p_lwp;		/* XXX lwp kill other threads */

	caps_exit(lp->lwp_thread);
	aio_proc_rundown(p);

	/* are we a task leader? */
	if(p == p->p_leader) {
        	struct kill_args killArgs;
		killArgs.signum = SIGKILL;
		q = p->p_peers;
		while(q) {
			killArgs.pid = q->p_pid;
			/*
		         * The interface for kill is better
			 * than the internal signal
			 */
			sys_kill(&killArgs);
			nq = q;
			q = q->p_peers;
		}
		while (p->p_peers) 
		  tsleep((caddr_t)p, 0, "exit1", 0);
	} 

#ifdef PGINPROF
	vmsizmon();
#endif
	STOPEVENT(p, S_EXIT, rv);
	wakeup(&p->p_stype);	/* Wakeup anyone in procfs' PIOCWAIT */

	/* 
	 * Check if any loadable modules need anything done at process exit.
	 * e.g. SYSV IPC stuff
	 * XXX what if one of these generates an error?
	 */
	TAILQ_FOREACH(ep, &exit_list, next) 
		(*ep->function)(p->p_thread);

	if (p->p_flag & P_PROFIL)
		stopprofclock(p);
	MALLOC(p->p_ru, struct rusage *, sizeof(struct rusage),
		M_ZOMBIE, M_WAITOK);
	/*
	 * If parent is waiting for us to exit or exec,
	 * P_PPWAIT is set; we will wakeup the parent below.
	 */
	p->p_flag &= ~(P_TRACED | P_PPWAIT);
	p->p_flag |= P_WEXIT;
	SIGEMPTYSET(p->p_siglist);
	if (timevalisset(&p->p_realtimer.it_value))
		callout_stop(&p->p_ithandle);

	/*
	 * Reset any sigio structures pointing to us as a result of
	 * F_SETOWN with our pid.
	 */
	funsetownlst(&p->p_sigiolst);

	/*
	 * Close open files and release open-file table.
	 * This may block!
	 */
	fdfree(p);
	p->p_fd = NULL;

	if(p->p_leader->p_peers) {
		q = p->p_leader;
		while(q->p_peers != p)
			q = q->p_peers;
		q->p_peers = p->p_peers;
		wakeup((caddr_t)p->p_leader);
	}

	/*
	 * XXX Shutdown SYSV semaphores
	 */
	semexit(p);

	KKASSERT(p->p_numposixlocks == 0);

	/* The next two chunks should probably be moved to vmspace_exit. */
	vm = p->p_vmspace;

	/*
	 * Release upcalls associated with this process
	 */
	if (vm->vm_upcalls)
		upc_release(vm, &p->p_lwp);

	/* clean up data related to virtual kernel operation */
	if (p->p_vkernel) {
		vkernel_drop(p->p_vkernel);
		p->p_vkernel = NULL;
	}

	/*
	 * Release user portion of address space.
	 * This releases references to vnodes,
	 * which could cause I/O if the file has been unlinked.
	 * Need to do this early enough that we can still sleep.
	 * Can't free the entire vmspace as the kernel stack
	 * may be mapped within that space also.
	 *
	 * Processes sharing the same vmspace may exit in one order, and
	 * get cleaned up by vmspace_exit() in a different order.  The
	 * last exiting process to reach this point releases as much of
	 * the environment as it can, and the last process cleaned up
	 * by vmspace_exit() (which decrements exitingcnt) cleans up the
	 * remainder.
	 */
	++vm->vm_exitingcnt;
	if (--vm->vm_refcnt == 0) {
		shmexit(vm);
		pmap_remove_pages(vmspace_pmap(vm), VM_MIN_ADDRESS,
		    VM_MAXUSER_ADDRESS);
		(void) vm_map_remove(&vm->vm_map, VM_MIN_ADDRESS,
		    VM_MAXUSER_ADDRESS);
	}

	if (SESS_LEADER(p)) {
		struct session *sp = p->p_session;
		struct vnode *vp;

		if (sp->s_ttyvp) {
			/*
			 * We are the controlling process.  Signal the 
			 * foreground process group, drain the controlling
			 * terminal, and revoke access to the controlling
			 * terminal.
			 *
			 * NOTE: while waiting for the process group to exit
			 * it is possible that one of the processes in the
			 * group will revoke the tty, so we have to recheck.
			 */
			if (sp->s_ttyp && (sp->s_ttyp->t_session == sp)) {
				if (sp->s_ttyp->t_pgrp)
					pgsignal(sp->s_ttyp->t_pgrp, SIGHUP, 1);
				(void) ttywait(sp->s_ttyp);
				/*
				 * The tty could have been revoked
				 * if we blocked.
				 */
				if ((vp = sp->s_ttyvp) != NULL) {
					ttyclosesession(sp, 0);
					vx_lock(vp);
					VOP_REVOKE(vp, REVOKEALL);
					vx_unlock(vp);
					vrele(vp);	/* s_ttyvp ref */
				}
			}
			/*
			 * Release the tty.  If someone has it open via
			 * /dev/tty then close it (since they no longer can
			 * once we've NULL'd it out).
			 */
			if (sp->s_ttyvp)
				ttyclosesession(sp, 1);
			/*
			 * s_ttyp is not zero'd; we use this to indicate
			 * that the session once had a controlling terminal.
			 * (for logging and informational purposes)
			 */
		}
		sp->s_leader = NULL;
	}
	fixjobc(p, p->p_pgrp, 0);
	(void)acct_process(p);
#ifdef KTRACE
	/*
	 * release trace file
	 */
	if (p->p_tracenode)
		ktrdestroy(&p->p_tracenode);
	p->p_traceflag = 0;
#endif
	/*
	 * Release reference to text vnode
	 */
	if ((vtmp = p->p_textvp) != NULL) {
		p->p_textvp = NULL;
		vrele(vtmp);
	}

	/*
	 * Move the process to the zombie list.  This will block
	 * until the process p_lock count reaches 0.  The process will
	 * not be reaped until TDF_EXITING is set by cpu_thread_exit(),
	 * which is called from cpu_proc_exit().
	 */
	proc_move_allproc_zombie(p);

	q = LIST_FIRST(&p->p_children);
	if (q)		/* only need this if any child is S_ZOMB */
		wakeup((caddr_t) initproc);
	for (; q != 0; q = nq) {
		nq = LIST_NEXT(q, p_sibling);
		LIST_REMOVE(q, p_sibling);
		LIST_INSERT_HEAD(&initproc->p_children, q, p_sibling);
		q->p_pptr = initproc;
		q->p_sigparent = SIGCHLD;
		/*
		 * Traced processes are killed
		 * since their existence means someone is screwing up.
		 */
		if (q->p_flag & P_TRACED) {
			q->p_flag &= ~P_TRACED;
			ksignal(q, SIGKILL);
		}
	}

	/*
	 * Save exit status and final rusage info, adding in child rusage
	 * info and self times.
	 */
	p->p_xstat = rv;
	*p->p_ru = p->p_stats->p_ru;
	calcru(p, &p->p_ru->ru_utime, &p->p_ru->ru_stime, NULL);
	ruadd(p->p_ru, &p->p_stats->p_cru);

	/*
	 * notify interested parties of our demise.
	 */
	KNOTE(&p->p_klist, NOTE_EXIT);

	/*
	 * Notify parent that we're gone.  If parent has the PS_NOCLDWAIT
	 * flag set, notify process 1 instead (and hope it will handle
	 * this situation).
	 */
	if (p->p_pptr->p_procsig->ps_flag & PS_NOCLDWAIT) {
		struct proc *pp = p->p_pptr;
		proc_reparent(p, initproc);
		/*
		 * If this was the last child of our parent, notify
		 * parent, so in case he was wait(2)ing, he will
		 * continue.
		 */
		if (LIST_EMPTY(&pp->p_children))
			wakeup((caddr_t)pp);
	}

	if (p->p_sigparent && p->p_pptr != initproc) {
	        ksignal(p->p_pptr, p->p_sigparent);
	} else {
	        ksignal(p->p_pptr, SIGCHLD);
	}

	wakeup((caddr_t)p->p_pptr);
	/*
	 * cpu_exit is responsible for clearing curproc, since
	 * it is heavily integrated with the thread/switching sequence.
	 *
	 * Other substructures are freed from wait().
	 */
	plimit_free(&p->p_limit);

	/*
	 * Release the current user process designation on the process so
	 * the userland scheduler can work in someone else.
	 */
	p->p_usched->release_curproc(lp);

	/*
	 * Finally, call machine-dependent code to release the remaining
	 * resources including address space, the kernel stack and pcb.
	 * The address space is released by "vmspace_free(p->p_vmspace)";
	 * This is machine-dependent, as we may have to change stacks
	 * or ensure that the current one isn't reallocated before we
	 * finish.  cpu_exit will end with a call to cpu_switch(), finishing
	 * our execution (pun intended).
	 */
	cpu_proc_exit();
}

int
sys_wait4(struct wait_args *uap)
{
	struct rusage rusage;
	int error, status;

	error = kern_wait(uap->pid, uap->status ? &status : NULL,
	    uap->options, uap->rusage ? &rusage : NULL, &uap->sysmsg_fds[0]);

	if (error == 0 && uap->status)
		error = copyout(&status, uap->status, sizeof(*uap->status));
	if (error == 0 && uap->rusage)
		error = copyout(&rusage, uap->rusage, sizeof(*uap->rusage));
	return (error);
}

/*
 * wait1()
 *
 * wait_args(int pid, int *status, int options, struct rusage *rusage)
 */
int
kern_wait(pid_t pid, int *status, int options, struct rusage *rusage, int *res)
{
	struct thread *td = curthread;
	struct proc *q = td->td_proc;
	struct proc *p, *t;
	int nfound, error;

	if (pid == 0)
		pid = -q->p_pgid;
	if (options &~ (WUNTRACED|WNOHANG|WLINUXCLONE))
		return (EINVAL);
loop:
	/*
	 * Hack for backwards compatibility with badly written user code.  
	 * Or perhaps we have to do this anyway, it is unclear. XXX
	 *
	 * The problem is that if a process group is stopped and the parent
	 * is doing a wait*(..., WUNTRACED, ...), it will see the STOP
	 * of the child and then stop itself when it tries to return from the
	 * system call.  When the process group is resumed the parent will
	 * then get the STOP status even though the child has now resumed
	 * (a followup wait*() will get the CONT status).
	 *
	 * Previously the CONT would overwrite the STOP because the tstop
	 * was handled within tsleep(), and the parent would only see
	 * the CONT when both are stopped and continued together.  This litte
	 * two-line hack restores this effect.
	 */
	while (q->p_flag & P_STOPPED)  
            tstop(q);

	nfound = 0;
	LIST_FOREACH(p, &q->p_children, p_sibling) {
		if (pid != WAIT_ANY &&
		    p->p_pid != pid && p->p_pgid != -pid)
			continue;

		/* This special case handles a kthread spawned by linux_clone 
		 * (see linux_misc.c).  The linux_wait4 and linux_waitpid 
		 * functions need to be able to distinguish between waiting
		 * on a process and waiting on a thread.  It is a thread if
		 * p_sigparent is not SIGCHLD, and the WLINUXCLONE option
		 * signifies we want to wait for threads and not processes.
		 */
		if ((p->p_sigparent != SIGCHLD) ^ 
		    ((options & WLINUXCLONE) != 0)) {
			continue;
		}

		nfound++;
		if (p->p_flag & P_ZOMBIE) {
			/*
			 * Other kernel threads may be in the middle of 
			 * accessing the proc.  For example, kern/kern_proc.c
			 * could be blocked writing proc data to a sysctl.
			 * At the moment, if this occurs, we are not woken
			 * up and rely on a one-second retry.
			 */
			if (p->p_lock) {
				while (p->p_lock)
					tsleep(p, 0, "reap3", hz);
			}
			lwkt_wait_free(p->p_thread);

			/*
			 * The process's thread may still be in the middle
			 * of switching away, we can't rip its stack out from
			 * under it until TDF_EXITING is set and both
			 * TDF_RUNNING and TDF_PREEMPT_LOCK are clear.
			 * TDF_PREEMPT_LOCK must be checked because TDF_RUNNING
			 * will be cleared temporarily if a thread gets
			 * preempted.
			 *
			 * YYY no wakeup occurs so we depend on the timeout.
			 */
			if ((p->p_thread->td_flags & (TDF_RUNNING|TDF_PREEMPT_LOCK|TDF_EXITING)) != TDF_EXITING) {
				tsleep(p->p_thread, 0, "reap2", 1);
				goto loop;
			}

			/* scheduling hook for heuristic */
			p->p_usched->heuristic_exiting(td->td_lwp, &p->p_lwp);

			/* Take care of our return values. */
			*res = p->p_pid;
			if (status)
				*status = p->p_xstat;
			if (rusage)
				*rusage = *p->p_ru;
			/*
			 * If we got the child via a ptrace 'attach',
			 * we need to give it back to the old parent.
			 */
			if (p->p_oppid && (t = pfind(p->p_oppid))) {
				p->p_oppid = 0;
				proc_reparent(p, t);
				ksignal(t, SIGCHLD);
				wakeup((caddr_t)t);
				return (0);
			}
			p->p_xstat = 0;
			ruadd(&q->p_stats->p_cru, p->p_ru);
			FREE(p->p_ru, M_ZOMBIE);
			p->p_ru = NULL;

			/*
			 * Decrement the count of procs running with this uid.
			 */
			chgproccnt(p->p_ucred->cr_ruidinfo, -1, 0);

			/*
			 * Free up credentials.
			 */
			crfree(p->p_ucred);
			p->p_ucred = NULL;

			/*
			 * Remove unused arguments
			 */
			if (p->p_args && --p->p_args->ar_ref == 0)
				FREE(p->p_args, M_PARGS);

			/*
			 * Finally finished with old proc entry.
			 * Unlink it from its process group and free it.
			 */
			leavepgrp(p);
			proc_remove_zombie(p);

			if (--p->p_procsig->ps_refcnt == 0) {
				if (p->p_sigacts != &p->p_addr->u_sigacts)
					FREE(p->p_sigacts, M_SUBPROC);
			        FREE(p->p_procsig, M_SUBPROC);
				p->p_procsig = NULL;
			}

			vm_waitproc(p);
			zfree(proc_zone, p);
			nprocs--;
			return (0);
		}
		if ((p->p_flag & P_STOPPED) && (p->p_flag & P_WAITED) == 0 &&
		    (p->p_flag & P_TRACED || options & WUNTRACED)) {
			p->p_flag |= P_WAITED;

			*res = p->p_pid;
			if (status)
				*status = W_STOPCODE(p->p_xstat);
			/* Zero rusage so we get something consistent. */
			if (rusage)
				bzero(rusage, sizeof(rusage));
			return (0);
		}
	}
	if (nfound == 0)
		return (ECHILD);
	if (options & WNOHANG) {
		*res = 0;
		return (0);
	}
	error = tsleep((caddr_t)q, PCATCH, "wait", 0);
	if (error)
		return (error);
	goto loop;
}

/*
 * make process 'parent' the new parent of process 'child'.
 */
void
proc_reparent(struct proc *child, struct proc *parent)
{

	if (child->p_pptr == parent)
		return;

	LIST_REMOVE(child, p_sibling);
	LIST_INSERT_HEAD(&parent->p_children, child, p_sibling);
	child->p_pptr = parent;
}

/*
 * The next two functions are to handle adding/deleting items on the
 * exit callout list
 * 
 * at_exit():
 * Take the arguments given and put them onto the exit callout list,
 * However first make sure that it's not already there.
 * returns 0 on success.
 */

int
at_exit(exitlist_fn function)
{
	struct exitlist *ep;

#ifdef INVARIANTS
	/* Be noisy if the programmer has lost track of things */
	if (rm_at_exit(function)) 
		printf("WARNING: exit callout entry (%p) already present\n",
		    function);
#endif
	ep = kmalloc(sizeof(*ep), M_ATEXIT, M_NOWAIT);
	if (ep == NULL)
		return (ENOMEM);
	ep->function = function;
	TAILQ_INSERT_TAIL(&exit_list, ep, next);
	return (0);
}

/*
 * Scan the exit callout list for the given item and remove it.
 * Returns the number of items removed (0 or 1)
 */
int
rm_at_exit(exitlist_fn function)
{
	struct exitlist *ep;

	TAILQ_FOREACH(ep, &exit_list, next) {
		if (ep->function == function) {
			TAILQ_REMOVE(&exit_list, ep, next);
			kfree(ep, M_ATEXIT);
			return(1);
		}
	}	
	return (0);
}

void
check_sigacts(void)
{
	struct proc *p = curproc;
	struct sigacts *pss;

	if (p->p_procsig->ps_refcnt == 1 &&
	    p->p_sigacts != &p->p_addr->u_sigacts) {
		pss = p->p_sigacts;
		crit_enter();
		p->p_addr->u_sigacts = *pss;
		p->p_sigacts = &p->p_addr->u_sigacts;
		crit_exit();
		FREE(pss, M_SUBPROC);
	}
}

