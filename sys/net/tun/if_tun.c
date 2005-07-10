/*	$NetBSD: if_tun.c,v 1.14 1994/06/29 06:36:25 cgd Exp $	*/

/*
 * Copyright (c) 1988, Julian Onions <jpo@cs.nott.ac.uk>
 * Nottingham University 1987.
 *
 * This source may be freely distributed, however I would be interested
 * in any changes that are made.
 *
 * This driver takes packets off the IP i/f and hands them up to a
 * user process to have its wicked way with. This driver has it's
 * roots in a similar driver written by Phil Cockcroft (formerly) at
 * UCL. This driver is based much more on read/write/poll mode of
 * operation though.
 *
 * $FreeBSD: src/sys/net/if_tun.c,v 1.74.2.8 2002/02/13 00:43:11 dillon Exp $
 * $DragonFly: src/sys/net/tun/if_tun.c,v 1.23 2005/07/10 15:17:00 joerg Exp $
 */

#include "opt_atalk.h"
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipx.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <sys/sockio.h>
#include <sys/thread2.h>
#include <sys/ttycom.h>
#include <sys/poll.h>
#include <sys/signalvar.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/malloc.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/ifq_var.h>
#include <net/netisr.h>
#include <net/route.h>

#ifdef INET
#include <netinet/in.h>
#endif

#include <net/bpf.h>

#include "if_tunvar.h"
#include "if_tun.h"

static MALLOC_DEFINE(M_TUN, "tun", "Tunnel Interface");

static void tunattach (void *);
PSEUDO_SET(tunattach, if_tun);

static void tuncreate (dev_t dev);

#define TUNDEBUG	if (tundebug) if_printf
static int tundebug = 0;
SYSCTL_INT(_debug, OID_AUTO, if_tun_debug, CTLFLAG_RW, &tundebug, 0, "");

static int tunoutput (struct ifnet *, struct mbuf *, struct sockaddr *,
	    struct rtentry *rt);
static int tunifioctl (struct ifnet *, u_long, caddr_t, struct ucred *);
static int tuninit (struct ifnet *);
static void tunstart(struct ifnet *);

static	d_open_t	tunopen;
static	d_close_t	tunclose;
static	d_read_t	tunread;
static	d_write_t	tunwrite;
static	d_ioctl_t	tunioctl;
static	d_poll_t	tunpoll;

#define CDEV_MAJOR 52
static struct cdevsw tun_cdevsw = {
	/* name */	"tun",
	/* maj */	CDEV_MAJOR,
	/* flags */	0,
	/* port */	NULL,
	/* clone */	NULL,

	/* open */	tunopen,
	/* close */	tunclose,
	/* read */	tunread,
	/* write */	tunwrite,
	/* ioctl */	tunioctl,
	/* poll */	tunpoll,
	/* mmap */	nommap,
	/* strategy */	nostrategy,
	/* dump */	nodump,
	/* psize */	nopsize
};

static void
tunattach(void *dummy)
{
	cdevsw_add(&tun_cdevsw, 0, 0);
}

static void
tuncreate(dev)
	dev_t dev;
{
	struct tun_softc *sc;
	struct ifnet *ifp;

	dev = make_dev(&tun_cdevsw, minor(dev),
	    UID_UUCP, GID_DIALER, 0600, "tun%d", lminor(dev));

	MALLOC(sc, struct tun_softc *, sizeof(*sc), M_TUN, M_WAITOK);
	bzero(sc, sizeof *sc);
	sc->tun_flags = TUN_INITED;

	ifp = &sc->tun_if;
	if_initname(ifp, "tun", lminor(dev));
	ifp->if_mtu = TUNMTU;
	ifp->if_ioctl = tunifioctl;
	ifp->if_output = tunoutput;
	ifp->if_start = tunstart;
	ifp->if_flags = IFF_POINTOPOINT | IFF_MULTICAST;
	ifp->if_type = IFT_PPP;
	ifq_set_maxlen(&ifp->if_snd, ifqmaxlen);
	ifq_set_ready(&ifp->if_snd);
	ifp->if_softc = sc;
	if_attach(ifp);
	bpfattach(ifp, DLT_NULL, sizeof(u_int));
	dev->si_drv1 = sc;
}

/*
 * tunnel open - must be superuser & the device must be
 * configured in
 */
