/*
 * Copyright (c) 1980, 1986, 1993
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
 *	@(#)if.c	8.3 (Berkeley) 1/4/94
 * $FreeBSD: src/sys/net/if.c,v 1.185 2004/03/13 02:35:03 brooks Exp $
 * $DragonFly: src/sys/net/if.c,v 1.49 2006/12/22 23:44:54 swildner Exp $
 */

#include "opt_compat.h"
#include "opt_inet6.h"
#include "opt_inet.h"
#include "opt_polling.h"

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/socketops.h>
#include <sys/protosw.h>
#include <sys/kernel.h>
#include <sys/sockio.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>
#include <sys/domain.h>
#include <sys/thread.h>
#include <sys/serialize.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/ifq_var.h>
#include <net/radix.h>
#include <net/route.h>
#include <machine/stdarg.h>

#include <sys/thread2.h>

#if defined(INET) || defined(INET6)
/*XXX*/
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#ifdef INET6
#include <machine/clock.h> /* XXX: temporal workaround for fxp issue */
#include <netinet6/in6_var.h>
#include <netinet6/in6_ifattach.h>
#endif
#endif

#if defined(COMPAT_43)
#include <emulation/43bsd/43bsd_socket.h>
#endif /* COMPAT_43 */

/*
 * Support for non-ALTQ interfaces.
 */
static int	ifq_classic_enqueue(struct ifaltq *, struct mbuf *,
				    struct altq_pktattr *);
static struct mbuf *
		ifq_classic_dequeue(struct ifaltq *, struct mbuf *, int);
static int	ifq_classic_request(struct ifaltq *, int, void *);

/*
 * System initialization
 */

static void	if_attachdomain(void *);
static void	if_attachdomain1(struct ifnet *);
static int ifconf (u_long, caddr_t, struct ucred *);
static void ifinit (void *);
static void if_slowtimo (void *);
static void link_rtrequest (int, struct rtentry *, struct rt_addrinfo *);
static int  if_rtdel (struct radix_node *, void *);

SYSINIT(interfaces, SI_SUB_PROTO_IF, SI_ORDER_FIRST, ifinit, NULL)

MALLOC_DEFINE(M_IFADDR, "ifaddr", "interface address");
MALLOC_DEFINE(M_IFMADDR, "ether_multi", "link-level multicast address");
MALLOC_DEFINE(M_CLONE, "clone", "interface cloning framework");

int	ifqmaxlen = IFQ_MAXLEN;
struct	ifnethead ifnet;	/* depend on static init XXX */

#ifdef INET6
/*
 * XXX: declare here to avoid to include many inet6 related files..
 * should be more generalized?
 */
extern void	nd6_setmtu (struct ifnet *);
#endif

struct if_clone *if_clone_lookup (const char *, int *);
int if_clone_list (struct if_clonereq *);

LIST_HEAD(, if_clone) if_cloners = LIST_HEAD_INITIALIZER(if_cloners);
int if_cloners_count;

struct callout if_slowtimo_timer;

/*
 * Network interface utility routines.
 *
 * Routines with ifa_ifwith* names take sockaddr *'s as
 * parameters.
 */
/* ARGSUSED*/
void
ifinit(void *dummy)
{
	struct ifnet *ifp;

	callout_init(&if_slowtimo_timer);

	crit_enter();
	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		if (ifp->if_snd.ifq_maxlen == 0) {
			if_printf(ifp, "XXX: driver didn't set ifq_maxlen\n");
			ifp->if_snd.ifq_maxlen = ifqmaxlen;
		}
	}
	crit_exit();

	if_slowtimo(0);
}

int if_index = 0;
struct ifnet **ifindex2ifnet = NULL;

/*
 * Attach an interface to the list of "active" interfaces.
 *
 * The serializer is optional.  If non-NULL access to the interface
 * may be MPSAFE.
 */
void
if_attach(struct ifnet *ifp, lwkt_serialize_t serializer)
{
	unsigned socksize, ifasize;
	int namelen, masklen;
	struct sockaddr_dl *sdl;
	struct ifaddr *ifa;
	struct ifaltq *ifq;

	static int if_indexlim = 8;
	static boolean_t inited;

	if (!inited) {
		TAILQ_INIT(&ifnet);
		inited = TRUE;
	}

	/*
	 * The serializer can be passed in from the device, allowing the
	 * same serializer to be used for both the interrupt interlock and
	 * the device queue.  If not specified, the netif structure will
	 * use an embedded serializer.
	 */
	if (serializer == NULL) {
		serializer = &ifp->if_default_serializer;
		lwkt_serialize_init(serializer);
	}
	ifp->if_serializer = serializer;

	TAILQ_INSERT_TAIL(&ifnet, ifp, if_link);
	ifp->if_index = ++if_index;
	/*
	 * XXX -
	 * The old code would work if the interface passed a pre-existing
	 * chain of ifaddrs to this code.  We don't trust our callers to
	 * properly initialize the tailq, however, so we no longer allow
	 * this unlikely case.
	 */
	TAILQ_INIT(&ifp->if_addrhead);
	TAILQ_INIT(&ifp->if_prefixhead);
	LIST_INIT(&ifp->if_multiaddrs);
	getmicrotime(&ifp->if_lastchange);
	if (ifindex2ifnet == NULL || if_index >= if_indexlim) {
		unsigned int n;
		struct ifnet **q;

		if_indexlim <<= 1;

		/* grow ifindex2ifnet */
		n = if_indexlim * sizeof(*q);
		q = kmalloc(n, M_IFADDR, M_WAITOK | M_ZERO);
		if (ifindex2ifnet) {
			bcopy(ifindex2ifnet, q, n/2);
			kfree(ifindex2ifnet, M_IFADDR);
		}
		ifindex2ifnet = q;
	}

	ifindex2ifnet[if_index] = ifp;

	/*
	 * create a Link Level name for this device
	 */
	namelen = strlen(ifp->if_xname);
#define _offsetof(t, m) ((int)((caddr_t)&((t *)0)->m))
	masklen = _offsetof(struct sockaddr_dl, sdl_data[0]) + namelen;
	socksize = masklen + ifp->if_addrlen;
#define ROUNDUP(a) (1 + (((a) - 1) | (sizeof(long) - 1)))
	if (socksize < sizeof(*sdl))
		socksize = sizeof(*sdl);
	socksize = ROUNDUP(socksize);
	ifasize = sizeof(struct ifaddr) + 2 * socksize;
	ifa = kmalloc(ifasize, M_IFADDR, M_WAITOK | M_ZERO);
	sdl = (struct sockaddr_dl *)(ifa + 1);
	sdl->sdl_len = socksize;
	sdl->sdl_family = AF_LINK;
	bcopy(ifp->if_xname, sdl->sdl_data, namelen);
	sdl->sdl_nlen = namelen;
	sdl->sdl_index = ifp->if_index;
	sdl->sdl_type = ifp->if_type;
	ifp->if_lladdr = ifa;
	ifa->ifa_ifp = ifp;
	ifa->ifa_rtrequest = link_rtrequest;
	ifa->ifa_addr = (struct sockaddr *)sdl;
	sdl = (struct sockaddr_dl *)(socksize + (caddr_t)sdl);
	ifa->ifa_netmask = (struct sockaddr *)sdl;
	sdl->sdl_len = masklen;
	while (namelen != 0)
		sdl->sdl_data[--namelen] = 0xff;
	TAILQ_INSERT_HEAD(&ifp->if_addrhead, ifa, ifa_link);

	EVENTHANDLER_INVOKE(ifnet_attach_event, ifp);

	ifq = &ifp->if_snd;
	ifq->altq_type = 0;
	ifq->altq_disc = NULL;
	ifq->altq_flags &= ALTQF_CANTCHANGE;
	ifq->altq_tbr = NULL;
	ifq->altq_ifp = ifp;
	ifq_set_classic(ifq);

	if (!SLIST_EMPTY(&domains))
		if_attachdomain1(ifp);

	/* Announce the interface. */
	rt_ifannouncemsg(ifp, IFAN_ARRIVAL);
}

