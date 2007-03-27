/*
 * Copyright (c) 2006
 *	Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $FreeBSD: src/sys/dev/ral/rt2661.c,v 1.4 2006/03/21 21:15:43 damien Exp $
 * $DragonFly: src/sys/dev/netif/ral/rt2661.c,v 1.14 2007/03/27 13:34:53 sephe Exp $
 */

/*
 * Ralink Technology RT2561, RT2561S and RT2661 chipset driver
 * http://www.ralinktech.com/
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/serialize.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ifq_var.h>

#include <netproto/802_11/ieee80211_var.h>
#include <netproto/802_11/ieee80211_radiotap.h>

#include <dev/netif/ral/if_ralrate.h>
#include <dev/netif/ral/rt2661reg.h>
#include <dev/netif/ral/rt2661var.h>
#include <dev/netif/ral/rt2661_ucode.h>

#ifdef RAL_DEBUG
#define DPRINTF(x)	do { if (ral_debug > 0) kprintf x; } while (0)
#define DPRINTFN(n, x)	do { if (ral_debug >= (n)) kprintf x; } while (0)
int ral_debug = 1;
SYSCTL_INT(_debug, OID_AUTO, ral, CTLFLAG_RW, &ral_debug, 0, "ral debug level");
#else
#define DPRINTF(x)
#define DPRINTFN(n, x)
#endif

MALLOC_DEFINE(M_RT2661, "rt2661_ratectl", "rt2661 rate control data");

static void		rt2661_dma_map_addr(void *, bus_dma_segment_t *, int,
			    int);
static void		rt2661_dma_map_mbuf(void *, bus_dma_segment_t *, int,
					    bus_size_t, int);
static int		rt2661_alloc_tx_ring(struct rt2661_softc *,
			    struct rt2661_tx_ring *, int);
static void		rt2661_reset_tx_ring(struct rt2661_softc *,
			    struct rt2661_tx_ring *);
static void		rt2661_free_tx_ring(struct rt2661_softc *,
			    struct rt2661_tx_ring *);
static int		rt2661_alloc_rx_ring(struct rt2661_softc *,
			    struct rt2661_rx_ring *, int);
static void		rt2661_reset_rx_ring(struct rt2661_softc *,
			    struct rt2661_rx_ring *);
static void		rt2661_free_rx_ring(struct rt2661_softc *,
			    struct rt2661_rx_ring *);
static struct		ieee80211_node *rt2661_node_alloc(
			    struct ieee80211_node_table *);
static int		rt2661_media_change(struct ifnet *);
static void		rt2661_next_scan(void *);
static int		rt2661_newstate(struct ieee80211com *,
			    enum ieee80211_state, int);
static uint16_t		rt2661_eeprom_read(struct rt2661_softc *, uint8_t);
static void		rt2661_rx_intr(struct rt2661_softc *);
static void		rt2661_tx_intr(struct rt2661_softc *);
static void		rt2661_tx_dma_intr(struct rt2661_softc *,
			    struct rt2661_tx_ring *);
static void		rt2661_mcu_beacon_expire(struct rt2661_softc *);
static void		rt2661_mcu_wakeup(struct rt2661_softc *);
static void		rt2661_mcu_cmd_intr(struct rt2661_softc *);
static uint16_t		rt2661_txtime(int, int, uint32_t);
static uint8_t		rt2661_rxrate(struct rt2661_rx_desc *);
static uint8_t		rt2661_plcp_signal(int);
static void		rt2661_setup_tx_desc(struct rt2661_softc *,
			    struct rt2661_tx_desc *, uint32_t, uint16_t, int,
			    int, const bus_dma_segment_t *, int, int, int);
static struct mbuf *	rt2661_get_rts(struct rt2661_softc *,
			    struct ieee80211_frame *, uint16_t);
static int		rt2661_tx_data(struct rt2661_softc *, struct mbuf *,
			    struct ieee80211_node *, int);
static int		rt2661_tx_mgt(struct rt2661_softc *, struct mbuf *,
			    struct ieee80211_node *);
static void		rt2661_start(struct ifnet *);
static void		rt2661_watchdog(struct ifnet *);
static int		rt2661_reset(struct ifnet *);
static int		rt2661_ioctl(struct ifnet *, u_long, caddr_t,
				     struct ucred *);
static void		rt2661_bbp_write(struct rt2661_softc *, uint8_t,
			    uint8_t);
static uint8_t		rt2661_bbp_read(struct rt2661_softc *, uint8_t);
static void		rt2661_rf_write(struct rt2661_softc *, uint8_t,
			    uint32_t);
static int		rt2661_tx_cmd(struct rt2661_softc *, uint8_t,
			    uint16_t);
static void		rt2661_select_antenna(struct rt2661_softc *);
static void		rt2661_enable_mrr(struct rt2661_softc *);
static void		rt2661_set_txpreamble(struct rt2661_softc *);
static void		rt2661_set_basicrates(struct rt2661_softc *,
			    const struct ieee80211_rateset *);
static void		rt2661_select_band(struct rt2661_softc *,
			    struct ieee80211_channel *);
static void		rt2661_set_chan(struct rt2661_softc *,
			    struct ieee80211_channel *);
static void		rt2661_set_bssid(struct rt2661_softc *,
			    const uint8_t *);
static void		rt2661_set_macaddr(struct rt2661_softc *,
			   const uint8_t *);
static void		rt2661_update_promisc(struct rt2661_softc *);
static int		rt2661_wme_update(struct ieee80211com *) __unused;
static void		rt2661_update_slot(struct ifnet *);
static const char	*rt2661_get_rf(int);
static void		rt2661_read_eeprom(struct rt2661_softc *);
static int		rt2661_bbp_init(struct rt2661_softc *);
static void		rt2661_init(void *);
static void		rt2661_stop(void *);
static void		rt2661_intr(void *);
static int		rt2661_load_microcode(struct rt2661_softc *,
			    const uint8_t *, int);
#ifdef notyet
static void		rt2661_rx_tune(struct rt2661_softc *);
static void		rt2661_radar_start(struct rt2661_softc *);
static int		rt2661_radar_stop(struct rt2661_softc *);
#endif
static int		rt2661_prepare_beacon(struct rt2661_softc *);
static void		rt2661_enable_tsf_sync(struct rt2661_softc *);
static int		rt2661_get_rssi(struct rt2661_softc *, uint8_t);
static void		rt2661_led_newstate(struct rt2661_softc *,
					    enum ieee80211_state);

/*
 * Supported rates for 802.11a/b/g modes (in 500Kbps unit).
 */
static const struct ieee80211_rateset rt2661_rateset_11a =
	{ 8, { 12, 18, 24, 36, 48, 72, 96, 108 } };

static const struct ieee80211_rateset rt2661_rateset_11b =
	{ 4, { 2, 4, 11, 22 } };

static const struct ieee80211_rateset rt2661_rateset_11g =
	{ 12, { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 } };

static const struct {
	uint32_t	reg;
	uint32_t	val;
} rt2661_def_mac[] = {
	RT2661_DEF_MAC
};

static const struct {
	uint8_t	reg;
	uint8_t	val;
} rt2661_def_bbp[] = {
	RT2661_DEF_BBP
};

static const struct rfprog {
	uint8_t		chan;
	uint32_t	r1, r2, r3, r4;
}  rt2661_rf5225_1[] = {
	RT2661_RF5225_1
}, rt2661_rf5225_2[] = {
	RT2661_RF5225_2
};

#define LED_EE2MCU(bit)	{ \
	.ee_bit		= RT2661_EE_LED_##bit, \
	.mcu_bit	= RT2661_MCU_LED_##bit \
}
static const struct {
	uint16_t	ee_bit;
	uint16_t	mcu_bit;
} led_ee2mcu[] = {
	LED_EE2MCU(RDYG),
	LED_EE2MCU(RDYA),
	LED_EE2MCU(ACT),
	LED_EE2MCU(GPIO0),
	LED_EE2MCU(GPIO1),
	LED_EE2MCU(GPIO2),
	LED_EE2MCU(GPIO3),
	LED_EE2MCU(GPIO4)
};
#undef LED_EE2MCU

struct rt2661_dmamap {
	bus_dma_segment_t	segs[RT2661_MAX_SCATTER];
	int			nseg;
};

int
rt2661_attach(device_t dev, int id)
{
	struct rt2661_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	uint32_t val;
	const uint8_t *ucode = NULL;
	int error, i, ac, ntries, size = 0;

	callout_init(&sc->scan_ch);
	callout_init(&sc->rssadapt_ch);

	sc->sc_irq_rid = 0;
	sc->sc_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->sc_irq_rid,
					    RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_irq == NULL) {
		device_printf(dev, "could not allocate interrupt resource\n");
		return ENXIO;
	}

	/* wait for NIC to initialize */
	for (ntries = 0; ntries < 1000; ntries++) {
		if ((val = RAL_READ(sc, RT2661_MAC_CSR0)) != 0)
			break;
		DELAY(1000);
	}
	if (ntries == 1000) {
		device_printf(sc->sc_dev,
		    "timeout waiting for NIC to initialize\n");
		error = EIO;
		goto fail;
	}

	/* retrieve RF rev. no and various other things from EEPROM */
	rt2661_read_eeprom(sc);

	device_printf(dev, "MAC/BBP RT%X, RF %s\n", val,
	    rt2661_get_rf(sc->rf_rev));

	/*
	 * Load 8051 microcode into NIC.
	 */
	switch (id) {
	case 0x0301:
		ucode = rt2561s_ucode;
		size = sizeof rt2561s_ucode;
		break;
	case 0x0302:
		ucode = rt2561_ucode;
		size = sizeof rt2561_ucode;
		break;
	case 0x0401:
		ucode = rt2661_ucode;
		size = sizeof rt2661_ucode;
		break;
	}

	error = rt2661_load_microcode(sc, ucode, size);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not load 8051 microcode\n");
		goto fail;
	}

	/*
	 * Allocate Tx and Rx rings.
	 */
	for (ac = 0; ac < 4; ac++) {
		error = rt2661_alloc_tx_ring(sc, &sc->txq[ac],
		    RT2661_TX_RING_COUNT);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "could not allocate Tx ring %d\n", ac);
			goto fail;
		}
	}

	error = rt2661_alloc_tx_ring(sc, &sc->mgtq, RT2661_MGT_RING_COUNT);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not allocate Mgt ring\n");
		goto fail;
	}

	error = rt2661_alloc_rx_ring(sc, &sc->rxq, RT2661_RX_RING_COUNT);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not allocate Rx ring\n");
		goto fail;
	}

	STAILQ_INIT(&sc->tx_ratectl);

	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
					  SYSCTL_STATIC_CHILDREN(_hw),
					  OID_AUTO,
					  device_get_nameunit(dev),
					  CTLFLAG_RD, 0, "");
	if (sc->sysctl_tree == NULL) {
		device_printf(dev, "could not add sysctl node\n");
		error = ENXIO;
		goto fail;
	}

	ifp->if_softc = sc;
	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = rt2661_init;
	ifp->if_ioctl = rt2661_ioctl;
	ifp->if_start = rt2661_start;
	ifp->if_watchdog = rt2661_watchdog;
	ifq_set_maxlen(&ifp->if_snd, IFQ_MAXLEN);
	ifq_set_ready(&ifp->if_snd);

	ic->ic_phytype = IEEE80211_T_OFDM; /* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA; /* default to BSS mode */
	ic->ic_state = IEEE80211_S_INIT;
	rt2661_led_newstate(sc, IEEE80211_S_INIT);

	/* set device capabilities */
	ic->ic_caps =
	    IEEE80211_C_IBSS |		/* IBSS mode supported */
	    IEEE80211_C_MONITOR |	/* monitor mode supported */
	    IEEE80211_C_HOSTAP |	/* HostAp mode supported */
	    IEEE80211_C_TXPMGT |	/* tx power management */
	    IEEE80211_C_SHPREAMBLE |	/* short preamble supported */
	    IEEE80211_C_SHSLOT |	/* short slot time supported */
#ifdef notyet
	    IEEE80211_C_WME |		/* 802.11e */
#endif
	    IEEE80211_C_WEP |		/* WEP */
	    IEEE80211_C_WPA;		/* 802.11i */

	if (sc->rf_rev == RT2661_RF_5225 || sc->rf_rev == RT2661_RF_5325) {
		/* set supported .11a rates */
		ic->ic_sup_rates[IEEE80211_MODE_11A] = rt2661_rateset_11a;

		/* set supported .11a channels */
		for (i = 36; i <= 64; i += 4) {
			ic->ic_channels[i].ic_freq =
			    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A;
		}
		for (i = 100; i <= 140; i += 4) {
			ic->ic_channels[i].ic_freq =
			    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A;
		}
		for (i = 149; i <= 165; i += 4) {
			ic->ic_channels[i].ic_freq =
			    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A;
		}
	}

	/* set supported .11b and .11g rates */
	ic->ic_sup_rates[IEEE80211_MODE_11B] = rt2661_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = rt2661_rateset_11g;

	/* set supported .11b and .11g channels (1 through 14) */
	for (i = 1; i <= 14; i++) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}

	ieee80211_ifattach(ic);
	ic->ic_node_alloc = rt2661_node_alloc;