static	int
tunopen(dev_t dev, int flag, int mode, struct thread *td)
{
	struct ifnet	*ifp;
	struct tun_softc *tp;
	int	error;

	KKASSERT(td->td_proc);
	if ((error = suser(td)) != NULL)
		return (error);

	tp = dev->si_drv1;
	if (!tp) {
		tuncreate(dev);
		tp = dev->si_drv1;
	}
	if (tp->tun_flags & TUN_OPEN)
		return EBUSY;
	tp->tun_pid = td->td_proc->p_pid;
	ifp = &tp->tun_if;
	tp->tun_flags |= TUN_OPEN;
	TUNDEBUG(ifp, "open\n");
	return (0);
}

/*
 * tunclose - close the device - mark i/f down & delete
 * routing info
 */
static	int
tunclose(dev_t dev, int foo, int bar, struct thread *td)
{
	struct tun_softc *tp;
	struct ifnet	*ifp;

	tp = dev->si_drv1;
	ifp = &tp->tun_if;

	tp->tun_flags &= ~TUN_OPEN;
	tp->tun_pid = 0;

	/* Junk all pending output. */
	crit_enter();
	ifq_purge(&ifp->if_snd);
	crit_exit();

	if (ifp->if_flags & IFF_UP) {
		crit_enter();
		if_down(ifp);
		crit_exit();
	}

	if (ifp->if_flags & IFF_RUNNING) {
		struct ifaddr *ifa;

		crit_enter();
		/* find internet addresses and delete routes */
		TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
			if (ifa->ifa_addr->sa_family == AF_INET)
				rtinit(ifa, (int)RTM_DELETE,
				    tp->tun_flags & TUN_DSTADDR ? RTF_HOST : 0);
		ifp->if_flags &= ~IFF_RUNNING;
		crit_exit();
	}

	funsetown(tp->tun_sigio);
	selwakeup(&tp->tun_rsel);

	TUNDEBUG(ifp, "closed\n");
	return (0);
}

static int
tuninit(ifp)
	struct ifnet *ifp;
{
	struct tun_softc *tp = ifp->if_softc;
	struct ifaddr *ifa;
	int error = 0;

	TUNDEBUG(ifp, "tuninit\n");

	ifp->if_flags |= IFF_UP | IFF_RUNNING;
	getmicrotime(&ifp->if_lastchange);

	for (ifa = TAILQ_FIRST(&ifp->if_addrhead); ifa; 
	     ifa = TAILQ_NEXT(ifa, ifa_link)) {
		if (ifa->ifa_addr == NULL)
			error = EFAULT;
			/* XXX: Should maybe return straight off? */
		else {
#ifdef INET
			if (ifa->ifa_addr->sa_family == AF_INET) {
			    struct sockaddr_in *si;

			    si = (struct sockaddr_in *)ifa->ifa_addr;
			    if (si->sin_addr.s_addr)
				    tp->tun_flags |= TUN_IASET;

			    si = (struct sockaddr_in *)ifa->ifa_dstaddr;
			    if (si && si->sin_addr.s_addr)
				    tp->tun_flags |= TUN_DSTADDR;
			}
#endif
		}
	}
	return (error);
}

/*
 * Process an ioctl request.
 */
int
tunifioctl(ifp, cmd, data, cr)
	struct ifnet *ifp;
	u_long	cmd;
	caddr_t	data;
	struct ucred *cr;
{
	struct ifreq *ifr = (struct ifreq *)data;
	struct tun_softc *tp = ifp->if_softc;
	struct ifstat *ifs;
	int error = 0;

	crit_enter();

	switch(cmd) {
	case SIOCGIFSTATUS:
		ifs = (struct ifstat *)data;
		if (tp->tun_pid)
			sprintf(ifs->ascii + strlen(ifs->ascii),
			    "\tOpened by PID %d\n", tp->tun_pid);
		break;
	case SIOCSIFADDR:
		error = tuninit(ifp);
		TUNDEBUG(ifp, "address set, error=%d\n", error);
		break;
	case SIOCSIFDSTADDR:
		error = tuninit(ifp);
		TUNDEBUG(ifp, "destination address set, error=%d\n", error);
		break;
	case SIOCSIFMTU:
		ifp->if_mtu = ifr->ifr_mtu;
		TUNDEBUG(ifp, "mtu set\n");
		break;
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = EINVAL;
	}

	crit_exit();
	return (error);
}

/*
 * tunoutput - queue packets from higher level ready to put out.
 */
