/*
 * Copyright (c) 1995, 1996
 *	Matt Thomas <matt@3am-software.com>.  All rights reserved.
 * Copyright (c) 1982, 1989, 1993
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
 *	from: if_ethersubr.c,v 1.5 1994/12/13 22:31:45 wollman Exp
 * $FreeBSD: src/sys/net/if_fddisubr.c,v 1.41.2.8 2002/02/20 23:34:09 fjoe Exp $
 * $DragonFly: src/sys/net/Attic/if_fddisubr.c,v 1.16 2005/02/11 22:25:57 joerg Exp $
 */

#include "opt_atalk.h"
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipx.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/malloc.h>

#include <net/if.h>
#include <net/bpf.h>
#include <net/netisr.h>
#include <net/route.h>
#include <net/if_llc.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/ifq_var.h>

#if defined(INET) || defined(INET6)
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <net/ethernet.h>
#endif
#ifdef INET6
#include <netinet6/nd6.h>
#endif
#if defined(__DragonFly__) || defined(__FreeBSD__)
#include <netinet/if_fddi.h>
#else
#include <net/if_fddi.h>
#endif

#ifdef IPX
#include <netproto/ipx/ipx.h>
#include <netproto/ipx/ipx_if.h>
#endif

#ifdef NS
#include <netns/ns.h>
#include <netns/ns_if.h>
#endif

#ifdef DECNET
#include <netdnet/dn.h>
#endif

#ifdef NETATALK
#include <netproto/atalk/at.h>
#include <netproto/atalk/at_var.h>
#include <netproto/atalk/at_extern.h>

#define llc_snap_org_code llc_un.type_snap.org_code
#define llc_snap_ether_type llc_un.type_snap.ether_type

extern u_char	at_org_code[ 3 ];
extern u_char	aarp_org_code[ 3 ];
#endif /* NETATALK */

static	int fddi_resolvemulti (struct ifnet *, struct sockaddr **,
				   struct sockaddr *);
static void	fddi_input(struct ifnet *, struct mbuf *);
static int	fddi_output(struct ifnet *, struct mbuf *, struct sockaddr *,
			    struct rtentry *);

#define senderr(e) { error = (e); goto bad;}

/*
 * This really should be defined in if_llc.h but in case it isn't.
 */
#ifndef llc_snap
#define	llc_snap	llc_un.type_snap
#endif

#if defined(__DragonFly__)
#define	RTALLOC1(a, b)		_rtlookup(a, b, b ? RTL_DOCLONE : RTL_DONTCLONE)
#define	ARPRESOLVE(a, b, c, d, e, f)	arpresolve(a, b, c, d, e)
#elif defined(__FreeBSD__)
#define	RTALLOC1(a, b)			rtalloc1(a, b, 0UL)
#define	ARPRESOLVE(a, b, c, d, e, f)	arpresolve(a, b, c, d, e, f)
#endif
/*
 * FDDI output routine.
 * Encapsulate a packet of type family for the local net.
 * Use trailer local net encapsulation if enough data in first
 * packet leaves a multiple of 512 bytes of data in remainder.
 * Assumes that ifp is actually pointer to arpcom structure.
 */
static int
fddi_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
	    struct rtentry *rt)
{
	struct arpcom *ac = (struct arpcom *)ifp;
	u_int16_t type;
	u_char esrc[6], edst[6];
	struct fddi_header *fh;
	boolean_t hdrcmplt = FALSE;
	int s, loop_copy = 0, error;
	struct altq_pktattr pktattr;

	if ((ifp->if_flags & (IFF_UP|IFF_RUNNING)) != (IFF_UP|IFF_RUNNING))
		senderr(ENETDOWN);

	/*
	 * If the queueing discipline needs packet classification,
	 * do it before prepending link headers.
	 */
	ifq_classify(&ifp->if_snd, m, dst->sa_family, &pktattr);

