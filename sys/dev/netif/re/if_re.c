/*
 * Copyright (c) 2004
 *	Joerg Sonnenberger <joerg@bec.de>.  All rights reserved.
 *
 * Copyright (c) 1997, 1998-2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
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
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/re/if_re.c,v 1.25 2004/06/09 14:34:01 naddy Exp $
 * $DragonFly: src/sys/dev/netif/re/if_re.c,v 1.49 2008/09/17 07:51:59 sephe Exp $
 */

/*
 * RealTek 8139C+/8169/8169S/8110S/8168/8111/8101E PCI NIC driver
 *
 * Written by Bill Paul <wpaul@windriver.com>
 * Senior Networking Software Engineer
 * Wind River Systems
 */

/*
 * This driver is designed to support RealTek's next generation of
 * 10/100 and 10/100/1000 PCI ethernet controllers. There are currently
 * seven devices in this family: the RTL8139C+, the RTL8169, the RTL8169S,
 * RTL8110S, the RTL8168, the RTL8111 and the RTL8101E.
 *
 * The 8139C+ is a 10/100 ethernet chip. It is backwards compatible
 * with the older 8139 family, however it also supports a special
 * C+ mode of operation that provides several new performance enhancing
 * features. These include:
 *
 *	o Descriptor based DMA mechanism. Each descriptor represents
 *	  a single packet fragment. Data buffers may be aligned on
 *	  any byte boundary.
 *
 *	o 64-bit DMA
 *
 *	o TCP/IP checksum offload for both RX and TX
 *
 *	o High and normal priority transmit DMA rings
 *
 *	o VLAN tag insertion and extraction
 *
 *	o TCP large send (segmentation offload)
 *
 * Like the 8139, the 8139C+ also has a built-in 10/100 PHY. The C+
 * programming API is fairly straightforward. The RX filtering, EEPROM
 * access and PHY access is the same as it is on the older 8139 series
 * chips.
 *
 * The 8169 is a 64-bit 10/100/1000 gigabit ethernet MAC. It has almost the
 * same programming API and feature set as the 8139C+ with the following
 * differences and additions:
 *
 *	o 1000Mbps mode
 *
 *	o Jumbo frames
 *
 * 	o GMII and TBI ports/registers for interfacing with copper
 *	  or fiber PHYs
 *
 *      o RX and TX DMA rings can have up to 1024 descriptors
 *        (the 8139C+ allows a maximum of 64)
 *
 *	o Slight differences in register layout from the 8139C+
 *
 * The TX start and timer interrupt registers are at different locations
 * on the 8169 than they are on the 8139C+. Also, the status word in the
 * RX descriptor has a slightly different bit layout. The 8169 does not
 * have a built-in PHY. Most reference boards use a Marvell 88E1000 'Alaska'
 * copper gigE PHY.
 *
 * The 8169S/8110S 10/100/1000 devices have built-in copper gigE PHYs
 * (the 'S' stands for 'single-chip'). These devices have the same
 * programming API as the older 8169, but also have some vendor-specific
 * registers for the on-board PHY. The 8110S is a LAN-on-motherboard
 * part designed to be pin-compatible with the RealTek 8100 10/100 chip.
 * 
 * This driver takes advantage of the RX and TX checksum offload and
 * VLAN tag insertion/extraction features. It also implements TX
 * interrupt moderation using the timer interrupt registers, which
 * significantly reduces TX interrupt load. There is also support
 * for jumbo frames, however the 8169/8169S/8110S can not transmit
 * jumbo frames larger than 7440, so the max MTU possible with this
 * driver is 7422 bytes.
 */

#include "opt_polling.h"
#include "opt_ethernet.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/interrupt.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/ifq_var.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/vlan/if_vlan_var.h>
#include <net/vlan/if_vlan_ether.h>

#include <dev/netif/mii_layer/mii.h>
#include <dev/netif/mii_layer/miivar.h>

#include <bus/pci/pcidevs.h>
#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>

/* "device miibus" required.  See GENERIC if you get errors here. */
#include "miibus_if.h"

#include <dev/netif/re/if_rereg.h>
#include <dev/netif/re/if_revar.h>

#define RE_CSUM_FEATURES    (CSUM_IP | CSUM_TCP | CSUM_UDP)
#if 0
#define RE_DISABLE_HWCSUM
#endif

/*
 * Various supported device vendors/types and their names.
 */
static const struct re_type re_devs[] = {
	{ PCI_VENDOR_DLINK, PCI_PRODUCT_DLINK_DGE528T, RE_HWREV_8169S,
		"D-Link DGE-528(T) Gigabit Ethernet Adapter" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8139, RE_HWREV_8139CPLUS,
		"RealTek 8139C+ 10/100BaseTX" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8101E, RE_HWREV_8101E,
		"RealTek 8101E PCIe 10/100baseTX" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8168, RE_HWREV_8168_SPIN1,
		"RealTek 8168/8111B PCIe Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8168, RE_HWREV_8168_SPIN2,
		"RealTek 8168/8111B PCIe Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8168, RE_HWREV_8168_SPIN3,
		"RealTek 8168B/8111B PCIe Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8168, RE_HWREV_8168C,
		"RealTek 8168C/8111C PCIe Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169, RE_HWREV_8169,
		"RealTek 8169 Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169, RE_HWREV_8169S,
		"RealTek 8169S Single-chip Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169, RE_HWREV_8169_8110SB,
		"RealTek 8169SB/8110SB Single-chip Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169, RE_HWREV_8169_8110SC,
		"RealTek 8169SC/8110SC Single-chip Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169SC, RE_HWREV_8169_8110SC,
		"RealTek 8169SC/8110SC Single-chip Gigabit Ethernet" },
	{ PCI_VENDOR_REALTEK, PCI_PRODUCT_REALTEK_RT8169, RE_HWREV_8110S,
		"RealTek 8110S Single-chip Gigabit Ethernet" },
	{ PCI_VENDOR_COREGA, PCI_PRODUCT_COREGA_CG_LAPCIGT, RE_HWREV_8169S,
		"Corega CG-LAPCIGT Gigabit Ethernet" },
	{ PCI_VENDOR_LINKSYS, PCI_PRODUCT_LINKSYS_EG1032, RE_HWREV_8169S,
		"Linksys EG1032 Gigabit Ethernet" },
	{ PCI_VENDOR_USR2, PCI_PRODUCT_USR2_997902, RE_HWREV_8169S,
		"US Robotics 997902 Gigabit Ethernet" },
	{ 0, 0, 0, NULL }
};

static const struct re_hwrev re_hwrevs[] = {
	{ RE_HWREV_8139CPLUS,	RE_8139CPLUS,	RE_F_HASMPC,	"C+" },
	{ RE_HWREV_8168_SPIN1,	RE_8169,	RE_F_PCIE,	"8168" },
	{ RE_HWREV_8168_SPIN2,	RE_8169,	RE_F_PCIE,	"8168" },
	{ RE_HWREV_8168_SPIN3,	RE_8169,	RE_F_PCIE,	"8168" },
	{ RE_HWREV_8168C,	RE_8169,	RE_F_PCIE,	"8168C" },
	{ RE_HWREV_8169,	RE_8169,	RE_F_HASMPC,	"8169" },
	{ RE_HWREV_8169S,	RE_8169,	RE_F_HASMPC,	"8169S" },
	{ RE_HWREV_8110S,	RE_8169,	RE_F_HASMPC,	"8110S" },
	{ RE_HWREV_8169_8110SB,	RE_8169,	RE_F_HASMPC,	"8169SB" },
	{ RE_HWREV_8169_8110SC,	RE_8169,	0,		"8169SC" },
	{ RE_HWREV_8100E,	RE_8169,	RE_F_HASMPC,	"8100E" },
	{ RE_HWREV_8101E,	RE_8169,	RE_F_PCIE,	"8101E" },
	{ 0, 0, 0, NULL }
};

static int	re_probe(device_t);
static int	re_attach(device_t);
static int	re_detach(device_t);
static int	re_suspend(device_t);
static int	re_resume(device_t);
static void	re_shutdown(device_t);

static int	re_encap(struct re_softc *, struct mbuf **, int *, int *);

static void	re_dma_map_addr(void *, bus_dma_segment_t *, int, int);
static void	re_dma_map_desc(void *, bus_dma_segment_t *, int,
				bus_size_t, int);
static int	re_allocmem(device_t, struct re_softc *);
static int	re_newbuf(struct re_softc *, int, struct mbuf *);
static int	re_rx_list_init(struct re_softc *);
static int	re_tx_list_init(struct re_softc *);
static void	re_rxeof(struct re_softc *);
static void	re_txeof(struct re_softc *);
static void	re_intr(void *);
static void	re_tick(void *);
static void	re_tick_serialized(void *);

static void	re_start(struct ifnet *);
static int	re_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	re_init(void *);
static void	re_stop(struct re_softc *);
static void	re_watchdog(struct ifnet *);
static int	re_ifmedia_upd(struct ifnet *);
static void	re_ifmedia_sts(struct ifnet *, struct ifmediareq *);

static void	re_eeprom_putbyte(struct re_softc *, int);
static void	re_eeprom_getword(struct re_softc *, int, u_int16_t *);
static void	re_read_eeprom(struct re_softc *, caddr_t, int, int);
static int	re_gmii_readreg(device_t, int, int);
static int	re_gmii_writereg(device_t, int, int, int);

static int	re_miibus_readreg(device_t, int, int);
static int	re_miibus_writereg(device_t, int, int, int);
static void	re_miibus_statchg(device_t);

static void	re_setmulti(struct re_softc *);
static void	re_reset(struct re_softc *);

#ifdef RE_DIAG
static int	re_diag(struct re_softc *);
#endif

#ifdef DEVICE_POLLING
static void	re_poll(struct ifnet *ifp, enum poll_cmd cmd, int count);
#endif

static int	re_sysctl_tx_moderation(SYSCTL_HANDLER_ARGS);

static device_method_t re_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		re_probe),
	DEVMETHOD(device_attach,	re_attach),
	DEVMETHOD(device_detach,	re_detach),
	DEVMETHOD(device_suspend,	re_suspend),
	DEVMETHOD(device_resume,	re_resume),
	DEVMETHOD(device_shutdown,	re_shutdown),

	/* bus interface */
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD(bus_driver_added,	bus_generic_driver_added),

	/* MII interface */
	DEVMETHOD(miibus_readreg,	re_miibus_readreg),
	DEVMETHOD(miibus_writereg,	re_miibus_writereg),
	DEVMETHOD(miibus_statchg,	re_miibus_statchg),

	{ 0, 0 }
};

static driver_t re_driver = {
	"re",
	re_methods,
	sizeof(struct re_softc)
};