static void
if_attachdomain(void *dummy)
{
	struct ifnet *ifp;

	crit_enter();
	TAILQ_FOREACH(ifp, &ifnet, if_list)
		if_attachdomain1(ifp);
	crit_exit();
}
SYSINIT(domainifattach, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_FIRST,
	if_attachdomain, NULL);

static void
if_attachdomain1(struct ifnet *ifp)
{
	struct domain *dp;

	crit_enter();

	/* address family dependent data region */
	bzero(ifp->if_afdata, sizeof(ifp->if_afdata));
	SLIST_FOREACH(dp, &domains, dom_next)
		if (dp->dom_ifattach)
			ifp->if_afdata[dp->dom_family] =
				(*dp->dom_ifattach)(ifp);
	crit_exit();
}

/*
 * Detach an interface, removing it from the
 * list of "active" interfaces.
 */
void
if_detach(struct ifnet *ifp)
{
	struct ifaddr *ifa;
	struct radix_node_head	*rnh;
	int i;
	int cpu, origcpu;
	struct domain *dp;

	EVENTHANDLER_INVOKE(ifnet_detach_event, ifp);

	/*
	 * Remove routes and flush queues.
	 */
	crit_enter();
#ifdef DEVICE_POLLING
	if (ifp->if_flags & IFF_POLLING)
		ether_poll_deregister(ifp);
#endif
	if_down(ifp);

	if (ifq_is_enabled(&ifp->if_snd))
		altq_disable(&ifp->if_snd);
	if (ifq_is_attached(&ifp->if_snd))
		altq_detach(&ifp->if_snd);

	/*
	 * Clean up all addresses.
	 */
	ifp->if_lladdr = NULL;

	for (ifa = TAILQ_FIRST(&ifp->if_addrhead); ifa;
	     ifa = TAILQ_FIRST(&ifp->if_addrhead)) {
#ifdef INET
		/* XXX: Ugly!! ad hoc just for INET */
		if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
			struct ifaliasreq ifr;

			bzero(&ifr, sizeof ifr);
			ifr.ifra_addr = *ifa->ifa_addr;
			if (ifa->ifa_dstaddr)
				ifr.ifra_broadaddr = *ifa->ifa_dstaddr;
			if (in_control(NULL, SIOCDIFADDR, (caddr_t)&ifr, ifp,
				       NULL) == 0)
				continue;
		}
#endif /* INET */
#ifdef INET6
		if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET6) {
			in6_purgeaddr(ifa);
			/* ifp_addrhead is already updated */
			continue;
		}
#endif /* INET6 */
		TAILQ_REMOVE(&ifp->if_addrhead, ifa, ifa_link);
		IFAFREE(ifa);
	}

#ifdef INET
	/*
	 * Remove all IPv4 kernel structures related to ifp.
	 */
	in_ifdetach(ifp);
#endif

#ifdef INET6
	/*
	 * Remove all IPv6 kernel structs related to ifp.  This should be done
	 * before removing routing entries below, since IPv6 interface direct
	 * routes are expected to be removed by the IPv6-specific kernel API.
	 * Otherwise, the kernel will detect some inconsistency and bark it.
	 */
	in6_ifdetach(ifp);
#endif

	/*
	 * Delete all remaining routes using this interface
	 * Unfortuneatly the only way to do this is to slog through
	 * the entire routing table looking for routes which point
	 * to this interface...oh well...
	 */
	origcpu = mycpuid;
	for (cpu = 0; cpu < ncpus2; cpu++) {
		lwkt_migratecpu(cpu);
		for (i = 1; i <= AF_MAX; i++) {
			if ((rnh = rt_tables[mycpuid][i]) == NULL)
				continue;
			rnh->rnh_walktree(rnh, if_rtdel, ifp);
		}
	}
	lwkt_migratecpu(origcpu);

	/* Announce that the interface is gone. */
	rt_ifannouncemsg(ifp, IFAN_DEPARTURE);

	SLIST_FOREACH(dp, &domains, dom_next)
		if (dp->dom_ifdetach && ifp->if_afdata[dp->dom_family])
			(*dp->dom_ifdetach)(ifp,
				ifp->if_afdata[dp->dom_family]);

	/*
	 * Remove interface from ifindex2ifp[] and maybe decrement if_index.
	 */
	ifindex2ifnet[ifp->if_index] = NULL;
	while (if_index > 0 && ifindex2ifnet[if_index] == NULL)
		if_index--;

	TAILQ_REMOVE(&ifnet, ifp, if_link);
	crit_exit();
}

/*
 * Delete Routes for a Network Interface
 *
 * Called for each routing entry via the rnh->rnh_walktree() call above
 * to delete all route entries referencing a detaching network interface.
 *
 * Arguments:
 *	rn	pointer to node in the routing table
 *	arg	argument passed to rnh->rnh_walktree() - detaching interface
 *
 * Returns:
 *	0	successful
 *	errno	failed - reason indicated
 *
 */
static int
if_rtdel(struct radix_node *rn, void *arg)
{
	struct rtentry	*rt = (struct rtentry *)rn;
	struct ifnet	*ifp = arg;
	int		err;

	if (rt->rt_ifp == ifp) {

		/*
		 * Protect (sorta) against walktree recursion problems
		 * with cloned routes
		 */
		if (!(rt->rt_flags & RTF_UP))
			return (0);

		err = rtrequest(RTM_DELETE, rt_key(rt), rt->rt_gateway,
				rt_mask(rt), rt->rt_flags,
				(struct rtentry **) NULL);
		if (err) {
			log(LOG_WARNING, "if_rtdel: error %d\n", err);
		}
	}

	return (0);
}

/*
 * Create a clone network interface.
 */
