/*
 * Copyright (c) 2003, 2004 Matthew Dillon. All rights reserved.
 * Copyright (c) 2003, 2004 Jeffrey M. Hsu.  All rights reserved.
 * Copyright (c) 2003 Jonathan Lemon.  All rights reserved.
 * Copyright (c) 2003, 2004 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Jonathan Lemon, Jeffrey M. Hsu, and Matthew Dillon.
 *
 * Jonathan Lemon gave Jeffrey Hsu permission to combine his copyright
 * into this one around July 8 2004.
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
 *
 * $DragonFly: src/sys/net/netisr.c,v 1.34 2007/05/31 11:00:25 sephe Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/msgport.h>
#include <sys/proc.h>
#include <sys/interrupt.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/netisr.h>
#include <machine/cpufunc.h>

#include <sys/thread2.h>
#include <sys/msgport2.h>
#include <net/netmsg2.h>

static void netmsg_sync_func(struct netmsg *msg);

struct netmsg_port_registration {
    TAILQ_ENTRY(netmsg_port_registration) npr_entry;
    lwkt_port_t	npr_port;
};

static struct netisr netisrs[NETISR_MAX];
static TAILQ_HEAD(,netmsg_port_registration) netreglist;

/* Per-CPU thread to handle any protocol.  */
struct thread netisr_cpu[MAXCPU];
lwkt_port netisr_afree_rport;
lwkt_port netisr_adone_rport;
lwkt_port netisr_apanic_rport;
lwkt_port netisr_sync_port;

static int (*netmsg_fwd_port_fn)(lwkt_port_t, lwkt_msg_t);

/*
 * netisr_afree_rport replymsg function, only used to handle async
 * messages which the sender has abandoned to their fate.
 */
static void
netisr_autofree_reply(lwkt_port_t port, lwkt_msg_t msg)
{
    kfree(msg, M_LWKTMSG);
}

/*
 * We need a custom putport function to handle the case where the
 * message target is the current thread's message port.  This case
 * can occur when the TCP or UDP stack does a direct callback to NFS and NFS
 * then turns around and executes a network operation synchronously.
 *
 * To prevent deadlocking, we must execute these self-referential messages
 * synchronously, effectively turning the message into a glorified direct
 * procedure call back into the protocol stack.  The operation must be
 * complete on return or we will deadlock, so panic if it isn't.
 */
static int
netmsg_put_port(lwkt_port_t port, lwkt_msg_t lmsg)
{
    netmsg_t netmsg = (void *)lmsg;

    if ((lmsg->ms_flags & MSGF_SYNC) && port == &curthread->td_msgport) {
	netmsg->nm_dispatch(netmsg);
	if ((lmsg->ms_flags & MSGF_DONE) == 0)
	    panic("netmsg_put_port: self-referential deadlock on netport");
	return(EASYNC);
    } else {
	return(netmsg_fwd_port_fn(port, lmsg));
    }
}

/*
 * UNIX DOMAIN sockets still have to run their uipc functions synchronously,
 * because they depend on the user proc context for a number of things 
 * (like creds) which we have not yet incorporated into the message structure.
 *
 * However, we maintain or message/port abstraction.  Having a special 
 * synchronous port which runs the commands synchronously gives us the
 * ability to serialize operations in one place later on when we start
 * removing the BGL.
 *
 * We clear MSGF_DONE prior to executing the message in order to close
 * any potential replymsg races with the flags field.  If a synchronous
 * result code is returned we set MSGF_DONE again.  MSGF_DONE's flag state
 * must be correct or the caller will be confused.
 */
static int
netmsg_sync_putport(lwkt_port_t port, lwkt_msg_t lmsg)
{
    netmsg_t netmsg = (void *)lmsg;
    int error;

    lmsg->ms_flags &= ~MSGF_DONE;
    lmsg->ms_target_port = port;	/* required for abort */
    netmsg->nm_dispatch(netmsg);
    error = lwkt_waitmsg(lmsg, 0);
    return(error);
}