static devclass_t re_devclass;

DECLARE_DUMMY_MODULE(if_re);
DRIVER_MODULE(if_re, pci, re_driver, re_devclass, 0, 0);
DRIVER_MODULE(if_re, cardbus, re_driver, re_devclass, 0, 0);
DRIVER_MODULE(miibus, re, miibus_driver, miibus_devclass, 0, 0);

#define EE_SET(x)	\
	CSR_WRITE_1(sc, RE_EECMD, CSR_READ_1(sc, RE_EECMD) | (x))

#define EE_CLR(x)	\
	CSR_WRITE_1(sc, RE_EECMD, CSR_READ_1(sc, RE_EECMD) & ~(x))

/*
 * Send a read command and address to the EEPROM, check for ACK.
 */
static void
re_eeprom_putbyte(struct re_softc *sc, int addr)
{
	int d, i;

	d = addr | (RE_9346_READ << sc->re_eewidth);

	/*
	 * Feed in each bit and strobe the clock.
	 */
	for (i = 1 << (sc->re_eewidth + 3); i; i >>= 1) {
		if (d & i)
			EE_SET(RE_EE_DATAIN);
		else
			EE_CLR(RE_EE_DATAIN);
		DELAY(100);
		EE_SET(RE_EE_CLK);
		DELAY(150);
		EE_CLR(RE_EE_CLK);
		DELAY(100);
	}
}

/*
 * Read a word of data stored in the EEPROM at address 'addr.'
 */
static void
re_eeprom_getword(struct re_softc *sc, int addr, uint16_t *dest)
{
	int i;
	uint16_t word = 0;

	/*
	 * Send address of word we want to read.
	 */
	re_eeprom_putbyte(sc, addr);

	/*
	 * Start reading bits from EEPROM.
	 */
	for (i = 0x8000; i != 0; i >>= 1) {
		EE_SET(RE_EE_CLK);
		DELAY(100);
		if (CSR_READ_1(sc, RE_EECMD) & RE_EE_DATAOUT)
			word |= i;
		EE_CLR(RE_EE_CLK);
		DELAY(100);
	}

	*dest = word;
}

/*
 * Read a sequence of words from the EEPROM.
 */
static void
re_read_eeprom(struct re_softc *sc, caddr_t dest, int off, int cnt)
{
	int i;
	uint16_t word = 0, *ptr;

	CSR_SETBIT_1(sc, RE_EECMD, RE_EEMODE_PROGRAM);
	DELAY(100);

	for (i = 0; i < cnt; i++) {
		CSR_SETBIT_1(sc, RE_EECMD, RE_EE_SEL);
		re_eeprom_getword(sc, off + i, &word);
		CSR_CLRBIT_1(sc, RE_EECMD, RE_EE_SEL);
		ptr = (uint16_t *)(dest + (i * 2));
		*ptr = word;
	}

	CSR_CLRBIT_1(sc, RE_EECMD, RE_EEMODE_PROGRAM);
}

static int
re_gmii_readreg(device_t dev, int phy, int reg)
{
	struct re_softc *sc = device_get_softc(dev);
	u_int32_t rval;
	int i;

	if (phy != 1)
		return(0);

	/* Let the rgephy driver read the GMEDIASTAT register */

	if (reg == RE_GMEDIASTAT)
		return(CSR_READ_1(sc, RE_GMEDIASTAT));

	CSR_WRITE_4(sc, RE_PHYAR, reg << 16);
	DELAY(1000);

	for (i = 0; i < RE_TIMEOUT; i++) {
		rval = CSR_READ_4(sc, RE_PHYAR);
		if (rval & RE_PHYAR_BUSY)
			break;
		DELAY(100);
	}

	if (i == RE_TIMEOUT) {
		device_printf(dev, "PHY read failed\n");
		return(0);
	}

	return(rval & RE_PHYAR_PHYDATA);
}

static int
re_gmii_writereg(device_t dev, int phy, int reg, int data)
{
	struct re_softc *sc = device_get_softc(dev);
	uint32_t rval;
	int i;

	CSR_WRITE_4(sc, RE_PHYAR,
		    (reg << 16) | (data & RE_PHYAR_PHYDATA) | RE_PHYAR_BUSY);
	DELAY(1000);

	for (i = 0; i < RE_TIMEOUT; i++) {
		rval = CSR_READ_4(sc, RE_PHYAR);
		if ((rval & RE_PHYAR_BUSY) == 0)
			break;
		DELAY(100);
	}

	if (i == RE_TIMEOUT)
		device_printf(dev, "PHY write failed\n");

	return(0);
}

static int
re_miibus_readreg(device_t dev, int phy, int reg)
{
	struct re_softc	*sc = device_get_softc(dev);
	uint16_t rval = 0;
	uint16_t re8139_reg = 0;

	if (sc->re_type == RE_8169) {
		rval = re_gmii_readreg(dev, phy, reg);
		return(rval);
	}

	/* Pretend the internal PHY is only at address 0 */
	if (phy)
		return(0);

	switch(reg) {
	case MII_BMCR:
		re8139_reg = RE_BMCR;
		break;
	case MII_BMSR:
		re8139_reg = RE_BMSR;
		break;
	case MII_ANAR:
		re8139_reg = RE_ANAR;
		break;
	case MII_ANER:
		re8139_reg = RE_ANER;
		break;
	case MII_ANLPAR:
		re8139_reg = RE_LPAR;
		break;
	case MII_PHYIDR1:
	case MII_PHYIDR2:
		return(0);
	/*
	 * Allow the rlphy driver to read the media status
	 * register. If we have a link partner which does not
	 * support NWAY, this is the register which will tell
	 * us the results of parallel detection.
	 */
	case RE_MEDIASTAT:
		return(CSR_READ_1(sc, RE_MEDIASTAT));
	default:
		device_printf(dev, "bad phy register\n");
		return(0);
	}
	rval = CSR_READ_2(sc, re8139_reg);
	if (sc->re_type == RE_8139CPLUS && re8139_reg == RE_BMCR) {
		/* 8139C+ has different bit layout. */
		rval &= ~(BMCR_LOOP | BMCR_ISO);
	}
	return(rval);
}

static int
re_miibus_writereg(device_t dev, int phy, int reg, int data)
{
	struct re_softc *sc= device_get_softc(dev);
	u_int16_t re8139_reg = 0;

	if (sc->re_type == RE_8169)
		return(re_gmii_writereg(dev, phy, reg, data));

	/* Pretend the internal PHY is only at address 0 */
	if (phy)
		return(0);

	switch(reg) {
	case MII_BMCR:
		re8139_reg = RE_BMCR;
		if (sc->re_type == RE_8139CPLUS) {
			/* 8139C+ has different bit layout. */
			data &= ~(BMCR_LOOP | BMCR_ISO);
		}
		break;
	case MII_BMSR:
		re8139_reg = RE_BMSR;
		break;
	case MII_ANAR:
		re8139_reg = RE_ANAR;
		break;
	case MII_ANER:
		re8139_reg = RE_ANER;
		break;
	case MII_ANLPAR:
		re8139_reg = RE_LPAR;
		break;
	case MII_PHYIDR1:
	case MII_PHYIDR2:
		return(0);
	default:
		device_printf(dev, "bad phy register\n");
		return(0);
	}
	CSR_WRITE_2(sc, re8139_reg, data);
	return(0);
}

static void
re_miibus_statchg(device_t dev)
{
}

/*
 * Program the 64-bit multicast hash filter.
 */
static void
re_setmulti(struct re_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int h = 0;
	uint32_t hashes[2] = { 0, 0 };
	struct ifmultiaddr *ifma;
	uint32_t rxfilt;
	int mcnt = 0;

	rxfilt = CSR_READ_4(sc, RE_RXCFG);

	if (ifp->if_flags & IFF_ALLMULTI || ifp->if_flags & IFF_PROMISC) {
		rxfilt |= RE_RXCFG_RX_MULTI;
		CSR_WRITE_4(sc, RE_RXCFG, rxfilt);
		CSR_WRITE_4(sc, RE_MAR0, 0xFFFFFFFF);
		CSR_WRITE_4(sc, RE_MAR4, 0xFFFFFFFF);
		return;
	}

	/* first, zot all the existing hash bits */
	CSR_WRITE_4(sc, RE_MAR0, 0);
	CSR_WRITE_4(sc, RE_MAR4, 0);

	/* now program new ones */
	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;
		h = ether_crc32_be(LLADDR((struct sockaddr_dl *)
		    ifma->ifma_addr), ETHER_ADDR_LEN) >> 26;
		if (h < 32)
			hashes[0] |= (1 << h);
		else
			hashes[1] |= (1 << (h - 32));
		mcnt++;
	}

	if (mcnt)
		rxfilt |= RE_RXCFG_RX_MULTI;
	else
		rxfilt &= ~RE_RXCFG_RX_MULTI;

	CSR_WRITE_4(sc, RE_RXCFG, rxfilt);

	/*
	 * For some unfathomable reason, RealTek decided to reverse
	 * the order of the multicast hash registers in the PCI Express
	 * parts. This means we have to write the hash pattern in reverse
	 * order for those devices.
	 */
	if (sc->re_flags & RE_F_PCIE) {
		CSR_WRITE_4(sc, RE_MAR0, bswap32(hashes[0]));
		CSR_WRITE_4(sc, RE_MAR4, bswap32(hashes[1]));
	} else {
		CSR_WRITE_4(sc, RE_MAR0, hashes[0]);
		CSR_WRITE_4(sc, RE_MAR4, hashes[1]);
	}
}

static void
re_reset(struct re_softc *sc)
{
	int i;

	CSR_WRITE_1(sc, RE_COMMAND, RE_CMD_RESET);

	for (i = 0; i < RE_TIMEOUT; i++) {
		DELAY(10);
		if ((CSR_READ_1(sc, RE_COMMAND) & RE_CMD_RESET) == 0)
			break;
	}
	if (i == RE_TIMEOUT)
		if_printf(&sc->arpcom.ac_if, "reset never completed!\n");

	CSR_WRITE_1(sc, 0x82, 1);
}

#ifdef RE_DIAG
/*
 * The following routine is designed to test for a defect on some
 * 32-bit 8169 cards. Some of these NICs have the REQ64# and ACK64#
 * lines connected to the bus, however for a 32-bit only card, they
 * should be pulled high. The result of this defect is that the
 * NIC will not work right if you plug it into a 64-bit slot: DMA
 * operations will be done with 64-bit transfers, which will fail
 * because the 64-bit data lines aren't connected.
 *
 * There's no way to work around this (short of talking a soldering
 * iron to the board), however we can detect it. The method we use
 * here is to put the NIC into digital loopback mode, set the receiver
 * to promiscuous mode, and then try to send a frame. We then compare
 * the frame data we sent to what was received. If the data matches,
 * then the NIC is working correctly, otherwise we know the user has
 * a defective NIC which has been mistakenly plugged into a 64-bit PCI
 * slot. In the latter case, there's no way the NIC can work correctly,
 * so we print out a message on the console and abort the device attach.
 */