int
if_clone_create(char *name, int len)
{
	struct if_clone *ifc;
	char *dp;
	int wildcard, bytoff, bitoff;
	int unit;
	int err;

	ifc = if_clone_lookup(name, &unit);
	if (ifc == NULL)
		return (EINVAL);

	if (ifunit(name) != NULL)
		return (EEXIST);

	bytoff = bitoff = 0;
	wildcard = (unit < 0);
	/*
	 * Find a free unit if none was given.
	 */
	if (wildcard) {
		while (bytoff < ifc->ifc_bmlen &&
		    ifc->ifc_units[bytoff] == 0xff)
			bytoff++;
		if (bytoff >= ifc->ifc_bmlen)
			return (ENOSPC);
		while ((ifc->ifc_units[bytoff] & (1 << bitoff)) != 0)
			bitoff++;
		unit = (bytoff << 3) + bitoff;
	}

	if (unit > ifc->ifc_maxunit)
		return (ENXIO);

	err = (*ifc->ifc_create)(ifc, unit);
	if (err != 0)
		return (err);

	if (!wildcard) {
		bytoff = unit >> 3;
		bitoff = unit - (bytoff << 3);
	}

	/*
	 * Allocate the unit in the bitmap.
	 */
	KASSERT((ifc->ifc_units[bytoff] & (1 << bitoff)) == 0,
	    ("%s: bit is already set", __func__));
	ifc->ifc_units[bytoff] |= (1 << bitoff);

	/* In the wildcard case, we need to update the name. */
	if (wildcard) {
		for (dp = name; *dp != '\0'; dp++);
		if (ksnprintf(dp, len - (dp-name), "%d", unit) >
		    len - (dp-name) - 1) {
			/*
			 * This can only be a programmer error and
			 * there's no straightforward way to recover if
			 * it happens.
			 */
			panic("if_clone_create(): interface name too long");
		}

	}

	EVENTHANDLER_INVOKE(if_clone_event, ifc);

	return (0);
}

/*
 * Destroy a clone network interface.
 */
int
if_clone_destroy(const char *name)
{
	struct if_clone *ifc;
	struct ifnet *ifp;
	int bytoff, bitoff;
	int unit;

	ifc = if_clone_lookup(name, &unit);
	if (ifc == NULL)
		return (EINVAL);

	if (unit < ifc->ifc_minifs)
		return (EINVAL);

	ifp = ifunit(name);
	if (ifp == NULL)
		return (ENXIO);

	if (ifc->ifc_destroy == NULL)
		return (EOPNOTSUPP);

	(*ifc->ifc_destroy)(ifp);

	/*
	 * Compute offset in the bitmap and deallocate the unit.
	 */
	bytoff = unit >> 3;
	bitoff = unit - (bytoff << 3);
	KASSERT((ifc->ifc_units[bytoff] & (1 << bitoff)) != 0,
	    ("%s: bit is already cleared", __func__));
	ifc->ifc_units[bytoff] &= ~(1 << bitoff);
	return (0);
}

/*
 * Look up a network interface cloner.
 */
struct if_clone *
if_clone_lookup(const char *name, int *unitp)
{
	struct if_clone *ifc;
	const char *cp;
	int i;

	for (ifc = LIST_FIRST(&if_cloners); ifc != NULL;) {
		for (cp = name, i = 0; i < ifc->ifc_namelen; i++, cp++) {
			if (ifc->ifc_name[i] != *cp)
				goto next_ifc;
		}
		goto found_name;
 next_ifc:
		ifc = LIST_NEXT(ifc, ifc_list);
	}

	/* No match. */
	return ((struct if_clone *)NULL);

 found_name:
	if (*cp == '\0') {
		i = -1;
	} else {
		for (i = 0; *cp != '\0'; cp++) {
			if (*cp < '0' || *cp > '9') {
				/* Bogus unit number. */
				return (NULL);
			}
			i = (i * 10) + (*cp - '0');
		}
	}

	if (unitp != NULL)
		*unitp = i;
	return (ifc);
}

/*
 * Register a network interface cloner.
 */
void
if_clone_attach(struct if_clone *ifc)
{
	int bytoff, bitoff;
	int err;
	int len, maxclone;
	int unit;

	KASSERT(ifc->ifc_minifs - 1 <= ifc->ifc_maxunit,
	    ("%s: %s requested more units then allowed (%d > %d)",
	    __func__, ifc->ifc_name, ifc->ifc_minifs,
	    ifc->ifc_maxunit + 1));
	/*
	 * Compute bitmap size and allocate it.
	 */
	maxclone = ifc->ifc_maxunit + 1;
	len = maxclone >> 3;
	if ((len << 3) < maxclone)
		len++;
	ifc->ifc_units = kmalloc(len, M_CLONE, M_WAITOK | M_ZERO);
	ifc->ifc_bmlen = len;

	LIST_INSERT_HEAD(&if_cloners, ifc, ifc_list);
	if_cloners_count++;

	for (unit = 0; unit < ifc->ifc_minifs; unit++) {
		err = (*ifc->ifc_create)(ifc, unit);
		KASSERT(err == 0,
		    ("%s: failed to create required interface %s%d",
		    __func__, ifc->ifc_name, unit));

		/* Allocate the unit in the bitmap. */
		bytoff = unit >> 3;
		bitoff = unit - (bytoff << 3);
		ifc->ifc_units[bytoff] |= (1 << bitoff);
	}
}

/*
 * Unregister a network interface cloner.
 */
void
if_clone_detach(struct if_clone *ifc)
{

	LIST_REMOVE(ifc, ifc_list);
	kfree(ifc->ifc_units, M_CLONE);
	if_cloners_count--;
}

/*
 * Provide list of interface cloners to userspace.
 */
int
if_clone_list(struct if_clonereq *ifcr)
{
	char outbuf[IFNAMSIZ], *dst;
	struct if_clone *ifc;
	int count, error = 0;

	ifcr->ifcr_total = if_cloners_count;
	if ((dst = ifcr->ifcr_buffer) == NULL) {
		/* Just asking how many there are. */
		return (0);
	}

	if (ifcr->ifcr_count < 0)
		return (EINVAL);

	count = (if_cloners_count < ifcr->ifcr_count) ?
	    if_cloners_count : ifcr->ifcr_count;

	for (ifc = LIST_FIRST(&if_cloners); ifc != NULL && count != 0;
	     ifc = LIST_NEXT(ifc, ifc_list), count--, dst += IFNAMSIZ) {
		strlcpy(outbuf, ifc->ifc_name, IFNAMSIZ);
		error = copyout(outbuf, dst, IFNAMSIZ);
		if (error)
			break;
	}

	return (error);
}

/*
 * Locate an interface based on a complete address.
 */
struct ifaddr *
ifa_ifwithaddr(struct sockaddr *addr)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;

	TAILQ_FOREACH(ifp, &ifnet, if_link)
	    TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family != addr->sa_family)
			continue;
		if (sa_equal(addr, ifa->ifa_addr))
			return (ifa);
		if ((ifp->if_flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
		    /* IPv6 doesn't have broadcast */
		    ifa->ifa_broadaddr->sa_len != 0 &&
		    sa_equal(ifa->ifa_broadaddr, addr))
			return (ifa);
	}
	return ((struct ifaddr *)NULL);
}
/*
 * Locate the point to point interface with a given destination address.
 */
struct ifaddr *
ifa_ifwithdstaddr(struct sockaddr *addr)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;

	TAILQ_FOREACH(ifp, &ifnet, if_link)
	    if (ifp->if_flags & IFF_POINTOPOINT)
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr->sa_family != addr->sa_family)
				continue;
			if (ifa->ifa_dstaddr &&
			    sa_equal(addr, ifa->ifa_dstaddr))
				return (ifa);
	}
	return ((struct ifaddr *)NULL);
}

/*
 * Find an interface on a specific network.  If many, choice
 * is most specific found.
 */