static void
netisr_init(void)
{
    int i;

    TAILQ_INIT(&netreglist);

    /*
     * Create default per-cpu threads for generic protocol handling.
     */
    for (i = 0; i < ncpus; ++i) {
	lwkt_create(netmsg_service_loop, NULL, NULL, &netisr_cpu[i], 0, i,
		    "netisr_cpu %d", i);
	netmsg_service_port_init(&netisr_cpu[i].td_msgport);
    }

    /*
     * The netisr_afree_rport is a special reply port which automatically
     * frees the replied message.  The netisr_adone_rport simply marks
     * the message as being done.  The netisr_apanic_rport panics if
     * the message is replied to.
     */
    lwkt_initport_replyonly(&netisr_afree_rport, netisr_autofree_reply);
    lwkt_initport_replyonly_null(&netisr_adone_rport);
    lwkt_initport_panic(&netisr_apanic_rport);

    /*
     * The netisr_syncport is a special port which executes the message
     * synchronously and waits for it if EASYNC is returned.
     */
    lwkt_initport_putonly(&netisr_sync_port, netmsg_sync_putport);
}

SYSINIT(netisr, SI_SUB_PROTO_BEGIN, SI_ORDER_FIRST, netisr_init, NULL);

/*
 * Finish initializing the message port for a netmsg service.  This also
 * registers the port for synchronous cleanup operations such as when an
 * ifnet is being destroyed.  There is no deregistration API yet.
 */
void
netmsg_service_port_init(lwkt_port_t port)
{
    struct netmsg_port_registration *reg;

    /*
     * Override the putport function.  Our custom function checks for 
     * self-references and executes such commands synchronously.
     */
    if (netmsg_fwd_port_fn == NULL)
	netmsg_fwd_port_fn = port->mp_putport;
    KKASSERT(netmsg_fwd_port_fn == port->mp_putport);
    port->mp_putport = netmsg_put_port;

    /*
     * Keep track of ports using the netmsg API so we can synchronize
     * certain operations (such as freeing an ifnet structure) across all
     * consumers.
     */
    reg = kmalloc(sizeof(*reg), M_TEMP, M_WAITOK|M_ZERO);
    reg->npr_port = port;
    TAILQ_INSERT_TAIL(&netreglist, reg, npr_entry);
}

/*
 * This function synchronizes the caller with all netmsg services.  For
 * example, if an interface is being removed we must make sure that all
 * packets related to that interface complete processing before the structure
 * can actually be freed.  This sort of synchronization is an alternative to
 * ref-counting the netif, removing the ref counting overhead in favor of
 * placing additional overhead in the netif freeing sequence (where it is
 * inconsequential).
 */
void
netmsg_service_sync(void)
{
    struct netmsg_port_registration *reg;
    struct netmsg smsg;

    netmsg_init(&smsg, &curthread->td_msgport, 0, netmsg_sync_func);

    TAILQ_FOREACH(reg, &netreglist, npr_entry) {
	lwkt_domsg(reg->npr_port, &smsg.nm_lmsg, 0);
    }
}

/*
 * The netmsg function simply replies the message.  API semantics require
 * EASYNC to be returned if the netmsg function disposes of the message.
 */
static void
netmsg_sync_func(struct netmsg *msg)
{
    lwkt_replymsg(&msg->nm_lmsg, 0);
}

/*
 * Generic netmsg service loop.  Some protocols may roll their own but all
 * must do the basic command dispatch function call done here.
 */
void
netmsg_service_loop(void *arg)
{
    struct netmsg *msg;

    while ((msg = lwkt_waitport(&curthread->td_msgport, 0))) {
	msg->nm_dispatch(msg);
    }
}

/*
 * Call the netisr directly.
 * Queueing may be done in the msg port layer at its discretion.
 */
void
netisr_dispatch(int num, struct mbuf *m)
{
    /* just queue it for now XXX JH */
    netisr_queue(num, m);
}

/*
 * Same as netisr_dispatch(), but always queue.
 * This is either used in places where we are not confident that
 * direct dispatch is possible, or where queueing is required.
 */
