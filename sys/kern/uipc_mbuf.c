/*
 * Copyright (c) 2004 Jeffrey M. Hsu.  All rights reserved.
 * Copyright (c) 2004 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Jeffrey M. Hsu.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 */

/*
 * Copyright (c) 2004 Jeffrey M. Hsu.  All rights reserved.
 *
 * License terms: all terms for the DragonFly license above plus the following:
 *
 * 4. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *	This product includes software developed by Jeffrey M. Hsu
 *	for the DragonFly Project.
 *
 *    This requirement may be waived with permission from Jeffrey Hsu.
 *    This requirement will sunset and may be removed on July 8 2005,
 *    after which the standard DragonFly license (as shown above) will
 *    apply.
 */

/*
 * Copyright (c) 1982, 1986, 1988, 1991, 1993
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
 * @(#)uipc_mbuf.c	8.2 (Berkeley) 1/4/94
 * $FreeBSD: src/sys/kern/uipc_mbuf.c,v 1.51.2.24 2003/04/15 06:59:29 silby Exp $
 * $DragonFly: src/sys/kern/uipc_mbuf.c,v 1.38 2005/05/29 10:39:59 hsu Exp $
 */

#include "opt_param.h"
#include "opt_mbuf_stress_test.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/uio.h>
#include <sys/thread.h>
#include <sys/globaldata.h>
#include <sys/thread2.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

#ifdef INVARIANTS
#include <machine/cpu.h>
#endif

/*
 * mbuf cluster meta-data
 */
typedef struct mbcluster {
	struct mbcluster *mcl_next;
	int32_t	mcl_magic;
	int32_t	mcl_refs;
	void	*mcl_data;
} *mbcluster_t;

typedef struct mbuf *mbuf_t;

#define MCL_MAGIC	0x6d62636c

static void mbinit (void *);
SYSINIT(mbuf, SI_SUB_MBUF, SI_ORDER_FIRST, mbinit, NULL)

static u_long	mbtypes[MT_NTYPES];

struct mbstat mbstat;
int	max_linkhdr;
int	max_protohdr;
int	max_hdr;
int	max_datalen;
int	m_defragpackets;
int	m_defragbytes;
int	m_defraguseless;
int	m_defragfailure;
#ifdef MBUF_STRESS_TEST
int	m_defragrandomfailures;
#endif

int	nmbclusters;
int	nmbufs;
u_int	m_mballoc_wid = 0;
u_int	m_clalloc_wid = 0;

SYSCTL_INT(_kern_ipc, KIPC_MAX_LINKHDR, max_linkhdr, CTLFLAG_RW,
	   &max_linkhdr, 0, "");
SYSCTL_INT(_kern_ipc, KIPC_MAX_PROTOHDR, max_protohdr, CTLFLAG_RW,
	   &max_protohdr, 0, "");
SYSCTL_INT(_kern_ipc, KIPC_MAX_HDR, max_hdr, CTLFLAG_RW, &max_hdr, 0, "");
SYSCTL_INT(_kern_ipc, KIPC_MAX_DATALEN, max_datalen, CTLFLAG_RW,
	   &max_datalen, 0, "");
SYSCTL_INT(_kern_ipc, OID_AUTO, mbuf_wait, CTLFLAG_RW,
	   &mbuf_wait, 0, "");
SYSCTL_STRUCT(_kern_ipc, KIPC_MBSTAT, mbstat, CTLFLAG_RW, &mbstat, mbstat, "");
SYSCTL_OPAQUE(_kern_ipc, OID_AUTO, mbtypes, CTLFLAG_RD, mbtypes,
	   sizeof(mbtypes), "LU", "");
SYSCTL_INT(_kern_ipc, KIPC_NMBCLUSTERS, nmbclusters, CTLFLAG_RW, 
	   &nmbclusters, 0, "Maximum number of mbuf clusters available");
SYSCTL_INT(_kern_ipc, OID_AUTO, nmbufs, CTLFLAG_RW, &nmbufs, 0,
	   "Maximum number of mbufs available"); 
SYSCTL_INT(_kern_ipc, OID_AUTO, m_defragpackets, CTLFLAG_RD,
	   &m_defragpackets, 0, "");
SYSCTL_INT(_kern_ipc, OID_AUTO, m_defragbytes, CTLFLAG_RD,
	   &m_defragbytes, 0, "");
SYSCTL_INT(_kern_ipc, OID_AUTO, m_defraguseless, CTLFLAG_RD,
	   &m_defraguseless, 0, "");
SYSCTL_INT(_kern_ipc, OID_AUTO, m_defragfailure, CTLFLAG_RD,
	   &m_defragfailure, 0, "");
#ifdef MBUF_STRESS_TEST
SYSCTL_INT(_kern_ipc, OID_AUTO, m_defragrandomfailures, CTLFLAG_RW,
	   &m_defragrandomfailures, 0, "");
#endif

static int mcl_pool_count;
static int mcl_pool_max = 20;
static int mcl_free_max = 1000;
static int mbuf_free_max = 5000;
 
SYSCTL_INT(_kern_ipc, OID_AUTO, mcl_pool_max, CTLFLAG_RW, &mcl_pool_max, 0,
           "Maximum number of mbufs+cluster in free list");
SYSCTL_INT(_kern_ipc, OID_AUTO, mcl_pool_count, CTLFLAG_RD, &mcl_pool_count, 0,
           "Current number of mbufs+cluster in free list");
SYSCTL_INT(_kern_ipc, OID_AUTO, mcl_free_max, CTLFLAG_RW, &mcl_free_max, 0,
           "Maximum number of clusters on the free list");
SYSCTL_INT(_kern_ipc, OID_AUTO, mbuf_free_max, CTLFLAG_RW, &mbuf_free_max, 0,
           "Maximum number of mbufs on the free list");

static MALLOC_DEFINE(M_MBUF, "mbuf", "mbuf");
static MALLOC_DEFINE(M_MBUFCL, "mbufcl", "mbufcl");

static mbuf_t mmbfree;
static mbcluster_t mclfree;
static struct mbuf *mcl_pool;

static void m_reclaim (void);
static int m_mballoc(int nmb, int how);
static int m_clalloc(int ncl, int how);
static struct mbuf *m_mballoc_wait(int caller, int type);
static void m_mclref(void *arg);
static void m_mclfree(void *arg);

#ifndef NMBCLUSTERS
#define NMBCLUSTERS	(512 + maxusers * 16)
#endif
#ifndef NMBUFS
#define NMBUFS		(nmbclusters * 4)
#endif

/*
 * Perform sanity checks of tunables declared above.
 */
