/*-
 * Copyright (c) 2003-2008 Sam Leffler, Errno Consulting
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
 * $FreeBSD: head/sys/net80211/ieee80211_freebsd.h 195618 2009-07-11 15:02:45Z rpaulo $
 */
#ifndef _NET80211_IEEE80211_DRAGONFLY_H_
#define _NET80211_IEEE80211_DRAGONFLY_H_

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/types.h>
#include <sys/serialize.h>
#include <sys/sysctl.h>
#include <sys/condvar.h>
#include <sys/lock.h>
#include <sys/taskqueue.h>

#include <sys/mutex2.h>
#include <sys/serialize2.h>

#ifndef IF_PREPEND_LIST

/* XXX all are prepended to normal queue */
#define _IF_PREPEND_LIST(ifq, mhead, mtail, mcount, bcnt) do {	\
	(mtail)->m_nextpkt = (ifq)->ifsq_norm_head;		\
	if ((ifq)->ifsq_norm_tail == NULL)			\
		(ifq)->ifsq_norm_tail = (mtail);		\
	(ifq)->ifsq_norm_head = (mhead);			\
	(ifq)->ifsq_len += (mcount);				\
	(ifq)->ifsq_bcnt += (bcnt);				\
} while (0)

#define IF_PREPEND_LIST(ifq, mhead, mtail, mcount, bcnt) do {	\
	wlan_assert_serialized();				\
	_IF_PREPEND_LIST(ifq, mhead, mtail, mcount, bcnt);	\
} while (0)

#endif /* IF_PREPEND_LIST */

/*
 * Global serializer (operates like a non-reentrant lockmgr lock)
 */
extern struct lwkt_serialize wlan_global_serializer;
extern int ieee80211_force_swcrypto;

#define wlan_serialize_enter()	_wlan_serialize_enter(__func__)
#define wlan_serialize_exit()	_wlan_serialize_exit(__func__)
#define wlan_serialize_push()	_wlan_serialize_push(__func__)
#define wlan_serialize_pop(wst)	_wlan_serialize_pop(__func__, wst)
#define wlan_is_serialized()	_wlan_is_serialized()
void _wlan_serialize_enter(const char *funcname);
void _wlan_serialize_exit(const char *funcname);
int  _wlan_serialize_push(const char *funcname);
void _wlan_serialize_pop(const char *funcname, int wst);
int  _wlan_is_serialized(void);
int wlan_serialize_sleep(void *ident, int flags, const char *wmesg, int timo);

static __inline void
wlan_assert_serialized(void)
{
	ASSERT_SERIALIZED(&wlan_global_serializer);
}

static __inline void
wlan_assert_notserialized(void)
{
	ASSERT_NOT_SERIALIZED(&wlan_global_serializer);
}

/*
 * Node reference counting definitions.
 *
 * ieee80211_node_initref	initialize the reference count to 1
 * ieee80211_node_incref	add a reference
 * ieee80211_node_decref	remove a reference
 * ieee80211_node_dectestref	remove a reference and return 1 if this
 *				is the last reference, otherwise 0
 * ieee80211_node_refcnt	reference count for printing (only)
 */
#include <machine/atomic.h>

#define ieee80211_node_initref(_ni) \
	do { ((_ni)->ni_refcnt = 1); } while (0)
#define ieee80211_node_incref(_ni) \
	atomic_add_int(&(_ni)->ni_refcnt, 1)
#define	ieee80211_node_decref(_ni) \
	atomic_subtract_int(&(_ni)->ni_refcnt, 1)
struct ieee80211_node;
int	ieee80211_node_dectestref(struct ieee80211_node *ni);
#define	ieee80211_node_refcnt(_ni)	(_ni)->ni_refcnt

struct ifqueue;
struct ieee80211vap;
struct ieee80211com;
void	ieee80211_flush_ifq(struct ifaltq *, struct ieee80211vap *);

void	ieee80211_vap_destroy(struct ieee80211vap *);
int	ieee80211_vap_xmitpkt(struct ieee80211vap *vap, struct mbuf *m);
int	ieee80211_parent_xmitpkt(struct ieee80211com *ic, struct mbuf *m);
int	ieee80211_handoff(struct ifnet *, struct mbuf *);
uint16_t ieee80211_txtime(struct ieee80211_node *, u_int, uint8_t, uint32_t);