/*	ic->ic_wme.wme_update = rt2661_wme_update;*/
	ic->ic_updateslot = rt2661_update_slot;
	ic->ic_reset = rt2661_reset;
	/* enable s/w bmiss handling in sta mode */
	ic->ic_flags_ext |= IEEE80211_FEXT_SWBMISS;

	/* override state transition machine */
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = rt2661_newstate;
	ieee80211_media_init(ic, rt2661_media_change, ieee80211_media_status);

	bpfattach_dlt(ifp, DLT_IEEE802_11_RADIO,
	    sizeof (struct ieee80211_frame) + 64, &sc->sc_drvbpf);

	sc->sc_rxtap_len = sizeof sc->sc_rxtapu;
	sc->sc_rxtap.wr_ihdr.it_len = htole16(sc->sc_rxtap_len);
	sc->sc_rxtap.wr_ihdr.it_present = htole32(RT2661_RX_RADIOTAP_PRESENT);

	sc->sc_txtap_len = sizeof sc->sc_txtapu;
	sc->sc_txtap.wt_ihdr.it_len = htole16(sc->sc_txtap_len);
	sc->sc_txtap.wt_ihdr.it_present = htole32(RT2661_TX_RADIOTAP_PRESENT);

	/*
	 * Add a few sysctl knobs.
	 */
	sc->dwelltime = 200;

	SYSCTL_ADD_INT(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, "dwell",
	    CTLFLAG_RW, &sc->dwelltime, 0,
	    "channel dwell time (ms) for AP/station scanning");

	error = bus_setup_intr(dev, sc->sc_irq, INTR_MPSAFE, rt2661_intr,
			       sc, &sc->sc_ih, ifp->if_serializer);
	if (error != 0) {
		device_printf(dev, "could not set up interrupt\n");
		bpfdetach(ifp);
		ieee80211_ifdetach(ic);
		goto fail;
	}

	if (bootverbose)
		ieee80211_announce(ic);
	return 0;
fail:
	rt2661_detach(sc);
	return error;
}

int
rt2661_detach(void *xsc)
{
	struct rt2661_softc *sc = xsc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;

	if (device_is_attached(sc->sc_dev)) {
		lwkt_serialize_enter(ifp->if_serializer);

		callout_stop(&sc->scan_ch);
		callout_stop(&sc->rssadapt_ch);
		rt2661_stop(sc);
		bus_teardown_intr(sc->sc_dev, sc->sc_irq, sc->sc_ih);

		lwkt_serialize_exit(ifp->if_serializer);

		bpfdetach(ifp);
		ieee80211_ifdetach(ic);
	}

	rt2661_free_tx_ring(sc, &sc->txq[0]);
	rt2661_free_tx_ring(sc, &sc->txq[1]);
	rt2661_free_tx_ring(sc, &sc->txq[2]);
	rt2661_free_tx_ring(sc, &sc->txq[3]);
	rt2661_free_tx_ring(sc, &sc->mgtq);
	rt2661_free_rx_ring(sc, &sc->rxq);

	if (sc->sc_irq != NULL) {
		bus_release_resource(sc->sc_dev, SYS_RES_IRQ, sc->sc_irq_rid,
				     sc->sc_irq);
	}

	if (sc->sysctl_tree != NULL)
		sysctl_ctx_free(&sc->sysctl_ctx);

	return 0;
}

void
rt2661_shutdown(void *xsc)
{
	struct rt2661_softc *sc = xsc;
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	lwkt_serialize_enter(ifp->if_serializer);
	rt2661_stop(sc);
	lwkt_serialize_exit(ifp->if_serializer);
}

void
rt2661_suspend(void *xsc)
{
	struct rt2661_softc *sc = xsc;
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	lwkt_serialize_enter(ifp->if_serializer);
	rt2661_stop(sc);
	lwkt_serialize_exit(ifp->if_serializer);
}

void
rt2661_resume(void *xsc)
{
	struct rt2661_softc *sc = xsc;
	struct ifnet *ifp = sc->sc_ic.ic_ifp;

	lwkt_serialize_enter(ifp->if_serializer);
	if (ifp->if_flags & IFF_UP) {
		ifp->if_init(ifp->if_softc);
		if (ifp->if_flags & IFF_RUNNING)
			ifp->if_start(ifp);
	}
	lwkt_serialize_exit(ifp->if_serializer);
}

static void
rt2661_dma_map_addr(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	if (error != 0)
		return;

	KASSERT(nseg == 1, ("too many DMA segments, %d should be 1", nseg));

	*(bus_addr_t *)arg = segs[0].ds_addr;
}

static int
rt2661_alloc_tx_ring(struct rt2661_softc *sc, struct rt2661_tx_ring *ring,
    int count)
{
	int i, error;

	ring->count = count;
	ring->queued = 0;
	ring->cur = ring->next = 0;

	error = bus_dma_tag_create(NULL, 4, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, count * RT2661_TX_DESC_SIZE, 1,
	    count * RT2661_TX_DESC_SIZE, 0, &ring->desc_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not create desc DMA tag\n");
		goto fail;
	}

	error = bus_dmamem_alloc(ring->desc_dmat, (void **)&ring->desc,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO, &ring->desc_map);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not allocate DMA memory\n");
		goto fail;
	}

	error = bus_dmamap_load(ring->desc_dmat, ring->desc_map, ring->desc,
	    count * RT2661_TX_DESC_SIZE, rt2661_dma_map_addr, &ring->physaddr,
	    0);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not load desc DMA map\n");

		bus_dmamem_free(ring->desc_dmat, ring->desc, ring->desc_map);
		ring->desc = NULL;
		goto fail;
	}

	ring->data = kmalloc(count * sizeof (struct rt2661_data), M_DEVBUF,
	    M_WAITOK | M_ZERO);

	error = bus_dma_tag_create(NULL, 1, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES * RT2661_MAX_SCATTER,
	    RT2661_MAX_SCATTER, MCLBYTES, 0, &ring->data_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not create data DMA tag\n");
		goto fail;
	}

	for (i = 0; i < count; i++) {
		error = bus_dmamap_create(ring->data_dmat, 0,
		    &ring->data[i].map);
		if (error != 0) {
			device_printf(sc->sc_dev, "could not create DMA map\n");
			goto fail;
		}
	}
	return 0;

fail:	rt2661_free_tx_ring(sc, ring);
	return error;
}

static void
rt2661_reset_tx_ring(struct rt2661_softc *sc, struct rt2661_tx_ring *ring)
{
	struct rt2661_tx_desc *desc;
	struct rt2661_data *data;
	int i;

	for (i = 0; i < ring->count; i++) {
		desc = &ring->desc[i];
		data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(ring->data_dmat, data->map,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(ring->data_dmat, data->map);
			m_freem(data->m);
			data->m = NULL;
		}

		desc->flags = 0;
	}

	bus_dmamap_sync(ring->desc_dmat, ring->desc_map, BUS_DMASYNC_PREWRITE);

	ring->queued = 0;
	ring->cur = ring->next = 0;
}

static void
rt2661_free_tx_ring(struct rt2661_softc *sc, struct rt2661_tx_ring *ring)
{
	struct rt2661_data *data;
	int i;

	if (ring->desc != NULL) {
		bus_dmamap_sync(ring->desc_dmat, ring->desc_map,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(ring->desc_dmat, ring->desc_map);
		bus_dmamem_free(ring->desc_dmat, ring->desc, ring->desc_map);
		ring->desc = NULL;
	}

	if (ring->desc_dmat != NULL) {
		bus_dma_tag_destroy(ring->desc_dmat);
		ring->desc_dmat = NULL;
	}

	if (ring->data != NULL) {
		for (i = 0; i < ring->count; i++) {
			data = &ring->data[i];

			if (data->m != NULL) {
				bus_dmamap_sync(ring->data_dmat, data->map,
				    BUS_DMASYNC_POSTWRITE);
				bus_dmamap_unload(ring->data_dmat, data->map);
				m_freem(data->m);
				data->m = NULL;
			}

			if (data->map != NULL) {
				bus_dmamap_destroy(ring->data_dmat, data->map);
				data->map = NULL;
			}
		}

		kfree(ring->data, M_DEVBUF);
		ring->data = NULL;
	}

	if (ring->data_dmat != NULL) {
		bus_dma_tag_destroy(ring->data_dmat);
		ring->data_dmat = NULL;
	}
}

static int
rt2661_alloc_rx_ring(struct rt2661_softc *sc, struct rt2661_rx_ring *ring,
    int count)
{
	struct rt2661_rx_desc *desc;
	struct rt2661_data *data;
	bus_addr_t physaddr;
	int i, error;

	ring->count = count;
	ring->cur = ring->next = 0;

	error = bus_dma_tag_create(NULL, 4, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, count * RT2661_RX_DESC_SIZE, 1,
	    count * RT2661_RX_DESC_SIZE, 0, &ring->desc_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not create desc DMA tag\n");
		goto fail;
	}

	error = bus_dmamem_alloc(ring->desc_dmat, (void **)&ring->desc,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO, &ring->desc_map);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not allocate DMA memory\n");
		goto fail;
	}

	error = bus_dmamap_load(ring->desc_dmat, ring->desc_map, ring->desc,
	    count * RT2661_RX_DESC_SIZE, rt2661_dma_map_addr, &ring->physaddr,
	    0);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not load desc DMA map\n");

		bus_dmamem_free(ring->desc_dmat, ring->desc, ring->desc_map);
		ring->desc = NULL;
		goto fail;
	}

	ring->data = kmalloc(count * sizeof (struct rt2661_data), M_DEVBUF,
	    M_WAITOK | M_ZERO);

	/*
	 * Pre-allocate Rx buffers and populate Rx ring.
	 */
	error = bus_dma_tag_create(NULL, 1, 0, BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES, 1, MCLBYTES, 0,
	    &ring->data_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not create data DMA tag\n");
		goto fail;
	}

	for (i = 0; i < count; i++) {
		desc = &sc->rxq.desc[i];
		data = &sc->rxq.data[i];

		error = bus_dmamap_create(ring->data_dmat, 0, &data->map);
		if (error != 0) {
			device_printf(sc->sc_dev, "could not create DMA map\n");
			goto fail;
		}

		data->m = m_getcl(MB_DONTWAIT, MT_DATA, M_PKTHDR);
		if (data->m == NULL) {
			device_printf(sc->sc_dev,
			    "could not allocate rx mbuf\n");
			error = ENOMEM;
			goto fail;
		}

		error = bus_dmamap_load(ring->data_dmat, data->map,
		    mtod(data->m, void *), MCLBYTES, rt2661_dma_map_addr,
		    &physaddr, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "could not load rx buf DMA map");

			m_freem(data->m);
			data->m = NULL;
			goto fail;
		}

		desc->flags = htole32(RT2661_RX_BUSY);
		desc->physaddr = htole32(physaddr);
	}

	bus_dmamap_sync(ring->desc_dmat, ring->desc_map, BUS_DMASYNC_PREWRITE);

	return 0;

fail:	rt2661_free_rx_ring(sc, ring);
	return error;
}

static void
rt2661_reset_rx_ring(struct rt2661_softc *sc, struct rt2661_rx_ring *ring)
{
	int i;

	for (i = 0; i < ring->count; i++)
		ring->desc[i].flags = htole32(RT2661_RX_BUSY);

	bus_dmamap_sync(ring->desc_dmat, ring->desc_map, BUS_DMASYNC_PREWRITE);

	ring->cur = ring->next = 0;
}

static void
rt2661_free_rx_ring(struct rt2661_softc *sc, struct rt2661_rx_ring *ring)
{
	struct rt2661_data *data;
	int i;

	if (ring->desc != NULL) {
		bus_dmamap_sync(ring->desc_dmat, ring->desc_map,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(ring->desc_dmat, ring->desc_map);
		bus_dmamem_free(ring->desc_dmat, ring->desc, ring->desc_map);
		ring->desc = NULL;
	}

	if (ring->desc_dmat != NULL) {
		bus_dma_tag_destroy(ring->desc_dmat);
		ring->desc_dmat = NULL;
	}

	if (ring->data != NULL) {
		for (i = 0; i < ring->count; i++) {
			data = &ring->data[i];

			if (data->m != NULL) {
				bus_dmamap_sync(ring->data_dmat, data->map,
				    BUS_DMASYNC_POSTREAD);
				bus_dmamap_unload(ring->data_dmat, data->map);
				m_freem(data->m);
				data->m = NULL;
			}

			if (data->map != NULL) {
				bus_dmamap_destroy(ring->data_dmat, data->map);
				data->map = NULL;
			}
		}

		kfree(ring->data, M_DEVBUF);
		ring->data = NULL;
	}

	if (ring->data_dmat != NULL) {
		bus_dma_tag_destroy(ring->data_dmat);
		ring->data_dmat = NULL;
	}
}

static struct ieee80211_node *
rt2661_node_alloc(struct ieee80211_node_table *nt)
{
	struct rt2661_node *rn;

	rn = kmalloc(sizeof (struct rt2661_node), M_80211_NODE,
	    M_NOWAIT | M_ZERO);

	return (rn != NULL) ? &rn->ni : NULL;
}

static int
rt2661_media_change(struct ifnet *ifp)
{
	struct rt2661_softc *sc = ifp->if_softc;
	int error;

	error = ieee80211_media_change(ifp);
	if (error != ENETRESET)
		return error;

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING))
		rt2661_init(sc);
	return 0;
}