static void
tunable_mbinit(void *dummy)
{

	/*
	 * This has to be done before VM init.
	 */
	nmbclusters = NMBCLUSTERS;
	TUNABLE_INT_FETCH("kern.ipc.nmbclusters", &nmbclusters);
	nmbufs = NMBUFS;
	TUNABLE_INT_FETCH("kern.ipc.nmbufs", &nmbufs);
	/* Sanity checks */
	if (nmbufs < nmbclusters * 2)
		nmbufs = nmbclusters * 2;

	return;
}
SYSINIT(tunable_mbinit, SI_SUB_TUNABLES, SI_ORDER_ANY, tunable_mbinit, NULL);

/* "number of clusters of pages" */
#define NCL_INIT	1

#define NMB_INIT	16

/* ARGSUSED*/
static void
mbinit(void *dummy)
{
	mmbfree = NULL;
	mclfree = NULL;
	mbstat.m_msize = MSIZE;
	mbstat.m_mclbytes = MCLBYTES;
	mbstat.m_minclsize = MINCLSIZE;
	mbstat.m_mlen = MLEN;
	mbstat.m_mhlen = MHLEN;

	crit_enter();
	if (m_mballoc(NMB_INIT, MB_DONTWAIT) == 0)
		goto bad;
#if MCLBYTES <= PAGE_SIZE
	if (m_clalloc(NCL_INIT, MB_DONTWAIT) == 0)
		goto bad;
#else
	/* It's OK to call contigmalloc in this context. */
	if (m_clalloc(16, MB_WAIT) == 0)
		goto bad;
#endif
	crit_exit();
	return;
bad:
	crit_exit();
	panic("mbinit");
}

/*
 * Allocate at least nmb mbufs and place on mbuf free list.
 * Returns the number of mbufs successfully allocated, 0 if none.
 *
 * Must be called while in a critical section.
 */
static int
m_mballoc(int nmb, int how)
{
	int i;
	struct mbuf *m;

	/*
	 * If we've hit the mbuf limit, stop allocating (or trying to)
	 * in order to avoid exhausting kernel memory entirely.
	 */
	if ((nmb + mbstat.m_mbufs) > nmbufs)
		return (0);

	/*
	 * Attempt to allocate the requested number of mbufs, terminate when
	 * the allocation fails but if blocking is allowed allocate at least
	 * one.
	 */
	for (i = 0; i < nmb; ++i) {
		m = malloc(MSIZE, M_MBUF, M_NOWAIT|M_NULLOK|M_ZERO);
		if (m == NULL) {
			if (how == MB_WAIT) {
				mbstat.m_wait++;
				m = malloc(MSIZE, M_MBUF,
					    M_WAITOK|M_NULLOK|M_ZERO);
			}
			if (m == NULL)
				break;
		}
		m->m_next = mmbfree;
		mmbfree = m;
		++mbstat.m_mbufs;
		++mbtypes[MT_FREE];
		how = MB_DONTWAIT;
	}
	return(i);
}

/*
 * Once mbuf memory has been exhausted and if the call to the allocation macros
 * (or, in some cases, functions) is with MB_WAIT, then it is necessary to rely
 * solely on reclaimed mbufs. Here we wait for an mbuf to be freed for a 
 * designated (mbuf_wait) time. 
 */
static struct mbuf *
m_mballoc_wait(int caller, int type)
{
	struct mbuf *m;

	crit_enter();
	m_mballoc_wid++;
	if ((tsleep(&m_mballoc_wid, 0, "mballc", mbuf_wait)) == EWOULDBLOCK)
		m_mballoc_wid--;
	crit_exit();

	/*
	 * Now that we (think) that we've got something, we will redo an
	 * MGET, but avoid getting into another instance of m_mballoc_wait()
	 * XXX: We retry to fetch _even_ if the sleep timed out. This is left
	 *      this way, purposely, in the [unlikely] case that an mbuf was
	 *      freed but the sleep was not awakened in time. 
	 */
	m = NULL;
	switch (caller) {
	case MGET_C:
		MGET(m, MB_DONTWAIT, type);
		break;
	case MGETHDR_C:
		MGETHDR(m, MB_DONTWAIT, type);
		break;
	default:
		panic("m_mballoc_wait: invalid caller (%d)", caller);
	}

	crit_enter();
	if (m != NULL) {		/* We waited and got something... */
		mbstat.m_wait++;
		/* Wake up another if we have more free. */
		if (mmbfree != NULL)
			MMBWAKEUP();
	}
	crit_exit();
	return (m);
}

#if MCLBYTES > PAGE_SIZE
static int i_want_my_mcl;

static void
kproc_mclalloc(void)
{
	int status;

	crit_enter();
	for (;;) {
		tsleep(&i_want_my_mcl, 0, "mclalloc", 0);

		while (i_want_my_mcl > 0) {
			if (m_clalloc(1, MB_WAIT) == 0)
				printf("m_clalloc failed even in thread context!\n");
			--i_want_my_mcl;
		}
	}
	/* not reached */
	crit_exit();
}

static struct thread *mclallocthread;
static struct kproc_desc mclalloc_kp = {
	"mclalloc",
	kproc_mclalloc,
	&mclallocthread
};
SYSINIT(mclallocthread, SI_SUB_KTHREAD_UPDATE, SI_ORDER_ANY, kproc_start,
	   &mclalloc_kp);
#endif

/*
 * Allocate at least nmb mbuf clusters and place on mbuf free list.
 * Returns the number of mbuf clusters successfully allocated, 0 if none.
 *
 * Must be called while in a critical section.
 */