struct ifaddr *
ifa_ifwithnet(struct sockaddr *addr)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct ifaddr *ifa_maybe = (struct ifaddr *) 0;
	u_int af = addr->sa_family;
	char *addr_data = addr->sa_data, *cplim;

	/*
	 * AF_LINK addresses can be looked up directly by their index number,
	 * so do that if we can.
	 */
	if (af == AF_LINK) {
	    struct sockaddr_dl *sdl = (struct sockaddr_dl *)addr;

	    if (sdl->sdl_index && sdl->sdl_index <= if_index)
		return (ifindex2ifnet[sdl->sdl_index]->if_lladdr);
	}

	/*
	 * Scan though each interface, looking for ones that have
	 * addresses in this address family.
	 */
	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			char *cp, *cp2, *cp3;

			if (ifa->ifa_addr->sa_family != af)
next:				continue;
			if (af == AF_INET && ifp->if_flags & IFF_POINTOPOINT) {
				/*
				 * This is a bit broken as it doesn't
				 * take into account that the remote end may
				 * be a single node in the network we are
				 * looking for.
				 * The trouble is that we don't know the
				 * netmask for the remote end.
				 */
				if (ifa->ifa_dstaddr != NULL &&
				    sa_equal(addr, ifa->ifa_dstaddr))
					return (ifa);
			} else {
				/*
				 * if we have a special address handler,
				 * then use it instead of the generic one.
				 */
				if (ifa->ifa_claim_addr) {
					if ((*ifa->ifa_claim_addr)(ifa, addr)) {
						return (ifa);
					} else {
						continue;
					}
				}

				/*
				 * Scan all the bits in the ifa's address.
				 * If a bit dissagrees with what we are
				 * looking for, mask it with the netmask
				 * to see if it really matters.
				 * (A byte at a time)
				 */
				if (ifa->ifa_netmask == 0)
					continue;
				cp = addr_data;
				cp2 = ifa->ifa_addr->sa_data;
				cp3 = ifa->ifa_netmask->sa_data;
				cplim = ifa->ifa_netmask->sa_len +
					(char *)ifa->ifa_netmask;
				while (cp3 < cplim)
					if ((*cp++ ^ *cp2++) & *cp3++)
						goto next; /* next address! */
				/*
				 * If the netmask of what we just found
				 * is more specific than what we had before
				 * (if we had one) then remember the new one
				 * before continuing to search
				 * for an even better one.
				 */
				if (ifa_maybe == 0 ||
				    rn_refines((char *)ifa->ifa_netmask,
					       (char *)ifa_maybe->ifa_netmask))
					ifa_maybe = ifa;
			}
		}
	}
	return (ifa_maybe);
}

/*
 * Find an interface address specific to an interface best matching
 * a given address.
 */
struct ifaddr *
ifaof_ifpforaddr(struct sockaddr *addr, struct ifnet *ifp)
{
	struct ifaddr *ifa;
	char *cp, *cp2, *cp3;
	char *cplim;
	struct ifaddr *ifa_maybe = 0;
	u_int af = addr->sa_family;

	if (af >= AF_MAX)
		return (0);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family != af)
			continue;
		if (ifa_maybe == 0)
			ifa_maybe = ifa;
		if (ifa->ifa_netmask == NULL) {
			if (sa_equal(addr, ifa->ifa_addr) ||
			    (ifa->ifa_dstaddr != NULL &&
			     sa_equal(addr, ifa->ifa_dstaddr)))
				return (ifa);
			continue;
		}
		if (ifp->if_flags & IFF_POINTOPOINT) {
			if (sa_equal(addr, ifa->ifa_dstaddr))
				return (ifa);
		} else {
			cp = addr->sa_data;
			cp2 = ifa->ifa_addr->sa_data;
			cp3 = ifa->ifa_netmask->sa_data;
			cplim = ifa->ifa_netmask->sa_len + (char *)ifa->ifa_netmask;
			for (; cp3 < cplim; cp3++)
				if ((*cp++ ^ *cp2++) & *cp3)
					break;
			if (cp3 == cplim)
				return (ifa);
		}
	}
	return (ifa_maybe);
}

#include <net/route.h>

/*
 * Default action when installing a route with a Link Level gateway.
 * Lookup an appropriate real ifa to point to.
 * This should be moved to /sys/net/link.c eventually.
 */
static void
link_rtrequest(int cmd, struct rtentry *rt, struct rt_addrinfo *info)
{
	struct ifaddr *ifa;
	struct sockaddr *dst;
	struct ifnet *ifp;

	if (cmd != RTM_ADD || (ifa = rt->rt_ifa) == NULL ||
	    (ifp = ifa->ifa_ifp) == NULL || (dst = rt_key(rt)) == NULL)
		return;
	ifa = ifaof_ifpforaddr(dst, ifp);
	if (ifa != NULL) {
		IFAFREE(rt->rt_ifa);
		IFAREF(ifa);
		rt->rt_ifa = ifa;
		if (ifa->ifa_rtrequest && ifa->ifa_rtrequest != link_rtrequest)
			ifa->ifa_rtrequest(cmd, rt, info);
	}
}

/*
 * Mark an interface down and notify protocols of
 * the transition.
 * NOTE: must be called at splnet or eqivalent.
 */
void
if_unroute(struct ifnet *ifp, int flag, int fam)
{
	struct ifaddr *ifa;

	ifp->if_flags &= ~flag;
	getmicrotime(&ifp->if_lastchange);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		if (fam == PF_UNSPEC || (fam == ifa->ifa_addr->sa_family))
			pfctlinput(PRC_IFDOWN, ifa->ifa_addr);
	ifq_purge(&ifp->if_snd);
	rt_ifmsg(ifp);
}

/*
 * Mark an interface up and notify protocols of
 * the transition.
 * NOTE: must be called at splnet or eqivalent.
 */
void
if_route(struct ifnet *ifp, int flag, int fam)
{
	struct ifaddr *ifa;

	ifp->if_flags |= flag;
	getmicrotime(&ifp->if_lastchange);
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		if (fam == PF_UNSPEC || (fam == ifa->ifa_addr->sa_family))
			pfctlinput(PRC_IFUP, ifa->ifa_addr);
	rt_ifmsg(ifp);
#ifdef INET6
	in6_if_up(ifp);
#endif
}

/*
 * Mark an interface down and notify protocols of the transition.  An
 * interface going down is also considered to be a synchronizing event.
 * We must ensure that all packet processing related to the interface
 * has completed before we return so e.g. the caller can free the ifnet
 * structure that the mbufs may be referencing.
 *
 * NOTE: must be called at splnet or eqivalent.
 */
void
if_down(struct ifnet *ifp)
{
	if_unroute(ifp, IFF_UP, AF_UNSPEC);
	netmsg_service_sync();
}

/*
 * Mark an interface up and notify protocols of
 * the transition.
 * NOTE: must be called at splnet or eqivalent.
 */
void
if_up(struct ifnet *ifp)
{

	if_route(ifp, IFF_UP, AF_UNSPEC);
}

/*
 * Handle interface watchdog timer routines.  Called
 * from softclock, we decrement timers (if set) and
 * call the appropriate interface routine on expiration.
 */
static void
if_slowtimo(void *arg)
{
	struct ifnet *ifp;

	crit_enter();

	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		if (ifp->if_timer == 0 || --ifp->if_timer)
			continue;
		if (ifp->if_watchdog) {
			if (lwkt_serialize_try(ifp->if_serializer)) {
				(*ifp->if_watchdog)(ifp);
				lwkt_serialize_exit(ifp->if_serializer);
			} else {
				/* try again next timeout */
				++ifp->if_timer;
			}
		}
	}

	crit_exit();

	callout_reset(&if_slowtimo_timer, hz / IFNET_SLOWHZ, if_slowtimo, NULL);
}