/*
 * This function is called periodically (every 200ms) during scanning to
 * switch from one channel to another.
 */
static void
rt2661_next_scan(void *arg)
{
	struct rt2661_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;

	lwkt_serialize_enter(ifp->if_serializer);
	if (ic->ic_state == IEEE80211_S_SCAN)
		ieee80211_next_scan(ic);
	lwkt_serialize_exit(ifp->if_serializer);
}

/*
 * This function is called for each node present in the node station table.
 */
static void
rt2661_iter_func(void *arg, struct ieee80211_node *ni)
{
	struct rt2661_node *rn = (struct rt2661_node *)ni;

	ral_rssadapt_updatestats(&rn->rssadapt);
}

/*
 * This function is called periodically (every 100ms) in RUN state to update
 * the rate adaptation statistics.
 */
static void
rt2661_update_rssadapt(void *arg)
{
	struct rt2661_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;

	lwkt_serialize_enter(ifp->if_serializer);

	ieee80211_iterate_nodes(&ic->ic_sta, rt2661_iter_func, arg);
	callout_reset(&sc->rssadapt_ch, hz / 10, rt2661_update_rssadapt, sc);

	lwkt_serialize_exit(ifp->if_serializer);
}

static int
rt2661_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct rt2661_softc *sc = ic->ic_ifp->if_softc;
	enum ieee80211_state ostate;
	struct ieee80211_node *ni;
	uint32_t tmp;
	int error = 0;

	ostate = ic->ic_state;
	callout_stop(&sc->scan_ch);

	if (ostate != nstate)
		rt2661_led_newstate(sc, nstate);

	switch (nstate) {
	case IEEE80211_S_INIT:
		callout_stop(&sc->rssadapt_ch);

		if (ostate == IEEE80211_S_RUN) {
			/* abort TSF synchronization */
			tmp = RAL_READ(sc, RT2661_TXRX_CSR9);
			RAL_WRITE(sc, RT2661_TXRX_CSR9, tmp & ~0x00ffffff);
		}
		break;

	case IEEE80211_S_SCAN:
		rt2661_set_chan(sc, ic->ic_curchan);
		callout_reset(&sc->scan_ch, (sc->dwelltime * hz) / 1000,
		    rt2661_next_scan, sc);
		break;

	case IEEE80211_S_AUTH:
	case IEEE80211_S_ASSOC:
		rt2661_set_chan(sc, ic->ic_curchan);
		break;

	case IEEE80211_S_RUN:
		rt2661_set_chan(sc, ic->ic_curchan);

		ni = ic->ic_bss;

		if (ic->ic_opmode != IEEE80211_M_MONITOR) {
			rt2661_enable_mrr(sc);
			rt2661_set_txpreamble(sc);
			rt2661_set_basicrates(sc, &ni->ni_rates);
			rt2661_set_bssid(sc, ni->ni_bssid);
		}

		if (ic->ic_opmode == IEEE80211_M_HOSTAP ||
		    ic->ic_opmode == IEEE80211_M_IBSS) {
			if ((error = rt2661_prepare_beacon(sc)) != 0)
				break;
		}

		if (ic->ic_opmode != IEEE80211_M_MONITOR) {
			callout_reset(&sc->rssadapt_ch, hz / 10,
			    rt2661_update_rssadapt, sc);
			rt2661_enable_tsf_sync(sc);
		}
		break;
	}	

	return (error != 0) ? error : sc->sc_newstate(ic, nstate, arg);
}

/*
 * Read 16 bits at address 'addr' from the serial EEPROM (either 93C46 or
 * 93C66).
 */
static uint16_t
rt2661_eeprom_read(struct rt2661_softc *sc, uint8_t addr)
{
	uint32_t tmp;
	uint16_t val;
	int n;

	/* clock C once before the first command */
	RT2661_EEPROM_CTL(sc, 0);

	RT2661_EEPROM_CTL(sc, RT2661_S);
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_C);
	RT2661_EEPROM_CTL(sc, RT2661_S);

	/* write start bit (1) */
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_D);
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_D | RT2661_C);

	/* write READ opcode (10) */
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_D);
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_D | RT2661_C);
	RT2661_EEPROM_CTL(sc, RT2661_S);
	RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_C);

	/* write address (A5-A0 or A7-A0) */
	n = (RAL_READ(sc, RT2661_E2PROM_CSR) & RT2661_93C46) ? 5 : 7;
	for (; n >= 0; n--) {
		RT2661_EEPROM_CTL(sc, RT2661_S |
		    (((addr >> n) & 1) << RT2661_SHIFT_D));
		RT2661_EEPROM_CTL(sc, RT2661_S |
		    (((addr >> n) & 1) << RT2661_SHIFT_D) | RT2661_C);
	}

	RT2661_EEPROM_CTL(sc, RT2661_S);

	/* read data Q15-Q0 */
	val = 0;
	for (n = 15; n >= 0; n--) {
		RT2661_EEPROM_CTL(sc, RT2661_S | RT2661_C);
		tmp = RAL_READ(sc, RT2661_E2PROM_CSR);
		val |= ((tmp & RT2661_Q) >> RT2661_SHIFT_Q) << n;
		RT2661_EEPROM_CTL(sc, RT2661_S);
	}

	RT2661_EEPROM_CTL(sc, 0);

	/* clear Chip Select and clock C */
	RT2661_EEPROM_CTL(sc, RT2661_S);
	RT2661_EEPROM_CTL(sc, 0);
	RT2661_EEPROM_CTL(sc, RT2661_C);

	return val;
}

static void
rt2661_tx_intr(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	struct rt2661_tx_ratectl *rctl;
	struct rt2661_node *rn;
	uint32_t val, result;
	int retrycnt;

	for (;;) {
		val = RAL_READ(sc, RT2661_STA_CSR4);
		if (!(val & RT2661_TX_STAT_VALID))
			break;

		/* Gather statistics */
		result = RT2661_TX_RESULT(val);
		if (result == RT2661_TX_SUCCESS)
			ifp->if_opackets++;
		else
			ifp->if_oerrors++;

		/* No rate control */
		if (RT2661_TX_QID(val) == 0)
			continue;

		/* retrieve rate control algorithm context */
		rctl = STAILQ_FIRST(&sc->tx_ratectl);
		if (rctl == NULL) {
			/*
			 * XXX
			 * This really should not happen.  Maybe we should
			 * use assertion here?  But why should we rely on
			 * hardware to do the correct things?  Even the
			 * reference driver (RT61?) provided by Ralink does
			 * not provide enough clue that this kind of interrupt
			 * is promised to be generated for each packet.  So
			 * just print a message and keep going ...
			 */
			if_printf(ifp, "WARNING: no rate control information\n");
			continue;
		}
		STAILQ_REMOVE_HEAD(&sc->tx_ratectl, link);

		rn = (struct rt2661_node *)rctl->ni;

		switch (result) {
		case RT2661_TX_SUCCESS:
			retrycnt = RT2661_TX_RETRYCNT(val);

			DPRINTFN(10, ("data frame sent successfully after "
			    "%d retries\n", retrycnt));
			if (retrycnt == 0 && rctl->id.id_node != NULL) {
				ral_rssadapt_raise_rate(ic, &rn->rssadapt,
				    &rctl->id);
			}
			break;

		case RT2661_TX_RETRY_FAIL:
			DPRINTFN(9, ("sending data frame failed (too much "
			    "retries)\n"));
			if (rctl->id.id_node != NULL) {
				ral_rssadapt_lower_rate(ic, rctl->ni,
				    &rn->rssadapt, &rctl->id);
			}
			break;

		default:
			/* other failure */
			device_printf(sc->sc_dev,
			    "sending data frame failed 0x%08x\n", val);
			break;
		}

		ieee80211_free_node(rctl->ni);
		rctl->ni = NULL;
		kfree(rctl, M_RT2661);
	}
}