static int
m_clalloc(int ncl, int how)
{
	static int last_report;
	mbcluster_t mcl;
	void *data;
	int i;

	/*
	 * If we've hit the mbuf cluster limit, stop allocating (or trying to).
	 */
	if ((ncl + mbstat.m_clusters) > nmbclusters)
		ncl = 0;

	/*
	 * Attempt to allocate the requested number of mbuf clusters,
	 * terminate when the allocation fails but if blocking is allowed
	 * allocate at least one.
	 *
	 * We need to allocate two structures for each cluster... a 
	 * ref counting / governing structure and the actual data.  MCLBYTES
	 * should be a power of 2 which means that the slab allocator will
	 * return a buffer that does not cross a page boundary.
	 */
	for (i = 0; i < ncl; ++i) {
		/*
		 * Meta structure
		 */
		mcl = malloc(sizeof(*mcl), M_MBUFCL, M_NOWAIT|M_NULLOK|M_ZERO);
		if (mcl == NULL) {
			if (how == MB_WAIT) {
				mbstat.m_wait++;
				mcl = malloc(sizeof(*mcl), 
					    M_MBUFCL, M_WAITOK|M_NULLOK|M_ZERO);
			}
			if (mcl == NULL)
				break;
		}

		/*
		 * Physically contiguous data buffer.
		 */
#if MCLBYTES > PAGE_SIZE
		if (how != MB_WAIT) {
			i_want_my_mcl += ncl - i;
			wakeup(&i_want_my_mcl);
			mbstat.m_wait++;
			data = NULL;
		} else {
			data = contigmalloc_map(MCLBYTES, M_MBUFCL,
				M_WAITOK, 0ul, ~0ul, PAGE_SIZE, 0, kernel_map);
		}
#else
		data = malloc(MCLBYTES, M_MBUFCL, M_NOWAIT|M_NULLOK);
		if (data == NULL) {
			if (how == MB_WAIT) {
				mbstat.m_wait++;
				data = malloc(MCLBYTES, M_MBUFCL,
						M_WAITOK|M_NULLOK);
			}
		}
#endif
		if (data == NULL) {
			free(mcl, M_MBUFCL);
			break;
		}
		mcl->mcl_next = mclfree;
		mcl->mcl_data = data;
		mcl->mcl_magic = MCL_MAGIC;
		mcl->mcl_refs = 0;
		mclfree = mcl;
		++mbstat.m_clfree;
		++mbstat.m_clusters;
		how = MB_DONTWAIT;
	}

	/*
	 * If we could not allocate any report failure no more often then
	 * once a second.
	 */
	if (i == 0) {
		mbstat.m_drops++;
		if (ticks < last_report || (ticks - last_report) >= hz) {
			last_report = ticks;
			printf("All mbuf clusters exhausted, please see tuning(7).\n");
		}
	}
	return (i);
}

/*
 * Once cluster memory has been exhausted and the allocation is called with
 * MB_WAIT, we rely on the mclfree pointers. If nothing is free, we will
 * sleep for a designated amount of time (mbuf_wait) or until we're woken up
 * due to sudden mcluster availability.
 *
 * Must be called while in a critical section.
 */
static void
m_clalloc_wait(void)
{
	/* If in interrupt context, and INVARIANTS, maintain sanity and die. */
	KASSERT(mycpu->gd_intr_nesting_level == 0, 
		("CLALLOC: CANNOT WAIT IN INTERRUPT"));

	/*
	 * Sleep until something's available or until we expire.
	 */
	m_clalloc_wid++;
	if ((tsleep(&m_clalloc_wid, 0, "mclalc", mbuf_wait)) == EWOULDBLOCK)
		m_clalloc_wid--;

	/*
	 * Try the allocation once more, and if we see mor then two
	 * free entries wake up others as well.
	 */
	m_clalloc(1, MB_WAIT);
	if (mclfree && mclfree->mcl_next) {
		MCLWAKEUP();
	}
}

/*
 * Return the number of references to this mbuf's data.  0 is returned
 * if the mbuf is not M_EXT, a reference count is returned if it is
 * M_EXT|M_EXT_CLUSTER, and 99 is returned if it is a special M_EXT.
 */
int
m_sharecount(struct mbuf *m)
{
    int count;

    switch(m->m_flags & (M_EXT|M_EXT_CLUSTER)) {
    case 0:
	count = 0;
	break;
    case M_EXT:
	count = 99;
	break;
    case M_EXT|M_EXT_CLUSTER:
	count = ((mbcluster_t)m->m_ext.ext_arg)->mcl_refs;
	break;
    default:
	panic("bad mbuf flags: %p", m);
	count = 0;
    }
    return(count);
}

/*
 * change mbuf to new type
 */
void
m_chtype(struct mbuf *m, int type)
{
	crit_enter();
	--mbtypes[m->m_type];
	++mbtypes[type];
	m->m_type = type;
	crit_exit();
}

/*
 * When MGET fails, ask protocols to free space when short of memory,
 * then re-attempt to allocate an mbuf.
 */
struct mbuf *
m_retry(int how, int t)
{
	struct mbuf *m;

	/*
	 * Must only do the reclaim if not in an interrupt context.
	 */
	if (how == MB_WAIT) {
		KASSERT(mycpu->gd_intr_nesting_level == 0,
		    ("MBALLOC: CANNOT WAIT IN INTERRUPT"));
		m_reclaim();
	}

	/*
	 * Try to pull a new mbuf out of the cache, if the cache is empty
	 * try to allocate a new one and if that doesn't work we give up.
	 */
	crit_enter();
	if ((m = mmbfree) == NULL) {
		m_mballoc(1, how);
		if ((m = mmbfree) == NULL) {
			static int last_report;

			mbstat.m_drops++;
			crit_exit();
			if (ticks < last_report || 
			    (ticks - last_report) >= hz) {
				last_report = ticks;
				printf("All mbufs exhausted, please see tuning(7).\n");
			}
			return (NULL);
		}
	}

	/*
	 * Cache case, adjust globals before leaving the critical section
	 */
	mmbfree = m->m_next;
	mbtypes[MT_FREE]--;
	mbtypes[t]++;
	mbstat.m_wait++;
	crit_exit();

	m->m_type = t;
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_dat;
	m->m_flags = 0;
	return (m);
}

/*
 * As above; retry an MGETHDR.
 */
struct mbuf *
m_retryhdr(int how, int t)
{
	struct mbuf *m;

	/*
	 * Must only do the reclaim if not in an interrupt context.
	 */
	if (how == MB_WAIT) {
		KASSERT(mycpu->gd_intr_nesting_level == 0,
		    ("MBALLOC: CANNOT WAIT IN INTERRUPT"));
		m_reclaim();
	}

	/*
	 * Try to pull a new mbuf out of the cache, if the cache is empty
	 * try to allocate a new one and if that doesn't work we give up.
	 */
	crit_enter();
	if ((m = mmbfree) == NULL) {
		m_mballoc(1, how);
		if ((m = mmbfree) == NULL) {
			static int last_report;

			mbstat.m_drops++;
			crit_exit();
			if (ticks < last_report || 
			    (ticks - last_report) >= hz) {
				last_report = ticks;
				printf("All mbufs exhausted, please see tuning(7).\n");
			}
			return (NULL);
		}
	}

	/*
	 * Cache case, adjust globals before leaving the critical section
	 */
	mmbfree = m->m_next;
	mbtypes[MT_FREE]--;
	mbtypes[t]++;
	mbstat.m_wait++;
	crit_exit();

	m->m_type = t;
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_pktdat;
	m->m_flags = M_PKTHDR;
	m->m_pkthdr.rcvif = NULL;
	SLIST_INIT(&m->m_pkthdr.tags);
	m->m_pkthdr.csum_flags = 0;
	return (m);
}