static int
re_diag(struct re_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mbuf *m0;
	struct ether_header *eh;
	struct re_desc *cur_rx;
	uint16_t status;
	uint32_t rxstat;
	int total_len, i, error = 0, phyaddr;
	uint8_t dst[ETHER_ADDR_LEN] = { 0x00, 'h', 'e', 'l', 'l', 'o' };
	uint8_t src[ETHER_ADDR_LEN] = { 0x00, 'w', 'o', 'r', 'l', 'd' };

	/* Allocate a single mbuf */

	MGETHDR(m0, MB_DONTWAIT, MT_DATA);
	if (m0 == NULL)
		return(ENOBUFS);

	/*
	 * Initialize the NIC in test mode. This sets the chip up
	 * so that it can send and receive frames, but performs the
	 * following special functions:
	 * - Puts receiver in promiscuous mode
	 * - Enables digital loopback mode
	 * - Leaves interrupts turned off
	 */

	ifp->if_flags |= IFF_PROMISC;
	sc->re_testmode = 1;
	re_reset(sc);
	re_init(sc);
	sc->re_link = 1;
	if (sc->re_type == RE_8169)
		phyaddr = 1;
	else
		phyaddr = 0;

	re_miibus_writereg(sc->re_dev, phyaddr, MII_BMCR, BMCR_RESET);
	for (i = 0; i < RE_TIMEOUT; i++) {
		status = re_miibus_readreg(sc->re_dev, phyaddr, MII_BMCR);
		if (!(status & BMCR_RESET))
			break;
	}

	re_miibus_writereg(sc->re_dev, phyaddr, MII_BMCR, BMCR_LOOP);
	CSR_WRITE_2(sc, RE_ISR, RE_INTRS_DIAG);

	DELAY(100000);

	/* Put some data in the mbuf */

	eh = mtod(m0, struct ether_header *);
	bcopy (dst, eh->ether_dhost, ETHER_ADDR_LEN);
	bcopy (src, eh->ether_shost, ETHER_ADDR_LEN);
	eh->ether_type = htons(ETHERTYPE_IP);
	m0->m_pkthdr.len = m0->m_len = ETHER_MIN_LEN - ETHER_CRC_LEN;

	/*
	 * Queue the packet, start transmission.
	 * Note: ifq_handoff() ultimately calls re_start() for us.
	 */

	CSR_WRITE_2(sc, RE_ISR, 0xFFFF);
	error = ifq_handoff(ifp, m0, NULL);
	if (error) {
		m0 = NULL;
		goto done;
	}
	m0 = NULL;

	/* Wait for it to propagate through the chip */

	DELAY(100000);
	for (i = 0; i < RE_TIMEOUT; i++) {
		status = CSR_READ_2(sc, RE_ISR);
		CSR_WRITE_2(sc, RE_ISR, status);
		if ((status & (RE_ISR_TIMEOUT_EXPIRED|RE_ISR_RX_OK)) ==
		    (RE_ISR_TIMEOUT_EXPIRED|RE_ISR_RX_OK))
			break;
		DELAY(10);
	}

	if (i == RE_TIMEOUT) {
		if_printf(ifp, "diagnostic failed to receive packet "
			  "in loopback mode\n");
		error = EIO;
		goto done;
	}

	/*
	 * The packet should have been dumped into the first
	 * entry in the RX DMA ring. Grab it from there.
	 */

	bus_dmamap_sync(sc->re_ldata.re_rx_list_tag,
			sc->re_ldata.re_rx_list_map, BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(sc->re_ldata.re_mtag, sc->re_ldata.re_rx_dmamap[0],
			BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->re_ldata.re_mtag, sc->re_ldata.re_rx_dmamap[0]);

	m0 = sc->re_ldata.re_rx_mbuf[0];
	sc->re_ldata.re_rx_mbuf[0] = NULL;
	eh = mtod(m0, struct ether_header *);

	cur_rx = &sc->re_ldata.re_rx_list[0];
	total_len = RE_RXBYTES(cur_rx);
	rxstat = le32toh(cur_rx->re_cmdstat);

	if (total_len != ETHER_MIN_LEN) {
		if_printf(ifp, "diagnostic failed, received short packet\n");
		error = EIO;
		goto done;
	}

	/* Test that the received packet data matches what we sent. */

	if (bcmp(eh->ether_dhost, dst, ETHER_ADDR_LEN) ||
	    bcmp(eh->ether_shost, &src, ETHER_ADDR_LEN) ||
	    be16toh(eh->ether_type) != ETHERTYPE_IP) {
		if_printf(ifp, "WARNING, DMA FAILURE!\n");
		if_printf(ifp, "expected TX data: %6D/%6D/0x%x\n",
		    dst, ":", src, ":", ETHERTYPE_IP);
		if_printf(ifp, "received RX data: %6D/%6D/0x%x\n",
		    eh->ether_dhost, ":",  eh->ether_shost, ":",
		    ntohs(eh->ether_type));
		if_printf(ifp, "You may have a defective 32-bit NIC plugged "
		    "into a 64-bit PCI slot.\n");
		if_printf(ifp, "Please re-install the NIC in a 32-bit slot "
		    "for proper operation.\n");
		if_printf(ifp, "Read the re(4) man page for more details.\n");
		error = EIO;
	}

done:
	/* Turn interface off, release resources */

	sc->re_testmode = 0;
	sc->re_link = 0;
	ifp->if_flags &= ~IFF_PROMISC;
	re_stop(sc);
	if (m0 != NULL)
		m_freem(m0);

	return (error);
}
#endif	/* RE_DIAG */

/*
 * Probe for a RealTek 8139C+/8169/8110 chip. Check the PCI vendor and device
 * IDs against our list and return a device name if we find a match.
 */
static int
re_probe(device_t dev)
{
	const struct re_type *t;
	struct re_softc *sc;
	int rid;
	uint32_t hwrev;
	uint16_t vendor, product;

	vendor = pci_get_vendor(dev);
	product = pci_get_device(dev);

	/*
	 * Only attach to rev.3 of the Linksys EG1032 adapter.
	 * Rev.2 is supported by sk(4).
	 */
	if (vendor == PCI_VENDOR_LINKSYS &&
	    product == PCI_PRODUCT_LINKSYS_EG1032 &&
	    pci_get_subdevice(dev) != PCI_SUBDEVICE_LINKSYS_EG1032_REV3)
			return ENXIO;

	for (t = re_devs; t->re_name != NULL; t++) {
		if (product == t->re_did && vendor == t->re_vid)
			break;
	}

	/*
	 * Check if we found a RealTek device.
	 */
	if (t->re_name == NULL)
		return(ENXIO);

	/*
	 * Temporarily map the I/O space so we can read the chip ID register.
	 */
	sc = kmalloc(sizeof(*sc), M_TEMP, M_WAITOK | M_ZERO);
	rid = RE_PCI_LOIO;
	sc->re_res = bus_alloc_resource_any(dev, SYS_RES_IOPORT, &rid,
					    RF_ACTIVE);
	if (sc->re_res == NULL) {
		device_printf(dev, "couldn't map ports/memory\n");
		kfree(sc, M_TEMP);
		return(ENXIO);
	}

	sc->re_btag = rman_get_bustag(sc->re_res);
	sc->re_bhandle = rman_get_bushandle(sc->re_res);

	hwrev = CSR_READ_4(sc, RE_TXCFG) & RE_TXCFG_HWREV;
	bus_release_resource(dev, SYS_RES_IOPORT, RE_PCI_LOIO, sc->re_res);
	kfree(sc, M_TEMP);

	/*
	 * and continue matching for the specific chip...
	 */
	for (; t->re_name != NULL; t++) {
		if (product == t->re_did && vendor == t->re_vid &&
		    t->re_basetype == hwrev) {
			device_set_desc(dev, t->re_name);
			return(0);
		}
	}

	if (bootverbose)
		kprintf("re: unknown hwrev %#x\n", hwrev);
	return(ENXIO);
}

/*
 * This routine takes the segment list provided as the result of
 * a bus_dma_map_load() operation and assigns the addresses/lengths
 * to RealTek DMA descriptors. This can be called either by the RX
 * code or the TX code. In the RX case, we'll probably wind up mapping
 * at most one segment. For the TX case, there could be any number of
 * segments since TX packets may span multiple mbufs. In either case,
 * if the number of segments is larger than the re_maxsegs limit
 * specified by the caller, we abort the mapping operation. Sadly,
 * whoever designed the buffer mapping API did not provide a way to
 * return an error from here, so we have to fake it a bit.
 */

static void
re_dma_map_desc(void *arg, bus_dma_segment_t *segs, int nseg,
		bus_size_t mapsize, int error)
{
	struct re_dmaload_arg *ctx;
	struct re_desc *d = NULL;
	int i = 0, idx;
	uint32_t cmdstat;

	if (error)
		return;

	ctx = arg;

	/* Signal error to caller if there's too many segments */
	if (nseg > ctx->re_maxsegs) {
		ctx->re_maxsegs = 0;
		return;
	}

	/*
	 * Map the segment array into descriptors. Note that we set the
	 * start-of-frame and end-of-frame markers for either TX or RX, but
	 * they really only have meaning in the TX case. (In the RX case,
	 * it's the chip that tells us where packets begin and end.)
	 * We also keep track of the end of the ring and set the
	 * end-of-ring bits as needed, and we set the ownership bits
	 * in all except the very first descriptor. (The caller will
	 * set this descriptor later when it start transmission or
	 * reception.)
	 */
	idx = ctx->re_idx;
	for (;;) {
		d = &ctx->re_ring[idx];
		if (le32toh(d->re_cmdstat) & RE_RDESC_STAT_OWN) {
			ctx->re_maxsegs = 0;
			return;
		}
		cmdstat = segs[i].ds_len;
		d->re_bufaddr_lo = htole32(RE_ADDR_LO(segs[i].ds_addr));
		d->re_bufaddr_hi = htole32(RE_ADDR_HI(segs[i].ds_addr));
		if (i == 0)
			cmdstat |= RE_TDESC_CMD_SOF;
		else
			cmdstat |= RE_TDESC_CMD_OWN;
		if (idx == (RE_RX_DESC_CNT - 1))
			cmdstat |= RE_TDESC_CMD_EOR;
		d->re_cmdstat = htole32(cmdstat | ctx->re_flags);
		i++;
		if (i == nseg)
			break;
		RE_DESC_INC(idx);
	}

	d->re_cmdstat |= htole32(RE_TDESC_CMD_EOF);
	ctx->re_maxsegs = nseg;
	ctx->re_idx = idx;
}

/*
 * Map a single buffer address.
 */

static void
re_dma_map_addr(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	uint32_t *addr;

	if (error)
		return;

	KASSERT(nseg == 1, ("too many DMA segments, %d should be 1", nseg));
	addr = arg;
	*addr = segs->ds_addr;
}

static int
re_allocmem(device_t dev, struct re_softc *sc)
{
	int error, i, nseg;

	/*
	 * Allocate map for RX mbufs.
	 */
	nseg = 32;
	error = bus_dma_tag_create(sc->re_parent_tag, ETHER_ALIGN, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL,
	    NULL, MCLBYTES * nseg, nseg, MCLBYTES, BUS_DMA_ALLOCNOW,
	    &sc->re_ldata.re_mtag);
	if (error) {
		device_printf(dev, "could not allocate dma tag\n");
		return(error);
	}

	/*
	 * Allocate map for TX descriptor list.
	 */
	error = bus_dma_tag_create(sc->re_parent_tag, RE_RING_ALIGN,
	    0, BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL,
            NULL, RE_TX_LIST_SZ, 1, RE_TX_LIST_SZ, BUS_DMA_ALLOCNOW,
	    &sc->re_ldata.re_tx_list_tag);
	if (error) {
		device_printf(dev, "could not allocate dma tag\n");
		return(error);
	}

	/* Allocate DMA'able memory for the TX ring */

        error = bus_dmamem_alloc(sc->re_ldata.re_tx_list_tag,
	    (void **)&sc->re_ldata.re_tx_list, BUS_DMA_WAITOK | BUS_DMA_ZERO,
            &sc->re_ldata.re_tx_list_map);
        if (error) {
		device_printf(dev, "could not allocate TX ring\n");
                return(error);
	}

	/* Load the map for the TX ring. */

	error = bus_dmamap_load(sc->re_ldata.re_tx_list_tag,
	     sc->re_ldata.re_tx_list_map, sc->re_ldata.re_tx_list,
	     RE_TX_LIST_SZ, re_dma_map_addr,
	     &sc->re_ldata.re_tx_list_addr, BUS_DMA_NOWAIT);
	if (error) {
		device_printf(dev, "could not get address of TX ring\n");
		return(error);
	}

	/* Create DMA maps for TX buffers */

	for (i = 0; i < RE_TX_DESC_CNT; i++) {
		error = bus_dmamap_create(sc->re_ldata.re_mtag, 0,
			    &sc->re_ldata.re_tx_dmamap[i]);
		if (error) {
			device_printf(dev, "can't create DMA map for TX\n");
			return(error);
		}
	}

	/*
	 * Allocate map for RX descriptor list.
	 */
	error = bus_dma_tag_create(sc->re_parent_tag, RE_RING_ALIGN,
	    0, BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL,
            NULL, RE_RX_LIST_SZ, 1, RE_RX_LIST_SZ, BUS_DMA_ALLOCNOW,
	    &sc->re_ldata.re_rx_list_tag);
	if (error) {
		device_printf(dev, "could not allocate dma tag\n");
		return(error);
	}

	/* Allocate DMA'able memory for the RX ring */

        error = bus_dmamem_alloc(sc->re_ldata.re_rx_list_tag,
	    (void **)&sc->re_ldata.re_rx_list, BUS_DMA_WAITOK | BUS_DMA_ZERO,
            &sc->re_ldata.re_rx_list_map);
        if (error) {
		device_printf(dev, "could not allocate RX ring\n");
                return(error);
	}

	/* Load the map for the RX ring. */

	error = bus_dmamap_load(sc->re_ldata.re_rx_list_tag,
	     sc->re_ldata.re_rx_list_map, sc->re_ldata.re_rx_list,
	     RE_RX_LIST_SZ, re_dma_map_addr,
	     &sc->re_ldata.re_rx_list_addr, BUS_DMA_NOWAIT);
	if (error) {
		device_printf(dev, "could not get address of RX ring\n");
		return(error);
	}

	/* Create DMA maps for RX buffers */

	for (i = 0; i < RE_RX_DESC_CNT; i++) {
		error = bus_dmamap_create(sc->re_ldata.re_mtag, 0,
			    &sc->re_ldata.re_rx_dmamap[i]);
		if (error) {
			device_printf(dev, "can't create DMA map for RX\n");
			return(ENOMEM);
		}
	}

	return(0);
}

/*
 * Attach the interface. Allocate softc structures, do ifmedia
 * setup and ethernet/BPF attach.
 */
static int
re_attach(device_t dev)
{
	struct re_softc	*sc = device_get_softc(dev);
	struct ifnet *ifp;
	const struct re_hwrev *hw_rev;
	uint8_t eaddr[ETHER_ADDR_LEN];
	uint16_t as[ETHER_ADDR_LEN / 2];
	uint16_t re_did = 0;
	uint32_t hwrev;
	int error = 0, rid, i;

	callout_init(&sc->re_timer);
#ifdef RE_DIAG
	sc->re_dev = dev;
#endif

	RE_ENABLE_TX_MODERATION(sc);

	sysctl_ctx_init(&sc->re_sysctl_ctx);
	sc->re_sysctl_tree = SYSCTL_ADD_NODE(&sc->re_sysctl_ctx,
					     SYSCTL_STATIC_CHILDREN(_hw),
					     OID_AUTO,
					     device_get_nameunit(dev),
					     CTLFLAG_RD, 0, "");
	if (sc->re_sysctl_tree == NULL) {
		device_printf(dev, "can't add sysctl node\n");
		error = ENXIO;
		goto fail;
	}
	SYSCTL_ADD_PROC(&sc->re_sysctl_ctx,
			SYSCTL_CHILDREN(sc->re_sysctl_tree),
			OID_AUTO, "tx_moderation",
			CTLTYPE_INT | CTLFLAG_RW,
			sc, 0, re_sysctl_tx_moderation, "I",
			"Enable/Disable TX moderation");

#ifndef BURN_BRIDGES
	/*
	 * Handle power management nonsense.
	 */

	if (pci_get_powerstate(dev) != PCI_POWERSTATE_D0) {
		uint32_t membase, irq;

		/* Save important PCI config data. */
		membase = pci_read_config(dev, RE_PCI_LOMEM, 4);
		irq = pci_read_config(dev, PCIR_INTLINE, 4);

		/* Reset the power state. */
		device_printf(dev, "chip is in D%d power mode "
		    "-- setting to D0\n", pci_get_powerstate(dev));

		pci_set_powerstate(dev, PCI_POWERSTATE_D0);

		/* Restore PCI config data. */
		pci_write_config(dev, RE_PCI_LOMEM, membase, 4);
		pci_write_config(dev, PCIR_INTLINE, irq, 4);
	}
#endif
	/*
	 * Map control/status registers.
	 */
	pci_enable_busmaster(dev);

	rid = RE_PCI_LOIO;
	sc->re_res = bus_alloc_resource_any(dev, SYS_RES_IOPORT, &rid,
					    RF_ACTIVE);

	if (sc->re_res == NULL) {
		device_printf(dev, "couldn't map ports\n");
		error = ENXIO;
		goto fail;
	}

	sc->re_btag = rman_get_bustag(sc->re_res);
	sc->re_bhandle = rman_get_bushandle(sc->re_res);

	/* Allocate interrupt */
	rid = 0;
	sc->re_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
					    RF_SHAREABLE | RF_ACTIVE);

	if (sc->re_irq == NULL) {
		device_printf(dev, "couldn't map interrupt\n");
		error = ENXIO;
		goto fail;
	}

	/* Reset the adapter. */
	re_reset(sc);

	hwrev = CSR_READ_4(sc, RE_TXCFG) & RE_TXCFG_HWREV;
	for (hw_rev = re_hwrevs; hw_rev->re_desc != NULL; hw_rev++) {
		if (hw_rev->re_rev == hwrev) {
			sc->re_type = hw_rev->re_type;
			sc->re_flags = hw_rev->re_flags;
			break;
		}
	}

	sc->re_eewidth = 6;
	re_read_eeprom(sc, (caddr_t)&re_did, 0, 1);
	if (re_did != 0x8129)
	        sc->re_eewidth = 8;

	/*
	 * Get station address from the EEPROM.
	 */
	re_read_eeprom(sc, (caddr_t)as, RE_EE_EADDR, 3);
	for (i = 0; i < ETHER_ADDR_LEN / 2; i++)
		as[i] = le16toh(as[i]);
	bcopy(as, eaddr, sizeof(eaddr));

	if (sc->re_type == RE_8169) {
		/* Set RX length mask */
		sc->re_rxlenmask = RE_RDESC_STAT_GFRAGLEN;
		sc->re_txstart = RE_GTXSTART;
	} else {
		/* Set RX length mask */
		sc->re_rxlenmask = RE_RDESC_STAT_FRAGLEN;
		sc->re_txstart = RE_TXSTART;
	}

	/*
	 * Allocate the parent bus DMA tag appropriate for PCI.
	 */
#define RE_NSEG_NEW 32
	error = bus_dma_tag_create(NULL,	/* parent */
			1, 0,			/* alignment, boundary */
			BUS_SPACE_MAXADDR_32BIT,/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			MAXBSIZE, RE_NSEG_NEW,	/* maxsize, nsegments */
			BUS_SPACE_MAXSIZE_32BIT,/* maxsegsize */
			BUS_DMA_ALLOCNOW,	/* flags */
			&sc->re_parent_tag);
	if (error)
		goto fail;

	error = re_allocmem(dev, sc);

	if (error)
		goto fail;

	/* Do MII setup */
	if (mii_phy_probe(dev, &sc->re_miibus,
	    re_ifmedia_upd, re_ifmedia_sts)) {
		device_printf(dev, "MII without any phy!\n");
		error = ENXIO;
		goto fail;
	}

	ifp = &sc->arpcom.ac_if;
	ifp->if_softc = sc;
	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_mtu = ETHERMTU;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = re_ioctl;
	ifp->if_start = re_start;
	ifp->if_capabilities = IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING;
	if (hwrev != RE_HWREV_8168C)	/* XXX does not work yet */
		ifp->if_capabilities |= IFCAP_HWCSUM;
#ifdef DEVICE_POLLING
	ifp->if_poll = re_poll;
#endif
	ifp->if_watchdog = re_watchdog;
	ifp->if_init = re_init;
	if (sc->re_type == RE_8169)
		ifp->if_baudrate = 1000000000;
	else
		ifp->if_baudrate = 100000000;
	ifq_set_maxlen(&ifp->if_snd, RE_IFQ_MAXLEN);
	ifq_set_ready(&ifp->if_snd);

#ifdef RE_DISABLE_HWCSUM
	ifp->if_capenable = ifp->if_capabilities & ~IFCAP_HWCSUM;
	ifp->if_hwassist = 0;
#else
	ifp->if_capenable = ifp->if_capabilities;
	if (ifp->if_capabilities & IFCAP_HWCSUM)
		ifp->if_hwassist = RE_CSUM_FEATURES;
	else
		ifp->if_hwassist = 0;
#endif	/* RE_DISABLE_HWCSUM */

	/*
	 * Call MI attach routine.
	 */
	ether_ifattach(ifp, eaddr, NULL);

#ifdef RE_DIAG
	/*
	 * Perform hardware diagnostic on the original RTL8169.
	 * Some 32-bit cards were incorrectly wired and would
	 * malfunction if plugged into a 64-bit slot.
	 */
	if (hwrev == RE_HWREV_8169) {
		lwkt_serialize_enter(ifp->if_serializer);
		error = re_diag(sc);
		lwkt_serialize_exit(ifp->if_serializer);

		if (error) {
			device_printf(dev, "hardware diagnostic failure\n");
			ether_ifdetach(ifp);
			goto fail;
		}
	}
#endif	/* RE_DIAG */

	/* Hook interrupt last to avoid having to lock softc */
	error = bus_setup_intr(dev, sc->re_irq, INTR_MPSAFE, re_intr, sc,
			       &sc->re_intrhand, ifp->if_serializer);

	if (error) {
		device_printf(dev, "couldn't set up irq\n");
		ether_ifdetach(ifp);
		goto fail;
	}

	ifp->if_cpuid = ithread_cpuid(rman_get_start(sc->re_irq));
	KKASSERT(ifp->if_cpuid >= 0 && ifp->if_cpuid < ncpus);

fail:
	if (error)
		re_detach(dev);

	return (error);
}