static void
rt2661_tx_dma_intr(struct rt2661_softc *sc, struct rt2661_tx_ring *txq)
{
	struct rt2661_tx_desc *desc;
	struct rt2661_data *data;

	bus_dmamap_sync(txq->desc_dmat, txq->desc_map, BUS_DMASYNC_POSTREAD);

	for (;;) {
		desc = &txq->desc[txq->next];
		data = &txq->data[txq->next];

		if ((le32toh(desc->flags) & RT2661_TX_BUSY) ||
		    !(le32toh(desc->flags) & RT2661_TX_VALID))
			break;

		bus_dmamap_sync(txq->data_dmat, data->map,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(txq->data_dmat, data->map);
		m_freem(data->m);
		data->m = NULL;

		/* descriptor is no longer valid */
		desc->flags &= ~htole32(RT2661_TX_VALID);

		DPRINTFN(15, ("tx dma done q=%p idx=%u\n", txq, txq->next));

		txq->queued--;
		if (++txq->next >= txq->count)	/* faster than % count */
			txq->next = 0;
	}

	bus_dmamap_sync(txq->desc_dmat, txq->desc_map, BUS_DMASYNC_PREWRITE);

	if (txq->queued < txq->count) {
		struct ifnet *ifp = &sc->sc_ic.ic_if;

		sc->sc_tx_timer = 0;
		ifp->if_flags &= ~IFF_OACTIVE;
		rt2661_start(ifp);
	}
}

static void
rt2661_rx_intr(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	struct rt2661_rx_desc *desc;
	struct rt2661_data *data;
	bus_addr_t physaddr;
	struct ieee80211_frame_min *wh;
	struct ieee80211_node *ni;
	struct rt2661_node *rn;
	struct mbuf *mnew, *m;
	int error;

	bus_dmamap_sync(sc->rxq.desc_dmat, sc->rxq.desc_map,
	    BUS_DMASYNC_POSTREAD);

	for (;;) {
		int rssi;

		desc = &sc->rxq.desc[sc->rxq.cur];
		data = &sc->rxq.data[sc->rxq.cur];

		if (le32toh(desc->flags) & RT2661_RX_BUSY)
			break;

		if ((le32toh(desc->flags) & RT2661_RX_PHY_ERROR) ||
		    (le32toh(desc->flags) & RT2661_RX_CRC_ERROR)) {
			/*
			 * This should not happen since we did not request
			 * to receive those frames when we filled TXRX_CSR0.
			 */
			DPRINTFN(5, ("PHY or CRC error flags 0x%08x\n",
			    le32toh(desc->flags)));
			ifp->if_ierrors++;
			goto skip;
		}

		if ((le32toh(desc->flags) & RT2661_RX_CIPHER_MASK) != 0) {
			ifp->if_ierrors++;
			goto skip;
		}

		/*
		 * Try to allocate a new mbuf for this ring element and load it
		 * before processing the current mbuf. If the ring element
		 * cannot be loaded, drop the received packet and reuse the old
		 * mbuf. In the unlikely case that the old mbuf can't be
		 * reloaded either, explicitly panic.
		 */
		mnew = m_getcl(MB_DONTWAIT, MT_DATA, M_PKTHDR);
		if (mnew == NULL) {
			ifp->if_ierrors++;
			goto skip;
		}

		bus_dmamap_sync(sc->rxq.data_dmat, data->map,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->rxq.data_dmat, data->map);

		error = bus_dmamap_load(sc->rxq.data_dmat, data->map,
		    mtod(mnew, void *), MCLBYTES, rt2661_dma_map_addr,
		    &physaddr, 0);
		if (error != 0) {
			m_freem(mnew);

			/* try to reload the old mbuf */
			error = bus_dmamap_load(sc->rxq.data_dmat, data->map,
			    mtod(data->m, void *), MCLBYTES,
			    rt2661_dma_map_addr, &physaddr, 0);
			if (error != 0) {
				/* very unlikely that it will fail... */
				panic("%s: could not load old rx mbuf",
				    device_get_name(sc->sc_dev));
			}
			ifp->if_ierrors++;
			goto skip;
		}

		/*
	 	 * New mbuf successfully loaded, update Rx ring and continue
		 * processing.
		 */
		m = data->m;
		data->m = mnew;
		desc->physaddr = htole32(physaddr);

		/* finalize mbuf */
		m->m_pkthdr.rcvif = ifp;
		m->m_pkthdr.len = m->m_len =
		    (le32toh(desc->flags) >> 16) & 0xfff;

		rssi = rt2661_get_rssi(sc, desc->rssi);

		wh = mtod(m, struct ieee80211_frame_min *);
		ni = ieee80211_find_rxnode(ic, wh);

		/* Error happened during RSSI conversion. */
		if (rssi < 0)
			rssi = ni->ni_rssi;

		if (sc->sc_drvbpf != NULL) {
			struct rt2661_rx_radiotap_header *tap = &sc->sc_rxtap;
			uint32_t tsf_lo, tsf_hi;

			/* get timestamp (low and high 32 bits) */
			tsf_hi = RAL_READ(sc, RT2661_TXRX_CSR13);
			tsf_lo = RAL_READ(sc, RT2661_TXRX_CSR12);

			tap->wr_tsf =
			    htole64(((uint64_t)tsf_hi << 32) | tsf_lo);
			tap->wr_flags = 0;
			tap->wr_rate = rt2661_rxrate(desc);
			tap->wr_chan_freq = htole16(ic->ic_curchan->ic_freq);
			tap->wr_chan_flags = htole16(ic->ic_curchan->ic_flags);
			tap->wr_antsignal = rssi;

			bpf_ptap(sc->sc_drvbpf, m, tap, sc->sc_rxtap_len);
		}

		/* send the frame to the 802.11 layer */
		ieee80211_input(ic, m, ni, rssi, 0);

		/* give rssi to the rate adatation algorithm */
		rn = (struct rt2661_node *)ni;
		ral_rssadapt_input(ic, ni, &rn->rssadapt, rssi);

		/* node is no longer needed */
		ieee80211_free_node(ni);

skip:		desc->flags |= htole32(RT2661_RX_BUSY);

		DPRINTFN(15, ("rx intr idx=%u\n", sc->rxq.cur));

		sc->rxq.cur = (sc->rxq.cur + 1) % RT2661_RX_RING_COUNT;
	}

	bus_dmamap_sync(sc->rxq.desc_dmat, sc->rxq.desc_map,
	    BUS_DMASYNC_PREWRITE);
}

/* ARGSUSED */
static void
rt2661_mcu_beacon_expire(struct rt2661_softc *sc)
{
	/* do nothing */
}

static void
rt2661_mcu_wakeup(struct rt2661_softc *sc)
{
	RAL_WRITE(sc, RT2661_MAC_CSR11, 5 << 16);

	RAL_WRITE(sc, RT2661_SOFT_RESET_CSR, 0x7);
	RAL_WRITE(sc, RT2661_IO_CNTL_CSR, 0x18);
	RAL_WRITE(sc, RT2661_PCI_USEC_CSR, 0x20);

	/* send wakeup command to MCU */
	rt2661_tx_cmd(sc, RT2661_MCU_CMD_WAKEUP, 0);
}

static void
rt2661_mcu_cmd_intr(struct rt2661_softc *sc)
{
	RAL_READ(sc, RT2661_M2H_CMD_DONE_CSR);
	RAL_WRITE(sc, RT2661_M2H_CMD_DONE_CSR, 0xffffffff);
}

static void
rt2661_intr(void *arg)
{
	struct rt2661_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	uint32_t r1, r2;

	/* disable MAC and MCU interrupts */
	RAL_WRITE(sc, RT2661_INT_MASK_CSR, 0xffffff7f);
	RAL_WRITE(sc, RT2661_MCU_INT_MASK_CSR, 0xffffffff);

	/* don't re-enable interrupts if we're shutting down */
	if (!(ifp->if_flags & IFF_RUNNING))
		return;

	r1 = RAL_READ(sc, RT2661_INT_SOURCE_CSR);
	RAL_WRITE(sc, RT2661_INT_SOURCE_CSR, r1);

	r2 = RAL_READ(sc, RT2661_MCU_INT_SOURCE_CSR);
	RAL_WRITE(sc, RT2661_MCU_INT_SOURCE_CSR, r2);

	if (r1 & RT2661_MGT_DONE)
		rt2661_tx_dma_intr(sc, &sc->mgtq);

	if (r1 & RT2661_RX_DONE)
		rt2661_rx_intr(sc);

	if (r1 & RT2661_TX0_DMA_DONE)
		rt2661_tx_dma_intr(sc, &sc->txq[0]);

	if (r1 & RT2661_TX1_DMA_DONE)
		rt2661_tx_dma_intr(sc, &sc->txq[1]);

	if (r1 & RT2661_TX2_DMA_DONE)
		rt2661_tx_dma_intr(sc, &sc->txq[2]);

	if (r1 & RT2661_TX3_DMA_DONE)
		rt2661_tx_dma_intr(sc, &sc->txq[3]);

	if (r1 & RT2661_TX_DONE)
		rt2661_tx_intr(sc);

	if (r2 & RT2661_MCU_CMD_DONE)
		rt2661_mcu_cmd_intr(sc);

	if (r2 & RT2661_MCU_BEACON_EXPIRE)
		rt2661_mcu_beacon_expire(sc);

	if (r2 & RT2661_MCU_WAKEUP)
		rt2661_mcu_wakeup(sc);

	/* re-enable MAC and MCU interrupts */
	RAL_WRITE(sc, RT2661_INT_MASK_CSR, 0x0000ff10);
	RAL_WRITE(sc, RT2661_MCU_INT_MASK_CSR, 0);
}

/* quickly determine if a given rate is CCK or OFDM */
#define RAL_RATE_IS_OFDM(rate) ((rate) >= 12 && (rate) != 22)

#define RAL_ACK_SIZE	14	/* 10 + 4(FCS) */
#define RAL_CTS_SIZE	14	/* 10 + 4(FCS) */

#define RAL_SIFS	10	/* us */

/*
 * This function is only used by the Rx radiotap code. It returns the rate at
 * which a given frame was received.
 */
static uint8_t
rt2661_rxrate(struct rt2661_rx_desc *desc)
{
	if (le32toh(desc->flags) & RT2661_RX_OFDM) {
		/* reverse function of rt2661_plcp_signal */
		switch (desc->rate & 0xf) {
		case 0xb:	return 12;
		case 0xf:	return 18;
		case 0xa:	return 24;
		case 0xe:	return 36;
		case 0x9:	return 48;
		case 0xd:	return 72;
		case 0x8:	return 96;
		case 0xc:	return 108;
		}
	} else {
		if (desc->rate == 10)
			return 2;
		if (desc->rate == 20)
			return 4;
		if (desc->rate == 55)
			return 11;
		if (desc->rate == 110)
			return 22;
	}
	return 2;	/* should not get there */
}

/*
 * Compute the duration (in us) needed to transmit `len' bytes at rate `rate'.
 * The function automatically determines the operating mode depending on the
 * given rate. `flags' indicates whether short preamble is in use or not.
 */
static uint16_t
rt2661_txtime(int len, int rate, uint32_t flags)
{
	uint16_t txtime;

	if (RAL_RATE_IS_OFDM(rate)) {
		/* IEEE Std 802.11a-1999, pp. 37 */
		txtime = (8 + 4 * len + 3 + rate - 1) / rate;
		txtime = 16 + 4 + 4 * txtime + 6;
	} else {
		/* IEEE Std 802.11b-1999, pp. 28 */
		txtime = (16 * len + rate - 1) / rate;
		if (rate != 2 && (flags & IEEE80211_F_SHPREAMBLE))
			txtime +=  72 + 24;
		else
			txtime += 144 + 48;
	}

	return txtime;
}

static uint8_t
rt2661_plcp_signal(int rate)
{
	switch (rate) {
	/* CCK rates (returned values are device-dependent) */
	case 2:		return 0x0;
	case 4:		return 0x1;
	case 11:	return 0x2;
	case 22:	return 0x3;

	/* OFDM rates (cf IEEE Std 802.11a-1999, pp. 14 Table 80) */
	case 12:	return 0xb;
	case 18:	return 0xf;
	case 24:	return 0xa;
	case 36:	return 0xe;
	case 48:	return 0x9;
	case 72:	return 0xd;
	case 96:	return 0x8;
	case 108:	return 0xc;

	/* unsupported rates (should not get there) */
	default:	return 0xff;
	}
}

static void
rt2661_setup_tx_desc(struct rt2661_softc *sc, struct rt2661_tx_desc *desc,
    uint32_t flags, uint16_t xflags, int len, int rate,
    const bus_dma_segment_t *segs, int nsegs, int ac, int ratectl)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint16_t plcp_length;
	int i, remainder;

	desc->flags = htole32(flags);
	desc->flags |= htole32(len << 16);
	desc->flags |= htole32(RT2661_TX_VALID);

	desc->xflags = htole16(xflags);
	desc->xflags |= htole16(nsegs << 13);

	desc->wme = htole16(
	    RT2661_QID(ac) |
	    RT2661_AIFSN(2) |
	    RT2661_LOGCWMIN(4) |
	    RT2661_LOGCWMAX(10));

	/*
	 * Remember whether TX rate control information should be gathered.
	 * This field is driver private data only.  It will be made available
	 * by the NIC in STA_CSR4 on Tx done interrupts.
	 */
	desc->qid = ratectl;

	/* setup PLCP fields */
	desc->plcp_signal  = rt2661_plcp_signal(rate);
	desc->plcp_service = 4;

	len += IEEE80211_CRC_LEN;
	if (RAL_RATE_IS_OFDM(rate)) {
		desc->flags |= htole32(RT2661_TX_OFDM);

		plcp_length = len & 0xfff;
		desc->plcp_length_hi = plcp_length >> 6;
		desc->plcp_length_lo = plcp_length & 0x3f;
	} else {
		plcp_length = (16 * len + rate - 1) / rate;
		if (rate == 22) {
			remainder = (16 * len) % 22;
			if (remainder != 0 && remainder < 7)
				desc->plcp_service |= RT2661_PLCP_LENGEXT;
		}
		desc->plcp_length_hi = plcp_length >> 8;
		desc->plcp_length_lo = plcp_length & 0xff;

		if (rate != 2 && (ic->ic_flags & IEEE80211_F_SHPREAMBLE))
			desc->plcp_signal |= 0x08;
	}

	/* RT2x61 supports scatter with up to 5 segments */
	for (i = 0; i < nsegs; i++) {
		desc->addr[i] = htole32(segs[i].ds_addr);
		desc->len [i] = htole16(segs[i].ds_len);
	}

	desc->flags |= htole32(RT2661_TX_BUSY);
}

static int
rt2661_tx_mgt(struct rt2661_softc *sc, struct mbuf *m0,
    struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct rt2661_tx_desc *desc;
	struct rt2661_data *data;
	struct ieee80211_frame *wh;
	struct rt2661_dmamap map;
	uint16_t dur;
	uint32_t flags = 0;	/* XXX HWSEQ */
	int rate, error;

	desc = &sc->mgtq.desc[sc->mgtq.cur];
	data = &sc->mgtq.data[sc->mgtq.cur];

	/* send mgt frames at the lowest available rate */
	rate = IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan) ? 12 : 2;

	error = bus_dmamap_load_mbuf(sc->mgtq.data_dmat, data->map, m0,
				     rt2661_dma_map_mbuf, &map, 0);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not map mbuf (error %d)\n",
		    error);
		m_freem(m0);
		return error;
	}

	if (sc->sc_drvbpf != NULL) {
		struct rt2661_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		tap->wt_rate = rate;
		tap->wt_chan_freq = htole16(ic->ic_curchan->ic_freq);
		tap->wt_chan_flags = htole16(ic->ic_curchan->ic_flags);

		bpf_ptap(sc->sc_drvbpf, m0, tap, sc->sc_txtap_len);
	}

	data->m = m0;

	wh = mtod(m0, struct ieee80211_frame *);

	if (!IEEE80211_IS_MULTICAST(wh->i_addr1)) {
		flags |= RT2661_TX_NEED_ACK;

		dur = rt2661_txtime(RAL_ACK_SIZE, rate, ic->ic_flags) +
		    RAL_SIFS;
		*(uint16_t *)wh->i_dur = htole16(dur);

		/* tell hardware to add timestamp in probe responses */
		if ((wh->i_fc[0] &
		    (IEEE80211_FC0_TYPE_MASK | IEEE80211_FC0_SUBTYPE_MASK)) ==
		    (IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_PROBE_RESP))
			flags |= RT2661_TX_TIMESTAMP;
	}

	rt2661_setup_tx_desc(sc, desc, flags, 0 /* XXX HWSEQ */,
	    m0->m_pkthdr.len, rate, map.segs, map.nseg, RT2661_QID_MGT, 0);

	bus_dmamap_sync(sc->mgtq.data_dmat, data->map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(sc->mgtq.desc_dmat, sc->mgtq.desc_map,
	    BUS_DMASYNC_PREWRITE);

	DPRINTFN(10, ("sending mgt frame len=%u idx=%u rate=%u\n",
	    m0->m_pkthdr.len, sc->mgtq.cur, rate));

	/* kick mgt */
	sc->mgtq.queued++;
	sc->mgtq.cur = (sc->mgtq.cur + 1) % RT2661_MGT_RING_COUNT;
	RAL_WRITE(sc, RT2661_TX_CNTL_CSR, RT2661_KICK_MGT);

	ieee80211_free_node(ni);

	return 0;
}