static void
m_reclaim(void)
{
	struct domain *dp;
	struct protosw *pr;

	crit_enter();
	SLIST_FOREACH(dp, &domains, dom_next) {
		for (pr = dp->dom_protosw; pr < dp->dom_protoswNPROTOSW; pr++) {
			if (pr->pr_drain)
				(*pr->pr_drain)();
		}
	}
	crit_exit();
	mbstat.m_drain++;
}

/*
 * Allocate an mbuf.  If no mbufs are immediately available try to
 * bring a bunch more into our cache (mmbfree list).  A critical
 * section is required to protect the mmbfree list and counters
 * against interrupts.
 */
struct mbuf *
m_get(int how, int type)
{
	struct mbuf *m;

	/*
	 * Try to pull a new mbuf out of the cache, if the cache is empty
	 * try to allocate a new one and if that doesn't work try even harder
	 * by calling m_retryhdr().
	 */
	crit_enter();
	if ((m = mmbfree) == NULL) {
		m_mballoc(1, how);
		if ((m = mmbfree) == NULL) {
			crit_exit();
			m = m_retry(how, type);
			if (m == NULL && how == MB_WAIT)
				m = m_mballoc_wait(MGET_C, type);
			return (m);
		}
	}

	/*
	 * Cache case, adjust globals before leaving the critical section
	 */
	mmbfree = m->m_next;
	mbtypes[MT_FREE]--;
	mbtypes[type]++;
	crit_exit();

	m->m_type = type;
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_dat;
	m->m_flags = 0;
	return (m);
}

struct mbuf *
m_gethdr(int how, int type)
{
	struct mbuf *m;

	/*
	 * Try to pull a new mbuf out of the cache, if the cache is empty
	 * try to allocate a new one and if that doesn't work try even harder
	 * by calling m_retryhdr().
	 */
	crit_enter();
	if ((m = mmbfree) == NULL) {
		m_mballoc(1, how);
		if ((m = mmbfree) == NULL) {
			crit_exit();
			m = m_retryhdr(how, type);
			if (m == NULL && how == MB_WAIT)
				m = m_mballoc_wait(MGETHDR_C, type);
			return(m);
		}
	}

	/*
	 * Cache case, adjust globals before leaving the critical section
	 */
	mmbfree = m->m_next;
	mbtypes[MT_FREE]--;
	mbtypes[type]++;
	crit_exit();

	m->m_type = type;
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_pktdat;
	m->m_flags = M_PKTHDR;
	m->m_pkthdr.rcvif = NULL;
	SLIST_INIT(&m->m_pkthdr.tags);
	m->m_pkthdr.csum_flags = 0;
	m->m_pkthdr.fw_flags = 0;
	return (m);
}

struct mbuf *
m_getclr(int how, int type)
{
	struct mbuf *m;

	if ((m = m_get(how, type)) != NULL) {
		bzero(mtod(m, caddr_t), MLEN);
	}
	return (m);
}

/*
 * m_getcl() returns an mbuf with an attached cluster.
 * Because many network drivers use this kind of buffers a lot, it is
 * convenient to keep a small pool of free buffers of this kind.
 * Even a small size such as 10 gives about 10% improvement in the
 * forwarding rate in a bridge or router.
 * The size of this free list is controlled by the sysctl variable
 * mcl_pool_max. The list is populated on m_freem(), and used in
 * m_getcl() if elements are available.
 */
struct mbuf *
m_getcl(int how, short type, int flags)
{
	struct mbuf *mp;

	crit_enter();
	if (flags & M_PKTHDR) {
		if (type == MT_DATA && mcl_pool) {
			mp = mcl_pool;
			mcl_pool = mp->m_nextpkt;
			--mcl_pool_count;
			crit_exit();
			mp->m_nextpkt = NULL;
			mp->m_data = mp->m_ext.ext_buf;
			mp->m_flags = M_PKTHDR|M_EXT|M_EXT_CLUSTER;
			mp->m_pkthdr.rcvif = NULL;
			mp->m_pkthdr.csum_flags = 0;
			return mp;
		}
		MGETHDR(mp, how, type);
	} else {
		MGET(mp, how, type);
	}
	if (mp) {
		m_mclget(mp, how);
		if ((mp->m_flags & M_EXT) == 0) {
			m_free(mp);
			mp = NULL;
		}
	}
	crit_exit();
	return (mp);
}

/*
 * Allocate chain of requested length.
 */
struct mbuf *
m_getc(int len, int how, int type)
{
	struct mbuf *n, *nfirst = NULL, **ntail = &nfirst;
	int nsize;

	while (len > 0) {
		n = m_getl(len, how, type, 0, &nsize);
		if (n == NULL)
			goto failed;
		n->m_len = 0;
		*ntail = n;
		ntail = &n->m_next;
		len -= nsize;
	}
	return (nfirst);

failed:
	m_freem(nfirst);
	return (NULL);
}

/*
 * Allocate len-worth of mbufs and/or mbuf clusters (whatever fits best)
 * and return a pointer to the head of the allocated chain. If m0 is
 * non-null, then we assume that it is a single mbuf or an mbuf chain to
 * which we want len bytes worth of mbufs and/or clusters attached, and so
 * if we succeed in allocating it, we will just return a pointer to m0.
 *
 * If we happen to fail at any point during the allocation, we will free
 * up everything we have already allocated and return NULL.
 *
 * Deprecated.  Use m_getc() and m_cat() instead.
 */
struct mbuf *
m_getm(struct mbuf *m0, int len, int how, int type)
{
	struct mbuf *nfirst;

	nfirst = m_getc(len, how, type);

	if (m0 != NULL) {
		m_last(m0)->m_next = nfirst;
		return (m0);
	}

	return (nfirst);
}

/*
 *  m_mclget() - Adds a cluster to a normal mbuf, M_EXT is set on success.
 */