/*
 * Shutdown hardware and free up resources. This can be called any
 * time after the mutex has been initialized. It is called in both
 * the error case in attach and the normal detach case so it needs
 * to be careful about only freeing resources that have actually been
 * allocated.
 */
static int
re_detach(device_t dev)
{
	struct re_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int i;

	/* These should only be active if attach succeeded */
	if (device_is_attached(dev)) {
		lwkt_serialize_enter(ifp->if_serializer);
		re_stop(sc);
		bus_teardown_intr(dev, sc->re_irq, sc->re_intrhand);
		lwkt_serialize_exit(ifp->if_serializer);

		ether_ifdetach(ifp);
	}
	if (sc->re_miibus)
		device_delete_child(dev, sc->re_miibus);
	bus_generic_detach(dev);

	if (sc->re_irq)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->re_irq);
	if (sc->re_res) {
		bus_release_resource(dev, SYS_RES_IOPORT, RE_PCI_LOIO,
				     sc->re_res);
	}

	/* Unload and free the RX DMA ring memory and map */

	if (sc->re_ldata.re_rx_list_tag) {
		bus_dmamap_unload(sc->re_ldata.re_rx_list_tag,
		    sc->re_ldata.re_rx_list_map);
		bus_dmamem_free(sc->re_ldata.re_rx_list_tag,
		    sc->re_ldata.re_rx_list,
		    sc->re_ldata.re_rx_list_map);
		bus_dma_tag_destroy(sc->re_ldata.re_rx_list_tag);
	}

	/* Unload and free the TX DMA ring memory and map */

	if (sc->re_ldata.re_tx_list_tag) {
		bus_dmamap_unload(sc->re_ldata.re_tx_list_tag,
		    sc->re_ldata.re_tx_list_map);
		bus_dmamem_free(sc->re_ldata.re_tx_list_tag,
		    sc->re_ldata.re_tx_list,
		    sc->re_ldata.re_tx_list_map);
		bus_dma_tag_destroy(sc->re_ldata.re_tx_list_tag);
	}

	/* Destroy all the RX and TX buffer maps */

	if (sc->re_ldata.re_mtag) {
		for (i = 0; i < RE_TX_DESC_CNT; i++)
			bus_dmamap_destroy(sc->re_ldata.re_mtag,
			    sc->re_ldata.re_tx_dmamap[i]);
		for (i = 0; i < RE_RX_DESC_CNT; i++)
			bus_dmamap_destroy(sc->re_ldata.re_mtag,
			    sc->re_ldata.re_rx_dmamap[i]);
		bus_dma_tag_destroy(sc->re_ldata.re_mtag);
	}

	/* Unload and free the stats buffer and map */

	if (sc->re_ldata.re_stag) {
		bus_dmamap_unload(sc->re_ldata.re_stag,
		    sc->re_ldata.re_rx_list_map);
		bus_dmamem_free(sc->re_ldata.re_stag,
		    sc->re_ldata.re_stats,
		    sc->re_ldata.re_smap);
		bus_dma_tag_destroy(sc->re_ldata.re_stag);
	}

	if (sc->re_parent_tag)
		bus_dma_tag_destroy(sc->re_parent_tag);

	return(0);
}