int
tunoutput(ifp, m0, dst, rt)
	struct ifnet   *ifp;
	struct mbuf    *m0;
	struct sockaddr *dst;
	struct rtentry *rt;
{
	struct tun_softc *tp = ifp->if_softc;
	int error;
	struct altq_pktattr pktattr;

	TUNDEBUG(ifp, "tunoutput\n");

	if ((tp->tun_flags & TUN_READY) != TUN_READY) {
		TUNDEBUG(ifp, "not ready 0%o\n", tp->tun_flags);
		m_freem (m0);
		return EHOSTDOWN;
	}

	/*
	 * if the queueing discipline needs packet classification,
	 * do it before prepending link headers.
	 */
	ifq_classify(&ifp->if_snd, m0, dst->sa_family, &pktattr);

	/* BPF write needs to be handled specially */
	if (dst->sa_family == AF_UNSPEC) {
		dst->sa_family = *(mtod(m0, int *));
		m0->m_len -= sizeof(int);
		m0->m_pkthdr.len -= sizeof(int);
		m0->m_data += sizeof(int);
	}

	if (ifp->if_bpf) {
		/*
		 * We need to prepend the address family as
		 * a four byte field.
		 */
		uint32_t af = dst->sa_family;

		bpf_ptap(ifp->if_bpf, m0, &af, sizeof(af));
	}

	/* prepend sockaddr? this may abort if the mbuf allocation fails */
	if (tp->tun_flags & TUN_LMODE) {
		/* allocate space for sockaddr */
		M_PREPEND(m0, dst->sa_len, MB_DONTWAIT);

		/* if allocation failed drop packet */
		if (m0 == NULL){
			crit_enter();
			IF_DROP(&ifp->if_snd);
			crit_exit();
			ifp->if_oerrors++;
			return (ENOBUFS);
		} else {
			bcopy(dst, m0->m_data, dst->sa_len);
		}
	}

	if (tp->tun_flags & TUN_IFHEAD) {
		/* Prepend the address family */
		M_PREPEND(m0, 4, MB_DONTWAIT);

		/* if allocation failed drop packet */
		if (m0 == NULL){
			crit_enter();
			IF_DROP(&ifp->if_snd);
			crit_exit();
			ifp->if_oerrors++;
			return ENOBUFS;
		} else
			*(u_int32_t *)m0->m_data = htonl(dst->sa_family);
	} else {
#ifdef INET
		if (dst->sa_family != AF_INET)
#endif
		{
			m_freem(m0);
			return EAFNOSUPPORT;
		}
	}

	error = ifq_handoff(ifp, m0, &pktattr);
	if (error) {
		ifp->if_collisions++;
	} else {
		ifp->if_opackets++;
		if (tp->tun_flags & TUN_RWAIT) {
			tp->tun_flags &= ~TUN_RWAIT;
		wakeup((caddr_t)tp);
		}
		if (tp->tun_flags & TUN_ASYNC && tp->tun_sigio)
			pgsigio(tp->tun_sigio, SIGIO, 0);
		selwakeup(&tp->tun_rsel);
	}
	return (error);
}

/*
 * the cdevsw interface is now pretty minimal.
 */