	switch (dst->sa_family) {
#ifdef INET
	case AF_INET: {
		if (!arpresolve(ifp, rt, m, dst, edst))
			return (0);	/* if not yet resolved */
		type = htons(ETHERTYPE_IP);
		break;
	}
#endif
#ifdef INET6
	case AF_INET6:
		if (!nd6_storelladdr(&ac->ac_if, rt, m, dst, (u_char *)edst)) {
			/* Something bad happened */
			return (0);
		}
		type = htons(ETHERTYPE_IPV6);
		break;
#endif
#ifdef IPX
	case AF_IPX:
		type = htons(ETHERTYPE_IPX);
		bcopy(&(((struct sockaddr_ipx *)dst)->sipx_addr.x_host), edst,
		      sizeof edst);
		break;
#endif
#ifdef NETATALK
	case AF_APPLETALK: {
		struct at_ifaddr *aa;

		if (!aarpresolve((struct arpcom *) ifp, m,
				 (struct sockaddr_at *)dst, edst))
			return (0);

		/*
		 * ifaddr is the first thing in at_ifaddr
		 */
		if ((aa = at_ifawithnet((struct sockaddr_at *)dst)) == NULL) {
			error = 0;	/* XXX */
			goto bad;
		}

		/*
		 * In the phase 2 case, we need to prepend an mbuf
		 * for the llc header.  Since we must preserve the
		 * value of m, which is passed to us by value, we
		 * m_copy() the first mbuf, and use it for our llc
		 * header.
		 */
		if (aa->aa_flags & AFA_PHASE2) {
			struct llc llc;

			M_PREPEND(m, sizeof(struct llc), MB_WAIT);
			if (m == NULL)
				senderr(ENOBUFS);
			llc.llc_dsap = llc.llc_ssap = LLC_SNAP_LSAP;
			llc.llc_control = LLC_UI;
			bcopy(at_org_code, llc.llc_snap_org_code,
			      sizeof at_org_code);
			llc.llc_snap_ether_type = htons(ETHERTYPE_AT);
			bcopy(&llc, mtod(m, caddr_t), sizeof(struct llc));
			type = 0;
		} else {
			type = htons(ETHERTYPE_AT);
		}
		break;
	}
#endif /* NETATALK */
#ifdef NS
	case AF_NS:
		type = htons(ETHERTYPE_NS);
		bcopy(&(((struct sockaddr_ns *)dst)->sns_addr.x_host), edst,
		      sizeof edst);
		break;
#endif

	case pseudo_AF_HDRCMPLT:
	{
		struct ether_header *eh;

		hdrcmplt = TRUE;
		eh = (struct ether_header *)dst->sa_data;
		memcpy(esrc, eh->ether_shost, sizeof esrc);
		/* FALLTHROUGH */
	}

	case AF_UNSPEC:
	{
		struct ether_header *eh;

		loop_copy = -1;
		eh = (struct ether_header *)dst->sa_data;
		memcpy(edst, eh->ether_dhost, sizeof edst);
		if (*edst & 1)
			m->m_flags |= (M_BCAST|M_MCAST);
		type = eh->ether_type;
		break;
	}

	case AF_IMPLINK:
	{
		fh = mtod(m, struct fddi_header *);
		error = EPROTONOSUPPORT;
		switch (fh->fddi_fc & (FDDIFC_C|FDDIFC_L|FDDIFC_F)) {
			case FDDIFC_LLC_ASYNC: {
				/* legal priorities are 0 through 7 */
				if ((fh->fddi_fc & FDDIFC_Z) > 7)
					goto bad;
				break;
			}
			case FDDIFC_LLC_SYNC: {
				/* FDDIFC_Z bits reserved, must be zero */
				if (fh->fddi_fc & FDDIFC_Z)
					goto bad;
				break;
			}
			case FDDIFC_SMT: {
				/* FDDIFC_Z bits must be non zero */
				if ((fh->fddi_fc & FDDIFC_Z) == 0)
					goto bad;
				break;
			}
			default: {
				/* anything else is too dangerous */
				goto bad;
			}
		}
		error = 0;
		if (fh->fddi_dhost[0] & 1)
			m->m_flags |= (M_BCAST | M_MCAST);
		goto queue_it;
	}
	default:
		printf("%s: can't handle af%d\n", ifp->if_xname,
			dst->sa_family);
		senderr(EAFNOSUPPORT);
	}