static int
re_newbuf(struct re_softc *sc, int idx, struct mbuf *m)
{
	struct re_dmaload_arg arg;
	struct mbuf *n = NULL;
	int error;

	if (m == NULL) {
		n = m_getcl(MB_DONTWAIT, MT_DATA, M_PKTHDR);
		if (n == NULL)
			return(ENOBUFS);
		m = n;
	} else
		m->m_data = m->m_ext.ext_buf;

	m->m_len = m->m_pkthdr.len = MCLBYTES;

	/*
	 * NOTE:
	 * Some re(4) chips(e.g. RTL8101E) need address of the receive buffer
	 * to be 8-byte aligned, so don't call m_adj(m, ETHER_ALIGN) here.
	 */

	arg.sc = sc;
	arg.re_idx = idx;
	arg.re_maxsegs = 1;
	arg.re_flags = 0;
	arg.re_ring = sc->re_ldata.re_rx_list;

        error = bus_dmamap_load_mbuf(sc->re_ldata.re_mtag,
	    sc->re_ldata.re_rx_dmamap[idx], m, re_dma_map_desc,
	    &arg, BUS_DMA_NOWAIT);
	if (error || arg.re_maxsegs != 1) {
		if (n != NULL)
			m_freem(n);
		return (ENOMEM);
	}

	sc->re_ldata.re_rx_list[idx].re_cmdstat |= htole32(RE_RDESC_CMD_OWN);
	sc->re_ldata.re_rx_mbuf[idx] = m;

        bus_dmamap_sync(sc->re_ldata.re_mtag, sc->re_ldata.re_rx_dmamap[idx],
		        BUS_DMASYNC_PREREAD);

	return(0);
}

static int
re_tx_list_init(struct re_softc *sc)
{
	bzero(sc->re_ldata.re_tx_list, RE_TX_LIST_SZ);
	bzero(&sc->re_ldata.re_tx_mbuf, RE_TX_DESC_CNT * sizeof(struct mbuf *));

	bus_dmamap_sync(sc->re_ldata.re_tx_list_tag,
			sc->re_ldata.re_tx_list_map, BUS_DMASYNC_PREWRITE);
	sc->re_ldata.re_tx_prodidx = 0;
	sc->re_ldata.re_tx_considx = 0;
	sc->re_ldata.re_tx_free = RE_TX_DESC_CNT;

	return(0);
}

static int
re_rx_list_init(struct re_softc *sc)
{
	int i, error;

	bzero(sc->re_ldata.re_rx_list, RE_RX_LIST_SZ);
	bzero(&sc->re_ldata.re_rx_mbuf, RE_RX_DESC_CNT * sizeof(struct mbuf *));

	for (i = 0; i < RE_RX_DESC_CNT; i++) {
		error = re_newbuf(sc, i, NULL);
		if (error)
			return(error);
	}

	/* Flush the RX descriptors */

	bus_dmamap_sync(sc->re_ldata.re_rx_list_tag,
			sc->re_ldata.re_rx_list_map, BUS_DMASYNC_PREWRITE);

	sc->re_ldata.re_rx_prodidx = 0;
	sc->re_head = sc->re_tail = NULL;

	return(0);
}

/*
 * RX handler for C+ and 8169. For the gigE chips, we support
 * the reception of jumbo frames that have been fragmented
 * across multiple 2K mbuf cluster buffers.
 */