/*
 * Build a RTS control frame.
 */
static struct mbuf *
rt2661_get_rts(struct rt2661_softc *sc, struct ieee80211_frame *wh,
    uint16_t dur)
{
	struct ieee80211_frame_rts *rts;
	struct mbuf *m;

	MGETHDR(m, MB_DONTWAIT, MT_DATA);
	if (m == NULL) {
		sc->sc_ic.ic_stats.is_tx_nobuf++;
		device_printf(sc->sc_dev, "could not allocate RTS frame\n");
		return NULL;
	}

	rts = mtod(m, struct ieee80211_frame_rts *);

	rts->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_CTL |
	    IEEE80211_FC0_SUBTYPE_RTS;
	rts->i_fc[1] = IEEE80211_FC1_DIR_NODS;
	*(uint16_t *)rts->i_dur = htole16(dur);
	IEEE80211_ADDR_COPY(rts->i_ra, wh->i_addr1);
	IEEE80211_ADDR_COPY(rts->i_ta, wh->i_addr2);

	m->m_pkthdr.len = m->m_len = sizeof (struct ieee80211_frame_rts);

	return m;
}

static int
rt2661_tx_data(struct rt2661_softc *sc, struct mbuf *m0,
    struct ieee80211_node *ni, int ac)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct rt2661_tx_ring *txq = &sc->txq[ac];
	struct rt2661_tx_desc *desc;
	struct rt2661_data *data;
	struct rt2661_tx_ratectl *rctl;
	struct rt2661_node *rn;
	struct ieee80211_rateset *rs;
	struct ieee80211_frame *wh;
	struct ieee80211_key *k;
	const struct chanAccParams *cap;
	struct mbuf *mnew;
	struct rt2661_dmamap map;
	uint16_t dur;
	uint32_t flags = 0;
	int error, rate, noack = 0;

	wh = mtod(m0, struct ieee80211_frame *);

	if (ic->ic_fixed_rate != IEEE80211_FIXED_RATE_NONE) {
		rs = &ic->ic_sup_rates[ic->ic_curmode];
		rate = rs->rs_rates[ic->ic_fixed_rate];
	} else {
		rs = &ni->ni_rates;
		rn = (struct rt2661_node *)ni;
		ni->ni_txrate = ral_rssadapt_choose(&rn->rssadapt, rs,
		    wh, m0->m_pkthdr.len, NULL, 0);
		rate = rs->rs_rates[ni->ni_txrate];
	}
	rate &= IEEE80211_RATE_VAL;

	if (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_QOS) {
		cap = &ic->ic_wme.wme_chanParams;
		noack = cap->cap_wmeParams[ac].wmep_noackPolicy;
	}

	if (wh->i_fc[1] & IEEE80211_FC1_WEP) {
		k = ieee80211_crypto_encap(ic, ni, m0);
		if (k == NULL) {
			m_freem(m0);
			return ENOBUFS;
		}

		/* packet header may have moved, reset our local pointer */
		wh = mtod(m0, struct ieee80211_frame *);
	}

	/*
	 * IEEE Std 802.11-1999, pp 82: "A STA shall use an RTS/CTS exchange
	 * for directed frames only when the length of the MPDU is greater
	 * than the length threshold indicated by [...]" ic_rtsthreshold.
	 */
	if (!IEEE80211_IS_MULTICAST(wh->i_addr1) &&
	    m0->m_pkthdr.len > ic->ic_rtsthreshold) {
		struct mbuf *m;
		uint16_t dur;
		int rtsrate, ackrate;

		rtsrate = IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan) ? 12 : 2;
		ackrate = ieee80211_ack_rate(ni, rate);

		dur = rt2661_txtime(m0->m_pkthdr.len + 4, rate, ic->ic_flags) +
		      rt2661_txtime(RAL_CTS_SIZE, rtsrate, ic->ic_flags) +
		      /* XXX: noack (QoS)? */
		      rt2661_txtime(RAL_ACK_SIZE, ackrate, ic->ic_flags) +
		      3 * RAL_SIFS;

		m = rt2661_get_rts(sc, wh, dur);

		desc = &txq->desc[txq->cur];
		data = &txq->data[txq->cur];

		error = bus_dmamap_load_mbuf(txq->data_dmat, data->map, m,
					     rt2661_dma_map_mbuf, &map, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "could not map mbuf (error %d)\n", error);
			m_freem(m);
			m_freem(m0);
			return error;
		}

		data->m = m;

		rt2661_setup_tx_desc(sc, desc, RT2661_TX_NEED_ACK |
				     RT2661_TX_MORE_FRAG, 0, m->m_pkthdr.len,
				     rtsrate, map.segs, map.nseg, ac, 0);

		bus_dmamap_sync(txq->data_dmat, data->map,
		    BUS_DMASYNC_PREWRITE);

		txq->queued++;
		txq->cur = (txq->cur + 1) % RT2661_TX_RING_COUNT;

		/*
		 * IEEE Std 802.11-1999: when an RTS/CTS exchange is used, the
		 * asynchronous data frame shall be transmitted after the CTS
		 * frame and a SIFS period.
		 */
		flags |= RT2661_TX_LONG_RETRY | RT2661_TX_IFS;
	}

	data = &txq->data[txq->cur];
	desc = &txq->desc[txq->cur];

	error = bus_dmamap_load_mbuf(txq->data_dmat, data->map, m0,
				     rt2661_dma_map_mbuf, &map, 0);
	if (error != 0 && error != EFBIG) {
		device_printf(sc->sc_dev, "could not map mbuf (error %d)\n",
		    error);
		m_freem(m0);
		return error;
	}
	if (error != 0) {
		mnew = m_defrag(m0, MB_DONTWAIT);
		if (mnew == NULL) {
			device_printf(sc->sc_dev,
			    "could not defragment mbuf\n");
			m_freem(m0);
			return ENOBUFS;
		}
		m0 = mnew;

		error = bus_dmamap_load_mbuf(txq->data_dmat, data->map, m0,
					     rt2661_dma_map_mbuf, &map, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "could not map mbuf (error %d)\n", error);
			m_freem(m0);
			return error;
		}

		/* packet header have moved, reset our local pointer */
		wh = mtod(m0, struct ieee80211_frame *);
	}

	if (sc->sc_drvbpf != NULL) {
		struct rt2661_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		tap->wt_rate = rate;
		tap->wt_chan_freq = htole16(ic->ic_curchan->ic_freq);
		tap->wt_chan_flags = htole16(ic->ic_curchan->ic_flags);

		bpf_ptap(sc->sc_drvbpf, m0, tap, sc->sc_txtap_len);
	}

	data->m = m0;

	rctl = kmalloc(sizeof(*rctl), M_RT2661, M_NOWAIT);
	if (rctl != NULL) {
		rctl->ni = ni;

		/* remember link conditions for rate adaptation algorithm */
		if (ic->ic_fixed_rate == IEEE80211_FIXED_RATE_NONE) {
			rctl->id.id_len = m0->m_pkthdr.len;
			rctl->id.id_rateidx = ni->ni_txrate;
			rctl->id.id_node = ni;
			rctl->id.id_rssi = ni->ni_rssi;
		} else {
			rctl->id.id_node = NULL;
		}
		STAILQ_INSERT_TAIL(&sc->tx_ratectl, rctl, link);
	}

	if (!noack && !IEEE80211_IS_MULTICAST(wh->i_addr1)) {
		flags |= RT2661_TX_NEED_ACK;

		dur = rt2661_txtime(RAL_ACK_SIZE, ieee80211_ack_rate(ni, rate),
		    ic->ic_flags) + RAL_SIFS;
		*(uint16_t *)wh->i_dur = htole16(dur);
	}

	rt2661_setup_tx_desc(sc, desc, flags, 0, m0->m_pkthdr.len, rate,
			     map.segs, map.nseg, ac, rctl != NULL);

	bus_dmamap_sync(txq->data_dmat, data->map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(txq->desc_dmat, txq->desc_map, BUS_DMASYNC_PREWRITE);

	DPRINTFN(10, ("sending data frame len=%u idx=%u rate=%u\n",
	    m0->m_pkthdr.len, txq->cur, rate));

	/* kick Tx */
	txq->queued++;
	txq->cur = (txq->cur + 1) % RT2661_TX_RING_COUNT;
	RAL_WRITE(sc, RT2661_TX_CNTL_CSR, 1 << ac);

	if (rctl == NULL)
		ieee80211_free_node(ni);

	return 0;
}

static void
rt2661_start(struct ifnet *ifp)
{
	struct rt2661_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m0;
	struct ether_header *eh;
	struct ieee80211_node *ni;
	int ac;

	/* prevent management frames from being sent if we're not ready */
	if (!(ifp->if_flags & IFF_RUNNING))
		return;

	for (;;) {
		IF_POLL(&ic->ic_mgtq, m0);
		if (m0 != NULL) {
			if (sc->mgtq.queued >= RT2661_MGT_RING_COUNT) {
				ifp->if_flags |= IFF_OACTIVE;
				break;
			}
			IF_DEQUEUE(&ic->ic_mgtq, m0);

			ni = (struct ieee80211_node *)m0->m_pkthdr.rcvif;
			m0->m_pkthdr.rcvif = NULL;

			if (ic->ic_rawbpf != NULL)
				bpf_mtap(ic->ic_rawbpf, m0);

			if (rt2661_tx_mgt(sc, m0, ni) != 0)
				break;

		} else {
			if (ic->ic_state != IEEE80211_S_RUN)
				break;

			m0 = ifq_dequeue(&ifp->if_snd, NULL);
			if (m0 == NULL)
				break;

			if (m0->m_len < sizeof (struct ether_header) &&
			    !(m0 = m_pullup(m0, sizeof (struct ether_header))))
				continue;

			eh = mtod(m0, struct ether_header *);
			ni = ieee80211_find_txnode(ic, eh->ether_dhost);
			if (ni == NULL) {
				m_freem(m0);
				ifp->if_oerrors++;
				continue;
			}

			/* classify mbuf so we can find which tx ring to use */
			if (ieee80211_classify(ic, m0, ni) != 0) {
				m_freem(m0);
				ieee80211_free_node(ni);
				ifp->if_oerrors++;
				continue;
			}

			/* no QoS encapsulation for EAPOL frames */
			ac = (eh->ether_type != htons(ETHERTYPE_PAE)) ?
			    M_WME_GETAC(m0) : WME_AC_BE;

			if (sc->txq[ac].queued >= RT2661_TX_RING_COUNT - 1) {
				/* there is no place left in this ring */
				ifp->if_flags |= IFF_OACTIVE;
				m_freem(m0);
				ieee80211_free_node(ni);
				break;
			}

			BPF_MTAP(ifp, m0);

			m0 = ieee80211_encap(ic, m0, ni);
			if (m0 == NULL) {
				ieee80211_free_node(ni);
				ifp->if_oerrors++;
				continue;
			}

			if (ic->ic_rawbpf != NULL)
				bpf_mtap(ic->ic_rawbpf, m0);

			if (rt2661_tx_data(sc, m0, ni, ac) != 0) {
				ieee80211_free_node(ni);
				ifp->if_oerrors++;
				break;
			}
		}

		sc->sc_tx_timer = 5;
		ifp->if_timer = 1;
	}
}

static void
rt2661_watchdog(struct ifnet *ifp)
{
	struct rt2661_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	ifp->if_timer = 0;

	if (sc->sc_tx_timer > 0) {
		if (--sc->sc_tx_timer == 0) {
			device_printf(sc->sc_dev, "device timeout\n");
			rt2661_init(sc);
			ifp->if_oerrors++;
			return;
		}
		ifp->if_timer = 1;
	}

	ieee80211_watchdog(ic);
}

/*
 * This function allows for fast channel switching in monitor mode (used by
 * net-mgmt/kismet). In IBSS mode, we must explicitly reset the interface to
 * generate a new beacon frame.
 */
static int
rt2661_reset(struct ifnet *ifp)
{
	struct rt2661_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	if (ic->ic_opmode != IEEE80211_M_MONITOR)
		return ENETRESET;

	rt2661_set_chan(sc, ic->ic_curchan);

	return 0;
}

static int
rt2661_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data, struct ucred *cr)
{
	struct rt2661_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (ifp->if_flags & IFF_RUNNING)
				rt2661_update_promisc(sc);
			else
				rt2661_init(sc);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				rt2661_stop(sc);
		}
		break;

	default:
		error = ieee80211_ioctl(ic, cmd, data, cr);
	}

	if (error == ENETRESET) {
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING) &&
		    (ic->ic_roaming != IEEE80211_ROAMING_MANUAL))
			rt2661_init(sc);
		error = 0;
	}
	return error;
}

static void
rt2661_bbp_write(struct rt2661_softc *sc, uint8_t reg, uint8_t val)
{
	uint32_t tmp;
	int ntries;

	for (ntries = 0; ntries < 100; ntries++) {
		if (!(RAL_READ(sc, RT2661_PHY_CSR3) & RT2661_BBP_BUSY))
			break;
		DELAY(1);
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev, "could not write to BBP\n");
		return;
	}

	tmp = RT2661_BBP_BUSY | (reg & 0x7f) << 8 | val;
	RAL_WRITE(sc, RT2661_PHY_CSR3, tmp);

	DPRINTFN(15, ("BBP R%u <- 0x%02x\n", reg, val));
}