int
netisr_queue(int num, struct mbuf *m)
{
    struct netisr *ni;
    struct netmsg_packet *pmsg;
    lwkt_port_t port;

    KASSERT((num > 0 && num <= (sizeof(netisrs)/sizeof(netisrs[0]))),
	("netisr_queue: bad isr %d", num));

    ni = &netisrs[num];
    if (ni->ni_handler == NULL) {
	kprintf("netisr_queue: unregistered isr %d\n", num);
	return (EIO);
    }

    if ((port = ni->ni_mport(&m)) == NULL)
	return (EIO);

    pmsg = &m->m_hdr.mh_netmsg;

    netmsg_init(&pmsg->nm_netmsg, &netisr_apanic_rport, 0, ni->ni_handler);
    pmsg->nm_packet = m;
    pmsg->nm_netmsg.nm_lmsg.u.ms_result = num;
    lwkt_sendmsg(port, &pmsg->nm_netmsg.nm_lmsg);
    return (0);
}

void
netisr_register(int num, lwkt_portfn_t mportfn, netisr_fn_t handler)
{
    KASSERT((num > 0 && num <= (sizeof(netisrs)/sizeof(netisrs[0]))),
	("netisr_register: bad isr %d", num));
    netmsg_init(&netisrs[num].ni_netmsg, &netisr_adone_rport, 0, NULL);
    netisrs[num].ni_mport = mportfn;
    netisrs[num].ni_handler = handler;
}

int
netisr_unregister(int num)
{
    KASSERT((num > 0 && num <= (sizeof(netisrs)/sizeof(netisrs[0]))),
	("unregister_netisr: bad isr number: %d\n", num));

    /* XXX JH */
    return (0);
}

/*
 * Return message port for default handler thread on CPU 0.
 */
lwkt_port_t
cpu0_portfn(struct mbuf **mptr)
{
    return (&netisr_cpu[0].td_msgport);
}

lwkt_port_t
cpu_portfn(int cpu)
{
    return (&netisr_cpu[cpu].td_msgport);
}

/* ARGSUSED */
lwkt_port_t
cpu0_soport(struct socket *so __unused, struct sockaddr *nam __unused,
	    int req __unused)
{
    return (&netisr_cpu[0].td_msgport);
}

lwkt_port_t
sync_soport(struct socket *so __unused, struct sockaddr *nam __unused,
	    int req __unused)
{
    return (&netisr_sync_port);
}

/*
 * schednetisr() is used to call the netisr handler from the appropriate
 * netisr thread for polling and other purposes.
 *
 * This function may be called from a hard interrupt or IPI and must be
 * MP SAFE and non-blocking.  We use a fixed per-cpu message instead of
 * trying to allocate one.  We must get ourselves onto the target cpu
 * to safely check the MSGF_DONE bit on the message but since the message
 * will be sent to that cpu anyway this does not add any extra work beyond
 * what lwkt_sendmsg() would have already had to do to schedule the target
 * thread.
 */
static void
schednetisr_remote(void *data)
{
    int num = (int)data;
    struct netisr *ni = &netisrs[num];
    lwkt_port_t port = &netisr_cpu[0].td_msgport;
    struct netmsg *pmsg;

    pmsg = &netisrs[num].ni_netmsg;
    crit_enter();
    if (pmsg->nm_lmsg.ms_flags & MSGF_DONE) {
	netmsg_init(pmsg, &netisr_adone_rport, 0, ni->ni_handler);
	pmsg->nm_lmsg.u.ms_result = num;
	lwkt_sendmsg(port, &pmsg->nm_lmsg);
    }
    crit_exit();
}

void
schednetisr(int num)
{
    KASSERT((num > 0 && num <= (sizeof(netisrs)/sizeof(netisrs[0]))),
	("schednetisr: bad isr %d", num));
#ifdef SMP
    if (mycpu->gd_cpuid != 0)
	lwkt_send_ipiq(globaldata_find(0), schednetisr_remote, (void *)num);
    else
	schednetisr_remote((void *)num);
#else
    schednetisr_remote((void *)num);
#endif
}