/*
 * Map interface name to
 * interface structure pointer.
 */
struct ifnet *
ifunit(const char *name)
{
	struct ifnet *ifp;

	/*
	 * Search all the interfaces for this name/number
	 */

	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		if (strncmp(ifp->if_xname, name, IFNAMSIZ) == 0)
			break;
	}
	return (ifp);
}


/*
 * Map interface name in a sockaddr_dl to
 * interface structure pointer.
 */
struct ifnet *
if_withname(struct sockaddr *sa)
{
	char ifname[IFNAMSIZ+1];
	struct sockaddr_dl *sdl = (struct sockaddr_dl *)sa;

	if ( (sa->sa_family != AF_LINK) || (sdl->sdl_nlen == 0) ||
	     (sdl->sdl_nlen > IFNAMSIZ) )
		return NULL;

	/*
	 * ifunit wants a null-terminated name.  It may not be null-terminated
	 * in the sockaddr.  We don't want to change the caller's sockaddr,
	 * and there might not be room to put the trailing null anyway, so we
	 * make a local copy that we know we can null terminate safely.
	 */

	bcopy(sdl->sdl_data, ifname, sdl->sdl_nlen);
	ifname[sdl->sdl_nlen] = '\0';
	return ifunit(ifname);
}


/*
 * Interface ioctls.
 */