	if (type != 0) {
		struct llc *l;

		M_PREPEND(m, sizeof(struct llc), MB_DONTWAIT);
		if (m == NULL)
			return (ENOBUFS);
		l = mtod(m, struct llc *);
		l->llc_control = LLC_UI;
		l->llc_dsap = l->llc_ssap = LLC_SNAP_LSAP;
		l->llc_snap.org_code[0] = l->llc_snap.org_code[1] =
		    l->llc_snap.org_code[2] = 0;
		memcpy(&l->llc_snap.ether_type, &type, sizeof(u_int16_t));
	}

	/*
	 * Add local net header.  If no space in first mbuf,
	 * allocate another.
	 */
	M_PREPEND(m, sizeof(struct fddi_header), MB_DONTWAIT);
	if (m == NULL)
		return (ENOBUFS);
	fh = mtod(m, struct fddi_header *);
	fh->fddi_fc = FDDIFC_LLC_ASYNC|FDDIFC_LLC_PRIO4;
	memcpy(fh->fddi_dhost, edst, sizeof edst);

queue_it:
	if (hdrcmplt)
		memcpy(fh->fddi_shost, esrc, sizeof fh->fddi_shost);
	else
		memcpy(fh->fddi_shost, ac->ac_enaddr, sizeof fh->fddi_shost);
	/*
		memcpy(fh->fddi_shost, ac->ac_enaddr, sizeof fh->fddi_shost);
	 * Ethernet address or a broadcast address, loopback a copy.
	 * XXX To make a simplex device behave exactly like a duplex
	 * device, we should copy in the case of sending to our own
	 * ethernet address (thus letting the original actually appear
	 * on the wire). However, we don't do that here for security
	 * reasons and compatibility with the original behavior.
	 */
	if ((ifp->if_flags & IFF_SIMPLEX) &&
	   (loop_copy != -1)) {
		if ((m->m_flags & M_BCAST) || loop_copy) {
			struct mbuf *n = m_copypacket(m, MB_DONTWAIT);

			if_simloop(ifp, n, dst->sa_family,
				   sizeof(struct fddi_header));
		} else if (bcmp(fh->fddi_dhost, fh->fddi_shost,
				sizeof(fh->fddi_shost)) == 0) {
			if_simloop(ifp, m, dst->sa_family,
				   sizeof(struct fddi_header));
			senderr(0);	/* XXX */
		}
	}

	s = splimp();
	/*
	 * Queue message on interface, and start output if interface
	 * not yet active.
	 */
	error = ifq_enqueue(&ifp->if_snd, m, &pktattr);
	if (error) {
		splx(s);
		return(ENOBUFS);
	}
	ifp->if_obytes += m->m_pkthdr.len;
	if (m->m_flags & M_MCAST)
		ifp->if_omcasts++;
	if ((ifp->if_flags & IFF_OACTIVE) == 0)
		(*ifp->if_start)(ifp);
	splx(s);

	return (0);

bad:
	m_freem(m);
	return (error);
}

/*
 * Process a received FDDI packet;
 * the packet is in the mbuf chain m without
 * the fddi header, which is provided separately.
 */