static void
re_rxeof(struct re_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mbuf *m;
	struct re_desc 	*cur_rx;
	uint32_t rxstat, rxvlan;
	int i, total_len;
#ifdef ETHER_INPUT_CHAIN
	struct mbuf_chain chain[MAXCPU];
#endif

	/* Invalidate the descriptor memory */

	bus_dmamap_sync(sc->re_ldata.re_rx_list_tag,
			sc->re_ldata.re_rx_list_map, BUS_DMASYNC_POSTREAD);

#ifdef ETHER_INPUT_CHAIN
	ether_input_chain_init(chain);
#endif

	for (i = sc->re_ldata.re_rx_prodidx;
	     RE_OWN(&sc->re_ldata.re_rx_list[i]) == 0 ; RE_DESC_INC(i)) {
		cur_rx = &sc->re_ldata.re_rx_list[i];
		m = sc->re_ldata.re_rx_mbuf[i];
		total_len = RE_RXBYTES(cur_rx);
		rxstat = le32toh(cur_rx->re_cmdstat);
		rxvlan = le32toh(cur_rx->re_vlanctl);

		/* Invalidate the RX mbuf and unload its map */

		bus_dmamap_sync(sc->re_ldata.re_mtag,
				sc->re_ldata.re_rx_dmamap[i],
				BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->re_ldata.re_mtag,
				  sc->re_ldata.re_rx_dmamap[i]);

		if ((rxstat & RE_RDESC_STAT_EOF) == 0) {
			m->m_len = MCLBYTES - ETHER_ALIGN;
			if (sc->re_head == NULL) {
				sc->re_head = sc->re_tail = m;
			} else {
				sc->re_tail->m_next = m;
				sc->re_tail = m;
			}
			re_newbuf(sc, i, NULL);
			continue;
		}

		/*
		 * NOTE: for the 8139C+, the frame length field
		 * is always 12 bits in size, but for the gigE chips,
		 * it is 13 bits (since the max RX frame length is 16K).
		 * Unfortunately, all 32 bits in the status word
		 * were already used, so to make room for the extra
		 * length bit, RealTek took out the 'frame alignment
		 * error' bit and shifted the other status bits
		 * over one slot. The OWN, EOR, FS and LS bits are
		 * still in the same places. We have already extracted
		 * the frame length and checked the OWN bit, so rather
		 * than using an alternate bit mapping, we shift the
		 * status bits one space to the right so we can evaluate
		 * them using the 8169 status as though it was in the
		 * same format as that of the 8139C+.
		 */
		if (sc->re_type == RE_8169)
			rxstat >>= 1;

		if (rxstat & RE_RDESC_STAT_RXERRSUM) {
			ifp->if_ierrors++;
			/*
			 * If this is part of a multi-fragment packet,
			 * discard all the pieces.
			 */
			if (sc->re_head != NULL) {
				m_freem(sc->re_head);
				sc->re_head = sc->re_tail = NULL;
			}
			re_newbuf(sc, i, m);
			continue;
		}

		/*
		 * If allocating a replacement mbuf fails,
		 * reload the current one.
		 */

		if (re_newbuf(sc, i, NULL)) {
			ifp->if_ierrors++;
			if (sc->re_head != NULL) {
				m_freem(sc->re_head);
				sc->re_head = sc->re_tail = NULL;
			}
			re_newbuf(sc, i, m);
			continue;
		}

		if (sc->re_head != NULL) {
			m->m_len = total_len % (MCLBYTES - ETHER_ALIGN);
			/* 
			 * Special case: if there's 4 bytes or less
			 * in this buffer, the mbuf can be discarded:
			 * the last 4 bytes is the CRC, which we don't
			 * care about anyway.
			 */
			if (m->m_len <= ETHER_CRC_LEN) {
				sc->re_tail->m_len -=
				    (ETHER_CRC_LEN - m->m_len);
				m_freem(m);
			} else {
				m->m_len -= ETHER_CRC_LEN;
				sc->re_tail->m_next = m;
			}
			m = sc->re_head;
			sc->re_head = sc->re_tail = NULL;
			m->m_pkthdr.len = total_len - ETHER_CRC_LEN;
		} else
			m->m_pkthdr.len = m->m_len =
			    (total_len - ETHER_CRC_LEN);

		ifp->if_ipackets++;
		m->m_pkthdr.rcvif = ifp;

		/* Do RX checksumming if enabled */

		if (ifp->if_capenable & IFCAP_RXCSUM) {

			/* Check IP header checksum */
			if (rxstat & RE_RDESC_STAT_PROTOID)
				m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED;
			if ((rxstat & RE_RDESC_STAT_IPSUMBAD) == 0)
				m->m_pkthdr.csum_flags |= CSUM_IP_VALID;

			/* Check TCP/UDP checksum */
			if ((RE_TCPPKT(rxstat) &&
			    (rxstat & RE_RDESC_STAT_TCPSUMBAD) == 0) ||
			    (RE_UDPPKT(rxstat) &&
			    (rxstat & RE_RDESC_STAT_UDPSUMBAD)) == 0) {
				m->m_pkthdr.csum_flags |=
				    CSUM_DATA_VALID|CSUM_PSEUDO_HDR|
				    CSUM_FRAG_NOT_CHECKED;
				m->m_pkthdr.csum_data = 0xffff;
			}
		}

		if (rxvlan & RE_RDESC_VLANCTL_TAG) {
			m->m_flags |= M_VLANTAG;
			m->m_pkthdr.ether_vlantag =
				be16toh((rxvlan & RE_RDESC_VLANCTL_DATA));
		}
#ifdef ETHER_INPUT_CHAIN
		ether_input_chain(ifp, m, chain);
#else
		ifp->if_input(ifp, m);
#endif
	}

#ifdef ETHER_INPUT_CHAIN
	ether_input_dispatch(chain);
#endif

	/* Flush the RX DMA ring */

	bus_dmamap_sync(sc->re_ldata.re_rx_list_tag,
			sc->re_ldata.re_rx_list_map, BUS_DMASYNC_PREWRITE);

	sc->re_ldata.re_rx_prodidx = i;
}

static void
re_txeof(struct re_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t txstat;
	int idx;

	/* Invalidate the TX descriptor list */

	bus_dmamap_sync(sc->re_ldata.re_tx_list_tag,
			sc->re_ldata.re_tx_list_map, BUS_DMASYNC_POSTREAD);

	for (idx = sc->re_ldata.re_tx_considx;
	     sc->re_ldata.re_tx_free < RE_TX_DESC_CNT; RE_DESC_INC(idx)) {
		txstat = le32toh(sc->re_ldata.re_tx_list[idx].re_cmdstat);
		if (txstat & RE_TDESC_CMD_OWN)
			break;

		sc->re_ldata.re_tx_list[idx].re_bufaddr_lo = 0;

		/*
		 * We only stash mbufs in the last descriptor
		 * in a fragment chain, which also happens to
		 * be the only place where the TX status bits
		 * are valid.
		 */
		if (txstat & RE_TDESC_CMD_EOF) {
			m_freem(sc->re_ldata.re_tx_mbuf[idx]);
			sc->re_ldata.re_tx_mbuf[idx] = NULL;
			bus_dmamap_unload(sc->re_ldata.re_mtag,
			    sc->re_ldata.re_tx_dmamap[idx]);
			if (txstat & (RE_TDESC_STAT_EXCESSCOL|
			    RE_TDESC_STAT_COLCNT))
				ifp->if_collisions++;
			if (txstat & RE_TDESC_STAT_TXERRSUM)
				ifp->if_oerrors++;
			else
				ifp->if_opackets++;
		}
		sc->re_ldata.re_tx_free++;
	}

	/* No changes made to the TX ring, so no flush needed */
	if (sc->re_ldata.re_tx_free) {
		sc->re_ldata.re_tx_considx = idx;
		ifp->if_flags &= ~IFF_OACTIVE;
		ifp->if_timer = 0;
	}

	/*
	 * Some chips will ignore a second TX request issued while an
	 * existing transmission is in progress. If the transmitter goes
	 * idle but there are still packets waiting to be sent, we need
	 * to restart the channel here to flush them out. This only seems
	 * to be required with the PCIe devices.
	 */
	if (sc->re_ldata.re_tx_free < RE_TX_DESC_CNT)
		CSR_WRITE_1(sc, sc->re_txstart, RE_TXSTART_START);

	/*
	 * If not all descriptors have been released reaped yet,
	 * reload the timer so that we will eventually get another
	 * interrupt that will cause us to re-enter this routine.
	 * This is done in case the transmitter has gone idle.
	 */
	if (RE_TX_MODERATION_IS_ENABLED(sc) &&
	    sc->re_ldata.re_tx_free < RE_TX_DESC_CNT)
                CSR_WRITE_4(sc, RE_TIMERCNT, 1);
}

static void
re_tick(void *xsc)
{
	struct re_softc *sc = xsc;

	lwkt_serialize_enter(sc->arpcom.ac_if.if_serializer);
	re_tick_serialized(xsc);
	lwkt_serialize_exit(sc->arpcom.ac_if.if_serializer);
}

static void
re_tick_serialized(void *xsc)
{
	struct re_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mii_data *mii;

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii = device_get_softc(sc->re_miibus);
	mii_tick(mii);
	if (sc->re_link) {
		if (!(mii->mii_media_status & IFM_ACTIVE))
			sc->re_link = 0;
	} else {
		if (mii->mii_media_status & IFM_ACTIVE &&
		    IFM_SUBTYPE(mii->mii_media_active) != IFM_NONE) {
			sc->re_link = 1;
			if (!ifq_is_empty(&ifp->if_snd))
				if_devstart(ifp);
		}
	}

	callout_reset(&sc->re_timer, hz, re_tick, sc);
}

#ifdef DEVICE_POLLING

static void
re_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct re_softc *sc = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch(cmd) {
	case POLL_REGISTER:
		/* disable interrupts */
		CSR_WRITE_2(sc, RE_IMR, 0x0000);
		break;
	case POLL_DEREGISTER:
		/* enable interrupts */
		CSR_WRITE_2(sc, RE_IMR, sc->re_intrs);
		break;
	default:
		sc->rxcycles = count;
		re_rxeof(sc);
		re_txeof(sc);

		if (!ifq_is_empty(&ifp->if_snd))
			if_devstart(ifp);

		if (cmd == POLL_AND_CHECK_STATUS) { /* also check status register */
			uint16_t       status;

			status = CSR_READ_2(sc, RE_ISR);
			if (status == 0xffff)
				return;
			if (status)
				CSR_WRITE_2(sc, RE_ISR, status);

			/*
			 * XXX check behaviour on receiver stalls.
			 */

			if (status & RE_ISR_SYSTEM_ERR) {
				re_reset(sc);
				re_init(sc);
			}
		}
		break;
	}
}
#endif /* DEVICE_POLLING */