#define	IFNET_IS_UP_RUNNING(_ifp) \
	(((_ifp)->if_flags & IFF_UP) && \
	 ((_ifp)->if_flags & IFF_RUNNING))

/* XXX TODO: cap these at 1, as hz may not be 1000 */
#define	msecs_to_ticks(ms)	(((ms)*hz)/1000)
#define	ticks_to_msecs(t)	(1000*(t) / hz)
#define	ticks_to_secs(t)	((t) / hz)

#define ieee80211_time_after(a,b) 	((long)(b) - (long)(a) < 0)
#define ieee80211_time_before(a,b)	ieee80211_time_after(b,a)
#define ieee80211_time_after_eq(a,b)	((long)(a) - (long)(b) >= 0)
#define ieee80211_time_before_eq(a,b)	ieee80211_time_after_eq(b,a)

struct mbuf *ieee80211_getmgtframe(uint8_t **frm, int headroom, int pktlen);

/* tx path usage */
#define	M_ENCAP		M_PROTO1		/* 802.11 encap done */
#define	M_EAPOL		M_PROTO3		/* PAE/EAPOL frame */
#define	M_PWR_SAV	M_PROTO4		/* bypass PS handling */
#define	M_MORE_DATA	M_PROTO5		/* more data frames to follow */
#define	M_FF		M_PROTO6		/* fast frame / A-MSDU */
#define	M_TXCB		M_PROTO7		/* do tx complete callback */
#define	M_AMPDU_MPDU	M_PROTO8		/* ok for A-MPDU aggregation */
#define	M_80211_TX \
	(M_FRAG|M_FIRSTFRAG|M_LASTFRAG|M_ENCAP|M_EAPOL|M_PWR_SAV|\
	 M_MORE_DATA|M_FF|M_TXCB|M_AMPDU_MPDU)

/* rx path usage */
#define	M_AMPDU		M_PROTO1		/* A-MPDU subframe */
#define	M_WEP		M_PROTO2		/* WEP done by hardware */
#if 0
#define	M_AMPDU_MPDU	M_PROTO8		/* A-MPDU re-order done */
#endif
#define	M_80211_RX	(M_AMPDU|M_WEP|M_AMPDU_MPDU)

#define	IEEE80211_MBUF_TX_FLAG_BITS \
	"\20\1M_EXT\2M_PKTHDR\3M_EOR\4M_RDONLY\5M_ENCAP\6M_WEP\7M_EAPOL" \
	"\10M_PWR_SAV\11M_MORE_DATA\12M_BCAST\13M_MCAST\14M_FRAG\15M_FIRSTFRAG" \
	"\16M_LASTFRAG\17M_SKIP_FIREWALL\20M_FREELIST\21M_VLANTAG\22M_PROMISC" \
	"\23M_NOFREE\24M_FF\25M_TXCB\26M_AMPDU_MPDU\27M_FLOWID"

#define	IEEE80211_MBUF_RX_FLAG_BITS \
	"\20\1M_EXT\2M_PKTHDR\3M_EOR\4M_RDONLY\5M_AMPDU\6M_WEP\7M_PROTO3" \
	"\10M_PROTO4\11M_PROTO5\12M_BCAST\13M_MCAST\14M_FRAG\15M_FIRSTFRAG" \
	"\16M_LASTFRAG\17M_SKIP_FIREWALL\20M_FREELIST\21M_VLANTAG\22M_PROMISC" \
	"\23M_NOFREE\24M_PROTO6\25M_PROTO7\26M_AMPDU_MPDU\27M_FLOWID"

/*
 * Store WME access control bits in the vlan tag.
 * This is safe since it's done after the packet is classified
 * (where we use any previous tag) and because it's passed
 * directly in to the driver and there's no chance someone
 * else will clobber them on us.
 */
#define	M_WME_SETAC(m, ac) \
	((m)->m_pkthdr.ether_vlantag = (ac))
#define	M_WME_GETAC(m)	((m)->m_pkthdr.ether_vlantag)

/*
 * Mbufs on the power save queue are tagged with an age and
 * timed out.  We reuse the hardware checksum field in the
 * mbuf packet header to store this data.
 */
#define	M_AGE_SET(m,v)		(m->m_pkthdr.csum_data = v)
#define	M_AGE_GET(m)		(m->m_pkthdr.csum_data)
#define	M_AGE_SUB(m,adj)	(m->m_pkthdr.csum_data -= adj)

/*
 * Store the sequence number.
 */