static void
fddi_input(struct ifnet *ifp, struct mbuf *m)
{
	int isr;
	struct llc *l;
	struct fddi_header *fh = mtod(m, struct fddi_header *);

	if (m->m_len < sizeof(struct fddi_header)) {
		/* XXX error in the caller. */
		m_freem(m);
		return;
	}
	m_adj(m, sizeof(struct fddi_header));
	m->m_pkthdr.rcvif = ifp;

	if (!(ifp->if_flags & IFF_UP)) {
		m_freem(m);
		return;
	}
	getmicrotime(&ifp->if_lastchange);
	ifp->if_ibytes += m->m_pkthdr.len + sizeof *fh;
	if (fh->fddi_dhost[0] & 1) {
		if (bcmp(ifp->if_broadcastaddr, fh->fddi_dhost,
			 ifp->if_addrlen) == 0)
			m->m_flags |= M_BCAST;
		else
			m->m_flags |= M_MCAST;
		ifp->if_imcasts++;
	} else if ((ifp->if_flags & IFF_PROMISC) &&
	    bcmp(((struct arpcom *)ifp)->ac_enaddr, fh->fddi_dhost,
		 sizeof fh->fddi_dhost) != 0) {
		m_freem(m);
		return;
	}

#ifdef M_LINK0
	/*
	 * If this has a LLC priority of 0, then mark it so upper
	 * layers have a hint that it really came via a FDDI/Ethernet
	 * bridge.
	 */
	if ((fh->fddi_fc & FDDIFC_LLC_PRIO7) == FDDIFC_LLC_PRIO0)
		m->m_flags |= M_LINK0;
#endif

	l = mtod(m, struct llc *);
	switch (l->llc_dsap) {
#if defined(INET) || defined(INET6) || defined(NS) || defined(DECNET) || defined(IPX) || defined(NETATALK)
	case LLC_SNAP_LSAP:
	{
		u_int16_t type;
		if (l->llc_control != LLC_UI || l->llc_ssap != LLC_SNAP_LSAP)
			goto dropanyway;
#ifdef NETATALK
		if (bcmp(&(l->llc_snap_org_code)[0], at_org_code,
			 sizeof(at_org_code)) == 0 &&
		    sizeof(at_org_code) == 0 &&
		    ntohs(l->llc_snap_ether_type) == ETHERTYPE_AT) {
			m_adj(m, sizeof(struct llc));
			isr = NETISR_ATALK2;
			break;
		}

		if (bcmp(&(l->llc_snap_org_code)[0], aarp_org_code,
			 sizeof(aarp_org_code)) == 0 &&
			ntohs(l->llc_snap_ether_type) == ETHERTYPE_AARP) {
		    m_adj(m, sizeof(struct llc));
		    isr = NETISR_AARP;
		    break;
		}
#endif
		if (l->llc_snap.org_code[0] != 0 ||
		    l->llc_snap.org_code[1] != 0 ||
		    l->llc_snap.org_code[2] != 0)
			goto dropanyway;
		type = ntohs(l->llc_snap.ether_type);
		m_adj(m, 8);
		switch (type) {
#ifdef INET
		case ETHERTYPE_IP:
			if (ipflow_fastforward(m))
				return;
			isr = NETISR_IP;
			break;

		case ETHERTYPE_ARP:
			if (ifp->if_flags & IFF_NOARP)
				goto dropanyway;
			isr = NETISR_ARP;
			break;
#endif
#ifdef INET6
		case ETHERTYPE_IPV6:
			isr = NETISR_IPV6;
			break;
#endif
#ifdef IPX
		case ETHERTYPE_IPX:
			isr = NETISR_IPX;
			break;
#endif
#ifdef NS
		case ETHERTYPE_NS:
			isr = NETISR_NS;
			break;
#endif
#ifdef DECNET
		case ETHERTYPE_DECNET:
			isr = NETISR_DECNET;
			break;
#endif
#ifdef NETATALK
		case ETHERTYPE_AT:
	                isr = NETISR_ATALK1;
			break;
	        case ETHERTYPE_AARP:
			isr = NETISR_AARP;
			break;
#endif /* NETATALK */
		default:
			/* printf("fddi_input: unknown protocol 0x%x\n", type); */
			ifp->if_noproto++;
			goto dropanyway;
		}
		break;
	}
#endif /* INET || NS */

	default:
		/* printf("fddi_input: unknown dsap 0x%x\n", l->llc_dsap); */
		ifp->if_noproto++;
	dropanyway:
		m_freem(m);
		return;
	}

	netisr_dispatch(isr, m);
}

/*
 * Perform common duties while attaching to interface list
 */
#ifdef __NetBSD__
#define	ifa_next	ifa_list.tqe_next
#endif

void
fddi_ifattach(ifp)
	struct ifnet *ifp;
{
	struct ifaddr *ifa;
	struct sockaddr_dl *sdl;