static void
re_intr(void *arg)
{
	struct re_softc	*sc = arg;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint16_t status;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if (sc->suspended || (ifp->if_flags & IFF_UP) == 0)
		return;

	for (;;) {
		status = CSR_READ_2(sc, RE_ISR);
		/* If the card has gone away the read returns 0xffff. */
		if (status == 0xffff)
			break;
		if (status)
			CSR_WRITE_2(sc, RE_ISR, status);

		if ((status & sc->re_intrs) == 0)
			break;

		if (status & (RE_ISR_RX_OK | RE_ISR_RX_ERR | RE_ISR_FIFO_OFLOW))
			re_rxeof(sc);

		if ((status & sc->re_tx_ack) ||
		    (status & RE_ISR_TX_ERR) ||
		    (status & RE_ISR_TX_DESC_UNAVAIL))
			re_txeof(sc);

		if (status & RE_ISR_SYSTEM_ERR) {
			re_reset(sc);
			re_init(sc);
		}

		if (status & RE_ISR_LINKCHG) {
			callout_stop(&sc->re_timer);
			re_tick_serialized(sc);
		}
	}

	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

static int
re_encap(struct re_softc *sc, struct mbuf **m_head, int *idx, int *called_defrag)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mbuf *m, *m_new = NULL;
	struct re_dmaload_arg	arg;
	bus_dmamap_t		map;
	int			error;

	KASSERT(sc->re_ldata.re_tx_free > 4, ("not enough free TX desc\n"));

	*called_defrag = 0;
	m = *m_head;

	/*
	 * Set up checksum offload. Note: checksum offload bits must
	 * appear in all descriptors of a multi-descriptor transmit
	 * attempt. (This is according to testing done with an 8169
	 * chip. I'm not sure if this is a requirement or a bug.)
	 */

	arg.re_flags = 0;

	if (m->m_pkthdr.csum_flags & CSUM_IP)
		arg.re_flags |= RE_TDESC_CMD_IPCSUM;
	if (m->m_pkthdr.csum_flags & CSUM_TCP)
		arg.re_flags |= RE_TDESC_CMD_TCPCSUM;
	if (m->m_pkthdr.csum_flags & CSUM_UDP)
		arg.re_flags |= RE_TDESC_CMD_UDPCSUM;

	arg.sc = sc;
	arg.re_idx = *idx;
	arg.re_maxsegs = sc->re_ldata.re_tx_free;
	if (arg.re_maxsegs > 4)
		arg.re_maxsegs -= 4;
	arg.re_ring = sc->re_ldata.re_tx_list;

	map = sc->re_ldata.re_tx_dmamap[*idx];

	/*
	 * With some of the RealTek chips, using the checksum offload
	 * support in conjunction with the autopadding feature results
	 * in the transmission of corrupt frames. For example, if we
	 * need to send a really small IP fragment that's less than 60
	 * bytes in size, and IP header checksumming is enabled, the
	 * resulting ethernet frame that appears on the wire will
	 * have garbled payload. To work around this, if TX checksum
	 * offload is enabled, we always manually pad short frames out
	 * to the minimum ethernet frame size. We do this by pretending
	 * the mbuf chain has too many fragments so the coalescing code
	 * below can assemble the packet into a single buffer that's
	 * padded out to the mininum frame size.
	 *
	 * Note: this appears unnecessary for TCP, and doing it for TCP
	 * with PCIe adapters seems to result in bad checksums.
	 */
	if (arg.re_flags && !(arg.re_flags & RE_TDESC_CMD_TCPCSUM) &&
	    m->m_pkthdr.len < RE_MIN_FRAMELEN) {
		error = EFBIG;
	} else {
		error = bus_dmamap_load_mbuf(sc->re_ldata.re_mtag, map,
		    m, re_dma_map_desc, &arg, BUS_DMA_NOWAIT);
	}

	if (error && error != EFBIG) {
		if_printf(ifp, "can't map mbuf (error %d)\n", error);
		return(ENOBUFS);
	}

	/* Too many segments to map, coalesce into a single mbuf */

	if (error || arg.re_maxsegs == 0) {
		m_new = m_defrag_nofree(m, MB_DONTWAIT);
		if (m_new == NULL) {
			return(1);
		} else {
			m = m_new;
			*m_head = m;
		}

		/*
		 * Manually pad short frames, and zero the pad space
		 * to avoid leaking data.
		 */
		if (m_new->m_pkthdr.len < RE_MIN_FRAMELEN) {
			bzero(mtod(m_new, char *) + m_new->m_pkthdr.len,
			    RE_MIN_FRAMELEN - m_new->m_pkthdr.len);
			m_new->m_pkthdr.len += RE_MIN_FRAMELEN -
			    m_new->m_pkthdr.len;
			m_new->m_len = m_new->m_pkthdr.len;
		}

		*called_defrag = 1;
		arg.sc = sc;
		arg.re_idx = *idx;
		arg.re_maxsegs = sc->re_ldata.re_tx_free;
		arg.re_ring = sc->re_ldata.re_tx_list;

		error = bus_dmamap_load_mbuf(sc->re_ldata.re_mtag, map,
		    m, re_dma_map_desc, &arg, BUS_DMA_NOWAIT);
		if (error) {
			m_freem(m);
			if_printf(ifp, "can't map mbuf (error %d)\n", error);
			return(EFBIG);
		}
	}

	/*
	 * Insure that the map for this transmission
	 * is placed at the array index of the last descriptor
	 * in this chain.
	 */
	sc->re_ldata.re_tx_dmamap[*idx] =
	    sc->re_ldata.re_tx_dmamap[arg.re_idx];
	sc->re_ldata.re_tx_dmamap[arg.re_idx] = map;

	sc->re_ldata.re_tx_mbuf[arg.re_idx] = m;
	sc->re_ldata.re_tx_free -= arg.re_maxsegs;

	/*
	 * Set up hardware VLAN tagging. Note: vlan tag info must
	 * appear in the first descriptor of a multi-descriptor
	 * transmission attempt.
	 */

	if (m->m_flags & M_VLANTAG) {
		sc->re_ldata.re_tx_list[*idx].re_vlanctl =
		    htole32(htobe16(m->m_pkthdr.ether_vlantag) |
		    	    RE_TDESC_VLANCTL_TAG);
	}

	/* Transfer ownership of packet to the chip. */

	sc->re_ldata.re_tx_list[arg.re_idx].re_cmdstat |=
	    htole32(RE_TDESC_CMD_OWN);
	if (*idx != arg.re_idx)
		sc->re_ldata.re_tx_list[*idx].re_cmdstat |=
		    htole32(RE_TDESC_CMD_OWN);

	RE_DESC_INC(arg.re_idx);
	*idx = arg.re_idx;

	return(0);
}

/*
 * Main transmit routine for C+ and gigE NICs.
 */