void
m_mclget(struct mbuf *m, int how)
{
	mbcluster_t mcl;

	KKASSERT((m->m_flags & M_EXT_OLD) == 0);

	/*
	 * Allocate a cluster, return if we can't get one.
	 */
	crit_enter();
	if ((mcl = mclfree) == NULL) {
		m_clalloc(1, how);
		if ((mcl = mclfree) == NULL) {
			if (how == MB_WAIT) {
				m_clalloc_wait();
				mcl = mclfree;
			}
			if (mcl == NULL) {
				crit_exit();
				return;
			}
		}
	}

	/*
	 * We have a cluster, unlink it from the free list and set the ref
	 * count.
	 */
	KKASSERT(mcl->mcl_refs == 0);
	mclfree = mcl->mcl_next;
	mcl->mcl_refs = 1;
	--mbstat.m_clfree;
	crit_exit();

	/*
	 * Add the cluster to the mbuf.  The caller will detect that the
	 * mbuf now has an attached cluster.
	 */
	m->m_ext.ext_arg = mcl;
	m->m_ext.ext_buf = mcl->mcl_data;
	m->m_ext.ext_nref.new = m_mclref;
	m->m_ext.ext_nfree.new = m_mclfree;
	m->m_ext.ext_size = MCLBYTES;

	m->m_data = m->m_ext.ext_buf;
	m->m_flags |= M_EXT | M_EXT_CLUSTER;
}

static void
m_mclfree(void *arg)
{
	mbcluster_t mcl = arg;

	KKASSERT(mcl->mcl_magic == MCL_MAGIC);
	KKASSERT(mcl->mcl_refs > 0);
	crit_enter();
	if (--mcl->mcl_refs == 0) {
		if (mbstat.m_clfree < mcl_free_max) {
			mcl->mcl_next = mclfree;
			mclfree = mcl;
			++mbstat.m_clfree;
			MCLWAKEUP();
		} else {
			mcl->mcl_magic = -1;
			free(mcl->mcl_data, M_MBUFCL);
			free(mcl, M_MBUFCL);
			--mbstat.m_clusters;
		}
	}
	crit_exit();
}

static void
m_mclref(void *arg)
{
	mbcluster_t mcl = arg;

	KKASSERT(mcl->mcl_magic == MCL_MAGIC);
	crit_enter();
	++mcl->mcl_refs;
	crit_exit();
}

/*
 * Helper routines for M_EXT reference/free
 */
static __inline void
m_extref(const struct mbuf *m)
{
	KKASSERT(m->m_ext.ext_nfree.any != NULL);
	crit_enter();
	if (m->m_flags & M_EXT_OLD)
		m->m_ext.ext_nref.old(m->m_ext.ext_buf, m->m_ext.ext_size);
	else
		m->m_ext.ext_nref.new(m->m_ext.ext_arg); 
	crit_exit();
}

/*
 * m_free()
 *
 * Free a single mbuf and any associated external storage.  The successor,
 * if any, is returned.
 *
 * We do need to check non-first mbuf for m_aux, since some of existing
 * code does not call M_PREPEND properly.
 * (example: call to bpf_mtap from drivers)
 */
struct mbuf *
m_free(struct mbuf *m)
{
	struct mbuf *n;

	crit_enter();
	KASSERT(m->m_type != MT_FREE, ("freeing free mbuf %p", m));

	/*
	 * Adjust our type count and delete any attached chains if the
	 * mbuf is a packet header.
	 */
	if ((m->m_flags & M_PKTHDR) != 0)
		m_tag_delete_chain(m, NULL);

	/*
	 * Place the mbuf on the appropriate free list.  Try to maintain a
	 * small cache of mbuf+cluster pairs.
	 */
	n = m->m_next;
	m->m_next = NULL;
	if (m->m_flags & M_EXT) {
		KKASSERT(m->m_ext.ext_nfree.any != NULL);
		if (mcl_pool_count < mcl_pool_max && m && m->m_next == NULL &&
		    (m->m_flags & (M_PKTHDR|M_EXT_CLUSTER)) == (M_PKTHDR|M_EXT_CLUSTER) &&
		    m->m_type == MT_DATA && M_EXT_WRITABLE(m) ) {
			KKASSERT(((mbcluster_t)m->m_ext.ext_arg)->mcl_magic == MCL_MAGIC);
			m->m_nextpkt = mcl_pool;
			mcl_pool = m;
			++mcl_pool_count;
			m = NULL;
		} else {
			if (m->m_flags & M_EXT_OLD)
				m->m_ext.ext_nfree.old(m->m_ext.ext_buf, m->m_ext.ext_size);
			else
				m->m_ext.ext_nfree.new(m->m_ext.ext_arg);
			m->m_flags = 0;
			m->m_ext.ext_arg = NULL;
			m->m_ext.ext_nref.new = NULL;
			m->m_ext.ext_nfree.new = NULL;
		}
	}
	if (m) {
		--mbtypes[m->m_type];
		if (mbtypes[MT_FREE] < mbuf_free_max) {
			m->m_type = MT_FREE;
			mbtypes[MT_FREE]++;
			m->m_next = mmbfree;
			mmbfree = m;
			MMBWAKEUP();
		} else {
			free(m, M_MBUF);
			--mbstat.m_mbufs;
		}
	}
	crit_exit();
	return (n);
}

void
m_freem(struct mbuf *m)
{
	crit_enter();
	while (m)
		m = m_free(m);
	crit_exit();
}

/*
 * mbuf utility routines
 */

/*
 * Lesser-used path for M_PREPEND:
 * allocate new mbuf to prepend to chain,
 * copy junk along.
 */
struct mbuf *
m_prepend(struct mbuf *m, int len, int how)
{
	struct mbuf *mn;

	MGET(mn, how, m->m_type);
	if (mn == (struct mbuf *)NULL) {
		m_freem(m);
		return ((struct mbuf *)NULL);
	}
	if (m->m_flags & M_PKTHDR)
		M_MOVE_PKTHDR(mn, m);
	mn->m_next = m;
	m = mn;
	if (len < MHLEN)
		MH_ALIGN(m, len);
	m->m_len = len;
	return (m);
}

/*
 * Make a copy of an mbuf chain starting "off0" bytes from the beginning,
 * continuing for "len" bytes.  If len is M_COPYALL, copy to end of mbuf.
 * The wait parameter is a choice of MB_WAIT/MB_DONTWAIT from caller.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 */
#define MCFail (mbstat.m_mcfail)