static uint8_t
rt2661_bbp_read(struct rt2661_softc *sc, uint8_t reg)
{
	uint32_t val;
	int ntries;

	for (ntries = 0; ntries < 100; ntries++) {
		if (!(RAL_READ(sc, RT2661_PHY_CSR3) & RT2661_BBP_BUSY))
			break;
		DELAY(1);
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev, "could not read from BBP\n");
		return 0;
	}

	val = RT2661_BBP_BUSY | RT2661_BBP_READ | reg << 8;
	RAL_WRITE(sc, RT2661_PHY_CSR3, val);

	for (ntries = 0; ntries < 100; ntries++) {
		val = RAL_READ(sc, RT2661_PHY_CSR3);
		if (!(val & RT2661_BBP_BUSY))
			return val & 0xff;
		DELAY(1);
	}

	device_printf(sc->sc_dev, "could not read from BBP\n");
	return 0;
}

static void
rt2661_rf_write(struct rt2661_softc *sc, uint8_t reg, uint32_t val)
{
	uint32_t tmp;
	int ntries;

	for (ntries = 0; ntries < 100; ntries++) {
		if (!(RAL_READ(sc, RT2661_PHY_CSR4) & RT2661_RF_BUSY))
			break;
		DELAY(1);
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev, "could not write to RF\n");
		return;
	}

	tmp = RT2661_RF_BUSY | RT2661_RF_21BIT | (val & 0x1fffff) << 2 |
	    (reg & 3);
	RAL_WRITE(sc, RT2661_PHY_CSR4, tmp);

	/* remember last written value in sc */
	sc->rf_regs[reg] = val;

	DPRINTFN(15, ("RF R[%u] <- 0x%05x\n", reg & 3, val & 0x1fffff));
}

static int
rt2661_tx_cmd(struct rt2661_softc *sc, uint8_t cmd, uint16_t arg)
{
	if (RAL_READ(sc, RT2661_H2M_MAILBOX_CSR) & RT2661_H2M_BUSY)
		return EIO;	/* there is already a command pending */

	RAL_WRITE(sc, RT2661_H2M_MAILBOX_CSR,
	    RT2661_H2M_BUSY | RT2661_TOKEN_NO_INTR << 16 | arg);

	RAL_WRITE(sc, RT2661_HOST_CMD_CSR, RT2661_KICK_CMD | cmd);

	return 0;
}

static void
rt2661_select_antenna(struct rt2661_softc *sc)
{
	uint8_t bbp4, bbp77;
	uint32_t tmp;

	bbp4  = rt2661_bbp_read(sc,  4);
	bbp77 = rt2661_bbp_read(sc, 77);

	/* TBD */

	/* make sure Rx is disabled before switching antenna */
	tmp = RAL_READ(sc, RT2661_TXRX_CSR0);
	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp | RT2661_DISABLE_RX);

	rt2661_bbp_write(sc,  4, bbp4);
	rt2661_bbp_write(sc, 77, bbp77);

	/* restore Rx filter */
	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp);
}

/*
 * Enable multi-rate retries for frames sent at OFDM rates.
 * In 802.11b/g mode, allow fallback to CCK rates.
 */
static void
rt2661_enable_mrr(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t tmp;

	tmp = RAL_READ(sc, RT2661_TXRX_CSR4);

	tmp &= ~RT2661_MRR_CCK_FALLBACK;
	if (!IEEE80211_IS_CHAN_5GHZ(ic->ic_bss->ni_chan))
		tmp |= RT2661_MRR_CCK_FALLBACK;
	tmp |= RT2661_MRR_ENABLED;

	RAL_WRITE(sc, RT2661_TXRX_CSR4, tmp);
}

static void
rt2661_set_txpreamble(struct rt2661_softc *sc)
{
	uint32_t tmp;

	tmp = RAL_READ(sc, RT2661_TXRX_CSR4);

	tmp &= ~RT2661_SHORT_PREAMBLE;
	if (sc->sc_ic.ic_flags & IEEE80211_F_SHPREAMBLE)
		tmp |= RT2661_SHORT_PREAMBLE;

	RAL_WRITE(sc, RT2661_TXRX_CSR4, tmp);
}

static void
rt2661_set_basicrates(struct rt2661_softc *sc,
    const struct ieee80211_rateset *rs)
{
#define RV(r)	((r) & IEEE80211_RATE_VAL)
	uint32_t mask = 0;
	uint8_t rate;
	int i, j;

	for (i = 0; i < rs->rs_nrates; i++) {
		rate = rs->rs_rates[i];

		if (!(rate & IEEE80211_RATE_BASIC))
			continue;

		/*
		 * Find h/w rate index.  We know it exists because the rate
		 * set has already been negotiated.
		 */
		for (j = 0; rt2661_rateset_11g.rs_rates[j] != RV(rate); j++);

		mask |= 1 << j;
	}

	RAL_WRITE(sc, RT2661_TXRX_CSR5, mask);

	DPRINTF(("Setting basic rate mask to 0x%x\n", mask));
#undef RV
}

/*
 * Reprogram MAC/BBP to switch to a new band.  Values taken from the reference
 * driver.
 */
static void
rt2661_select_band(struct rt2661_softc *sc, struct ieee80211_channel *c)
{
	uint8_t bbp17, bbp35, bbp96, bbp97, bbp98, bbp104;
	uint32_t tmp;

	/* update all BBP registers that depend on the band */
	bbp17 = 0x20; bbp96 = 0x48; bbp104 = 0x2c;
	bbp35 = 0x50; bbp97 = 0x48; bbp98  = 0x48;
	if (IEEE80211_IS_CHAN_5GHZ(c)) {
		bbp17 += 0x08; bbp96 += 0x10; bbp104 += 0x0c;
		bbp35 += 0x10; bbp97 += 0x10; bbp98  += 0x10;
	}
	if ((IEEE80211_IS_CHAN_2GHZ(c) && sc->ext_2ghz_lna) ||
	    (IEEE80211_IS_CHAN_5GHZ(c) && sc->ext_5ghz_lna)) {
		bbp17 += 0x10; bbp96 += 0x10; bbp104 += 0x10;
	}

	rt2661_bbp_write(sc,  17, bbp17);
	rt2661_bbp_write(sc,  96, bbp96);
	rt2661_bbp_write(sc, 104, bbp104);

	if ((IEEE80211_IS_CHAN_2GHZ(c) && sc->ext_2ghz_lna) ||
	    (IEEE80211_IS_CHAN_5GHZ(c) && sc->ext_5ghz_lna)) {
		rt2661_bbp_write(sc, 75, 0x80);
		rt2661_bbp_write(sc, 86, 0x80);
		rt2661_bbp_write(sc, 88, 0x80);
	}

	rt2661_bbp_write(sc, 35, bbp35);
	rt2661_bbp_write(sc, 97, bbp97);
	rt2661_bbp_write(sc, 98, bbp98);

	tmp = RAL_READ(sc, RT2661_PHY_CSR0);
	tmp &= ~(RT2661_PA_PE_2GHZ | RT2661_PA_PE_5GHZ);
	if (IEEE80211_IS_CHAN_2GHZ(c))
		tmp |= RT2661_PA_PE_2GHZ;
	else
		tmp |= RT2661_PA_PE_5GHZ;
	RAL_WRITE(sc, RT2661_PHY_CSR0, tmp);
}

static void
rt2661_set_chan(struct rt2661_softc *sc, struct ieee80211_channel *c)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct rfprog *rfprog;
	uint8_t bbp3, bbp94 = RT2661_BBPR94_DEFAULT;
	int8_t power;
	u_int i, chan;

	chan = ieee80211_chan2ieee(ic, c);
	if (chan == 0 || chan == IEEE80211_CHAN_ANY)
		return;

	/* select the appropriate RF settings based on what EEPROM says */
	rfprog = (sc->rfprog == 0) ? rt2661_rf5225_1 : rt2661_rf5225_2;

	/* find the settings for this channel (we know it exists) */
	for (i = 0; rfprog[i].chan != chan; i++);

	power = sc->txpow[i];
	if (power < 0) {
		bbp94 += power;
		power = 0;
	} else if (power > 31) {
		bbp94 += power - 31;
		power = 31;
	}

	/*
	 * If we are switching from the 2GHz band to the 5GHz band or
	 * vice-versa, BBP registers need to be reprogrammed.
	 */
	if (c->ic_flags != sc->sc_curchan->ic_flags) {
		rt2661_select_band(sc, c);
		rt2661_select_antenna(sc);
	}
	sc->sc_curchan = c;

	rt2661_rf_write(sc, RAL_RF1, rfprog[i].r1);
	rt2661_rf_write(sc, RAL_RF2, rfprog[i].r2);
	rt2661_rf_write(sc, RAL_RF3, rfprog[i].r3 | power << 7);
	rt2661_rf_write(sc, RAL_RF4, rfprog[i].r4 | sc->rffreq << 10);

	DELAY(200);

	rt2661_rf_write(sc, RAL_RF1, rfprog[i].r1);
	rt2661_rf_write(sc, RAL_RF2, rfprog[i].r2);
	rt2661_rf_write(sc, RAL_RF3, rfprog[i].r3 | power << 7 | 1);
	rt2661_rf_write(sc, RAL_RF4, rfprog[i].r4 | sc->rffreq << 10);

	DELAY(200);

	rt2661_rf_write(sc, RAL_RF1, rfprog[i].r1);
	rt2661_rf_write(sc, RAL_RF2, rfprog[i].r2);
	rt2661_rf_write(sc, RAL_RF3, rfprog[i].r3 | power << 7);
	rt2661_rf_write(sc, RAL_RF4, rfprog[i].r4 | sc->rffreq << 10);

	/* enable smart mode for MIMO-capable RFs */
	bbp3 = rt2661_bbp_read(sc, 3);

	bbp3 &= ~RT2661_SMART_MODE;
	if (sc->rf_rev == RT2661_RF_5325 || sc->rf_rev == RT2661_RF_2529)
		bbp3 |= RT2661_SMART_MODE;

	rt2661_bbp_write(sc, 3, bbp3);

	if (bbp94 != RT2661_BBPR94_DEFAULT)
		rt2661_bbp_write(sc, 94, bbp94);

	/* 5GHz radio needs a 1ms delay here */
	if (IEEE80211_IS_CHAN_5GHZ(c))
		DELAY(1000);
}

static void
rt2661_set_bssid(struct rt2661_softc *sc, const uint8_t *bssid)
{
	uint32_t tmp;

	tmp = bssid[0] | bssid[1] << 8 | bssid[2] << 16 | bssid[3] << 24;
	RAL_WRITE(sc, RT2661_MAC_CSR4, tmp);

	tmp = bssid[4] | bssid[5] << 8 | RT2661_ONE_BSSID << 16;
	RAL_WRITE(sc, RT2661_MAC_CSR5, tmp);
}

static void
rt2661_set_macaddr(struct rt2661_softc *sc, const uint8_t *addr)
{
	uint32_t tmp;

	tmp = addr[0] | addr[1] << 8 | addr[2] << 16 | addr[3] << 24;
	RAL_WRITE(sc, RT2661_MAC_CSR2, tmp);

	tmp = addr[4] | addr[5] << 8;
	RAL_WRITE(sc, RT2661_MAC_CSR3, tmp);
}

static void
rt2661_update_promisc(struct rt2661_softc *sc)
{
	struct ifnet *ifp = sc->sc_ic.ic_ifp;
	uint32_t tmp;

	tmp = RAL_READ(sc, RT2661_TXRX_CSR0);

	tmp &= ~RT2661_DROP_NOT_TO_ME;
	if (!(ifp->if_flags & IFF_PROMISC))
		tmp |= RT2661_DROP_NOT_TO_ME;

	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp);

	DPRINTF(("%s promiscuous mode\n", (ifp->if_flags & IFF_PROMISC) ?
	    "entering" : "leaving"));
}

/*
 * Update QoS (802.11e) settings for each h/w Tx ring.
 */