static	int
tunioctl(dev_t	dev, u_long cmd, caddr_t data, int flag, struct thread *td)
{
	struct tun_softc *tp = dev->si_drv1;
 	struct tuninfo *tunp;

	switch (cmd) {
 	case TUNSIFINFO:
 		tunp = (struct tuninfo *)data;
		if (tunp->mtu < IF_MINMTU)
			return (EINVAL);
 		tp->tun_if.if_mtu = tunp->mtu;
 		tp->tun_if.if_type = tunp->type;
 		tp->tun_if.if_baudrate = tunp->baudrate;
 		break;
 	case TUNGIFINFO:
 		tunp = (struct tuninfo *)data;
 		tunp->mtu = tp->tun_if.if_mtu;
 		tunp->type = tp->tun_if.if_type;
 		tunp->baudrate = tp->tun_if.if_baudrate;
 		break;
	case TUNSDEBUG:
		tundebug = *(int *)data;
		break;
	case TUNGDEBUG:
		*(int *)data = tundebug;
		break;
	case TUNSLMODE:
		if (*(int *)data) {
			tp->tun_flags |= TUN_LMODE;
			tp->tun_flags &= ~TUN_IFHEAD;
		} else
			tp->tun_flags &= ~TUN_LMODE;
		break;
	case TUNSIFHEAD:
		if (*(int *)data) {
			tp->tun_flags |= TUN_IFHEAD;
			tp->tun_flags &= ~TUN_LMODE;
		} else 
			tp->tun_flags &= ~TUN_IFHEAD;
		break;
	case TUNGIFHEAD:
		*(int *)data = (tp->tun_flags & TUN_IFHEAD) ? 1 : 0;
		break;
	case TUNSIFMODE:
		/* deny this if UP */
		if (tp->tun_if.if_flags & IFF_UP)
			return(EBUSY);

		switch (*(int *)data & ~IFF_MULTICAST) {
		case IFF_POINTOPOINT:
		case IFF_BROADCAST:
			tp->tun_if.if_flags &= ~(IFF_BROADCAST|IFF_POINTOPOINT);
			tp->tun_if.if_flags |= *(int *)data;
			break;
		default:
			return(EINVAL);
		}
		break;
	case TUNSIFPID:
		tp->tun_pid = curproc->p_pid;
		break;
	case FIONBIO:
		break;
	case FIOASYNC:
		if (*(int *)data)
			tp->tun_flags |= TUN_ASYNC;
		else
			tp->tun_flags &= ~TUN_ASYNC;
		break;
	case FIONREAD:
		crit_enter();

		if (!ifq_is_empty(&tp->tun_if.if_snd)) {
			struct mbuf *mb;

			mb = ifq_poll(&tp->tun_if.if_snd);
			for( *(int *)data = 0; mb != 0; mb = mb->m_next) 
				*(int *)data += mb->m_len;
		} else
			*(int *)data = 0;

		crit_exit();
		break;
	case FIOSETOWN:
		return (fsetown(*(int *)data, &tp->tun_sigio));

	case FIOGETOWN:
		*(int *)data = fgetown(tp->tun_sigio);
		return (0);

	/* This is deprecated, FIOSETOWN should be used instead. */
	case TIOCSPGRP:
		return (fsetown(-(*(int *)data), &tp->tun_sigio));

	/* This is deprecated, FIOGETOWN should be used instead. */
	case TIOCGPGRP:
		*(int *)data = -fgetown(tp->tun_sigio);
		return (0);

	default:
		return (ENOTTY);
	}
	return (0);
}

/*
 * The cdevsw read interface - reads a packet at a time, or at
 * least as much of a packet as can be read.
 */