static void
re_start(struct ifnet *ifp)
{
	struct re_softc	*sc = ifp->if_softc;
	struct mbuf *m_head;
	struct mbuf *m_head2;
	int called_defrag, idx, need_trans;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if (!sc->re_link) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	if ((ifp->if_flags & (IFF_OACTIVE | IFF_RUNNING)) != IFF_RUNNING)
		return;

	idx = sc->re_ldata.re_tx_prodidx;

	need_trans = 0;
	while (sc->re_ldata.re_tx_mbuf[idx] == NULL) {
		if (sc->re_ldata.re_tx_free <= 4) {
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		m_head = ifq_dequeue(&ifp->if_snd, NULL);
		if (m_head == NULL)
			break;

		m_head2 = m_head;
		if (re_encap(sc, &m_head2, &idx, &called_defrag)) {
			/*
			 * If we could not encapsulate the defragged packet,
			 * the returned m_head2 is garbage and we must dequeue
			 * and throw away the original packet.
			 */
			if (called_defrag)
				m_freem(m_head);
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		/*
		 * Clean out the packet we encapsulated.  If we defragged
		 * the packet the m_head2 is the one that got encapsulated
		 * and the original must be thrown away.  Otherwise m_head2
		 * *IS* the original.
		 */
		if (called_defrag)
			m_freem(m_head);
		need_trans = 1;

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */
		ETHER_BPF_MTAP(ifp, m_head2);
	}

	if (!need_trans) {
		if (RE_TX_MODERATION_IS_ENABLED(sc) &&
		    sc->re_ldata.re_tx_free != RE_TX_DESC_CNT)
			CSR_WRITE_4(sc, RE_TIMERCNT, 1);
		return;
	}

	/* Flush the TX descriptors */
	bus_dmamap_sync(sc->re_ldata.re_tx_list_tag,
			sc->re_ldata.re_tx_list_map, BUS_DMASYNC_PREWRITE);

	sc->re_ldata.re_tx_prodidx = idx;

	/*
	 * RealTek put the TX poll request register in a different
	 * location on the 8169 gigE chip. I don't know why.
	 */
	CSR_WRITE_1(sc, sc->re_txstart, RE_TXSTART_START);

	if (RE_TX_MODERATION_IS_ENABLED(sc)) {
		/*
		 * Use the countdown timer for interrupt moderation.
		 * 'TX done' interrupts are disabled. Instead, we reset the
		 * countdown timer, which will begin counting until it hits
		 * the value in the TIMERINT register, and then trigger an
		 * interrupt. Each time we write to the TIMERCNT register,
		 * the timer count is reset to 0.
		 */
		CSR_WRITE_4(sc, RE_TIMERCNT, 1);
	}

	/*
	 * Set a timeout in case the chip goes out to lunch.
	 */
	ifp->if_timer = 5;
}

static void
re_init(void *xsc)
{
	struct re_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct mii_data *mii;
	uint32_t rxcfg = 0;

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii = device_get_softc(sc->re_miibus);

	/*
	 * Cancel pending I/O and free all RX/TX buffers.
	 */
	re_stop(sc);

	/*
	 * Enable C+ RX and TX mode, as well as VLAN stripping and
	 * RX checksum offload. We must configure the C+ register
	 * before all others.
	 */
	CSR_WRITE_2(sc, RE_CPLUS_CMD, RE_CPLUSCMD_RXENB | RE_CPLUSCMD_TXENB |
		    RE_CPLUSCMD_PCI_MRW | RE_CPLUSCMD_VLANSTRIP |
		    (ifp->if_capenable & IFCAP_RXCSUM ?
		     RE_CPLUSCMD_RXCSUM_ENB : 0));

	/*
	 * Init our MAC address.  Even though the chipset
	 * documentation doesn't mention it, we need to enter "Config
	 * register write enable" mode to modify the ID registers.
	 */
	CSR_WRITE_1(sc, RE_EECMD, RE_EEMODE_WRITECFG);
	CSR_WRITE_4(sc, RE_IDR0,
	    htole32(*(uint32_t *)(&sc->arpcom.ac_enaddr[0])));
	CSR_WRITE_2(sc, RE_IDR4,
	    htole16(*(uint16_t *)(&sc->arpcom.ac_enaddr[4])));
	CSR_WRITE_1(sc, RE_EECMD, RE_EEMODE_OFF);

	/*
	 * For C+ mode, initialize the RX descriptors and mbufs.
	 */
	re_rx_list_init(sc);
	re_tx_list_init(sc);

	/*
	 * Load the addresses of the RX and TX lists into the chip.
	 */
	CSR_WRITE_4(sc, RE_RXLIST_ADDR_HI,
	    RE_ADDR_HI(sc->re_ldata.re_rx_list_addr));
	CSR_WRITE_4(sc, RE_RXLIST_ADDR_LO,
	    RE_ADDR_LO(sc->re_ldata.re_rx_list_addr));

	CSR_WRITE_4(sc, RE_TXLIST_ADDR_HI,
	    RE_ADDR_HI(sc->re_ldata.re_tx_list_addr));
	CSR_WRITE_4(sc, RE_TXLIST_ADDR_LO,
	    RE_ADDR_LO(sc->re_ldata.re_tx_list_addr));

	/*
	 * Enable transmit and receive.
	 */
	CSR_WRITE_1(sc, RE_COMMAND, RE_CMD_TX_ENB|RE_CMD_RX_ENB);

	/*
	 * Set the initial TX and RX configuration.
	 */
	if (sc->re_testmode) {
		if (sc->re_type == RE_8169)
			CSR_WRITE_4(sc, RE_TXCFG,
				    RE_TXCFG_CONFIG | RE_LOOPTEST_ON);
		else
			CSR_WRITE_4(sc, RE_TXCFG,
				    RE_TXCFG_CONFIG | RE_LOOPTEST_ON_CPLUS);
	} else
		CSR_WRITE_4(sc, RE_TXCFG, RE_TXCFG_CONFIG);

	CSR_WRITE_1(sc, RE_EARLY_TX_THRESH, 16);

	CSR_WRITE_4(sc, RE_RXCFG, RE_RXCFG_CONFIG);

	/* Set the individual bit to receive frames for this host only. */
	rxcfg = CSR_READ_4(sc, RE_RXCFG);
	rxcfg |= RE_RXCFG_RX_INDIV;

	/* If we want promiscuous mode, set the allframes bit. */
	if (ifp->if_flags & IFF_PROMISC) {
		rxcfg |= RE_RXCFG_RX_ALLPHYS;
		CSR_WRITE_4(sc, RE_RXCFG, rxcfg);
	} else {
		rxcfg &= ~RE_RXCFG_RX_ALLPHYS;
		CSR_WRITE_4(sc, RE_RXCFG, rxcfg);
	}

	/*
	 * Set capture broadcast bit to capture broadcast frames.
	 */
	if (ifp->if_flags & IFF_BROADCAST) {
		rxcfg |= RE_RXCFG_RX_BROAD;
		CSR_WRITE_4(sc, RE_RXCFG, rxcfg);
	} else {
		rxcfg &= ~RE_RXCFG_RX_BROAD;
		CSR_WRITE_4(sc, RE_RXCFG, rxcfg);
	}

	/*
	 * Program the multicast filter, if necessary.
	 */
	re_setmulti(sc);

#ifdef DEVICE_POLLING
	/*
	 * Disable interrupts if we are polling.
	 */
	if (ifp->if_flags & IFF_POLLING)
		CSR_WRITE_2(sc, RE_IMR, 0);
	else	/* otherwise ... */
#endif /* DEVICE_POLLING */
	/*
	 * Enable interrupts.
	 */
	if (sc->re_testmode)
		CSR_WRITE_2(sc, RE_IMR, 0);
	else
		CSR_WRITE_2(sc, RE_IMR, sc->re_intrs);
	CSR_WRITE_2(sc, RE_ISR, sc->re_intrs);

	/* Set initial TX threshold */
	sc->re_txthresh = RE_TX_THRESH_INIT;

	/* Start RX/TX process. */
	if (sc->re_flags & RE_F_HASMPC)
		CSR_WRITE_4(sc, RE_MISSEDPKT, 0);
#ifdef notdef
	/* Enable receiver and transmitter. */
	CSR_WRITE_1(sc, RE_COMMAND, RE_CMD_TX_ENB|RE_CMD_RX_ENB);
#endif

	if (RE_TX_MODERATION_IS_ENABLED(sc)) {
		/*
		 * Initialize the timer interrupt register so that
		 * a timer interrupt will be generated once the timer
		 * reaches a certain number of ticks. The timer is
		 * reloaded on each transmit. This gives us TX interrupt
		 * moderation, which dramatically improves TX frame rate.
		 */
		if (sc->re_type == RE_8169)
			CSR_WRITE_4(sc, RE_TIMERINT_8169, 0x800);
		else
			CSR_WRITE_4(sc, RE_TIMERINT, 0x400);
	}

	/*
	 * For 8169 gigE NICs, set the max allowed RX packet
	 * size so we can receive jumbo frames.
	 */
	if (sc->re_type == RE_8169)
		CSR_WRITE_2(sc, RE_MAXRXPKTLEN, 16383);

	if (sc->re_testmode) {
		return;
	}

	mii_mediachg(mii);

	CSR_WRITE_1(sc, RE_CFG1, RE_CFG1_DRVLOAD|RE_CFG1_FULLDUPLEX);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	sc->re_link = 0;
	callout_reset(&sc->re_timer, hz, re_tick, sc);
}

/*
 * Set media options.
 */
static int
re_ifmedia_upd(struct ifnet *ifp)
{
	struct re_softc *sc = ifp->if_softc;
	struct mii_data *mii;

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii = device_get_softc(sc->re_miibus);
	mii_mediachg(mii);

	return(0);
}

/*
 * Report current media status.
 */
static void
re_ifmedia_sts(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct re_softc *sc = ifp->if_softc;
	struct mii_data *mii;

	ASSERT_SERIALIZED(ifp->if_serializer);

	mii = device_get_softc(sc->re_miibus);

	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
}

static int
re_ioctl(struct ifnet *ifp, u_long command, caddr_t data, struct ucred *cr)
{
	struct re_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *) data;
	struct mii_data *mii;
	int error = 0;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch(command) {
	case SIOCSIFMTU:
		if (ifr->ifr_mtu > RE_JUMBO_MTU)
			error = EINVAL;
		ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
			re_init(sc);
		else if (ifp->if_flags & IFF_RUNNING)
			re_stop(sc);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		re_setmulti(sc);
		error = 0;
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		mii = device_get_softc(sc->re_miibus);
		error = ifmedia_ioctl(ifp, ifr, &mii->mii_media, command);
		break;
	case SIOCSIFCAP:
		ifp->if_capenable &= ~(IFCAP_HWCSUM);
		ifp->if_capenable |=
		    ifr->ifr_reqcap & (IFCAP_HWCSUM);
		if (ifp->if_capenable & IFCAP_TXCSUM)
			ifp->if_hwassist = RE_CSUM_FEATURES;
		else
			ifp->if_hwassist = 0;
		if (ifp->if_flags & IFF_RUNNING)
			re_init(sc);
		break;
	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}
	return(error);
}

static void
re_watchdog(struct ifnet *ifp)
{
	struct re_softc *sc = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if_printf(ifp, "watchdog timeout\n");

	ifp->if_oerrors++;

	re_txeof(sc);
	re_rxeof(sc);

	re_init(sc);

	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

/*
 * Stop the adapter and free any mbufs allocated to the
 * RX and TX lists.
 */
static void
re_stop(struct re_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int i;

	ASSERT_SERIALIZED(ifp->if_serializer);

	ifp->if_timer = 0;
	callout_stop(&sc->re_timer);

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);

	CSR_WRITE_1(sc, RE_COMMAND, 0x00);
	CSR_WRITE_2(sc, RE_IMR, 0x0000);
	CSR_WRITE_2(sc, RE_ISR, 0xFFFF);

	if (sc->re_head != NULL) {
		m_freem(sc->re_head);
		sc->re_head = sc->re_tail = NULL;
	}

	/* Free the TX list buffers. */
	for (i = 0; i < RE_TX_DESC_CNT; i++) {
		if (sc->re_ldata.re_tx_mbuf[i] != NULL) {
			bus_dmamap_unload(sc->re_ldata.re_mtag,
					  sc->re_ldata.re_tx_dmamap[i]);
			m_freem(sc->re_ldata.re_tx_mbuf[i]);
			sc->re_ldata.re_tx_mbuf[i] = NULL;
		}
	}

	/* Free the RX list buffers. */
	for (i = 0; i < RE_RX_DESC_CNT; i++) {
		if (sc->re_ldata.re_rx_mbuf[i] != NULL) {
			bus_dmamap_unload(sc->re_ldata.re_mtag,
					  sc->re_ldata.re_rx_dmamap[i]);
			m_freem(sc->re_ldata.re_rx_mbuf[i]);
			sc->re_ldata.re_rx_mbuf[i] = NULL;
		}
	}
}

/*
 * Device suspend routine.  Stop the interface and save some PCI
 * settings in case the BIOS doesn't restore them properly on
 * resume.
 */
static int
re_suspend(device_t dev)
{
#ifndef BURN_BRIDGES
	int i;
#endif
	struct re_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	re_stop(sc);

#ifndef BURN_BRIDGES
	for (i = 0; i < 5; i++)
		sc->saved_maps[i] = pci_read_config(dev, PCIR_MAPS + i * 4, 4);
	sc->saved_biosaddr = pci_read_config(dev, PCIR_BIOS, 4);
	sc->saved_intline = pci_read_config(dev, PCIR_INTLINE, 1);
	sc->saved_cachelnsz = pci_read_config(dev, PCIR_CACHELNSZ, 1);
	sc->saved_lattimer = pci_read_config(dev, PCIR_LATTIMER, 1);
#endif

	sc->suspended = 1;

	lwkt_serialize_exit(ifp->if_serializer);

	return (0);
}

/*
 * Device resume routine.  Restore some PCI settings in case the BIOS
 * doesn't, re-enable busmastering, and restart the interface if
 * appropriate.
 */
static int
re_resume(device_t dev)
{
	struct re_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
#ifndef BURN_BRIDGES
	int i;
#endif

	lwkt_serialize_enter(ifp->if_serializer);

#ifndef BURN_BRIDGES
	/* better way to do this? */
	for (i = 0; i < 5; i++)
		pci_write_config(dev, PCIR_MAPS + i * 4, sc->saved_maps[i], 4);
	pci_write_config(dev, PCIR_BIOS, sc->saved_biosaddr, 4);
	pci_write_config(dev, PCIR_INTLINE, sc->saved_intline, 1);
	pci_write_config(dev, PCIR_CACHELNSZ, sc->saved_cachelnsz, 1);
	pci_write_config(dev, PCIR_LATTIMER, sc->saved_lattimer, 1);

	/* reenable busmastering */
	pci_enable_busmaster(dev);
	pci_enable_io(dev, SYS_RES_IOPORT);
#endif

	/* reinitialize interface if necessary */
	if (ifp->if_flags & IFF_UP)
		re_init(sc);

	sc->suspended = 0;

	lwkt_serialize_exit(ifp->if_serializer);

	return (0);
}

/*
 * Stop all chip I/O so that the kernel's probe routines don't
 * get confused by errant DMAs when rebooting.
 */
static void
re_shutdown(device_t dev)
{
	struct re_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);
	re_stop(sc);
	lwkt_serialize_exit(ifp->if_serializer);
}

static int
re_sysctl_tx_moderation(SYSCTL_HANDLER_ARGS)
{
	struct re_softc *sc = arg1;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error = 0, mod, mod_old;

	lwkt_serialize_enter(ifp->if_serializer);

	mod_old = mod = RE_TX_MODERATION_IS_ENABLED(sc);

	error = sysctl_handle_int(oidp, &mod, 0, req);
	if (error || req->newptr == NULL || mod == mod_old)
		goto back;
	if (mod != 0 && mod != 1) {
		error = EINVAL;
		goto back;
	}

	if (mod)
		RE_ENABLE_TX_MODERATION(sc);
	else
		RE_DISABLE_TX_MODERATION(sc);

	if ((ifp->if_flags & (IFF_RUNNING | IFF_UP)) == (IFF_RUNNING | IFF_UP))
		re_init(sc);
back:
	lwkt_serialize_exit(ifp->if_serializer);
	return error;
}