	ifp->if_input = fddi_input;
	ifp->if_output = fddi_output;
	ifp->if_type = IFT_FDDI;
	ifp->if_addrlen = 6;
	ifp->if_broadcastaddr = fddibroadcastaddr;
	ifp->if_hdrlen = 21;
	ifp->if_mtu = FDDIMTU;
	ifp->if_resolvemulti = fddi_resolvemulti;
	ifp->if_baudrate = 100000000;
#ifdef IFF_NOTRAILERS
	ifp->if_flags |= IFF_NOTRAILERS;
#endif
	if_attach(ifp);
#if defined(__DragonFly__) || defined(__FreeBSD__)
	ifa = ifnet_addrs[ifp->if_index - 1];
	sdl = (struct sockaddr_dl *)ifa->ifa_addr;
	sdl->sdl_type = IFT_FDDI;
	sdl->sdl_alen = ifp->if_addrlen;
	bcopy(((struct arpcom *)ifp)->ac_enaddr, LLADDR(sdl), ifp->if_addrlen);
#elif defined(__NetBSD__)
	LIST_INIT(&((struct arpcom *)ifp)->ac_multiaddrs);
	for (ifa = ifp->if_addrlist.tqh_first; ifa != NULL; ifa = ifa->ifa_list.tqe_next)
#else
	for (ifa = ifp->if_addrlist; ifa != NULL; ifa = ifa->ifa_next)
#endif
#if !defined(__DragonFly__) && !defined(__FreeBSD__)
		if ((sdl = (struct sockaddr_dl *)ifa->ifa_addr) &&
		    sdl->sdl_family == AF_LINK) {
			sdl->sdl_type = IFT_FDDI;
			sdl->sdl_alen = ifp->if_addrlen;
			bcopy(((struct arpcom *)ifp)->ac_enaddr, LLADDR(sdl),
			      ifp->if_addrlen);
			break;
		}
#endif
	bpfattach(ifp, DLT_FDDI, sizeof(struct fddi_header));
}

static int
fddi_resolvemulti(ifp, llsa, sa)
	struct ifnet *ifp;
	struct sockaddr **llsa;
	struct sockaddr *sa;
{
	struct sockaddr_dl *sdl;
	struct sockaddr_in *sin;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	u_char *e_addr;

	switch(sa->sa_family) {
	case AF_LINK:
		/*
		 * No mapping needed. Just check that it's a valid MC address.
		 */
		sdl = (struct sockaddr_dl *)sa;
		e_addr = LLADDR(sdl);
		if ((e_addr[0] & 1) != 1)
			return EADDRNOTAVAIL;
		*llsa = 0;
		return 0;

#ifdef INET
	case AF_INET:
		sin = (struct sockaddr_in *)sa;
		if (!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
			return EADDRNOTAVAIL;
		MALLOC(sdl, struct sockaddr_dl *, sizeof *sdl, M_IFMADDR,
		       M_WAITOK);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_FDDI;
		sdl->sdl_nlen = 0;
		sdl->sdl_alen = ETHER_ADDR_LEN;	/* XXX */
		sdl->sdl_slen = 0;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IP_MULTICAST(&sin->sin_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return 0;
#endif
#ifdef INET6
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)sa;
		if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
			/*
			 * An IP6 address of 0 means listen to all
			 * of the Ethernet multicast address used for IP6.
			 * (This is used for multicast routers.)
			 */
			ifp->if_flags |= IFF_ALLMULTI;
			*llsa = 0;
			return 0;
		}
		if (!IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr))
			return EADDRNOTAVAIL;
		MALLOC(sdl, struct sockaddr_dl *, sizeof *sdl, M_IFMADDR,
		       M_WAITOK);
		sdl->sdl_len = sizeof *sdl;
		sdl->sdl_family = AF_LINK;
		sdl->sdl_index = ifp->if_index;
		sdl->sdl_type = IFT_FDDI;
		sdl->sdl_nlen = 0;
		sdl->sdl_alen = ETHER_ADDR_LEN;	/* XXX */
		sdl->sdl_slen = 0;
		e_addr = LLADDR(sdl);
		ETHER_MAP_IPV6_MULTICAST(&sin6->sin6_addr, e_addr);
		*llsa = (struct sockaddr *)sdl;
		return 0;
#endif

	default:
		/*
		 * Well, the text isn't quite right, but it's the name
		 * that counts...
		 */
		return EAFNOSUPPORT;
	}
}