static int
rt2661_wme_update(struct ieee80211com *ic)
{
	struct rt2661_softc *sc = ic->ic_ifp->if_softc;
	const struct wmeParams *wmep;

	wmep = ic->ic_wme.wme_chanParams.cap_wmeParams;

	/* XXX: not sure about shifts. */
	/* XXX: the reference driver plays with AC_VI settings too. */

	/* update TxOp */
	RAL_WRITE(sc, RT2661_AC_TXOP_CSR0,
	    wmep[WME_AC_BE].wmep_txopLimit << 16 |
	    wmep[WME_AC_BK].wmep_txopLimit);
	RAL_WRITE(sc, RT2661_AC_TXOP_CSR1,
	    wmep[WME_AC_VI].wmep_txopLimit << 16 |
	    wmep[WME_AC_VO].wmep_txopLimit);

	/* update CWmin */
	RAL_WRITE(sc, RT2661_CWMIN_CSR,
	    wmep[WME_AC_BE].wmep_logcwmin << 12 |
	    wmep[WME_AC_BK].wmep_logcwmin <<  8 |
	    wmep[WME_AC_VI].wmep_logcwmin <<  4 |
	    wmep[WME_AC_VO].wmep_logcwmin);

	/* update CWmax */
	RAL_WRITE(sc, RT2661_CWMAX_CSR,
	    wmep[WME_AC_BE].wmep_logcwmax << 12 |
	    wmep[WME_AC_BK].wmep_logcwmax <<  8 |
	    wmep[WME_AC_VI].wmep_logcwmax <<  4 |
	    wmep[WME_AC_VO].wmep_logcwmax);

	/* update Aifsn */
	RAL_WRITE(sc, RT2661_AIFSN_CSR,
	    wmep[WME_AC_BE].wmep_aifsn << 12 |
	    wmep[WME_AC_BK].wmep_aifsn <<  8 |
	    wmep[WME_AC_VI].wmep_aifsn <<  4 |
	    wmep[WME_AC_VO].wmep_aifsn);

	return 0;
}

static void
rt2661_update_slot(struct ifnet *ifp)
{
	struct rt2661_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t slottime;
	uint32_t tmp;

	slottime = (ic->ic_flags & IEEE80211_F_SHSLOT) ? 9 : 20;

	tmp = RAL_READ(sc, RT2661_MAC_CSR9);
	tmp = (tmp & ~0xff) | slottime;
	RAL_WRITE(sc, RT2661_MAC_CSR9, tmp);
}

static const char *
rt2661_get_rf(int rev)
{
	switch (rev) {
	case RT2661_RF_5225:	return "RT5225";
	case RT2661_RF_5325:	return "RT5325 (MIMO XR)";
	case RT2661_RF_2527:	return "RT2527";
	case RT2661_RF_2529:	return "RT2529 (MIMO XR)";
	default:		return "unknown";
	}
}

static void
rt2661_read_eeprom(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint16_t val;
	int i;

	/* read MAC address */
	val = rt2661_eeprom_read(sc, RT2661_EEPROM_MAC01);
	ic->ic_myaddr[0] = val & 0xff;
	ic->ic_myaddr[1] = val >> 8;

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_MAC23);
	ic->ic_myaddr[2] = val & 0xff;
	ic->ic_myaddr[3] = val >> 8;

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_MAC45);
	ic->ic_myaddr[4] = val & 0xff;
	ic->ic_myaddr[5] = val >> 8;

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_ANTENNA);
	/* XXX: test if different from 0xffff? */
	sc->rf_rev   = (val >> 11) & 0x1f;
	sc->hw_radio = (val >> 10) & 0x1;
	sc->rx_ant   = (val >> 4)  & 0x3;
	sc->tx_ant   = (val >> 2)  & 0x3;
	sc->nb_ant   = val & 0x3;

	DPRINTF(("RF revision=%d\n", sc->rf_rev));

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_CONFIG2);
	sc->ext_5ghz_lna = (val >> 6) & 0x1;
	sc->ext_2ghz_lna = (val >> 4) & 0x1;

	DPRINTF(("External 2GHz LNA=%d\nExternal 5GHz LNA=%d\n",
	    sc->ext_2ghz_lna, sc->ext_5ghz_lna));

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_RSSI_2GHZ_OFFSET);
	if ((val & 0xff) != 0xff)
		sc->rssi_2ghz_corr = (int8_t)(val & 0xff);	/* signed */

	/* Only [-10, 10] is valid */
	if (sc->rssi_2ghz_corr < -10 || sc->rssi_2ghz_corr > 10)
		sc->rssi_2ghz_corr = 0;

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_RSSI_5GHZ_OFFSET);
	if ((val & 0xff) != 0xff)
		sc->rssi_5ghz_corr = (int8_t)(val & 0xff);	/* signed */

	/* Only [-10, 10] is valid */
	if (sc->rssi_5ghz_corr < -10 || sc->rssi_5ghz_corr > 10)
		sc->rssi_5ghz_corr = 0;

	/* adjust RSSI correction for external low-noise amplifier */
	if (sc->ext_2ghz_lna)
		sc->rssi_2ghz_corr -= 14;
	if (sc->ext_5ghz_lna)
		sc->rssi_5ghz_corr -= 14;

	DPRINTF(("RSSI 2GHz corr=%d\nRSSI 5GHz corr=%d\n",
	    sc->rssi_2ghz_corr, sc->rssi_5ghz_corr));

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_FREQ_OFFSET);
	if ((val >> 8) != 0xff)
		sc->rfprog = (val >> 8) & 0x3;
	if ((val & 0xff) != 0xff)
		sc->rffreq = val & 0xff;

	DPRINTF(("RF prog=%d\nRF freq=%d\n", sc->rfprog, sc->rffreq));

	/* read Tx power for all a/b/g channels */
	for (i = 0; i < 19; i++) {
		val = rt2661_eeprom_read(sc, RT2661_EEPROM_TXPOWER + i);
		sc->txpow[i * 2] = (int8_t)(val >> 8);		/* signed */
		DPRINTF(("Channel=%d Tx power=%d\n",
		    rt2661_rf5225_1[i * 2].chan, sc->txpow[i * 2]));
		sc->txpow[i * 2 + 1] = (int8_t)(val & 0xff);	/* signed */
		DPRINTF(("Channel=%d Tx power=%d\n",
		    rt2661_rf5225_1[i * 2 + 1].chan, sc->txpow[i * 2 + 1]));
	}

	/* read vendor-specific BBP values */
	for (i = 0; i < 16; i++) {
		val = rt2661_eeprom_read(sc, RT2661_EEPROM_BBP_BASE + i);
		if (val == 0 || val == 0xffff)
			continue;	/* skip invalid entries */
		sc->bbp_prom[i].reg = val >> 8;
		sc->bbp_prom[i].val = val & 0xff;
		DPRINTF(("BBP R%d=%02x\n", sc->bbp_prom[i].reg,
		    sc->bbp_prom[i].val));
	}

	val = rt2661_eeprom_read(sc, RT2661_EEPROM_LED_OFFSET);
	DPRINTF(("LED %02x\n", val));
	if (val == 0xffff) {
		sc->mcu_led = RT2661_MCU_LED_DEFAULT;
	} else {
#define N(arr)	(int)(sizeof(arr) / sizeof(arr[0]))

		for (i = 0; i < N(led_ee2mcu); ++i) {
			if (val & led_ee2mcu[i].ee_bit)
				sc->mcu_led |= led_ee2mcu[i].mcu_bit;
		}

#undef N

		sc->mcu_led |= ((val >> RT2661_EE_LED_MODE_SHIFT) &
				RT2661_EE_LED_MODE_MASK);
	}
}

static int
rt2661_bbp_init(struct rt2661_softc *sc)
{
#define N(a)	(sizeof (a) / sizeof ((a)[0]))
	int i, ntries;
	uint8_t val;

	/* wait for BBP to be ready */
	for (ntries = 0; ntries < 100; ntries++) {
		val = rt2661_bbp_read(sc, 0);
		if (val != 0 && val != 0xff)
			break;
		DELAY(100);
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev, "timeout waiting for BBP\n");
		return EIO;
	}

	/* initialize BBP registers to default values */
	for (i = 0; i < N(rt2661_def_bbp); i++) {
		rt2661_bbp_write(sc, rt2661_def_bbp[i].reg,
		    rt2661_def_bbp[i].val);
	}

	/* write vendor-specific BBP values (from EEPROM) */
	for (i = 0; i < 16; i++) {
		if (sc->bbp_prom[i].reg == 0)
			continue;
		rt2661_bbp_write(sc, sc->bbp_prom[i].reg, sc->bbp_prom[i].val);
	}

	return 0;
#undef N
}

static void
rt2661_init(void *priv)
{
#define N(a)	(sizeof (a) / sizeof ((a)[0]))
	struct rt2661_softc *sc = priv;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	uint32_t tmp, sta[3];
	int i, ntries;

	rt2661_stop(sc);

	/* initialize Tx rings */
	RAL_WRITE(sc, RT2661_AC1_BASE_CSR, sc->txq[1].physaddr);
	RAL_WRITE(sc, RT2661_AC0_BASE_CSR, sc->txq[0].physaddr);
	RAL_WRITE(sc, RT2661_AC2_BASE_CSR, sc->txq[2].physaddr);
	RAL_WRITE(sc, RT2661_AC3_BASE_CSR, sc->txq[3].physaddr);

	/* initialize Mgt ring */
	RAL_WRITE(sc, RT2661_MGT_BASE_CSR, sc->mgtq.physaddr);

	/* initialize Rx ring */
	RAL_WRITE(sc, RT2661_RX_BASE_CSR, sc->rxq.physaddr);

	/* initialize Tx rings sizes */
	RAL_WRITE(sc, RT2661_TX_RING_CSR0,
	    RT2661_TX_RING_COUNT << 24 |
	    RT2661_TX_RING_COUNT << 16 |
	    RT2661_TX_RING_COUNT <<  8 |
	    RT2661_TX_RING_COUNT);

	RAL_WRITE(sc, RT2661_TX_RING_CSR1,
	    RT2661_TX_DESC_WSIZE << 16 |
	    RT2661_TX_RING_COUNT <<  8 |	/* XXX: HCCA ring unused */
	    RT2661_MGT_RING_COUNT);

	/* initialize Rx rings */
	RAL_WRITE(sc, RT2661_RX_RING_CSR,
	    RT2661_RX_DESC_BACK  << 16 |
	    RT2661_RX_DESC_WSIZE <<  8 |
	    RT2661_RX_RING_COUNT);

	/* XXX: some magic here */
	RAL_WRITE(sc, RT2661_TX_DMA_DST_CSR, 0xaa);

	/* load base addresses of all 5 Tx rings (4 data + 1 mgt) */
	RAL_WRITE(sc, RT2661_LOAD_TX_RING_CSR, 0x1f);

	/* load base address of Rx ring */
	RAL_WRITE(sc, RT2661_RX_CNTL_CSR, 2);

	/* initialize MAC registers to default values */
	for (i = 0; i < N(rt2661_def_mac); i++)
		RAL_WRITE(sc, rt2661_def_mac[i].reg, rt2661_def_mac[i].val);

	IEEE80211_ADDR_COPY(ic->ic_myaddr, IF_LLADDR(ifp));
	rt2661_set_macaddr(sc, ic->ic_myaddr);

	/* set host ready */
	RAL_WRITE(sc, RT2661_MAC_CSR1, 3);
	RAL_WRITE(sc, RT2661_MAC_CSR1, 0);

	/* wait for BBP/RF to wakeup */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (RAL_READ(sc, RT2661_MAC_CSR12) & 8)
			break;
		DELAY(1000);
	}
	if (ntries == 1000) {
		kprintf("timeout waiting for BBP/RF to wakeup\n");
		rt2661_stop(sc);
		return;
	}

	if (rt2661_bbp_init(sc) != 0) {
		rt2661_stop(sc);
		return;
	}

	/* select default channel */
	sc->sc_curchan = ic->ic_curchan;
	rt2661_select_band(sc, sc->sc_curchan);
	rt2661_select_antenna(sc);
	rt2661_set_chan(sc, sc->sc_curchan);

	/* update Rx filter */
	tmp = RAL_READ(sc, RT2661_TXRX_CSR0) & 0xffff;

	tmp |= RT2661_DROP_PHY_ERROR | RT2661_DROP_CRC_ERROR;
	if (ic->ic_opmode != IEEE80211_M_MONITOR) {
		tmp |= RT2661_DROP_CTL | RT2661_DROP_VER_ERROR |
		       RT2661_DROP_ACKCTS;
		if (ic->ic_opmode != IEEE80211_M_HOSTAP)
			tmp |= RT2661_DROP_TODS;
		if (!(ifp->if_flags & IFF_PROMISC))
			tmp |= RT2661_DROP_NOT_TO_ME;
	}

	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp);

	/* clear STA registers */
	RAL_READ_REGION_4(sc, RT2661_STA_CSR0, sta, N(sta));

	/* initialize ASIC */
	RAL_WRITE(sc, RT2661_MAC_CSR1, 4);

	/* clear any pending interrupt */
	RAL_WRITE(sc, RT2661_INT_SOURCE_CSR, 0xffffffff);

	/* enable interrupts */
	RAL_WRITE(sc, RT2661_INT_MASK_CSR, 0x0000ff10);
	RAL_WRITE(sc, RT2661_MCU_INT_MASK_CSR, 0);

	/* kick Rx */
	RAL_WRITE(sc, RT2661_RX_CNTL_CSR, 1);

	ifp->if_flags &= ~IFF_OACTIVE;
	ifp->if_flags |= IFF_RUNNING;

	if (ic->ic_opmode != IEEE80211_M_MONITOR) {
		if (ic->ic_roaming != IEEE80211_ROAMING_MANUAL)
			ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	} else
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
#undef N
}