int
ifioctl(struct socket *so, u_long cmd, caddr_t data, struct ucred *cred)
{
	struct ifnet *ifp;
	struct ifreq *ifr;
	struct ifstat *ifs;
	int error;
	short oif_flags;
	int new_flags;
	size_t namelen, onamelen;
	char new_name[IFNAMSIZ];
	struct ifaddr *ifa;
	struct sockaddr_dl *sdl;

	switch (cmd) {

	case SIOCGIFCONF:
	case OSIOCGIFCONF:
		return (ifconf(cmd, data, cred));
	}
	ifr = (struct ifreq *)data;

	switch (cmd) {
	case SIOCIFCREATE:
	case SIOCIFDESTROY:
		if ((error = suser_cred(cred, 0)) != 0)
			return (error);
		return ((cmd == SIOCIFCREATE) ?
			if_clone_create(ifr->ifr_name, sizeof(ifr->ifr_name)) :
			if_clone_destroy(ifr->ifr_name));

	case SIOCIFGCLONERS:
		return (if_clone_list((struct if_clonereq *)data));
	}

	ifp = ifunit(ifr->ifr_name);
	if (ifp == 0)
		return (ENXIO);
	switch (cmd) {

	case SIOCGIFFLAGS:
		ifr->ifr_flags = ifp->if_flags;
		ifr->ifr_flagshigh = ifp->if_flags >> 16;
		break;

	case SIOCGIFCAP:
		ifr->ifr_reqcap = ifp->if_capabilities;
		ifr->ifr_curcap = ifp->if_capenable;
		break;

	case SIOCGIFMETRIC:
		ifr->ifr_metric = ifp->if_metric;
		break;

	case SIOCGIFMTU:
		ifr->ifr_mtu = ifp->if_mtu;
		break;

	case SIOCGIFPHYS:
		ifr->ifr_phys = ifp->if_physical;
		break;

	case SIOCSIFFLAGS:
		error = suser_cred(cred, 0);
		if (error)
			return (error);
		new_flags = (ifr->ifr_flags & 0xffff) |
		    (ifr->ifr_flagshigh << 16);
		if (ifp->if_flags & IFF_SMART) {
			/* Smart drivers twiddle their own routes */
		} else if (ifp->if_flags & IFF_UP &&
		    (new_flags & IFF_UP) == 0) {
			crit_enter();
			if_down(ifp);
			crit_exit();
		} else if (new_flags & IFF_UP &&
		    (ifp->if_flags & IFF_UP) == 0) {
			crit_enter();
			if_up(ifp);
			crit_exit();
		}

#ifdef DEVICE_POLLING
		if ((new_flags ^ ifp->if_flags) & IFF_POLLING) {
			if (new_flags & IFF_POLLING) {
				ether_poll_register(ifp);
			} else {
				ether_poll_deregister(ifp);
			}
		}
#endif

		ifp->if_flags = (ifp->if_flags & IFF_CANTCHANGE) |
			(new_flags &~ IFF_CANTCHANGE);
		if (new_flags & IFF_PPROMISC) {
			/* Permanently promiscuous mode requested */
			ifp->if_flags |= IFF_PROMISC;
		} else if (ifp->if_pcount == 0) {
			ifp->if_flags &= ~IFF_PROMISC;
		}
		if (ifp->if_ioctl) {
			lwkt_serialize_enter(ifp->if_serializer);
			ifp->if_ioctl(ifp, cmd, data, cred);
			lwkt_serialize_exit(ifp->if_serializer);
		}
		getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCSIFCAP:
		error = suser_cred(cred, 0);
		if (error)
			return (error);
		if (ifr->ifr_reqcap & ~ifp->if_capabilities)
			return (EINVAL);
		lwkt_serialize_enter(ifp->if_serializer);
		ifp->if_ioctl(ifp, cmd, data, cred);
		lwkt_serialize_exit(ifp->if_serializer);
		break;

	case SIOCSIFNAME:
		error = suser_cred(cred, 0);
		if (error != 0)
			return (error);
		error = copyinstr(ifr->ifr_data, new_name, IFNAMSIZ, NULL);
		if (error != 0)
			return (error);
		if (new_name[0] == '\0')
			return (EINVAL);
		if (ifunit(new_name) != NULL)
			return (EEXIST);

		EVENTHANDLER_INVOKE(ifnet_detach_event, ifp);

		/* Announce the departure of the interface. */
		rt_ifannouncemsg(ifp, IFAN_DEPARTURE);

		strlcpy(ifp->if_xname, new_name, sizeof(ifp->if_xname));
		ifa = TAILQ_FIRST(&ifp->if_addrhead);
		/* XXX IFA_LOCK(ifa); */
		sdl = (struct sockaddr_dl *)ifa->ifa_addr;
		namelen = strlen(new_name);
		onamelen = sdl->sdl_nlen;
		/*
		 * Move the address if needed.  This is safe because we
		 * allocate space for a name of length IFNAMSIZ when we
		 * create this in if_attach().
		 */
		if (namelen != onamelen) {
			bcopy(sdl->sdl_data + onamelen,
			    sdl->sdl_data + namelen, sdl->sdl_alen);
		}
		bcopy(new_name, sdl->sdl_data, namelen);
		sdl->sdl_nlen = namelen;
		sdl = (struct sockaddr_dl *)ifa->ifa_netmask;
		bzero(sdl->sdl_data, onamelen);
		while (namelen != 0)
			sdl->sdl_data[--namelen] = 0xff;
		/* XXX IFA_UNLOCK(ifa) */

		EVENTHANDLER_INVOKE(ifnet_attach_event, ifp);

		/* Announce the return of the interface. */
		rt_ifannouncemsg(ifp, IFAN_ARRIVAL);
		break;

	case SIOCSIFMETRIC:
		error = suser_cred(cred, 0);
		if (error)
			return (error);
		ifp->if_metric = ifr->ifr_metric;
		getmicrotime(&ifp->if_lastchange);
		break;

	case SIOCSIFPHYS:
		error = suser_cred(cred, 0);
		if (error)
			return error;
		if (!ifp->if_ioctl)
		        return EOPNOTSUPP;
		lwkt_serialize_enter(ifp->if_serializer);
		error = ifp->if_ioctl(ifp, cmd, data, cred);
		lwkt_serialize_exit(ifp->if_serializer);
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		return (error);

	case SIOCSIFMTU:
	{
		u_long oldmtu = ifp->if_mtu;

		error = suser_cred(cred, 0);
		if (error)
			return (error);
		if (ifp->if_ioctl == NULL)
			return (EOPNOTSUPP);
		if (ifr->ifr_mtu < IF_MINMTU || ifr->ifr_mtu > IF_MAXMTU)
			return (EINVAL);
		lwkt_serialize_enter(ifp->if_serializer);
		error = ifp->if_ioctl(ifp, cmd, data, cred);
		lwkt_serialize_exit(ifp->if_serializer);
		if (error == 0) {
			getmicrotime(&ifp->if_lastchange);
			rt_ifmsg(ifp);
		}
		/*
		 * If the link MTU changed, do network layer specific procedure.
		 */
		if (ifp->if_mtu != oldmtu) {
#ifdef INET6
			nd6_setmtu(ifp);
#endif
		}
		return (error);
	}

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		error = suser_cred(cred, 0);
		if (error)
			return (error);

		/* Don't allow group membership on non-multicast interfaces. */
		if ((ifp->if_flags & IFF_MULTICAST) == 0)
			return EOPNOTSUPP;

		/* Don't let users screw up protocols' entries. */
		if (ifr->ifr_addr.sa_family != AF_LINK)
			return EINVAL;

		if (cmd == SIOCADDMULTI) {
			struct ifmultiaddr *ifma;
			error = if_addmulti(ifp, &ifr->ifr_addr, &ifma);
		} else {
			error = if_delmulti(ifp, &ifr->ifr_addr);
		}
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		return error;

	case SIOCSIFPHYADDR:
	case SIOCDIFPHYADDR:
#ifdef INET6
	case SIOCSIFPHYADDR_IN6:
#endif
	case SIOCSLIFPHYADDR:
        case SIOCSIFMEDIA:
	case SIOCSIFGENERIC:
		error = suser_cred(cred, 0);
		if (error)
			return (error);
		if (ifp->if_ioctl == 0)
			return (EOPNOTSUPP);
		lwkt_serialize_enter(ifp->if_serializer);
		error = ifp->if_ioctl(ifp, cmd, data, cred);
		lwkt_serialize_exit(ifp->if_serializer);
		if (error == 0)
			getmicrotime(&ifp->if_lastchange);
		return error;

	case SIOCGIFSTATUS:
		ifs = (struct ifstat *)data;
		ifs->ascii[0] = '\0';

	case SIOCGIFPSRCADDR:
	case SIOCGIFPDSTADDR:
	case SIOCGLIFPHYADDR:
	case SIOCGIFMEDIA:
	case SIOCGIFGENERIC:
		if (ifp->if_ioctl == NULL)
			return (EOPNOTSUPP);
		lwkt_serialize_enter(ifp->if_serializer);
		error = ifp->if_ioctl(ifp, cmd, data, cred);
		lwkt_serialize_exit(ifp->if_serializer);
		return (error);

	case SIOCSIFLLADDR:
		error = suser_cred(cred, 0);
		if (error)
			return (error);
		return if_setlladdr(ifp,
		    ifr->ifr_addr.sa_data, ifr->ifr_addr.sa_len);

	default:
		oif_flags = ifp->if_flags;
		if (so->so_proto == 0)
			return (EOPNOTSUPP);
#ifndef COMPAT_43
		error = so_pru_control(so, cmd, data, ifp);
#else
	    {
		int ocmd = cmd;

		switch (cmd) {

		case SIOCSIFDSTADDR:
		case SIOCSIFADDR:
		case SIOCSIFBRDADDR:
		case SIOCSIFNETMASK:
#if BYTE_ORDER != BIG_ENDIAN
			if (ifr->ifr_addr.sa_family == 0 &&
			    ifr->ifr_addr.sa_len < 16) {
				ifr->ifr_addr.sa_family = ifr->ifr_addr.sa_len;
				ifr->ifr_addr.sa_len = 16;
			}
#else
			if (ifr->ifr_addr.sa_len == 0)
				ifr->ifr_addr.sa_len = 16;
#endif
			break;

		case OSIOCGIFADDR:
			cmd = SIOCGIFADDR;
			break;

		case OSIOCGIFDSTADDR:
			cmd = SIOCGIFDSTADDR;
			break;

		case OSIOCGIFBRDADDR:
			cmd = SIOCGIFBRDADDR;
			break;

		case OSIOCGIFNETMASK:
			cmd = SIOCGIFNETMASK;
		}
		error =  so_pru_control(so, cmd, data, ifp);
		switch (ocmd) {

		case OSIOCGIFADDR:
		case OSIOCGIFDSTADDR:
		case OSIOCGIFBRDADDR:
		case OSIOCGIFNETMASK:
			*(u_short *)&ifr->ifr_addr = ifr->ifr_addr.sa_family;

		}
	    }
#endif /* COMPAT_43 */

		if ((oif_flags ^ ifp->if_flags) & IFF_UP) {
#ifdef INET6
			DELAY(100);/* XXX: temporary workaround for fxp issue*/
			if (ifp->if_flags & IFF_UP) {
				crit_enter();
				in6_if_up(ifp);
				crit_exit();
			}
#endif
		}
		return (error);

	}
	return (0);
}

/*
 * Set/clear promiscuous mode on interface ifp based on the truth value
 * of pswitch.  The calls are reference counted so that only the first
 * "on" request actually has an effect, as does the final "off" request.
 * Results are undefined if the "off" and "on" requests are not matched.
 */
int
ifpromisc(struct ifnet *ifp, int pswitch)
{
	struct ifreq ifr;
	int error;
	int oldflags;

	oldflags = ifp->if_flags;
	if (ifp->if_flags & IFF_PPROMISC) {
		/* Do nothing if device is in permanently promiscuous mode */
		ifp->if_pcount += pswitch ? 1 : -1;
		return (0);
	}
	if (pswitch) {
		/*
		 * If the device is not configured up, we cannot put it in
		 * promiscuous mode.
		 */
		if ((ifp->if_flags & IFF_UP) == 0)
			return (ENETDOWN);
		if (ifp->if_pcount++ != 0)
			return (0);
		ifp->if_flags |= IFF_PROMISC;
		log(LOG_INFO, "%s: promiscuous mode enabled\n",
		    ifp->if_xname);
	} else {
		if (--ifp->if_pcount > 0)
			return (0);
		ifp->if_flags &= ~IFF_PROMISC;
		log(LOG_INFO, "%s: promiscuous mode disabled\n",
		    ifp->if_xname);
	}
	ifr.ifr_flags = ifp->if_flags;
	ifr.ifr_flagshigh = ifp->if_flags >> 16;
	lwkt_serialize_enter(ifp->if_serializer);
	error = ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr,
				 (struct ucred *)NULL);
	lwkt_serialize_exit(ifp->if_serializer);
	if (error == 0)
		rt_ifmsg(ifp);
	else
		ifp->if_flags = oldflags;
	return error;
}