struct mbuf *
m_copym(const struct mbuf *m, int off0, int len, int wait)
{
	struct mbuf *n, **np;
	int off = off0;
	struct mbuf *top;
	int copyhdr = 0;

	KASSERT(off >= 0, ("m_copym, negative off %d", off));
	KASSERT(len >= 0, ("m_copym, negative len %d", len));
	if (off == 0 && m->m_flags & M_PKTHDR)
		copyhdr = 1;
	while (off > 0) {
		KASSERT(m != NULL, ("m_copym, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	np = &top;
	top = 0;
	while (len > 0) {
		if (m == 0) {
			KASSERT(len == M_COPYALL, 
			    ("m_copym, length > size of mbuf chain"));
			break;
		}
		MGET(n, wait, m->m_type);
		*np = n;
		if (n == 0)
			goto nospace;
		if (copyhdr) {
			if (!m_dup_pkthdr(n, m, wait))
				goto nospace;
			if (len == M_COPYALL)
				n->m_pkthdr.len -= off0;
			else
				n->m_pkthdr.len = len;
			copyhdr = 0;
		}
		n->m_len = min(len, m->m_len - off);
		if (m->m_flags & M_EXT) {
			n->m_data = m->m_data + off;
			m_extref(m);
			n->m_ext = m->m_ext;
			n->m_flags |= m->m_flags & 
					(M_EXT | M_EXT_OLD | M_EXT_CLUSTER);
		} else {
			bcopy(mtod(m, caddr_t)+off, mtod(n, caddr_t),
			    (unsigned)n->m_len);
		}
		if (len != M_COPYALL)
			len -= n->m_len;
		off = 0;
		m = m->m_next;
		np = &n->m_next;
	}
	if (top == 0)
		MCFail++;
	return (top);
nospace:
	m_freem(top);
	MCFail++;
	return (0);
}

/*
 * Copy an entire packet, including header (which must be present).
 * An optimization of the common case `m_copym(m, 0, M_COPYALL, how)'.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 * Preserve alignment of the first mbuf so if the creator has left
 * some room at the beginning (e.g. for inserting protocol headers)
 * the copies also have the room available.
 */
struct mbuf *
m_copypacket(struct mbuf *m, int how)
{
	struct mbuf *top, *n, *o;

	MGET(n, how, m->m_type);
	top = n;
	if (!n)
		goto nospace;

	if (!m_dup_pkthdr(n, m, how))
		goto nospace;
	n->m_len = m->m_len;
	if (m->m_flags & M_EXT) {
		n->m_data = m->m_data;
		m_extref(m);
		n->m_ext = m->m_ext;
		n->m_flags |= m->m_flags & (M_EXT | M_EXT_OLD | M_EXT_CLUSTER);
	} else {
		n->m_data = n->m_pktdat + (m->m_data - m->m_pktdat );
		bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
	}

	m = m->m_next;
	while (m) {
		MGET(o, how, m->m_type);
		if (!o)
			goto nospace;

		n->m_next = o;
		n = n->m_next;

		n->m_len = m->m_len;
		if (m->m_flags & M_EXT) {
			n->m_data = m->m_data;
			m_extref(m);
			n->m_ext = m->m_ext;
			n->m_flags |= m->m_flags &
					 (M_EXT | M_EXT_OLD | M_EXT_CLUSTER);
		} else {
			bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
		}

		m = m->m_next;
	}
	return top;
nospace:
	m_freem(top);
	MCFail++;
	return 0;
}

/*
 * Copy data from an mbuf chain starting "off" bytes from the beginning,
 * continuing for "len" bytes, into the indicated buffer.
 */
void
m_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	unsigned count;

	KASSERT(off >= 0, ("m_copydata, negative off %d", off));
	KASSERT(len >= 0, ("m_copydata, negative len %d", len));
	while (off > 0) {
		KASSERT(m != NULL, ("m_copydata, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		KASSERT(m != NULL, ("m_copydata, length > size of mbuf chain"));
		count = min(m->m_len - off, len);
		bcopy(mtod(m, caddr_t) + off, cp, count);
		len -= count;
		cp += count;
		off = 0;
		m = m->m_next;
	}
}

/*
 * Copy a packet header mbuf chain into a completely new chain, including
 * copying any mbuf clusters.  Use this instead of m_copypacket() when
 * you need a writable copy of an mbuf chain.
 */
struct mbuf *
m_dup(struct mbuf *m, int how)
{
	struct mbuf **p, *top = NULL;
	int remain, moff, nsize;

	/* Sanity check */
	if (m == NULL)
		return (NULL);
	KASSERT((m->m_flags & M_PKTHDR) != 0, ("%s: !PKTHDR", __func__));

	/* While there's more data, get a new mbuf, tack it on, and fill it */
	remain = m->m_pkthdr.len;
	moff = 0;
	p = &top;
	while (remain > 0 || top == NULL) {	/* allow m->m_pkthdr.len == 0 */
		struct mbuf *n;

		/* Get the next new mbuf */
		n = m_getl(remain, how, m->m_type, top == NULL ? M_PKTHDR : 0,
			   &nsize);
		if (n == NULL)
			goto nospace;
		if (top == NULL)
			if (!m_dup_pkthdr(n, m, how))
				goto nospace0;

		/* Link it into the new chain */
		*p = n;
		p = &n->m_next;

		/* Copy data from original mbuf(s) into new mbuf */
		n->m_len = 0;
		while (n->m_len < nsize && m != NULL) {
			int chunk = min(nsize - n->m_len, m->m_len - moff);

			bcopy(m->m_data + moff, n->m_data + n->m_len, chunk);
			moff += chunk;
			n->m_len += chunk;
			remain -= chunk;
			if (moff == m->m_len) {
				m = m->m_next;
				moff = 0;
			}
		}

		/* Check correct total mbuf length */
		KASSERT((remain > 0 && m != NULL) || (remain == 0 && m == NULL),
			("%s: bogus m_pkthdr.len", __func__));
	}
	return (top);

nospace:
	m_freem(top);
nospace0:
	mbstat.m_mcfail++;
	return (NULL);
}

/*
 * Concatenate mbuf chain n to m.
 * Both chains must be of the same type (e.g. MT_DATA).
 * Any m_pkthdr is not updated.
 */
void
m_cat(struct mbuf *m, struct mbuf *n)
{
	m = m_last(m);
	while (n) {
		if (m->m_flags & M_EXT ||
		    m->m_data + m->m_len + n->m_len >= &m->m_dat[MLEN]) {
			/* just join the two chains */
			m->m_next = n;
			return;
		}
		/* splat the data from one into the other */
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		    (u_int)n->m_len);
		m->m_len += n->m_len;
		n = m_free(n);
	}
}

void
m_adj(struct mbuf *mp, int req_len)
{
	int len = req_len;
	struct mbuf *m;
	int count;

	if ((m = mp) == NULL)
		return;
	if (len >= 0) {
		/*
		 * Trim from head.
		 */
		while (m != NULL && len > 0) {
			if (m->m_len <= len) {
				len -= m->m_len;
				m->m_len = 0;
				m = m->m_next;
			} else {
				m->m_len -= len;
				m->m_data += len;
				len = 0;
			}
		}
		m = mp;
		if (mp->m_flags & M_PKTHDR)
			m->m_pkthdr.len -= (req_len - len);
	} else {
		/*
		 * Trim from tail.  Scan the mbuf chain,
		 * calculating its length and finding the last mbuf.
		 * If the adjustment only affects this mbuf, then just
		 * adjust and return.  Otherwise, rescan and truncate
		 * after the remaining size.
		 */
		len = -len;
		count = 0;
		for (;;) {
			count += m->m_len;
			if (m->m_next == (struct mbuf *)0)
				break;
			m = m->m_next;
		}
		if (m->m_len >= len) {
			m->m_len -= len;
			if (mp->m_flags & M_PKTHDR)
				mp->m_pkthdr.len -= len;
			return;
		}
		count -= len;
		if (count < 0)
			count = 0;
		/*
		 * Correct length for chain is "count".
		 * Find the mbuf with last data, adjust its length,
		 * and toss data from remaining mbufs on chain.
		 */
		m = mp;
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len = count;
		for (; m; m = m->m_next) {
			if (m->m_len >= count) {
				m->m_len = count;
				break;
			}
			count -= m->m_len;
		}
		while (m->m_next)
			(m = m->m_next) ->m_len = 0;
	}
}

/*
 * Rearange an mbuf chain so that len bytes are contiguous
 * and in the data area of an mbuf (so that mtod will work for a structure
 * of size len).  Returns the resulting mbuf chain on success, frees it and
 * returns null on failure.  If there is room, it will add up to
 * max_protohdr-len extra bytes to the contiguous region in an attempt to
 * avoid being called next time.
 */
#define MPFail (mbstat.m_mpfail)

struct mbuf *
m_pullup(struct mbuf *n, int len)
{
	struct mbuf *m;
	int count;
	int space;

	/*
	 * If first mbuf has no cluster, and has room for len bytes
	 * without shifting current data, pullup into it,
	 * otherwise allocate a new mbuf to prepend to the chain.
	 */
	if ((n->m_flags & M_EXT) == 0 &&
	    n->m_data + len < &n->m_dat[MLEN] && n->m_next) {
		if (n->m_len >= len)
			return (n);
		m = n;
		n = n->m_next;
		len -= m->m_len;
	} else {
		if (len > MHLEN)
			goto bad;
		MGET(m, MB_DONTWAIT, n->m_type);
		if (m == 0)
			goto bad;
		m->m_len = 0;
		if (n->m_flags & M_PKTHDR)
			M_MOVE_PKTHDR(m, n);
	}
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		  (unsigned)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
bad:
	m_freem(n);
	MPFail++;
	return (0);
}

/*
 * Partition an mbuf chain in two pieces, returning the tail --
 * all but the first len0 bytes.  In case of failure, it returns NULL and
 * attempts to restore the chain to its original state.
 *
 * Note that the resulting mbufs might be read-only, because the new
 * mbuf can end up sharing an mbuf cluster with the original mbuf if
 * the "breaking point" happens to lie within a cluster mbuf. Use the
 * M_WRITABLE() macro to check for this case.
 */
struct mbuf *
m_split(struct mbuf *m0, int len0, int wait)
{
	struct mbuf *m, *n;
	unsigned len = len0, remain;

	for (m = m0; m && len > m->m_len; m = m->m_next)
		len -= m->m_len;
	if (m == 0)
		return (0);
	remain = m->m_len - len;
	if (m0->m_flags & M_PKTHDR) {
		MGETHDR(n, wait, m0->m_type);
		if (n == 0)
			return (0);
		n->m_pkthdr.rcvif = m0->m_pkthdr.rcvif;
		n->m_pkthdr.len = m0->m_pkthdr.len - len0;
		m0->m_pkthdr.len = len0;
		if (m->m_flags & M_EXT)
			goto extpacket;
		if (remain > MHLEN) {
			/* m can't be the lead packet */
			MH_ALIGN(n, 0);
			n->m_next = m_split(m, len, wait);
			if (n->m_next == 0) {
				(void) m_free(n);
				return (0);
			} else {
				n->m_len = 0;
				return (n);
			}
		} else
			MH_ALIGN(n, remain);
	} else if (remain == 0) {
		n = m->m_next;
		m->m_next = 0;
		return (n);
	} else {
		MGET(n, wait, m->m_type);
		if (n == 0)
			return (0);
		M_ALIGN(n, remain);
	}
extpacket:
	if (m->m_flags & M_EXT) {
		n->m_data = m->m_data + len;
		m_extref(m);
		n->m_ext = m->m_ext;
		n->m_flags |= m->m_flags & (M_EXT | M_EXT_OLD | M_EXT_CLUSTER);
	} else {
		bcopy(mtod(m, caddr_t) + len, mtod(n, caddr_t), remain);
	}
	n->m_len = remain;
	m->m_len = len;
	n->m_next = m->m_next;
	m->m_next = 0;
	return (n);
}

/*
 * Routine to copy from device local memory into mbufs.
 * Note: "offset" is ill-defined and always called as 0, so ignore it.
 */
struct mbuf *
m_devget(char *buf, int len, int offset, struct ifnet *ifp,
    void (*copy)(volatile const void *from, volatile void *to, size_t length))
{
	struct mbuf *m, *mfirst = NULL, **mtail;
	int nsize, flags;

	if (copy == NULL)
		copy = bcopy;
	mtail = &mfirst;
	flags = M_PKTHDR;

	while (len > 0) {
		m = m_getl(len, MB_DONTWAIT, MT_DATA, flags, &nsize);
		if (m == NULL) {
			m_freem(mfirst);
			return (NULL);
		}
		m->m_len = min(len, nsize);

		if (flags & M_PKTHDR) {
			if (len + max_linkhdr <= nsize)
				m->m_data += max_linkhdr;
			m->m_pkthdr.rcvif = ifp;
			m->m_pkthdr.len = len;
			flags = 0;
		}

		copy(buf, m->m_data, (unsigned)m->m_len);
		buf += m->m_len;
		len -= m->m_len;
		*mtail = m;
		mtail = &m->m_next;
	}

	return (mfirst);
}

/*
 * Copy data from a buffer back into the indicated mbuf chain,
 * starting "off" bytes from the beginning, extending the mbuf
 * chain if necessary.
 */
void
m_copyback(struct mbuf *m0, int off, int len, caddr_t cp)
{
	int mlen;
	struct mbuf *m = m0, *n;
	int totlen = 0;

	if (m0 == 0)
		return;
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == 0) {
			n = m_getclr(MB_DONTWAIT, m->m_type);
			if (n == 0)
				goto out;
			n->m_len = min(MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	while (len > 0) {
		mlen = min (m->m_len - off, len);
		bcopy(cp, off + mtod(m, caddr_t), (unsigned)mlen);
		cp += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == 0) {
			n = m_get(MB_DONTWAIT, m->m_type);
			if (n == 0)
				break;
			n->m_len = min(MLEN, len);
			m->m_next = n;
		}
		m = m->m_next;
	}
out:	if (((m = m0)->m_flags & M_PKTHDR) && (m->m_pkthdr.len < totlen))
		m->m_pkthdr.len = totlen;
}

void
m_print(const struct mbuf *m)
{
	int len;
	const struct mbuf *m2;

	len = m->m_pkthdr.len;
	m2 = m;
	while (len) {
		printf("%p %*D\n", m2, m2->m_len, (u_char *)m2->m_data, "-");
		len -= m2->m_len;
		m2 = m2->m_next;
	}
	return;
}

/*
 * "Move" mbuf pkthdr from "from" to "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 */
void
m_move_pkthdr(struct mbuf *to, struct mbuf *from)
{
	KASSERT((to->m_flags & M_EXT) == 0, ("m_move_pkthdr: to has cluster"));

	to->m_flags = from->m_flags & M_COPYFLAGS;
	to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;		/* especially tags */
	SLIST_INIT(&from->m_pkthdr.tags);	/* purge tags from src */
	from->m_flags &= ~M_PKTHDR;
}

/*
 * Duplicate "from"'s mbuf pkthdr in "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 * In particular, this does a deep copy of the packet tags.
 */
int
m_dup_pkthdr(struct mbuf *to, const struct mbuf *from, int how)
{
	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;
	SLIST_INIT(&to->m_pkthdr.tags);
	return (m_tag_copy_chain(to, from, how));
}

/*
 * Defragment a mbuf chain, returning the shortest possible
 * chain of mbufs and clusters.  If allocation fails and
 * this cannot be completed, NULL will be returned, but
 * the passed in chain will be unchanged.  Upon success,
 * the original chain will be freed, and the new chain
 * will be returned.
 *
 * If a non-packet header is passed in, the original
 * mbuf (chain?) will be returned unharmed.
 *
 * m_defrag_nofree doesn't free the passed in mbuf.
 */
struct mbuf *
m_defrag(struct mbuf *m0, int how)
{
	struct mbuf *m_new;

	if ((m_new = m_defrag_nofree(m0, how)) == NULL)
		return (NULL);
	if (m_new != m0)
		m_freem(m0);
	return (m_new);
}

struct mbuf *
m_defrag_nofree(struct mbuf *m0, int how)
{
	struct mbuf	*m_new = NULL, *m_final = NULL;
	int		progress = 0, length, nsize;

	if (!(m0->m_flags & M_PKTHDR))
		return (m0);

#ifdef MBUF_STRESS_TEST
	if (m_defragrandomfailures) {
		int temp = arc4random() & 0xff;
		if (temp == 0xba)
			goto nospace;
	}
#endif
	
	m_final = m_getl(m0->m_pkthdr.len, how, MT_DATA, M_PKTHDR, &nsize);
	if (m_final == NULL)
		goto nospace;
	m_final->m_len = 0;	/* in case m0->m_pkthdr.len is zero */

	if (m_dup_pkthdr(m_final, m0, how) == NULL)
		goto nospace;

	m_new = m_final;

	while (progress < m0->m_pkthdr.len) {
		length = m0->m_pkthdr.len - progress;
		if (length > MCLBYTES)
			length = MCLBYTES;

		if (m_new == NULL) {
			m_new = m_getl(length, how, MT_DATA, 0, &nsize);
			if (m_new == NULL)
				goto nospace;
		}

		m_copydata(m0, progress, length, mtod(m_new, caddr_t));
		progress += length;
		m_new->m_len = length;
		if (m_new != m_final)
			m_cat(m_final, m_new);
		m_new = NULL;
	}
	if (m0->m_next == NULL)
		m_defraguseless++;
	m_defragpackets++;
	m_defragbytes += m_final->m_pkthdr.len;
	return (m_final);
nospace:
	m_defragfailure++;
	if (m_new)
		m_free(m_new);
	m_freem(m_final);
	return (NULL);
}

/*
 * Move data from uio into mbufs.
 */
struct mbuf *
m_uiomove(struct uio *uio, int wait, int len0)
{
	struct mbuf *head;		/* result mbuf chain */
	struct mbuf *m;			/* current working mbuf */
	struct mbuf **mp;
	int resid, nsize, flags = M_PKTHDR, error;

	resid = min(len0, uio->uio_resid);

	head = NULL;
	mp = &head;
	do {
		m = m_getl(resid, wait, MT_DATA, flags, &nsize);
		if (m == NULL)
			goto failed;
		if (flags) {
			m->m_pkthdr.len = 0;
			/* Leave room for protocol headers. */
			if (resid < MHLEN)
				MH_ALIGN(m, resid);
			flags = 0;
		}
		m->m_len = min(nsize, resid);
		error = uiomove(mtod(m, caddr_t), m->m_len, uio);
		if (error) {
			m_free(m);
			goto failed;
		}
		*mp = m;
		mp = &m->m_next;
		head->m_pkthdr.len += m->m_len;
		resid -= m->m_len;
	} while (resid > 0);

	return (head);

failed:
	m_freem(head);
	return (NULL);
}

struct mbuf *
m_last(struct mbuf *m)
{
	while (m->m_next)
		m = m->m_next;
	return (m);
}

/*
 * Return the number of bytes in an mbuf chain.
 * If lastm is not NULL, also return the last mbuf.
 */
u_int
m_lengthm(struct mbuf *m, struct mbuf **lastm)
{
	u_int len = 0;
	struct mbuf *prev = m;

	while (m) {
		len += m->m_len;
		prev = m;
		m = m->m_next;
	}
	if (lastm != NULL)
		*lastm = prev;
	return (len);
}

/*
 * Like m_lengthm(), except also keep track of mbuf usage.
 */
u_int
m_countm(struct mbuf *m, struct mbuf **lastm, u_int *pmbcnt)
{
	u_int len = 0, mbcnt = 0;
	struct mbuf *prev = m;

	while (m) {
		len += m->m_len;
		mbcnt += MSIZE;
		if (m->m_flags & M_EXT)
			mbcnt += m->m_ext.ext_size;
		prev = m;
		m = m->m_next;
	}
	if (lastm != NULL)
		*lastm = prev;
	*pmbcnt = mbcnt;
	return (len);
}