void
rt2661_stop(void *priv)
{
	struct rt2661_softc *sc = priv;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	struct rt2661_tx_ratectl *rctl;
	uint32_t tmp;

	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);

	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);

	/* abort Tx (for all 5 Tx rings) */
	RAL_WRITE(sc, RT2661_TX_CNTL_CSR, 0x1f << 16);

	/* disable Rx (value remains after reset!) */
	tmp = RAL_READ(sc, RT2661_TXRX_CSR0);
	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp | RT2661_DISABLE_RX);

	/* reset ASIC */
	RAL_WRITE(sc, RT2661_MAC_CSR1, 3);
	RAL_WRITE(sc, RT2661_MAC_CSR1, 0);

	/* disable interrupts */
	RAL_WRITE(sc, RT2661_INT_MASK_CSR, 0xffffffff);
	RAL_WRITE(sc, RT2661_MCU_INT_MASK_CSR, 0xffffffff);

	/* clear any pending interrupt */
	RAL_WRITE(sc, RT2661_INT_SOURCE_CSR, 0xffffffff);
	RAL_WRITE(sc, RT2661_MCU_INT_SOURCE_CSR, 0xffffffff);

	while ((rctl = STAILQ_FIRST(&sc->tx_ratectl)) != NULL) {
		STAILQ_REMOVE_HEAD(&sc->tx_ratectl, link);
		ieee80211_free_node(rctl->ni);
		rctl->ni = NULL;
		kfree(rctl, M_RT2661);
	}

	/* reset Tx and Rx rings */
	rt2661_reset_tx_ring(sc, &sc->txq[0]);
	rt2661_reset_tx_ring(sc, &sc->txq[1]);
	rt2661_reset_tx_ring(sc, &sc->txq[2]);
	rt2661_reset_tx_ring(sc, &sc->txq[3]);
	rt2661_reset_tx_ring(sc, &sc->mgtq);
	rt2661_reset_rx_ring(sc, &sc->rxq);
}

static int
rt2661_load_microcode(struct rt2661_softc *sc, const uint8_t *ucode, int size)
{
	int ntries;

	/* reset 8051 */
	RAL_WRITE(sc, RT2661_MCU_CNTL_CSR, RT2661_MCU_RESET);

	/* cancel any pending Host to MCU command */
	RAL_WRITE(sc, RT2661_H2M_MAILBOX_CSR, 0);
	RAL_WRITE(sc, RT2661_M2H_CMD_DONE_CSR, 0xffffffff);
	RAL_WRITE(sc, RT2661_HOST_CMD_CSR, 0);

	/* write 8051's microcode */
	RAL_WRITE(sc, RT2661_MCU_CNTL_CSR, RT2661_MCU_RESET | RT2661_MCU_SEL);
	RAL_WRITE_REGION_1(sc, RT2661_MCU_CODE_BASE, ucode, size);
	RAL_WRITE(sc, RT2661_MCU_CNTL_CSR, RT2661_MCU_RESET);

	/* kick 8051's ass */
	RAL_WRITE(sc, RT2661_MCU_CNTL_CSR, 0);

	/* wait for 8051 to initialize */
	for (ntries = 0; ntries < 500; ntries++) {
		if (RAL_READ(sc, RT2661_MCU_CNTL_CSR) & RT2661_MCU_READY)
			break;
		DELAY(100);
	}
	if (ntries == 500) {
		kprintf("timeout waiting for MCU to initialize\n");
		return EIO;
	}
	return 0;
}

#ifdef notyet
/*
 * Dynamically tune Rx sensitivity (BBP register 17) based on average RSSI and
 * false CCA count.  This function is called periodically (every seconds) when
 * in the RUN state.  Values taken from the reference driver.
 */
static void
rt2661_rx_tune(struct rt2661_softc *sc)
{
	uint8_t bbp17;
	uint16_t cca;
	int lo, hi, dbm;

	/*
	 * Tuning range depends on operating band and on the presence of an
	 * external low-noise amplifier.
	 */
	lo = 0x20;
	if (IEEE80211_IS_CHAN_5GHZ(sc->sc_curchan))
		lo += 0x08;
	if ((IEEE80211_IS_CHAN_2GHZ(sc->sc_curchan) && sc->ext_2ghz_lna) ||
	    (IEEE80211_IS_CHAN_5GHZ(sc->sc_curchan) && sc->ext_5ghz_lna))
		lo += 0x10;
	hi = lo + 0x20;

	/* retrieve false CCA count since last call (clear on read) */
	cca = RAL_READ(sc, RT2661_STA_CSR1) & 0xffff;

	if (dbm >= -35) {
		bbp17 = 0x60;
	} else if (dbm >= -58) {
		bbp17 = hi;
	} else if (dbm >= -66) {
		bbp17 = lo + 0x10;
	} else if (dbm >= -74) {
		bbp17 = lo + 0x08;
	} else {
		/* RSSI < -74dBm, tune using false CCA count */

		bbp17 = sc->bbp17; /* current value */

		hi -= 2 * (-74 - dbm);
		if (hi < lo)
			hi = lo;

		if (bbp17 > hi) {
			bbp17 = hi;

		} else if (cca > 512) {
			if (++bbp17 > hi)
				bbp17 = hi;
		} else if (cca < 100) {
			if (--bbp17 < lo)
				bbp17 = lo;
		}
	}

	if (bbp17 != sc->bbp17) {
		rt2661_bbp_write(sc, 17, bbp17);
		sc->bbp17 = bbp17;
	}
}

/*
 * Enter/Leave radar detection mode.
 * This is for 802.11h additional regulatory domains.
 */
static void
rt2661_radar_start(struct rt2661_softc *sc)
{
	uint32_t tmp;

	/* disable Rx */
	tmp = RAL_READ(sc, RT2661_TXRX_CSR0);
	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp | RT2661_DISABLE_RX);

	rt2661_bbp_write(sc, 82, 0x20);
	rt2661_bbp_write(sc, 83, 0x00);
	rt2661_bbp_write(sc, 84, 0x40);

	/* save current BBP registers values */
	sc->bbp18 = rt2661_bbp_read(sc, 18);
	sc->bbp21 = rt2661_bbp_read(sc, 21);
	sc->bbp22 = rt2661_bbp_read(sc, 22);
	sc->bbp16 = rt2661_bbp_read(sc, 16);
	sc->bbp17 = rt2661_bbp_read(sc, 17);
	sc->bbp64 = rt2661_bbp_read(sc, 64);

	rt2661_bbp_write(sc, 18, 0xff);
	rt2661_bbp_write(sc, 21, 0x3f);
	rt2661_bbp_write(sc, 22, 0x3f);
	rt2661_bbp_write(sc, 16, 0xbd);
	rt2661_bbp_write(sc, 17, sc->ext_5ghz_lna ? 0x44 : 0x34);
	rt2661_bbp_write(sc, 64, 0x21);

	/* restore Rx filter */
	RAL_WRITE(sc, RT2661_TXRX_CSR0, tmp);
}

static int
rt2661_radar_stop(struct rt2661_softc *sc)
{
	uint8_t bbp66;

	/* read radar detection result */
	bbp66 = rt2661_bbp_read(sc, 66);

	/* restore BBP registers values */
	rt2661_bbp_write(sc, 16, sc->bbp16);
	rt2661_bbp_write(sc, 17, sc->bbp17);
	rt2661_bbp_write(sc, 18, sc->bbp18);
	rt2661_bbp_write(sc, 21, sc->bbp21);
	rt2661_bbp_write(sc, 22, sc->bbp22);
	rt2661_bbp_write(sc, 64, sc->bbp64);

	return bbp66 == 1;
}
#endif

static int
rt2661_prepare_beacon(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_beacon_offsets bo;
	struct rt2661_tx_desc desc;
	struct mbuf *m0;
	int rate;

	m0 = ieee80211_beacon_alloc(ic, ic->ic_bss, &bo);
	if (m0 == NULL) {
		device_printf(sc->sc_dev, "could not allocate beacon frame\n");
		return ENOBUFS;
	}

	/* send beacons at the lowest available rate */
	rate = IEEE80211_IS_CHAN_5GHZ(ic->ic_bss->ni_chan) ? 12 : 2;

	rt2661_setup_tx_desc(sc, &desc, RT2661_TX_TIMESTAMP, RT2661_TX_HWSEQ,
	    m0->m_pkthdr.len, rate, NULL, 0, RT2661_QID_MGT, 0);

	/* copy the first 24 bytes of Tx descriptor into NIC memory */
	RAL_WRITE_REGION_1(sc, RT2661_HW_BEACON_BASE0, (uint8_t *)&desc, 24);

	/* copy beacon header and payload into NIC memory */
	RAL_WRITE_REGION_1(sc, RT2661_HW_BEACON_BASE0 + 24,
	    mtod(m0, uint8_t *), m0->m_pkthdr.len);

	m_freem(m0);
	return 0;
}

/*
 * Enable TSF synchronization and tell h/w to start sending beacons for IBSS
 * and HostAP operating modes.
 */
static void
rt2661_enable_tsf_sync(struct rt2661_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t tmp;

	if (ic->ic_opmode != IEEE80211_M_STA) {
		/*
		 * Change default 16ms TBTT adjustment to 8ms.
		 * Must be done before enabling beacon generation.
		 */
		RAL_WRITE(sc, RT2661_TXRX_CSR10, 1 << 12 | 8);
	}

	tmp = RAL_READ(sc, RT2661_TXRX_CSR9) & 0xff000000;

	/* set beacon interval (in 1/16ms unit) */
	tmp |= ic->ic_bss->ni_intval * 16;

	tmp |= RT2661_TSF_TICKING | RT2661_ENABLE_TBTT;
	if (ic->ic_opmode == IEEE80211_M_STA)
		tmp |= RT2661_TSF_MODE(1);
	else
		tmp |= RT2661_TSF_MODE(2) | RT2661_GENERATE_BEACON;

	RAL_WRITE(sc, RT2661_TXRX_CSR9, tmp);
}

/*
 * Retrieve the "Received Signal Strength Indicator" from the raw values
 * contained in Rx descriptors.  The computation depends on which band the
 * frame was received.  Correction values taken from the reference driver.
 */
static int
rt2661_get_rssi(struct rt2661_softc *sc, uint8_t raw)
{
	int lna, agc, rssi;

	lna = (raw >> 5) & 0x3;
	agc = raw & 0x1f;

	if (lna == 0) {
		/*
		 * No RSSI mapping
		 *
		 * NB: Since RSSI is relative to noise floor, -1 is
		 *     adequate for caller to know error happened.
		 */
		return -1;
	}

	rssi = (2 * agc) - RT2661_NOISE_FLOOR;

	if (IEEE80211_IS_CHAN_2GHZ(sc->sc_curchan)) {
		rssi += sc->rssi_2ghz_corr;

		if (lna == 1)
			rssi -= 64;
		else if (lna == 2)
			rssi -= 74;
		else if (lna == 3)
			rssi -= 90;
	} else {
		rssi += sc->rssi_5ghz_corr;

		if (lna == 1)
			rssi -= 64;
		else if (lna == 2)
			rssi -= 86;
		else if (lna == 3)
			rssi -= 100;
	}
	return rssi;
}

static void
rt2661_dma_map_mbuf(void *arg, bus_dma_segment_t *seg, int nseg,
		    bus_size_t map_size __unused, int error)
{
	struct rt2661_dmamap *map = arg;

	if (error)
		return;

	KASSERT(nseg <= RT2661_MAX_SCATTER, ("too many DMA segments"));

	bcopy(seg, map->segs, nseg * sizeof(bus_dma_segment_t));
	map->nseg = nseg;
}

static void
rt2661_led_newstate(struct rt2661_softc *sc, enum ieee80211_state nstate)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t off, on;
	uint32_t mail = sc->mcu_led;

	if (RAL_READ(sc, RT2661_H2M_MAILBOX_CSR) & RT2661_H2M_BUSY) {
		DPRINTF(("%s failed\n", __func__));
		return;
	}

	switch (nstate) {
	case IEEE80211_S_INIT:
		mail &= ~(RT2661_MCU_LED_LINKA | RT2661_MCU_LED_LINKG |
			  RT2661_MCU_LED_RF);
		break;
	default:
		if (ic->ic_curchan == NULL)
			return;

		on = RT2661_MCU_LED_LINKG;
		off = RT2661_MCU_LED_LINKA;
		if (IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan)) {
			on = RT2661_MCU_LED_LINKA;
			off = RT2661_MCU_LED_LINKG;
		}

		mail |= RT2661_MCU_LED_RF | on;
		mail &= ~off;
		break;
	}

	RAL_WRITE(sc, RT2661_H2M_MAILBOX_CSR,
		  RT2661_H2M_BUSY | RT2661_TOKEN_NO_INTR << 16 | mail);
	RAL_WRITE(sc, RT2661_HOST_CMD_CSR, RT2661_KICK_CMD | RT2661_MCU_SET_LED);
}