/*
 * Return interface configuration
 * of system.  List may be used
 * in later ioctl's (above) to get
 * other information.
 */
static int
ifconf(u_long cmd, caddr_t data, struct ucred *cred)
{
	struct ifconf *ifc = (struct ifconf *)data;
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct sockaddr *sa;
	struct ifreq ifr, *ifrp;
	int space = ifc->ifc_len, error = 0;

	ifrp = ifc->ifc_req;
	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		int addrs;

		if (space <= sizeof ifr)
			break;

		/*
		 * Zero the stack declared structure first to prevent
		 * memory disclosure.
		 */
		bzero(&ifr, sizeof(ifr));
		if (strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name))
		    >= sizeof(ifr.ifr_name)) {
			error = ENAMETOOLONG;
			break;
		}

		addrs = 0;
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (space <= sizeof ifr)
				break;
			sa = ifa->ifa_addr;
			if (cred->cr_prison &&
			    prison_if(cred, sa))
				continue;
			addrs++;
#ifdef COMPAT_43
			if (cmd == OSIOCGIFCONF) {
				struct osockaddr *osa =
					 (struct osockaddr *)&ifr.ifr_addr;
				ifr.ifr_addr = *sa;
				osa->sa_family = sa->sa_family;
				error = copyout(&ifr, ifrp, sizeof ifr);
				ifrp++;
			} else
#endif
			if (sa->sa_len <= sizeof(*sa)) {
				ifr.ifr_addr = *sa;
				error = copyout(&ifr, ifrp, sizeof ifr);
				ifrp++;
			} else {
				if (space < (sizeof ifr) + sa->sa_len -
					    sizeof(*sa))
					break;
				space -= sa->sa_len - sizeof(*sa);
				error = copyout(&ifr, ifrp,
						sizeof ifr.ifr_name);
				if (error == 0)
					error = copyout(sa, &ifrp->ifr_addr,
							sa->sa_len);
				ifrp = (struct ifreq *)
					(sa->sa_len + (caddr_t)&ifrp->ifr_addr);
			}
			if (error)
				break;
			space -= sizeof ifr;
		}
		if (error)
			break;
		if (!addrs) {
			bzero(&ifr.ifr_addr, sizeof ifr.ifr_addr);
			error = copyout(&ifr, ifrp, sizeof ifr);
			if (error)
				break;
			space -= sizeof ifr;
			ifrp++;
		}
	}
	ifc->ifc_len -= space;
	return (error);
}

/*
 * Just like if_promisc(), but for all-multicast-reception mode.
 */
int
if_allmulti(struct ifnet *ifp, int onswitch)
{
	int error = 0;
	struct ifreq ifr;

	crit_enter();

	if (onswitch) {
		if (ifp->if_amcount++ == 0) {
			ifp->if_flags |= IFF_ALLMULTI;
			ifr.ifr_flags = ifp->if_flags;
			ifr.ifr_flagshigh = ifp->if_flags >> 16;
			lwkt_serialize_enter(ifp->if_serializer);
			error = ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr,
					      (struct ucred *)NULL);
			lwkt_serialize_exit(ifp->if_serializer);
		}
	} else {
		if (ifp->if_amcount > 1) {
			ifp->if_amcount--;
		} else {
			ifp->if_amcount = 0;
			ifp->if_flags &= ~IFF_ALLMULTI;
			ifr.ifr_flags = ifp->if_flags;
			ifr.ifr_flagshigh = ifp->if_flags >> 16;
			lwkt_serialize_enter(ifp->if_serializer);
			error = ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr,
					      (struct ucred *)NULL);
			lwkt_serialize_exit(ifp->if_serializer);
		}
	}

	crit_exit();

	if (error == 0)
		rt_ifmsg(ifp);
	return error;
}

/*
 * Add a multicast listenership to the interface in question.
 * The link layer provides a routine which converts
 */
int
if_addmulti(
	struct ifnet *ifp,	/* interface to manipulate */
	struct sockaddr *sa,	/* address to add */
	struct ifmultiaddr **retifma)
{
	struct sockaddr *llsa, *dupsa;
	int error;
	struct ifmultiaddr *ifma;

	/*
	 * If the matching multicast address already exists
	 * then don't add a new one, just add a reference
	 */
	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (sa_equal(sa, ifma->ifma_addr)) {
			ifma->ifma_refcount++;
			if (retifma)
				*retifma = ifma;
			return 0;
		}
	}

	/*
	 * Give the link layer a chance to accept/reject it, and also
	 * find out which AF_LINK address this maps to, if it isn't one
	 * already.
	 */
	if (ifp->if_resolvemulti) {
		lwkt_serialize_enter(ifp->if_serializer);
		error = ifp->if_resolvemulti(ifp, &llsa, sa);
		lwkt_serialize_exit(ifp->if_serializer);
		if (error) 
			return error;
	} else {
		llsa = 0;
	}

	MALLOC(ifma, struct ifmultiaddr *, sizeof *ifma, M_IFMADDR, M_WAITOK);
	MALLOC(dupsa, struct sockaddr *, sa->sa_len, M_IFMADDR, M_WAITOK);
	bcopy(sa, dupsa, sa->sa_len);

	ifma->ifma_addr = dupsa;
	ifma->ifma_lladdr = llsa;
	ifma->ifma_ifp = ifp;
	ifma->ifma_refcount = 1;
	ifma->ifma_protospec = 0;
	rt_newmaddrmsg(RTM_NEWMADDR, ifma);

	/*
	 * Some network interfaces can scan the address list at
	 * interrupt time; lock them out.
	 */
	crit_enter();
	LIST_INSERT_HEAD(&ifp->if_multiaddrs, ifma, ifma_link);
	crit_exit();
	*retifma = ifma;

	if (llsa != 0) {
		LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
			if (sa_equal(ifma->ifma_addr, llsa))
				break;
		}
		if (ifma) {
			ifma->ifma_refcount++;
		} else {
			MALLOC(ifma, struct ifmultiaddr *, sizeof *ifma,
			       M_IFMADDR, M_WAITOK);
			MALLOC(dupsa, struct sockaddr *, llsa->sa_len,
			       M_IFMADDR, M_WAITOK);
			bcopy(llsa, dupsa, llsa->sa_len);
			ifma->ifma_addr = dupsa;
			ifma->ifma_ifp = ifp;
			ifma->ifma_refcount = 1;
			crit_enter();
			LIST_INSERT_HEAD(&ifp->if_multiaddrs, ifma, ifma_link);
			crit_exit();
		}
	}
	/*
	 * We are certain we have added something, so call down to the
	 * interface to let them know about it.
	 */
	crit_enter();
	lwkt_serialize_enter(ifp->if_serializer);
	ifp->if_ioctl(ifp, SIOCADDMULTI, 0, (struct ucred *)NULL);
	lwkt_serialize_exit(ifp->if_serializer);
	crit_exit();

	return 0;
}

/*
 * Remove a reference to a multicast address on this interface.  Yell
 * if the request does not match an existing membership.
 */