#define	M_SEQNO_SET(m, seqno) \
	((m)->m_pkthdr.wlan_seqno = (seqno))
#define	M_SEQNO_GET(m)	((m)->m_pkthdr.wlan_seqno)

#define	MTAG_ABI_NET80211	1132948340	/* net80211 ABI */

struct ieee80211_cb {
	void	(*func)(struct ieee80211_node *, void *, int status);
	void	*arg;
};
#define	NET80211_TAG_CALLBACK	0	/* xmit complete callback */
int	ieee80211_add_callback(struct mbuf *m,
		void (*func)(struct ieee80211_node *, void *, int), void *arg);
void	ieee80211_process_callback(struct ieee80211_node *, struct mbuf *, int);

#define	NET80211_TAG_XMIT_PARAMS	1
/* See below; this is after the bpf_params definition */

void	get_random_bytes(void *, size_t);

#define	NET80211_TAG_RECV_PARAMS	2

void	ieee80211_sysctl_attach(struct ieee80211com *);
void	ieee80211_sysctl_detach(struct ieee80211com *);
void	ieee80211_sysctl_vattach(struct ieee80211vap *);
void	ieee80211_sysctl_vdetach(struct ieee80211vap *);

SYSCTL_DECL(_net_wlan);
int	ieee80211_sysctl_msecs_ticks(SYSCTL_HANDLER_ARGS);

void	ieee80211_load_module(const char *);

/*
 * A "policy module" is an adjunct module to net80211 that provides
 * functionality that typically includes policy decisions.  This
 * modularity enables extensibility and vendor-supplied functionality.
 */