static	int
tunread(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
	int flag;
{
	struct tun_softc *tp = dev->si_drv1;
	struct ifnet	*ifp = &tp->tun_if;
	struct mbuf	*m0;
	int		error=0, len;

	TUNDEBUG(ifp, "read\n");
	if ((tp->tun_flags & TUN_READY) != TUN_READY) {
		TUNDEBUG(ifp, "not ready 0%o\n", tp->tun_flags);
		return EHOSTDOWN;
	}

	tp->tun_flags &= ~TUN_RWAIT;

	crit_enter();

	while ((m0 = ifq_dequeue(&ifp->if_snd)) == NULL) {
		if (flag & IO_NDELAY) {
			crit_exit();
			return EWOULDBLOCK;
		}
		tp->tun_flags |= TUN_RWAIT;
		if ((error = tsleep(tp, PCATCH, "tunread", 0)) != 0) {
			crit_exit();
			return error;
		}
	}

	crit_exit();

	while (m0 && uio->uio_resid > 0 && error == 0) {
		len = min(uio->uio_resid, m0->m_len);
		if (len != 0)
			error = uiomove(mtod(m0, caddr_t), len, uio);
		m0 = m_free(m0);
	}

	if (m0) {
		TUNDEBUG(ifp, "Dropping mbuf\n");
		m_freem(m0);
	}
	return error;
}

/*
 * the cdevsw write interface - an atomic write is a packet - or else!
 */
static	int
tunwrite(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
	int flag;
{
	struct tun_softc *tp = dev->si_drv1;
	struct ifnet	*ifp = &tp->tun_if;
	struct mbuf	*top, **mp, *m;
	int		error=0, tlen, mlen;
	uint32_t	family;
	int		isr;

	TUNDEBUG(ifp, "tunwrite\n");

	if (uio->uio_resid == 0)
		return 0;

	if (uio->uio_resid < 0 || uio->uio_resid > TUNMRU) {
		TUNDEBUG(ifp, "len=%d!\n", uio->uio_resid);
		return EIO;
	}
	tlen = uio->uio_resid;

	/* get a header mbuf */
	MGETHDR(m, MB_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	mlen = MHLEN;

	top = 0;
	mp = &top;
	while (error == 0 && uio->uio_resid > 0) {
		m->m_len = min(mlen, uio->uio_resid);
		error = uiomove(mtod (m, caddr_t), m->m_len, uio);
		*mp = m;
		mp = &m->m_next;
		if (uio->uio_resid > 0) {
			MGET (m, MB_DONTWAIT, MT_DATA);
			if (m == 0) {
				error = ENOBUFS;
				break;
			}
			mlen = MLEN;
		}
	}
	if (error) {
		if (top)
			m_freem (top);
		ifp->if_ierrors++;
		return error;
	}

	top->m_pkthdr.len = tlen;
	top->m_pkthdr.rcvif = ifp;

	if (ifp->if_bpf) {
		if (tp->tun_flags & TUN_IFHEAD) {
			/*
			 * Conveniently, we already have a 4-byte address
			 * family prepended to our packet !
			 * Inconveniently, it's in the wrong byte order !
			 */
			if ((top = m_pullup(top, sizeof(family))) == NULL)
				return ENOBUFS;
			*mtod(top, u_int32_t *) =
			    ntohl(*mtod(top, u_int32_t *));
			bpf_mtap(ifp->if_bpf, top);
			*mtod(top, u_int32_t *) =
			    htonl(*mtod(top, u_int32_t *));
		} else {
			/*
			 * We need to prepend the address family as
			 * a four byte field.
			 */
			static const uint32_t af = AF_INET;

			bpf_ptap(ifp->if_bpf, top, &af, sizeof(af));
		}
	}

	if (tp->tun_flags & TUN_IFHEAD) {
		if (top->m_len < sizeof(family) &&
		    (top = m_pullup(top, sizeof(family))) == NULL)
				return ENOBUFS;
		family = ntohl(*mtod(top, u_int32_t *));
		m_adj(top, sizeof(family));
	} else
		family = AF_INET;

	ifp->if_ibytes += top->m_pkthdr.len;
	ifp->if_ipackets++;

	switch (family) {
#ifdef INET
	case AF_INET:
		isr = NETISR_IP;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		isr = NETISR_IPV6;
		break;
#endif
#ifdef IPX
	case AF_IPX:
		isr = NETISR_IPX;
		break;
#endif
#ifdef NETATALK
	case AF_APPLETALK:
		isr = NETISR_ATALK2;
		break;
#endif
	default:
		m_freem(m);
		return (EAFNOSUPPORT);
	}

	netisr_dispatch(isr, top);
	return (0);
}

/*
 * tunpoll - the poll interface, this is only useful on reads
 * really. The write detect always returns true, write never blocks
 * anyway, it either accepts the packet or drops it.
 */
static	int
tunpoll(dev_t dev, int events, struct thread *td)
{
	struct tun_softc *tp = dev->si_drv1;
	struct ifnet	*ifp = &tp->tun_if;
	int		revents = 0;

	TUNDEBUG(ifp, "tunpoll\n");

	crit_enter();

	if (events & (POLLIN | POLLRDNORM)) {
		if (!ifq_is_empty(&ifp->if_snd)) {
			TUNDEBUG(ifp, "tunpoll q=%d\n", ifp->if_snd.ifq_len);
			revents |= events & (POLLIN | POLLRDNORM);
		} else {
			TUNDEBUG(ifp, "tunpoll waiting\n");
			selrecord(td, &tp->tun_rsel);
		}
	}
	if (events & (POLLOUT | POLLWRNORM))
		revents |= events & (POLLOUT | POLLWRNORM);

	crit_exit();

	return (revents);
}

/*
 * Start packet transmission on the interface.
 * when the interface queue is rate-limited by ALTQ,
 * if_start is needed to drain packets from the queue in order
 * to notify readers when outgoing packets become ready.
 */
static void
tunstart(struct ifnet *ifp)
{
	struct tun_softc *tp = ifp->if_softc;
	struct mbuf *m;

	if (!ifq_is_enabled(&ifp->if_snd))
		return;

	m = ifq_poll(&ifp->if_snd);
	if (m != NULL) {
		if (tp->tun_flags & TUN_RWAIT) {
			tp->tun_flags &= ~TUN_RWAIT;
			wakeup((caddr_t)tp);
		}
		if (tp->tun_flags & TUN_ASYNC && tp->tun_sigio)
			pgsigio(tp->tun_sigio, SIGIO, 0);
		selwakeup(&tp->tun_rsel);
	}
}