int
if_delmulti(struct ifnet *ifp, struct sockaddr *sa)
{
	struct ifmultiaddr *ifma;

	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link)
		if (sa_equal(sa, ifma->ifma_addr))
			break;
	if (ifma == 0)
		return ENOENT;

	if (ifma->ifma_refcount > 1) {
		ifma->ifma_refcount--;
		return 0;
	}

	rt_newmaddrmsg(RTM_DELMADDR, ifma);
	sa = ifma->ifma_lladdr;
	crit_enter();
	LIST_REMOVE(ifma, ifma_link);
	/*
	 * Make sure the interface driver is notified
	 * in the case of a link layer mcast group being left.
	 */
	if (ifma->ifma_addr->sa_family == AF_LINK && sa == 0) {
		lwkt_serialize_enter(ifp->if_serializer);
		ifp->if_ioctl(ifp, SIOCDELMULTI, 0, (struct ucred *)NULL);
		lwkt_serialize_exit(ifp->if_serializer);
	}
	crit_exit();
	kfree(ifma->ifma_addr, M_IFMADDR);
	kfree(ifma, M_IFMADDR);
	if (sa == 0)
		return 0;

	/*
	 * Now look for the link-layer address which corresponds to
	 * this network address.  It had been squirreled away in
	 * ifma->ifma_lladdr for this purpose (so we don't have
	 * to call ifp->if_resolvemulti() again), and we saved that
	 * value in sa above.  If some nasty deleted the
	 * link-layer address out from underneath us, we can deal because
	 * the address we stored was is not the same as the one which was
	 * in the record for the link-layer address.  (So we don't complain
	 * in that case.)
	 */
	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link)
		if (sa_equal(sa, ifma->ifma_addr))
			break;
	if (ifma == 0)
		return 0;

	if (ifma->ifma_refcount > 1) {
		ifma->ifma_refcount--;
		return 0;
	}

	crit_enter();
	lwkt_serialize_enter(ifp->if_serializer);
	LIST_REMOVE(ifma, ifma_link);
	ifp->if_ioctl(ifp, SIOCDELMULTI, 0, (struct ucred *)NULL);
	lwkt_serialize_exit(ifp->if_serializer);
	crit_exit();
	kfree(ifma->ifma_addr, M_IFMADDR);
	kfree(sa, M_IFMADDR);
	kfree(ifma, M_IFMADDR);

	return 0;
}

/*
 * Set the link layer address on an interface.
 *
 * At this time we only support certain types of interfaces,
 * and we don't allow the length of the address to change.
 */
int
if_setlladdr(struct ifnet *ifp, const u_char *lladdr, int len)
{
	struct sockaddr_dl *sdl;
	struct ifaddr *ifa;
	struct ifreq ifr;

	sdl = IF_LLSOCKADDR(ifp);
	if (sdl == NULL)
		return (EINVAL);
	if (len != sdl->sdl_alen)	/* don't allow length to change */
		return (EINVAL);
	switch (ifp->if_type) {
	case IFT_ETHER:			/* these types use struct arpcom */
	case IFT_FDDI:
	case IFT_XETHER:
	case IFT_ISO88025:
	case IFT_L2VLAN:
		bcopy(lladdr, ((struct arpcom *)ifp->if_softc)->ac_enaddr, len);
		/* FALLTHROUGH */
	case IFT_ARCNET:
		bcopy(lladdr, LLADDR(sdl), len);
		break;
	default:
		return (ENODEV);
	}
	/*
	 * If the interface is already up, we need
	 * to re-init it in order to reprogram its
	 * address filter.
	 */
	lwkt_serialize_enter(ifp->if_serializer);
	if ((ifp->if_flags & IFF_UP) != 0) {
		ifp->if_flags &= ~IFF_UP;
		ifr.ifr_flags = ifp->if_flags;
		ifr.ifr_flagshigh = ifp->if_flags >> 16;
		ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr,
			      (struct ucred *)NULL);
		ifp->if_flags |= IFF_UP;
		ifr.ifr_flags = ifp->if_flags;
		ifr.ifr_flagshigh = ifp->if_flags >> 16;
		ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr,
				 (struct ucred *)NULL);
#ifdef INET
		/*
		 * Also send gratuitous ARPs to notify other nodes about
		 * the address change.
		 */
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
			if (ifa->ifa_addr != NULL &&
			    ifa->ifa_addr->sa_family == AF_INET)
				arp_ifinit(ifp, ifa);
		}
#endif
	}
	lwkt_serialize_exit(ifp->if_serializer);
	return (0);
}

struct ifmultiaddr *
ifmaof_ifpforaddr(struct sockaddr *sa, struct ifnet *ifp)
{
	struct ifmultiaddr *ifma;

	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link)
		if (sa_equal(ifma->ifma_addr, sa))
			break;

	return ifma;
}

/*
 * The name argument must be a pointer to storage which will last as
 * long as the interface does.  For physical devices, the result of
 * device_get_name(dev) is a good choice and for pseudo-devices a
 * static string works well.
 */
void
if_initname(struct ifnet *ifp, const char *name, int unit)
{
	ifp->if_dname = name;
	ifp->if_dunit = unit;
	if (unit != IF_DUNIT_NONE)
		ksnprintf(ifp->if_xname, IFNAMSIZ, "%s%d", name, unit);
	else
		strlcpy(ifp->if_xname, name, IFNAMSIZ);
}

int
if_printf(struct ifnet *ifp, const char *fmt, ...)
{
	__va_list ap;
	int retval;

	retval = kprintf("%s: ", ifp->if_xname);
	__va_start(ap, fmt);
	retval += kvprintf(fmt, ap);
	__va_end(ap);
	return (retval);
}

SYSCTL_NODE(_net, PF_LINK, link, CTLFLAG_RW, 0, "Link layers");
SYSCTL_NODE(_net_link, 0, generic, CTLFLAG_RW, 0, "Generic link-management");

void
ifq_set_classic(struct ifaltq *ifq)
{
	ifq->altq_enqueue = ifq_classic_enqueue;
	ifq->altq_dequeue = ifq_classic_dequeue;
	ifq->altq_request = ifq_classic_request;
}

static int
ifq_classic_enqueue(struct ifaltq *ifq, struct mbuf *m,
		    struct altq_pktattr *pa __unused)
{
	crit_enter();
	if (IF_QFULL(ifq)) {
		m_freem(m);
		crit_exit();
		return(ENOBUFS);
	} else {
		IF_ENQUEUE(ifq, m);
		crit_exit();
		return(0);
	}	
}

static struct mbuf *
ifq_classic_dequeue(struct ifaltq *ifq, struct mbuf *mpolled, int op)
{
	struct mbuf *m;

	crit_enter();
	switch (op) {
	case ALTDQ_POLL:
		IF_POLL(ifq, m);
		break;
	case ALTDQ_REMOVE:
		IF_DEQUEUE(ifq, m);
		break;
	default:
		panic("unsupported ALTQ dequeue op: %d", op);
	}
	crit_exit();
	KKASSERT(mpolled == NULL || mpolled == m);
	return(m);
}

static int
ifq_classic_request(struct ifaltq *ifq, int req, void *arg)
{
	crit_enter();
	switch (req) {
	case ALTRQ_PURGE:
		IF_DRAIN(ifq);
		break;
	default:
		panic("unspported ALTQ request: %d", req);
	}
	crit_exit();
	return(0);
}