#define	_IEEE80211_POLICY_MODULE(policy, name, version)			\
typedef void (*policy##_setup)(int);					\
SET_DECLARE(policy##_set, policy##_setup);				\
static int								\
wlan_##name##_modevent(module_t mod, int type, void *unused)		\
{									\
	policy##_setup * const *iter, f;				\
	int error;							\
									\
	switch (type) {							\
	case MOD_LOAD:							\
		SET_FOREACH(iter, policy##_set) {			\
			f = (void*) *iter;				\
			f(type);					\
		}							\
		error = 0;						\
		break;							\
	case MOD_UNLOAD:						\
		error = 0;						\
		if (nrefs) {						\
			kprintf("wlan_" #name ": still in use (%u "	\
				"dynamic refs)\n",			\
				nrefs);					\
			error = EBUSY;					\
		} else if (type == MOD_UNLOAD) {			\
			SET_FOREACH(iter, policy##_set) {		\
				f = (void*) *iter;			\
				f(type);				\
			}						\
		}							\
		break;							\
	default:							\
		error = EINVAL;						\
		break;							\
	}								\
									\
	return error;							\
}									\
static moduledata_t name##_mod = {					\
	"wlan_" #name,							\
	wlan_##name##_modevent,						\
	0								\
};									\
DECLARE_MODULE(wlan_##name, name##_mod, SI_SUB_DRIVERS, SI_ORDER_FIRST);\
MODULE_VERSION(wlan_##name, version);					\
MODULE_DEPEND(wlan_##name, wlan, 1, 1, 1)

/*
 * Crypto modules implement cipher support.
 */
#define	IEEE80211_CRYPTO_MODULE(name, version)				\
_IEEE80211_POLICY_MODULE(crypto, name, version);			\
static void								\
name##_modevent(int type)						\
{									\
	/* wlan already serialized! */					\
	if (type == MOD_LOAD)						\
		ieee80211_crypto_register(&name);			\
	else								\
		ieee80211_crypto_unregister(&name);			\
}									\
TEXT_SET(crypto##_set, name##_modevent)

/*
 * Scanner modules provide scanning policy.
 */
#define	IEEE80211_SCANNER_MODULE(name, version)				\
	_IEEE80211_POLICY_MODULE(scanner, name, version)

#define	IEEE80211_SCANNER_ALG(name, alg, v)				\
static void								\
name##_modevent(int type)						\
{									\
	/* wlan already serialized! */					\
	if (type == MOD_LOAD)						\
		ieee80211_scanner_register(alg, &v);			\
	else								\
		ieee80211_scanner_unregister(alg, &v);			\
}									\
TEXT_SET(scanner_set, name##_modevent);					\

/*
 * ACL modules implement acl policy.
 */
#define	IEEE80211_ACL_MODULE(name, alg, version)			\
_IEEE80211_POLICY_MODULE(acl, name, version);				\
static void								\
alg##_modevent(int type)						\
{									\
	/* wlan already serialized! */					\
	if (type == MOD_LOAD)						\
		ieee80211_aclator_register(&alg);			\
	else								\
		ieee80211_aclator_unregister(&alg);			\
}									\
TEXT_SET(acl_set, alg##_modevent);					\

/*
 * Authenticator modules handle 802.1x/WPA authentication.
 */
#define	IEEE80211_AUTH_MODULE(name, version)				\
	_IEEE80211_POLICY_MODULE(auth, name, version)

#define	IEEE80211_AUTH_ALG(name, alg, v)				\
static void								\
name##_modevent(int type)						\
{									\
	/* wlan already serialized! */					\
	if (type == MOD_LOAD)						\
		ieee80211_authenticator_register(alg, &v);		\
	else								\
		ieee80211_authenticator_unregister(alg);		\
}									\
TEXT_SET(auth_set, name##_modevent)

/*
 * Rate control modules provide tx rate control support.
 */
#define	IEEE80211_RATECTL_MODULE(alg, version)				\
	_IEEE80211_POLICY_MODULE(ratectl, alg, version);		\

#define	IEEE80211_RATECTL_ALG(name, alg, v)				\
static void								\
alg##_modevent(int type)						\
{									\
	/* wlan already serialized! */					\
	if (type == MOD_LOAD)						\
		ieee80211_ratectl_register(alg, &v);			\
	else								\
		ieee80211_ratectl_unregister(alg);			\
}									\
TEXT_SET(ratectl##_set, alg##_modevent)

struct ieee80211req;
typedef int ieee80211_ioctl_getfunc(struct ieee80211vap *,
    struct ieee80211req *);
SET_DECLARE(ieee80211_ioctl_getset, ieee80211_ioctl_getfunc);
#define	IEEE80211_IOCTL_GET(_name, _get) TEXT_SET(ieee80211_ioctl_getset, _get)

typedef int ieee80211_ioctl_setfunc(struct ieee80211vap *,
    struct ieee80211req *);
SET_DECLARE(ieee80211_ioctl_setset, ieee80211_ioctl_setfunc);
#define	IEEE80211_IOCTL_SET(_name, _set) TEXT_SET(ieee80211_ioctl_setset, _set)
#endif /* _KERNEL */

/* XXX this stuff belongs elsewhere */
/*
 * Message formats for messages from the net80211 layer to user
 * applications via the routing socket.  These messages are appended
 * to an if_announcemsghdr structure.
 */
struct ieee80211_join_event {
	uint8_t		iev_addr[6];
};

struct ieee80211_leave_event {
	uint8_t		iev_addr[6];
};

struct ieee80211_replay_event {
	uint8_t		iev_src[6];	/* src MAC */
	uint8_t		iev_dst[6];	/* dst MAC */
	uint8_t		iev_cipher;	/* cipher type */
	uint8_t		iev_keyix;	/* key id/index */
	uint64_t	iev_keyrsc;	/* RSC from key */
	uint64_t	iev_rsc;	/* RSC from frame */
};

struct ieee80211_michael_event {
	uint8_t		iev_src[6];	/* src MAC */
	uint8_t		iev_dst[6];	/* dst MAC */
	uint8_t		iev_cipher;	/* cipher type */
	uint8_t		iev_keyix;	/* key id/index */
};

struct ieee80211_wds_event {
	uint8_t		iev_addr[6];
};

struct ieee80211_csa_event {
	uint32_t	iev_flags;	/* channel flags */
	uint16_t	iev_freq;	/* setting in Mhz */
	uint8_t		iev_ieee;	/* IEEE channel number */
	uint8_t		iev_mode;	/* CSA mode */
	uint8_t		iev_count;	/* CSA count */
};

struct ieee80211_cac_event {
	uint32_t	iev_flags;	/* channel flags */
	uint16_t	iev_freq;	/* setting in Mhz */
	uint8_t		iev_ieee;	/* IEEE channel number */
	/* XXX timestamp? */
	uint8_t		iev_type;	/* IEEE80211_NOTIFY_CAC_* */
};

struct ieee80211_radar_event {
	uint32_t	iev_flags;	/* channel flags */
	uint16_t	iev_freq;	/* setting in Mhz */
	uint8_t		iev_ieee;	/* IEEE channel number */
	/* XXX timestamp? */
};

struct ieee80211_auth_event {
	uint8_t		iev_addr[6];
};

struct ieee80211_deauth_event {
	uint8_t		iev_addr[6];
};

struct ieee80211_country_event {
	uint8_t		iev_addr[6];
	uint8_t		iev_cc[2];	/* ISO country code */
};

struct ieee80211_radio_event {
	uint8_t		iev_state;	/* 1 on, 0 off */
};

#define	RTM_IEEE80211_ASSOC	100	/* station associate (bss mode) */
#define	RTM_IEEE80211_REASSOC	101	/* station re-associate (bss mode) */
#define	RTM_IEEE80211_DISASSOC	102	/* station disassociate (bss mode) */
#define	RTM_IEEE80211_JOIN	103	/* station join (ap mode) */
#define	RTM_IEEE80211_LEAVE	104	/* station leave (ap mode) */
#define	RTM_IEEE80211_SCAN	105	/* scan complete, results available */
#define	RTM_IEEE80211_REPLAY	106	/* sequence counter replay detected */
#define	RTM_IEEE80211_MICHAEL	107	/* Michael MIC failure detected */
#define	RTM_IEEE80211_REJOIN	108	/* station re-associate (ap mode) */
#define	RTM_IEEE80211_WDS	109	/* WDS discovery (ap mode) */
#define	RTM_IEEE80211_CSA	110	/* Channel Switch Announcement event */
#define	RTM_IEEE80211_RADAR	111	/* radar event */
#define	RTM_IEEE80211_CAC	112	/* Channel Availability Check event */
#define	RTM_IEEE80211_DEAUTH	113	/* station deauthenticate */
#define	RTM_IEEE80211_AUTH	114	/* station authenticate (ap mode) */
#define	RTM_IEEE80211_COUNTRY	115	/* discovered country code (sta mode) */
#define	RTM_IEEE80211_RADIO	116	/* RF kill switch state change */

/*
 * Structure prepended to raw packets sent through the bpf
 * interface when set to DLT_IEEE802_11_RADIO.  This allows
 * user applications to specify pretty much everything in
 * an Atheros tx descriptor.  XXX need to generalize.
 *
 * XXX cannot be more than 14 bytes as it is copied to a sockaddr's
 * XXX sa_data area.
 */
struct ieee80211_bpf_params {
	uint8_t		ibp_vers;	/* version */
#define	IEEE80211_BPF_VERSION	0
	uint8_t		ibp_len;	/* header length in bytes */
	uint8_t		ibp_flags;
#define	IEEE80211_BPF_SHORTPRE	0x01	/* tx with short preamble */
#define	IEEE80211_BPF_NOACK	0x02	/* tx with no ack */
#define	IEEE80211_BPF_CRYPTO	0x04	/* tx with h/w encryption */
#define	IEEE80211_BPF_FCS	0x10	/* frame incldues FCS */
#define	IEEE80211_BPF_DATAPAD	0x20	/* frame includes data padding */
#define	IEEE80211_BPF_RTS	0x40	/* tx with RTS/CTS */
#define	IEEE80211_BPF_CTS	0x80	/* tx with CTS only */
	uint8_t		ibp_pri;	/* WME/WMM AC+tx antenna */
	uint8_t		ibp_try0;	/* series 1 try count */
	uint8_t		ibp_rate0;	/* series 1 IEEE tx rate */
	uint8_t		ibp_power;	/* tx power (device units) */
	uint8_t		ibp_ctsrate;	/* IEEE tx rate for CTS */
	uint8_t		ibp_try1;	/* series 2 try count */
	uint8_t		ibp_rate1;	/* series 2 IEEE tx rate */
	uint8_t		ibp_try2;	/* series 3 try count */
	uint8_t		ibp_rate2;	/* series 3 IEEE tx rate */
	uint8_t		ibp_try3;	/* series 4 try count */
	uint8_t		ibp_rate3;	/* series 4 IEEE tx rate */
};

#ifdef _KERNEL
struct ieee80211_tx_params {
	struct ieee80211_bpf_params params;
};
int	ieee80211_add_xmit_params(struct mbuf *m,
	    const struct ieee80211_bpf_params *);
int	ieee80211_get_xmit_params(struct mbuf *m,
	    struct ieee80211_bpf_params *);

#define	IEEE80211_MAX_CHAINS		3
#define	IEEE80211_MAX_EVM_PILOTS	6

#define	IEEE80211_R_NF		0x0000001	/* global NF value valid */
#define	IEEE80211_R_RSSI	0x0000002	/* global RSSI value valid */
#define	IEEE80211_R_C_CHAIN	0x0000004	/* RX chain count valid */
#define	IEEE80211_R_C_NF	0x0000008	/* per-chain NF value valid */
#define	IEEE80211_R_C_RSSI	0x0000010	/* per-chain RSSI value valid */
#define	IEEE80211_R_C_EVM	0x0000020	/* per-chain EVM valid */
#define	IEEE80211_R_C_HT40	0x0000040	/* RX'ed packet is 40mhz, pilots 4,5 valid */
#define	IEEE80211_R_FREQ	0x0000080	/* Freq value populated, MHz */
#define	IEEE80211_R_IEEE	0x0000100	/* IEEE value populated */
#define	IEEE80211_R_BAND	0x0000200	/* Frequency band populated */

struct ieee80211_rx_stats {
	uint32_t r_flags;		/* IEEE80211_R_* flags */
	uint8_t c_chain;		/* number of RX chains involved */
	int16_t	c_nf_ctl[IEEE80211_MAX_CHAINS];	/* per-chain NF */
	int16_t	c_nf_ext[IEEE80211_MAX_CHAINS];	/* per-chain NF */
	int16_t	c_rssi_ctl[IEEE80211_MAX_CHAINS];	/* per-chain RSSI */
	int16_t	c_rssi_ext[IEEE80211_MAX_CHAINS];	/* per-chain RSSI */
	uint8_t nf;			/* global NF */
	uint8_t rssi;			/* global RSSI */
	uint8_t evm[IEEE80211_MAX_CHAINS][IEEE80211_MAX_EVM_PILOTS];
					/* per-chain, per-pilot EVM values */
	uint16_t c_freq;
	uint8_t c_ieee;
};

struct ieee80211_rx_params {
	struct ieee80211_rx_stats params;
};
int	ieee80211_add_rx_params(struct mbuf *m,
	    const struct ieee80211_rx_stats *rxs);
int	ieee80211_get_rx_params(struct mbuf *m,
	    struct ieee80211_rx_stats *rxs);
#endif /* _KERNEL */

/*
 * FreeBSD overrides
 */
const char *ether_sprintf(const u_char *buf);

#define V_ifnet	ifnet
#define IFF_DRV_RUNNING	IFF_RUNNING
#define if_drv_flags	if_flags

typedef struct lock	ieee80211_psq_lock_t;
typedef struct lock	ieee80211_ageq_lock_t;
typedef struct lock	ieee80211_node_lock_t;
typedef struct lock	ieee80211_scan_lock_t;
typedef struct lock	ieee80211_com_lock_t;
typedef struct lock	ieee80211_tx_lock_t;
typedef struct lock	ieee80211_scan_table_lock_t;
typedef struct lock	ieee80211_scan_iter_lock_t;
typedef struct lock	acl_lock_t;
typedef struct lock	ieee80211_rte_lock_t;
typedef struct lock	ieee80211_rt_lock_t;

#define IEEE80211_LOCK_OBJ(ic)			(&(ic)->ic_comlock)

#define IEEE80211_LOCK_INIT(ic, name)		lockinit(&(ic)->ic_comlock, name, 0, LK_CANRECURSE)
#define IEEE80211_NODE_LOCK_INIT(ic, name)	lockinit(&(nt)->nt_nodelock, name, 0, LK_CANRECURSE)
#define IEEE80211_NODE_ITERATE_LOCK_INIT(ic, name)	lockinit(&(nt)->nt_scanlock, name, 0, LK_CANRECURSE)
#define IEEE80211_SCAN_TABLE_LOCK_INIT(st, name)	lockinit(&(st)->st_lock, name, 0, LK_CANRECURSE)
#define IEEE80211_SCAN_ITER_LOCK_INIT(st, name)	lockinit(&(st)->st_scanlock, name, 0, LK_CANRECURSE)
#define IEEE80211_TX_LOCK_INIT(ic, name)	lockinit(&(ic)->ic_txlock, name, 0, LK_CANRECURSE)
#define IEEE80211_AGEQ_LOCK_INIT(aq, name)	lockinit(&(aq)->aq_lock, name, 0, LK_CANRECURSE)
#define IEEE80211_PSQ_INIT(psq, name)		lockinit(&(psq)->psq_lock, name, 0, LK_CANRECURSE)
#define ACL_LOCK_INIT(as, name)		lockinit(&(as)->as_lock, name, 0, LK_CANRECURSE)
#define MESH_RT_ENTRY_LOCK_INIT(st, name)	lockinit(&(st)->rt_lock, name, 0, LK_CANRECURSE)
#define MESH_RT_LOCK_INIT(ms, name)	lockinit(&(ms)->ms_rt_lock, name, 0, LK_CANRECURSE)

#define IEEE80211_LOCK_DESTROY(ic)		lockuninit(&(ic)->ic_comlock)
#define IEEE80211_NODE_LOCK_DESTROY(nt)		lockuninit(&(nt)->nt_nodelock)
#define IEEE80211_NODE_ITERATE_LOCK_DESTROY(nt)	lockuninit(&(nt)->nt_scanlock)
#define IEEE80211_SCAN_TABLE_LOCK_DESTROY(st)	lockuninit(&(st)->st_lock)
#define IEEE80211_SCAN_ITER_LOCK_DESTROY(st)	lockuninit(&(st)->st_scanlock)
#define IEEE80211_TX_LOCK_DESTROY(ic)		lockuninit(&(ic)->ic_txlock)
#define IEEE80211_AGEQ_LOCK_DESTROY(aq)		lockuninit(&(aq)->aq_lock)
#define IEEE80211_PSQ_DESTROY(psq)		lockuninit(&(psq)->psq_lock)
#define ACL_LOCK_DESTROY(as)			lockuninit(&(as)->as_lock)
#define MESH_RT_ENTRY_LOCK_DESTROY(rt)		lockuninit(&(rt)->rt_lock)
#define MESH_RT_LOCK_DESTROY(ms)		lockuninit(&(ms)->ms_rt_lock)

#define IEEE80211_LOCK(ic)			lockmgr(&(ic)->ic_comlock, LK_EXCLUSIVE)
#define IEEE80211_NODE_LOCK(nt)			lockmgr(&(nt)->nt_nodelock, LK_EXCLUSIVE)
#define IEEE80211_NODE_ITERATE_LOCK(nt)		lockmgr(&(nt)->nt_scanlock, LK_EXCLUSIVE)
#define IEEE80211_SCAN_TABLE_LOCK(st)		lockmgr(&(st)->st_lock, LK_EXCLUSIVE)
#define IEEE80211_SCAN_ITER_LOCK(st)		lockmgr(&(st)->st_scanlock, LK_EXCLUSIVE)
#define IEEE80211_TX_LOCK(ic)			lockmgr(&(ic)->ic_txlock, LK_EXCLUSIVE)
#define IEEE80211_AGEQ_LOCK(aq)			lockmgr(&(aq)->aq_lock, LK_EXCLUSIVE)
#define IEEE80211_PSQ_LOCK(psq)			lockmgr(&(psq)->psq_lock, LK_EXCLUSIVE)
#define ACL_LOCK(as)				lockmgr(&(as)->as_lock, LK_EXCLUSIVE)
#define MESH_RT_ENTRY_LOCK(rt)			lockmgr(&(rt)->rt_lock, LK_EXCLUSIVE)
#define MESH_RT_LOCK(ms)			lockmgr(&(ms)->ms_rt_lock, LK_EXCLUSIVE)

#define IEEE80211_UNLOCK(ic)			lockmgr(&(ic)->ic_comlock, LK_RELEASE)
#define IEEE80211_NODE_UNLOCK(nt)		lockmgr(&(nt)->nt_nodelock, LK_RELEASE)
#define IEEE80211_NODE_ITERATE_UNLOCK(nt)	lockmgr(&(nt)->nt_scanlock, LK_RELEASE)
#define IEEE80211_SCAN_TABLE_UNLOCK(nt)		lockmgr(&(st)->st_lock, LK_RELEASE)
#define IEEE80211_SCAN_ITER_UNLOCK(nt)		lockmgr(&(st)->st_scanlock, LK_RELEASE)
#define IEEE80211_TX_UNLOCK(ic)			lockmgr(&(ic)->ic_txlock, LK_RELEASE)
#define IEEE80211_AGEQ_UNLOCK(aq)		lockmgr(&(aq)->aq_lock, LK_RELEASE)
#define IEEE80211_PSQ_UNLOCK(psq)		lockmgr(&(psq)->psq_lock, LK_RELEASE)
#define ACL_UNLOCK(as)				lockmgr(&(as)->as_lock, LK_RELEASE)
#define MESH_RT_ENTRY_UNLOCK(rt)		lockmgr(&(rt)->rt_lock, LK_RELEASE)
#define MESH_RT_UNLOCK(ms)			lockmgr(&(ms)->ms_rt_lock, LK_RELEASE)

#define IEEE80211_LOCK_ASSERT(ic)		\
				KKASSERT(lockstatus(&(ic)->ic_comlock, curthread) == LK_EXCLUSIVE)
#define IEEE80211_UNLOCK_ASSERT(ic)		\
				KKASSERT(lockstatus(&(ic)->ic_comlock, curthread) != LK_EXCLUSIVE)
#define IEEE80211_NODE_LOCK_ASSERT(nt)		\
				KKASSERT(lockstatus(&(nt)->nt_nodelock, curthread) == LK_EXCLUSIVE)
#define IEEE80211_NODE_ITERATE_LOCK_ASSERT(nt)		\
				KKASSERT(lockstatus(&(nt)->nt_scanlock, curthread) == LK_EXCLUSIVE)
#define IEEE80211_TX_LOCK_ASSERT(ic)		\
				KKASSERT(lockstatus(&(ic)->ic_txlock, curthread) == LK_EXCLUSIVE)
#define IEEE80211_TX_UNLOCK_ASSERT(ic)		\
				KKASSERT(lockstatus(&(ic)->ic_txlock, curthread) != LK_EXCLUSIVE)
#define IEEE80211_AGEQ_LOCK_ASSERT(aq)		\
				KKASSERT(lockstatus(&(aq)->aq_lock, curthread) == LK_EXCLUSIVE)
#define ACL_LOCK_ASSERT(as)		\
				KKASSERT(lockstatus(&(as)->as_lock, curthread) == LK_EXCLUSIVE)
#define MESH_RT_ENTRY_LOCK_ASSERT(rt)		\
				KKASSERT(lockstatus(&(rt)->rt_lock, curthread) == LK_EXCLUSIVE)
#define MESH_RT_LOCK_ASSERT(ms)		\
				KKASSERT(lockstatus(&(ms)->ms_rt_lock, curthread) == LK_EXCLUSIVE)

#define IEEE80211_NODE_IS_LOCKED(nt)		\
				(lockstatus(&(nt)->nt_nodelock, curthread) == LK_EXCLUSIVE)

#define arc4random	karc4random

#define IEEE80211_AGEQ_INIT(aq, name)
#define IEEE80211_AGEQ_DESTROY(aq)
#define CURVNET_SET(x)
#define CURVNET_RESTORE()
#define ifa_free(ifa)

#define ALIGNED_POINTER(p, t)	(((uintptr_t)(p) & (sizeof(t) - 1)) == 0)

#define osdep_va_list		__va_list
#define osdep_va_start		__va_start
#define osdep_va_end		__va_end

/*
 * DragonFly does not implement _SAFE macros because they are generally not
 * actually safe in a MP environment, and so it is bad programming practice
 * to use them.
 */
#define TAILQ_FOREACH_SAFE(scan, list, next, save)	\
	for (scan = TAILQ_FIRST(list); (save = scan ? TAILQ_NEXT(scan, next) : NULL), scan; scan = save) 	\

#define callout_init_mtx(callo, lk, flags)		\
				callout_init_lk(callo, lk)
#define callout_schedule_dfly(callo, timo, func, args)	\
				callout_reset(callo, timo, func, args)

/*
 * if_inc macros
 */
#define ifd_IFCOUNTER_IERRORS	ifd_ierrors
#define ifd_IFCOUNTER_IPACKETS	ifd_ipackets
#define ifd_IFCOUNTER_IBYTES	ifd_ibytes
#define ifd_IFCOUNTER_OERRORS	ifd_oerrors
#define ifd_IFCOUNTER_OPACKETS	ifd_opackets
#define ifd_IFCOUNTER_OMCASTS	ifd_omcasts
#define ifd_IFCOUNTER_OBYTES	ifd_obytes

#define if_inc_counter		IFNET_STAT_INC

#define IEEE80211_FREE(ptr, type)	kfree((ptr), (type))

#endif /* _NET80211_IEEE80211_DRAGONFLY_H_ */
