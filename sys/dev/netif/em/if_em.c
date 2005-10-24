/**************************************************************************

Copyright (c) 2004 Joerg Sonnenberger <joerg@bec.de>.  All rights reserved.

Copyright (c) 2001-2003, Intel Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 3. Neither the name of the Intel Corporation nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

***************************************************************************/

/*$FreeBSD: src/sys/dev/em/if_em.c,v 1.2.2.15 2003/06/09 22:10:15 pdeuskar Exp $*/
/*$DragonFly: src/sys/dev/netif/em/if_em.c,v 1.41 2005/10/24 08:06:15 sephe Exp $*/

#include "opt_polling.h"

#include <dev/netif/em/if_em.h>
#include <net/ifq_var.h>

/*********************************************************************
 *  Set this to one to display debug statistics                                                   
 *********************************************************************/
int             em_display_debug_stats = 0;

/*********************************************************************
 *  Driver version
 *********************************************************************/

char em_driver_version[] = "1.7.35";


/*********************************************************************
 *  PCI Device ID Table
 *
 *  Used by probe to select devices to load on
 *  Last field stores an index into em_strings
 *  Last entry must be all 0s
 *
 *  { Vendor ID, Device ID, SubVendor ID, SubDevice ID, String Index }
 *********************************************************************/

static em_vendor_info_t em_vendor_info_array[] =
{
	/* Intel(R) PRO/1000 Network Connection */
	{ 0x8086, 0x1000, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1001, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1004, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1008, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1009, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x100C, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x100D, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x100E, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x100F, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1010, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1011, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1012, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1013, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1014, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1015, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1016, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1017, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1018, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1019, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x101A, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x101D, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x101E, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1026, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1027, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1028, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1075, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1076, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1077, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1078, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x1079, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x107A, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x107B, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x107C, PCI_ANY_ID, PCI_ANY_ID, 0},
	{ 0x8086, 0x108A, PCI_ANY_ID, PCI_ANY_ID, 0},
	/* required last entry */
	{ 0, 0, 0, 0, 0}
};

/*********************************************************************
 *  Table of branding strings for all supported NICs.
 *********************************************************************/

static const char *em_strings[] = {
	"Intel(R) PRO/1000 Network Connection"
};

/*********************************************************************
 *  Function prototypes            
 *********************************************************************/
static int	em_probe(device_t);
static int	em_attach(device_t);
static int	em_detach(device_t);
static int	em_shutdown(device_t);
static void	em_intr(void *);
static void	em_start(struct ifnet *);
static void	em_start_serialized(struct ifnet *);
static int	em_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	em_watchdog(struct ifnet *);
static void	em_init(void *);
static void	em_init_serialized(void *);
static void	em_stop(void *);
static void	em_media_status(struct ifnet *, struct ifmediareq *);
static int	em_media_change(struct ifnet *);
static void	em_identify_hardware(struct adapter *);
static void	em_local_timer(void *);
static int	em_hardware_init(struct adapter *);
static void	em_setup_interface(device_t, struct adapter *);
static int	em_setup_transmit_structures(struct adapter *);
static void	em_initialize_transmit_unit(struct adapter *);
static int	em_setup_receive_structures(struct adapter *);
static void	em_initialize_receive_unit(struct adapter *);
static void	em_enable_intr(struct adapter *);
static void	em_disable_intr(struct adapter *);
static void	em_free_transmit_structures(struct adapter *);
static void	em_free_receive_structures(struct adapter *);
static void	em_update_stats_counters(struct adapter *);
static void	em_clean_transmit_interrupts(struct adapter *);
static int	em_allocate_receive_structures(struct adapter *);
static int	em_allocate_transmit_structures(struct adapter *);
static void	em_process_receive_interrupts(struct adapter *, int);
static void	em_receive_checksum(struct adapter *, struct em_rx_desc *,
				    struct mbuf *);
static void	em_transmit_checksum_setup(struct adapter *, struct mbuf *,
					   uint32_t *, uint32_t *);
static void	em_set_promisc(struct adapter *);
static void	em_disable_promisc(struct adapter *);
static void	em_set_multi(struct adapter *);
static void	em_print_hw_stats(struct adapter *);
static void	em_print_link_status(struct adapter *);
static int	em_get_buf(int i, struct adapter *, struct mbuf *, int how);
static void	em_enable_vlans(struct adapter *);
static int	em_encap(struct adapter *, struct mbuf *);
static void	em_smartspeed(struct adapter *);
static int	em_82547_fifo_workaround(struct adapter *, int);
static void	em_82547_update_fifo_head(struct adapter *, int);
static int	em_82547_tx_fifo_reset(struct adapter *);
static void	em_82547_move_tail(void *arg);
static void	em_82547_move_tail_serialized(void *arg);
static int	em_dma_malloc(struct adapter *, bus_size_t,
			      struct em_dma_alloc *, int);
static void	em_dma_free(struct adapter *, struct em_dma_alloc *);
static void	em_print_debug_info(struct adapter *);
static int	em_is_valid_ether_addr(uint8_t *);
static int	em_sysctl_stats(SYSCTL_HANDLER_ARGS);
static int	em_sysctl_debug_info(SYSCTL_HANDLER_ARGS);
static uint32_t	em_fill_descriptors(uint64_t address, uint32_t length, 
				   PDESC_ARRAY desc_array);
static int	em_sysctl_int_delay(SYSCTL_HANDLER_ARGS);
static int	em_sysctl_int_throttle(SYSCTL_HANDLER_ARGS);
static void	em_add_int_delay_sysctl(struct adapter *, const char *,
					const char *,
					struct em_int_delay_info *, int, int);

/*********************************************************************
 *  FreeBSD Device Interface Entry Points                    
 *********************************************************************/

static device_method_t em_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, em_probe),
	DEVMETHOD(device_attach, em_attach),
	DEVMETHOD(device_detach, em_detach),
	DEVMETHOD(device_shutdown, em_shutdown),
	{0, 0}
};

static driver_t em_driver = {
	"em", em_methods, sizeof(struct adapter),
};

static devclass_t em_devclass;

DECLARE_DUMMY_MODULE(if_em);
DRIVER_MODULE(if_em, pci, em_driver, em_devclass, 0, 0);

/*********************************************************************
 *  Tunable default values.
 *********************************************************************/

#define E1000_TICKS_TO_USECS(ticks)	((1024 * (ticks) + 500) / 1000)
#define E1000_USECS_TO_TICKS(usecs)	((1000 * (usecs) + 512) / 1024)

static int em_tx_int_delay_dflt = E1000_TICKS_TO_USECS(EM_TIDV);
static int em_rx_int_delay_dflt = E1000_TICKS_TO_USECS(EM_RDTR);
static int em_tx_abs_int_delay_dflt = E1000_TICKS_TO_USECS(EM_TADV);
static int em_rx_abs_int_delay_dflt = E1000_TICKS_TO_USECS(EM_RADV);
static int em_int_throttle_ceil = 10000;

TUNABLE_INT("hw.em.tx_int_delay", &em_tx_int_delay_dflt);
TUNABLE_INT("hw.em.rx_int_delay", &em_rx_int_delay_dflt);
TUNABLE_INT("hw.em.tx_abs_int_delay", &em_tx_abs_int_delay_dflt);
TUNABLE_INT("hw.em.rx_abs_int_delay", &em_rx_abs_int_delay_dflt);
TUNABLE_INT("hw.em.int_throttle_ceil", &em_int_throttle_ceil);

/*********************************************************************
 *  Device identification routine
 *
 *  em_probe determines if the driver should be loaded on
 *  adapter based on PCI vendor/device id of the adapter.
 *
 *  return 0 on success, positive on failure
 *********************************************************************/

static int
em_probe(device_t dev)
{
	em_vendor_info_t *ent;

	uint16_t pci_vendor_id = 0;
	uint16_t pci_device_id = 0;
	uint16_t pci_subvendor_id = 0;
	uint16_t pci_subdevice_id = 0;
	char adapter_name[60];

	INIT_DEBUGOUT("em_probe: begin");

	pci_vendor_id = pci_get_vendor(dev);
	if (pci_vendor_id != EM_VENDOR_ID)
		return(ENXIO);

	pci_device_id = pci_get_device(dev);
	pci_subvendor_id = pci_get_subvendor(dev);
	pci_subdevice_id = pci_get_subdevice(dev);

	ent = em_vendor_info_array;
	while (ent->vendor_id != 0) {
		if ((pci_vendor_id == ent->vendor_id) &&
		    (pci_device_id == ent->device_id) &&

		    ((pci_subvendor_id == ent->subvendor_id) ||
		     (ent->subvendor_id == PCI_ANY_ID)) &&

		    ((pci_subdevice_id == ent->subdevice_id) ||
		     (ent->subdevice_id == PCI_ANY_ID))) {
			snprintf(adapter_name, sizeof(adapter_name),
				 "%s, Version - %s",  em_strings[ent->index], 
				 em_driver_version);
			device_set_desc_copy(dev, adapter_name);
			return(0);
		}
		ent++;
	}

	return(ENXIO);
}

/*********************************************************************
 *  Device initialization routine
 *
 *  The attach entry point is called when the driver is being loaded.
 *  This routine identifies the type of hardware, allocates all resources 
 *  and initializes the hardware.     
 *  
 *  return 0 on success, positive on failure
 *********************************************************************/

static int
em_attach(device_t dev)
{
	struct adapter *adapter;
	int tsize, rsize;
	int i, val, rid;
	int error = 0;

	INIT_DEBUGOUT("em_attach: begin");

	adapter = device_get_softc(dev);

	lwkt_serialize_init(&adapter->serializer);

	callout_init(&adapter->timer);
	callout_init(&adapter->tx_fifo_timer);

	adapter->dev = dev;
	adapter->osdep.dev = dev;

	/* SYSCTL stuff */
	sysctl_ctx_init(&adapter->sysctl_ctx);
	adapter->sysctl_tree = SYSCTL_ADD_NODE(&adapter->sysctl_ctx,
					       SYSCTL_STATIC_CHILDREN(_hw),
					       OID_AUTO, 
					       device_get_nameunit(dev),
					       CTLFLAG_RD,
					       0, "");

	if (adapter->sysctl_tree == NULL) {
		error = EIO;
		goto fail;
	}

	SYSCTL_ADD_PROC(&adapter->sysctl_ctx,  
			SYSCTL_CHILDREN(adapter->sysctl_tree),
			OID_AUTO, "debug_info", CTLTYPE_INT|CTLFLAG_RW, 
			(void *)adapter, 0,
			em_sysctl_debug_info, "I", "Debug Information");

	SYSCTL_ADD_PROC(&adapter->sysctl_ctx,  
			SYSCTL_CHILDREN(adapter->sysctl_tree),
			OID_AUTO, "stats", CTLTYPE_INT|CTLFLAG_RW, 
			(void *)adapter, 0,
			em_sysctl_stats, "I", "Statistics");

	/* Determine hardware revision */
	em_identify_hardware(adapter);

	/* Set up some sysctls for the tunable interrupt delays */
	em_add_int_delay_sysctl(adapter, "rx_int_delay",
				"receive interrupt delay in usecs",
				&adapter->rx_int_delay,
				E1000_REG_OFFSET(&adapter->hw, RDTR),
				em_rx_int_delay_dflt);
        em_add_int_delay_sysctl(adapter, "tx_int_delay",
				"transmit interrupt delay in usecs",
				&adapter->tx_int_delay,
				E1000_REG_OFFSET(&adapter->hw, TIDV),
				em_tx_int_delay_dflt);
	if (adapter->hw.mac_type >= em_82540) {
		em_add_int_delay_sysctl(adapter, "rx_abs_int_delay",
					"receive interrupt delay limit in usecs",
					&adapter->rx_abs_int_delay,
					E1000_REG_OFFSET(&adapter->hw, RADV),
					em_rx_abs_int_delay_dflt);
		em_add_int_delay_sysctl(adapter, "tx_abs_int_delay",
					"transmit interrupt delay limit in usecs",
					&adapter->tx_abs_int_delay,
					E1000_REG_OFFSET(&adapter->hw, TADV),
					em_tx_abs_int_delay_dflt);
		SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
			SYSCTL_CHILDREN(adapter->sysctl_tree),
			OID_AUTO, "int_throttle_ceil", CTLTYPE_INT|CTLFLAG_RW,
			adapter, 0, em_sysctl_int_throttle, "I", NULL);
	}
     
	/* Parameters (to be read from user) */   
	adapter->num_tx_desc = EM_MAX_TXD;
	adapter->num_rx_desc = EM_MAX_RXD;
	adapter->hw.autoneg = DO_AUTO_NEG;
	adapter->hw.wait_autoneg_complete = WAIT_FOR_AUTO_NEG_DEFAULT;
	adapter->hw.autoneg_advertised = AUTONEG_ADV_DEFAULT;
	adapter->hw.tbi_compatibility_en = TRUE;
	adapter->rx_buffer_len = EM_RXBUFFER_2048;

	/*
	 * These parameters control the automatic generation(Tx) and
	 * response(Rx) to Ethernet PAUSE frames.
	 */
	adapter->hw.fc_high_water = FC_DEFAULT_HI_THRESH;
	adapter->hw.fc_low_water  = FC_DEFAULT_LO_THRESH;
	adapter->hw.fc_pause_time = FC_DEFAULT_TX_TIMER;
	adapter->hw.fc_send_xon   = TRUE;
	adapter->hw.fc = em_fc_full;

	adapter->hw.phy_init_script = 1;
	adapter->hw.phy_reset_disable = FALSE;

#ifndef EM_MASTER_SLAVE
	adapter->hw.master_slave = em_ms_hw_default;
#else
	adapter->hw.master_slave = EM_MASTER_SLAVE;
#endif

	/* 
	 * Set the max frame size assuming standard ethernet 
	 * sized frames 
	 */   
	adapter->hw.max_frame_size = ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN;

	adapter->hw.min_frame_size = 
	    MINIMUM_ETHERNET_PACKET_SIZE + ETHER_CRC_LEN;

	/* 
	 * This controls when hardware reports transmit completion 
	 * status. 
	 */
	adapter->hw.report_tx_early = 1;

	rid = EM_MMBA;
	adapter->res_memory = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
						     &rid, RF_ACTIVE);
	if (!(adapter->res_memory)) {
		device_printf(dev, "Unable to allocate bus resource: memory\n");
		error = ENXIO;
		goto fail;
	}
	adapter->osdep.mem_bus_space_tag = 
	    rman_get_bustag(adapter->res_memory);
	adapter->osdep.mem_bus_space_handle = 
	    rman_get_bushandle(adapter->res_memory);
	adapter->hw.hw_addr = (uint8_t *)&adapter->osdep.mem_bus_space_handle;

	if (adapter->hw.mac_type > em_82543) {
		/* Figure our where our IO BAR is ? */
		rid = EM_MMBA;
		for (i = 0; i < 5; i++) {
			val = pci_read_config(dev, rid, 4);
			if (val & 0x00000001) {
				adapter->io_rid = rid;
				break;
			}
			rid += 4;
		}

		adapter->res_ioport = bus_alloc_resource_any(dev,
		    SYS_RES_IOPORT, &adapter->io_rid, RF_ACTIVE);
		if (!(adapter->res_ioport)) {
			device_printf(dev, "Unable to allocate bus resource: ioport\n");
			error = ENXIO;
			goto fail;
		}

		adapter->hw.reg_io_tag = rman_get_bustag(adapter->res_ioport);
		adapter->hw.reg_io_handle = rman_get_bushandle(adapter->res_ioport);
	}

	rid = 0x0;
	adapter->res_interrupt = bus_alloc_resource_any(dev, SYS_RES_IRQ,
	    &rid, RF_SHAREABLE | RF_ACTIVE);
	if (!(adapter->res_interrupt)) {
		device_printf(dev, "Unable to allocate bus resource: interrupt\n");
		error = ENXIO;
		goto fail;
	}

	adapter->hw.back = &adapter->osdep;

	/* Initialize eeprom parameters */
	em_init_eeprom_params(&adapter->hw);

	tsize = adapter->num_tx_desc * sizeof(struct em_tx_desc);

	/* Allocate Transmit Descriptor ring */
	if (em_dma_malloc(adapter, tsize, &adapter->txdma, BUS_DMA_WAITOK)) {
		device_printf(dev, "Unable to allocate TxDescriptor memory\n");
		error = ENOMEM;
		goto fail;
	}
	adapter->tx_desc_base = (struct em_tx_desc *) adapter->txdma.dma_vaddr;

	rsize = adapter->num_rx_desc * sizeof(struct em_rx_desc);

	/* Allocate Receive Descriptor ring */
	if (em_dma_malloc(adapter, rsize, &adapter->rxdma, BUS_DMA_WAITOK)) {
		device_printf(dev, "Unable to allocate rx_desc memory\n");
		error = ENOMEM;
		goto fail;
	}
	adapter->rx_desc_base = (struct em_rx_desc *) adapter->rxdma.dma_vaddr;

	/* Initialize the hardware */
	if (em_hardware_init(adapter)) {
		device_printf(dev, "Unable to initialize the hardware\n");
		error = EIO;
		goto fail;
	}

	/* Copy the permanent MAC address out of the EEPROM */
	if (em_read_mac_addr(&adapter->hw) < 0) {
		device_printf(dev, "EEPROM read error while reading mac address\n");
		error = EIO;
		goto fail;
	}

	if (!em_is_valid_ether_addr(adapter->hw.mac_addr)) {
		device_printf(dev, "Invalid mac address\n");
		error = EIO;
		goto fail;
	}

	/* Setup OS specific network interface */
	em_setup_interface(dev, adapter);

	/* Initialize statistics */
	em_clear_hw_cntrs(&adapter->hw);
	em_update_stats_counters(adapter);
	adapter->hw.get_link_status = 1;
	em_check_for_link(&adapter->hw);

	/* Print the link status */
	if (adapter->link_active == 1) {
		em_get_speed_and_duplex(&adapter->hw, &adapter->link_speed, 
					&adapter->link_duplex);
		device_printf(dev, "Speed: %d Mbps, Duplex: %s\n",
		    adapter->link_speed,
		    adapter->link_duplex == FULL_DUPLEX ? "Full" : "Half");
	} else
		device_printf(dev, "Speed: N/A, Duplex:N/A\n");

	/* Identify 82544 on PCIX */
	em_get_bus_info(&adapter->hw);	
	if (adapter->hw.bus_type == em_bus_type_pcix &&
	    adapter->hw.mac_type == em_82544)
		adapter->pcix_82544 = TRUE;
        else
		adapter->pcix_82544 = FALSE;

	error = bus_setup_intr(dev, adapter->res_interrupt, 0,
			   (void (*)(void *)) em_intr, adapter,
			   &adapter->int_handler_tag, &adapter->serializer);
	if (error) {
		device_printf(dev, "Error registering interrupt handler!\n");
		ether_ifdetach(&adapter->interface_data.ac_if);
		goto fail;
	}

	INIT_DEBUGOUT("em_attach: end");
	return(0);

fail:
	em_detach(dev);
	return(error);
}

/*********************************************************************
 *  Device removal routine
 *
 *  The detach entry point is called when the driver is being removed.
 *  This routine stops the adapter and deallocates all the resources
 *  that were allocated for driver operation.
 *  
 *  return 0 on success, positive on failure
 *********************************************************************/

static int
em_detach(device_t dev)
{
	struct adapter * adapter = device_get_softc(dev);

	INIT_DEBUGOUT("em_detach: begin");

	lwkt_serialize_enter(&adapter->serializer);
	adapter->in_detach = 1;

	if (device_is_attached(dev)) {
		em_stop(adapter);
		em_phy_hw_reset(&adapter->hw);
		ether_ifdetach(&adapter->interface_data.ac_if);
	}
	bus_generic_detach(dev);

	if (adapter->res_interrupt != NULL) {
		bus_teardown_intr(dev, adapter->res_interrupt, 
				  adapter->int_handler_tag);
		bus_release_resource(dev, SYS_RES_IRQ, 0, 
				     adapter->res_interrupt);
	}
	if (adapter->res_memory != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, EM_MMBA, 
				     adapter->res_memory);
	}

	if (adapter->res_ioport != NULL) {
		bus_release_resource(dev, SYS_RES_IOPORT, adapter->io_rid, 
				     adapter->res_ioport);
	}

	/* Free Transmit Descriptor ring */
	if (adapter->tx_desc_base != NULL) {
		em_dma_free(adapter, &adapter->txdma);
		adapter->tx_desc_base = NULL;
	}

	/* Free Receive Descriptor ring */
	if (adapter->rx_desc_base != NULL) {
		em_dma_free(adapter, &adapter->rxdma);
		adapter->rx_desc_base = NULL;
	}

	adapter->sysctl_tree = NULL;
	sysctl_ctx_free(&adapter->sysctl_ctx);

	lwkt_serialize_exit(&adapter->serializer);
	return(0);
}

/*********************************************************************
 *
 *  Shutdown entry point
 *
 **********************************************************************/ 

static int
em_shutdown(device_t dev)
{
	struct adapter *adapter = device_get_softc(dev);
	em_stop(adapter);
	return(0);
}

/*********************************************************************
 *  Transmit entry point
 *
 *  em_start is called by the stack to initiate a transmit.
 *  The driver will remain in this routine as long as there are
 *  packets to transmit and transmit resources are available.
 *  In case resources are not available stack is notified and
 *  the packet is requeued.
 **********************************************************************/

static void
em_start(struct ifnet *ifp)
{
	struct adapter *adapter = ifp->if_softc;

	lwkt_serialize_enter(&adapter->serializer);
	em_start_serialized(ifp);
	lwkt_serialize_exit(&adapter->serializer);
}

static void
em_start_serialized(struct ifnet *ifp)
{
	struct mbuf *m_head;
	struct adapter *adapter = ifp->if_softc;

	if (!adapter->link_active)
		return;
	while (!ifq_is_empty(&ifp->if_snd)) {
		m_head = ifq_poll(&ifp->if_snd);

		if (m_head == NULL)
			break;

		if (em_encap(adapter, m_head)) { 
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}
		m_head = ifq_dequeue(&ifp->if_snd);

		/* Send a copy of the frame to the BPF listener */
		BPF_MTAP(ifp, m_head);
        
		/* Set timeout in case hardware has problems transmitting */
		ifp->if_timer = EM_TX_TIMEOUT;        
	}
}

/*********************************************************************
 *  Ioctl entry point
 *
 *  em_ioctl is called when the user wants to configure the
 *  interface.
 *
 *  return 0 on success, positive on failure
 **********************************************************************/

static int
em_ioctl(struct ifnet *ifp, u_long command, caddr_t data, struct ucred *cr)
{
	int mask, error = 0;
	struct ifreq *ifr = (struct ifreq *) data;
	struct adapter *adapter = ifp->if_softc;

	lwkt_serialize_enter(&adapter->serializer);

	if (adapter->in_detach)
		goto out;

	switch (command) {
	case SIOCSIFADDR:
	case SIOCGIFADDR:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCxIFADDR (Get/Set Interface Addr)");
		lwkt_serialize_exit(&adapter->serializer);
		ether_ioctl(ifp, command, data);
		lwkt_serialize_enter(&adapter->serializer);
		break;
	case SIOCSIFMTU:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCSIFMTU (Set Interface MTU)");
		if (ifr->ifr_mtu > MAX_JUMBO_FRAME_SIZE - ETHER_HDR_LEN) {
			error = EINVAL;
		} else {
			ifp->if_mtu = ifr->ifr_mtu;
			adapter->hw.max_frame_size = 
			ifp->if_mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;
			em_init_serialized(adapter);
		}
		break;
	case SIOCSIFFLAGS:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCSIFFLAGS (Set Interface Flags)");
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING))
				em_init_serialized(adapter);
			em_disable_promisc(adapter);
			em_set_promisc(adapter);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				em_stop(adapter);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOC(ADD|DEL)MULTI");
		if (ifp->if_flags & IFF_RUNNING) {
			em_disable_intr(adapter);
			em_set_multi(adapter);
			if (adapter->hw.mac_type == em_82542_rev2_0)
				em_initialize_receive_unit(adapter);
			em_enable_intr(adapter);
		}
		break;
	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCxIFMEDIA (Get/Set Interface Media)");
		error = ifmedia_ioctl(ifp, ifr, &adapter->media, command);
		break;
	case SIOCSIFCAP:
		IOCTL_DEBUGOUT("ioctl rcv'd: SIOCSIFCAP (Set Capabilities)");
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if (mask & IFCAP_HWCSUM) {
			if (IFCAP_HWCSUM & ifp->if_capenable)
				ifp->if_capenable &= ~IFCAP_HWCSUM;
			else
				ifp->if_capenable |= IFCAP_HWCSUM;
			if (ifp->if_flags & IFF_RUNNING)
				em_init_serialized(adapter);
		}
		break;
	default:
		IOCTL_DEBUGOUT1("ioctl received: UNKNOWN (0x%x)", (int)command);
		error = EINVAL;
	}

out:
	lwkt_serialize_exit(&adapter->serializer);
	return(error);
}

/*********************************************************************
 *  Watchdog entry point
 *
 *  This routine is called whenever hardware quits transmitting.
 *
 **********************************************************************/

static void
em_watchdog(struct ifnet *ifp)
{
	struct adapter * adapter;
	adapter = ifp->if_softc;

	/* If we are in this routine because of pause frames, then
	 * don't reset the hardware.
	 */
	if (E1000_READ_REG(&adapter->hw, STATUS) & E1000_STATUS_TXOFF) {
		ifp->if_timer = EM_TX_TIMEOUT;
		return;
	}

	if (em_check_for_link(&adapter->hw))
		if_printf(ifp, "watchdog timeout -- resetting\n");

	ifp->if_flags &= ~IFF_RUNNING;

	em_init(adapter);

	ifp->if_oerrors++;
}

/*********************************************************************
 *  Init entry point
 *
 *  This routine is used in two ways. It is used by the stack as
 *  init entry point in network interface structure. It is also used
 *  by the driver as a hw/sw initialization routine to get to a 
 *  consistent state.
 *
 *  return 0 on success, positive on failure
 **********************************************************************/

static void
em_init(void *arg)
{
	struct adapter *adapter = arg;

	lwkt_serialize_enter(&adapter->serializer);
	em_init_serialized(arg);
	lwkt_serialize_exit(&adapter->serializer);
}

static void
em_init_serialized(void *arg)
{
	struct adapter *adapter = arg;
	uint32_t pba;
	struct ifnet *ifp = &adapter->interface_data.ac_if;

	INIT_DEBUGOUT("em_init: begin");

	em_stop(adapter);

	/*
	 * Packet Buffer Allocation (PBA)
	 * Writing PBA sets the receive portion of the buffer
	 * the remainder is used for the transmit buffer.
	 *
	 * Devices before the 82547 had a Packet Buffer of 64K.
	 *   Default allocation: PBA=48K for Rx, leaving 16K for Tx.
	 * After the 82547 the buffer was reduced to 40K.
	 *   Default allocation: PBA=30K for Rx, leaving 10K for Tx.
	 *   Note: default does not leave enough room for Jumbo Frame >10k.
	 */
	if(adapter->hw.mac_type < em_82547) {
		/* Total FIFO is 64K */
		if(adapter->rx_buffer_len > EM_RXBUFFER_8192)
			pba = E1000_PBA_40K; /* 40K for Rx, 24K for Tx */
		else
			pba = E1000_PBA_48K; /* 48K for Rx, 16K for Tx */
	} else {
		/* Total FIFO is 40K */
		if(adapter->hw.max_frame_size > EM_RXBUFFER_8192) {
			pba = E1000_PBA_22K; /* 22K for Rx, 18K for Tx */
		} else {
		        pba = E1000_PBA_30K; /* 30K for Rx, 10K for Tx */
		}
		adapter->tx_fifo_head = 0;
		adapter->tx_head_addr = pba << EM_TX_HEAD_ADDR_SHIFT;
		adapter->tx_fifo_size =
			(E1000_PBA_40K - pba) << EM_PBA_BYTES_SHIFT;
	}
	INIT_DEBUGOUT1("em_init: pba=%dK",pba);
	E1000_WRITE_REG(&adapter->hw, PBA, pba);

	/* Get the latest mac address, User can use a LAA */
	bcopy(adapter->interface_data.ac_enaddr, adapter->hw.mac_addr,
	      ETHER_ADDR_LEN);

	/* Initialize the hardware */
	if (em_hardware_init(adapter)) {
		if_printf(ifp, "Unable to initialize the hardware\n");
		return;
	}

	em_enable_vlans(adapter);

	/* Prepare transmit descriptors and buffers */
	if (em_setup_transmit_structures(adapter)) {
		if_printf(ifp, "Could not setup transmit structures\n");
		em_stop(adapter); 
		return;
	}
	em_initialize_transmit_unit(adapter);

	/* Setup Multicast table */
	em_set_multi(adapter);

	/* Prepare receive descriptors and buffers */
	if (em_setup_receive_structures(adapter)) {
		if_printf(ifp, "Could not setup receive structures\n");
		em_stop(adapter);
		return;
	}
	em_initialize_receive_unit(adapter);
	
	/* Don't loose promiscuous settings */
	em_set_promisc(adapter);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	if (adapter->hw.mac_type >= em_82543) {
		if (ifp->if_capenable & IFCAP_TXCSUM)
			ifp->if_hwassist = EM_CHECKSUM_FEATURES;
		else
			ifp->if_hwassist = 0;
	}

	callout_reset(&adapter->timer, hz, em_local_timer, adapter);
	em_clear_hw_cntrs(&adapter->hw);
	em_enable_intr(adapter);

	/* Don't reset the phy next time init gets called */
	adapter->hw.phy_reset_disable = TRUE;
}

#ifdef DEVICE_POLLING

static void
em_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct adapter *adapter = ifp->if_softc;
	uint32_t reg_icr;

	lwkt_serialize_enter(&adapter->serializer);
	switch(cmd) {
	case POLL_REGISTER:
		em_disable_intr(adapter);
		break;
	case POLL_DEREGISTER:
		em_enable_intr(adapter);
		break;
	case POLL_AND_CHECK_STATUS:
		reg_icr = E1000_READ_REG(&adapter->hw, ICR);
		if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
			callout_stop(&adapter->timer);
			adapter->hw.get_link_status = 1;
			em_check_for_link(&adapter->hw);
			em_print_link_status(adapter);
			callout_reset(&adapter->timer, hz, em_local_timer,
				      adapter);
		}
		/* fall through */
	case POLL_ONLY:
		if (ifp->if_flags & IFF_RUNNING) {
			em_process_receive_interrupts(adapter, count);
			em_clean_transmit_interrupts(adapter);
		}
		if (ifp->if_flags & IFF_RUNNING) {
			if (!ifq_is_empty(&ifp->if_snd))
				em_start_serialized(ifp);
		}
		break;
	}
	lwkt_serialize_exit(&adapter->serializer);
}

#endif /* DEVICE_POLLING */

/*********************************************************************
 *
 *  Interrupt Service routine
 *
 **********************************************************************/
static void
em_intr(void *arg)
{
	uint32_t reg_icr;
	struct ifnet *ifp;
	struct adapter *adapter = arg;

	ifp = &adapter->interface_data.ac_if;  

	reg_icr = E1000_READ_REG(&adapter->hw, ICR);
	if (!reg_icr)
		return;

	/* Link status change */
	if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		callout_stop(&adapter->timer);
		adapter->hw.get_link_status = 1;
		em_check_for_link(&adapter->hw);
		em_print_link_status(adapter);
		callout_reset(&adapter->timer, hz, em_local_timer, adapter);
	}

	/*
	 * note: do not attempt to improve efficiency by looping.  This 
	 * only results in unnecessary piecemeal collection of received
	 * packets and unnecessary piecemeal cleanups of the transmit ring.
	 */
	if (ifp->if_flags & IFF_RUNNING) {
		em_process_receive_interrupts(adapter, -1);
		em_clean_transmit_interrupts(adapter);
	}

	if ((ifp->if_flags & IFF_RUNNING) && !ifq_is_empty(&ifp->if_snd))
		em_start_serialized(ifp);
}

/*********************************************************************
 *
 *  Media Ioctl callback
 *
 *  This routine is called whenever the user queries the status of
 *  the interface using ifconfig.
 *
 **********************************************************************/
static void
em_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct adapter * adapter = ifp->if_softc;

	INIT_DEBUGOUT("em_media_status: begin");

	em_check_for_link(&adapter->hw);
	if (E1000_READ_REG(&adapter->hw, STATUS) & E1000_STATUS_LU) {
		if (adapter->link_active == 0) {
			em_get_speed_and_duplex(&adapter->hw, 
						&adapter->link_speed, 
						&adapter->link_duplex);
			adapter->link_active = 1;
		}
	} else {
		if (adapter->link_active == 1) {
			adapter->link_speed = 0;
			adapter->link_duplex = 0;
			adapter->link_active = 0;
		}
	}

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

	if (!adapter->link_active)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;

	if (adapter->hw.media_type == em_media_type_fiber) {
		ifmr->ifm_active |= IFM_1000_SX | IFM_FDX;
	} else {
		switch (adapter->link_speed) {
		case 10:
			ifmr->ifm_active |= IFM_10_T;
			break;
		case 100:
			ifmr->ifm_active |= IFM_100_TX;
			break;
		case 1000:
			ifmr->ifm_active |= IFM_1000_T;
			break;
		}
		if (adapter->link_duplex == FULL_DUPLEX)
			ifmr->ifm_active |= IFM_FDX;
		else
			ifmr->ifm_active |= IFM_HDX;
	}
}

/*********************************************************************
 *
 *  Media Ioctl callback
 *
 *  This routine is called when the user changes speed/duplex using
 *  media/mediopt option with ifconfig.
 *
 **********************************************************************/
static int
em_media_change(struct ifnet *ifp)
{
	struct adapter * adapter = ifp->if_softc;
	struct ifmedia  *ifm = &adapter->media;

	INIT_DEBUGOUT("em_media_change: begin");

	if (IFM_TYPE(ifm->ifm_media) != IFM_ETHER)
		return(EINVAL);

	lwkt_serialize_enter(&adapter->serializer);

	switch (IFM_SUBTYPE(ifm->ifm_media)) {
	case IFM_AUTO:
		adapter->hw.autoneg = DO_AUTO_NEG;
		adapter->hw.autoneg_advertised = AUTONEG_ADV_DEFAULT;
		break;
	case IFM_1000_SX:
	case IFM_1000_T:
		adapter->hw.autoneg = DO_AUTO_NEG;
		adapter->hw.autoneg_advertised = ADVERTISE_1000_FULL;
		break;
	case IFM_100_TX:
		adapter->hw.autoneg = FALSE;
		adapter->hw.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			adapter->hw.forced_speed_duplex = em_100_full;
		else
			adapter->hw.forced_speed_duplex	= em_100_half;
		break;
	case IFM_10_T:
		adapter->hw.autoneg = FALSE;
		adapter->hw.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			adapter->hw.forced_speed_duplex = em_10_full;
		else
			adapter->hw.forced_speed_duplex	= em_10_half;
		break;
	default:
		if_printf(ifp, "Unsupported media type\n");
	}
	/*
	 * As the speed/duplex settings may have changed we need to
	 * reset the PHY.
	 */
	adapter->hw.phy_reset_disable = FALSE;

	em_init_serialized(adapter);

	lwkt_serialize_exit(&adapter->serializer);
	return(0);
}

static void
em_tx_cb(void *arg, bus_dma_segment_t *seg, int nsegs, bus_size_t mapsize,
	 int error)
{
	struct em_q *q = arg;

	if (error)
		return;
	KASSERT(nsegs <= EM_MAX_SCATTER,
		("Too many DMA segments returned when mapping tx packet"));
	q->nsegs = nsegs;
	bcopy(seg, q->segs, nsegs * sizeof(seg[0]));
}

/*********************************************************************
 *
 *  This routine maps the mbufs to tx descriptors.
 *
 *  return 0 on success, positive on failure
 **********************************************************************/
static int
em_encap(struct adapter *adapter, struct mbuf *m_head)
{
	uint32_t txd_upper;
	uint32_t txd_lower, txd_used = 0, txd_saved = 0;
	int i, j, error;
	uint64_t address;

	/* For 82544 Workaround */
	DESC_ARRAY desc_array;
	uint32_t array_elements;
	uint32_t counter;

	struct ifvlan *ifv = NULL;
	struct em_q q;
        struct em_buffer *tx_buffer = NULL;
        struct em_tx_desc *current_tx_desc = NULL;
        struct ifnet *ifp = &adapter->interface_data.ac_if;

	/*
	 * Force a cleanup if number of TX descriptors
	 * available hits the threshold
	 */
	if (adapter->num_tx_desc_avail <= EM_TX_CLEANUP_THRESHOLD) {
		em_clean_transmit_interrupts(adapter);
		if (adapter->num_tx_desc_avail <= EM_TX_CLEANUP_THRESHOLD) {
			adapter->no_tx_desc_avail1++;
			return(ENOBUFS);
		}
	}
	/*
	 * Map the packet for DMA.
	 */
	if (bus_dmamap_create(adapter->txtag, BUS_DMA_NOWAIT, &q.map)) {
		adapter->no_tx_map_avail++;
		return(ENOMEM);
	}
	error = bus_dmamap_load_mbuf(adapter->txtag, q.map, m_head, em_tx_cb,
				     &q, BUS_DMA_NOWAIT);
	if (error != 0) {
		adapter->no_tx_dma_setup++;
		bus_dmamap_destroy(adapter->txtag, q.map);
		return(error);
	}
	KASSERT(q.nsegs != 0, ("em_encap: empty packet"));

	if (q.nsegs > adapter->num_tx_desc_avail) {
		adapter->no_tx_desc_avail2++;
		bus_dmamap_unload(adapter->txtag, q.map);
		bus_dmamap_destroy(adapter->txtag, q.map);
		return(ENOBUFS);
	}

	if (ifp->if_hwassist > 0) {
		em_transmit_checksum_setup(adapter,  m_head,
					   &txd_upper, &txd_lower);
	}
	else 
		txd_upper = txd_lower = 0;

	/* Find out if we are in vlan mode */
	if ((m_head->m_flags & (M_PROTO1|M_PKTHDR)) == (M_PROTO1|M_PKTHDR) &&
	    m_head->m_pkthdr.rcvif != NULL &&
	    m_head->m_pkthdr.rcvif->if_type == IFT_L2VLAN)
		ifv = m_head->m_pkthdr.rcvif->if_softc;

	i = adapter->next_avail_tx_desc;
	if (adapter->pcix_82544) {
		txd_saved = i;
		txd_used = 0;
	}
	for (j = 0; j < q.nsegs; j++) {
		/* If adapter is 82544 and on PCIX bus */
		if(adapter->pcix_82544) {
			array_elements = 0;
			address = htole64(q.segs[j].ds_addr);
			/* 
			 * Check the Address and Length combination and
			 * split the data accordingly
			 */
			array_elements = em_fill_descriptors(address,
							     htole32(q.segs[j].ds_len),
							     &desc_array);
			for (counter = 0; counter < array_elements; counter++) {
				if (txd_used == adapter->num_tx_desc_avail) {
					adapter->next_avail_tx_desc = txd_saved;
					adapter->no_tx_desc_avail2++;
					bus_dmamap_unload(adapter->txtag, q.map);
					bus_dmamap_destroy(adapter->txtag, q.map);
					return(ENOBUFS);
				}
				tx_buffer = &adapter->tx_buffer_area[i];
				current_tx_desc = &adapter->tx_desc_base[i];
				current_tx_desc->buffer_addr = htole64(
				desc_array.descriptor[counter].address);
				current_tx_desc->lower.data = htole32(
				(adapter->txd_cmd | txd_lower | 
				(uint16_t)desc_array.descriptor[counter].length));
				current_tx_desc->upper.data = htole32((txd_upper));
				if (++i == adapter->num_tx_desc)
					i = 0;

				tx_buffer->m_head = NULL;
				txd_used++;
			}
		} else {
			tx_buffer = &adapter->tx_buffer_area[i];
			current_tx_desc = &adapter->tx_desc_base[i];

			current_tx_desc->buffer_addr = htole64(q.segs[j].ds_addr);
			current_tx_desc->lower.data = htole32(
				adapter->txd_cmd | txd_lower | q.segs[j].ds_len);
			current_tx_desc->upper.data = htole32(txd_upper);

			if (++i == adapter->num_tx_desc)
				i = 0;

			tx_buffer->m_head = NULL;
		}
	}

	adapter->next_avail_tx_desc = i;
	if (adapter->pcix_82544)
		adapter->num_tx_desc_avail -= txd_used;
	else
		adapter->num_tx_desc_avail -= q.nsegs;

	if (ifv != NULL) {
		/* Set the vlan id */
		current_tx_desc->upper.fields.special = htole16(ifv->ifv_tag);

		/* Tell hardware to add tag */
		current_tx_desc->lower.data |= htole32(E1000_TXD_CMD_VLE);
	}

	tx_buffer->m_head = m_head;
	tx_buffer->map = q.map;
	bus_dmamap_sync(adapter->txtag, q.map, BUS_DMASYNC_PREWRITE);

	/*
	 * Last Descriptor of Packet needs End Of Packet (EOP)
	 */
	current_tx_desc->lower.data |= htole32(E1000_TXD_CMD_EOP);

	bus_dmamap_sync(adapter->txdma.dma_tag, adapter->txdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	/* 
	 * Advance the Transmit Descriptor Tail (Tdt), this tells the E1000
	 * that this frame is available to transmit.
	 */
	if (adapter->hw.mac_type == em_82547 &&
	    adapter->link_duplex == HALF_DUPLEX) {
		em_82547_move_tail_serialized(adapter);
	} else {
		E1000_WRITE_REG(&adapter->hw, TDT, i);
		if (adapter->hw.mac_type == em_82547) {
			em_82547_update_fifo_head(adapter,
						  m_head->m_pkthdr.len);
		}
	}

	return(0);
}

/*********************************************************************
 *
 * 82547 workaround to avoid controller hang in half-duplex environment.
 * The workaround is to avoid queuing a large packet that would span   
 * the internal Tx FIFO ring boundary. We need to reset the FIFO pointers
 * in this case. We do that only when FIFO is quiescent.
 *
 **********************************************************************/
static void
em_82547_move_tail(void *arg)
{
	struct adapter *adapter = arg;

	lwkt_serialize_enter(&adapter->serializer);
	em_82547_move_tail_serialized(arg);
	lwkt_serialize_exit(&adapter->serializer);
}

static void
em_82547_move_tail_serialized(void *arg)
{
	struct adapter *adapter = arg;
	uint16_t hw_tdt;
	uint16_t sw_tdt;
	struct em_tx_desc *tx_desc;
	uint16_t length = 0;
	boolean_t eop = 0;

	hw_tdt = E1000_READ_REG(&adapter->hw, TDT);
	sw_tdt = adapter->next_avail_tx_desc;

	while (hw_tdt != sw_tdt) {
		tx_desc = &adapter->tx_desc_base[hw_tdt];
		length += tx_desc->lower.flags.length;
		eop = tx_desc->lower.data & E1000_TXD_CMD_EOP;
		if(++hw_tdt == adapter->num_tx_desc)
			hw_tdt = 0;

		if(eop) {
			if (em_82547_fifo_workaround(adapter, length)) {
				adapter->tx_fifo_wrk_cnt++;
				callout_reset(&adapter->tx_fifo_timer, 1,
					em_82547_move_tail, adapter);
				break;
			}
			E1000_WRITE_REG(&adapter->hw, TDT, hw_tdt);
			em_82547_update_fifo_head(adapter, length);
			length = 0;
		}
	}	
}

static int
em_82547_fifo_workaround(struct adapter *adapter, int len)
{	
	int fifo_space, fifo_pkt_len;

	fifo_pkt_len = EM_ROUNDUP(len + EM_FIFO_HDR, EM_FIFO_HDR);

	if (adapter->link_duplex == HALF_DUPLEX) {
		fifo_space = adapter->tx_fifo_size - adapter->tx_fifo_head;

		if (fifo_pkt_len >= (EM_82547_PKT_THRESH + fifo_space)) {
			if (em_82547_tx_fifo_reset(adapter))
				return(0);
			else
				return(1);
		}
	}

	return(0);
}

static void
em_82547_update_fifo_head(struct adapter *adapter, int len)
{
	int fifo_pkt_len = EM_ROUNDUP(len + EM_FIFO_HDR, EM_FIFO_HDR);

	/* tx_fifo_head is always 16 byte aligned */
	adapter->tx_fifo_head += fifo_pkt_len;
	if (adapter->tx_fifo_head >= adapter->tx_fifo_size)
		adapter->tx_fifo_head -= adapter->tx_fifo_size;
}

static int
em_82547_tx_fifo_reset(struct adapter *adapter)
{	
	uint32_t tctl;

	if ( (E1000_READ_REG(&adapter->hw, TDT) ==
	      E1000_READ_REG(&adapter->hw, TDH)) &&
	     (E1000_READ_REG(&adapter->hw, TDFT) == 
	      E1000_READ_REG(&adapter->hw, TDFH)) &&
	     (E1000_READ_REG(&adapter->hw, TDFTS) ==
	      E1000_READ_REG(&adapter->hw, TDFHS)) &&
	     (E1000_READ_REG(&adapter->hw, TDFPC) == 0)) {

		/* Disable TX unit */
		tctl = E1000_READ_REG(&adapter->hw, TCTL);
		E1000_WRITE_REG(&adapter->hw, TCTL, tctl & ~E1000_TCTL_EN);

		/* Reset FIFO pointers */
		E1000_WRITE_REG(&adapter->hw, TDFT,  adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, TDFH,  adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, TDFTS, adapter->tx_head_addr);
		E1000_WRITE_REG(&adapter->hw, TDFHS, adapter->tx_head_addr);

		/* Re-enable TX unit */
		E1000_WRITE_REG(&adapter->hw, TCTL, tctl);
		E1000_WRITE_FLUSH(&adapter->hw);

		adapter->tx_fifo_head = 0;
		adapter->tx_fifo_reset_cnt++;

		return(TRUE);
	} else {
		return(FALSE);
	}
}

static void
em_set_promisc(struct adapter *adapter)
{
	uint32_t reg_rctl, ctrl;
	struct ifnet *ifp = &adapter->interface_data.ac_if;

	reg_rctl = E1000_READ_REG(&adapter->hw, RCTL);
	ctrl = E1000_READ_REG(&adapter->hw, CTRL);

	if (ifp->if_flags & IFF_PROMISC) {
		reg_rctl |= (E1000_RCTL_UPE | E1000_RCTL_MPE);
		E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);

		/*
		 * Disable VLAN stripping in promiscous mode.
		 * This enables bridging of vlan tagged frames to occur 
		 * and also allows vlan tags to be seen in tcpdump.
		 */
		ctrl &= ~E1000_CTRL_VME; 
		E1000_WRITE_REG(&adapter->hw, CTRL, ctrl);
	} else if (ifp->if_flags & IFF_ALLMULTI) {
		reg_rctl |= E1000_RCTL_MPE;
		reg_rctl &= ~E1000_RCTL_UPE;
		E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);
	}
}

static void
em_disable_promisc(struct adapter *adapter)
{
	uint32_t reg_rctl;

	reg_rctl = E1000_READ_REG(&adapter->hw, RCTL);

	reg_rctl &=  (~E1000_RCTL_UPE);
	reg_rctl &=  (~E1000_RCTL_MPE);
	E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);

	em_enable_vlans(adapter);
}

/*********************************************************************
 *  Multicast Update
 *
 *  This routine is called whenever multicast address list is updated.
 *
 **********************************************************************/

static void
em_set_multi(struct adapter *adapter)
{
	uint32_t reg_rctl = 0;
	uint8_t mta[MAX_NUM_MULTICAST_ADDRESSES * ETH_LENGTH_OF_ADDRESS];
	struct ifmultiaddr *ifma;
	int mcnt = 0;
	struct ifnet *ifp = &adapter->interface_data.ac_if;

	IOCTL_DEBUGOUT("em_set_multi: begin");

	if (adapter->hw.mac_type == em_82542_rev2_0) {
		reg_rctl = E1000_READ_REG(&adapter->hw, RCTL);
		if (adapter->hw.pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
			em_pci_clear_mwi(&adapter->hw);
		reg_rctl |= E1000_RCTL_RST;
		E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);
		msec_delay(5);
	}

	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;

		if (mcnt == MAX_NUM_MULTICAST_ADDRESSES)
			break;

		bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
		      &mta[mcnt*ETH_LENGTH_OF_ADDRESS], ETH_LENGTH_OF_ADDRESS);
		mcnt++;
	}

	if (mcnt >= MAX_NUM_MULTICAST_ADDRESSES) {
		reg_rctl = E1000_READ_REG(&adapter->hw, RCTL);
		reg_rctl |= E1000_RCTL_MPE;
		E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);
	} else
		em_mc_addr_list_update(&adapter->hw, mta, mcnt, 0, 1);

	if (adapter->hw.mac_type == em_82542_rev2_0) {
		reg_rctl = E1000_READ_REG(&adapter->hw, RCTL);
		reg_rctl &= ~E1000_RCTL_RST;
		E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);
		msec_delay(5);
		if (adapter->hw.pci_cmd_word & CMD_MEM_WRT_INVALIDATE)
                        em_pci_set_mwi(&adapter->hw);
	}
}

/*********************************************************************
 *  Timer routine
 *
 *  This routine checks for link status and updates statistics.
 *
 **********************************************************************/

static void
em_local_timer(void *arg)
{
	struct ifnet *ifp;
	struct adapter *adapter = arg;
	ifp = &adapter->interface_data.ac_if;

	lwkt_serialize_enter(&adapter->serializer);

	em_check_for_link(&adapter->hw);
	em_print_link_status(adapter);
	em_update_stats_counters(adapter);   
	if (em_display_debug_stats && ifp->if_flags & IFF_RUNNING)
		em_print_hw_stats(adapter);
	em_smartspeed(adapter);

	callout_reset(&adapter->timer, hz, em_local_timer, adapter);

	lwkt_serialize_exit(&adapter->serializer);
}

static void
em_print_link_status(struct adapter *adapter)
{
	if (E1000_READ_REG(&adapter->hw, STATUS) & E1000_STATUS_LU) {
		if (adapter->link_active == 0) {
			em_get_speed_and_duplex(&adapter->hw, 
						&adapter->link_speed, 
						&adapter->link_duplex);
			device_printf(adapter->dev, "Link is up %d Mbps %s\n",
			       adapter->link_speed,
			       ((adapter->link_duplex == FULL_DUPLEX) ?
				"Full Duplex" : "Half Duplex"));
			adapter->link_active = 1;
			adapter->smartspeed = 0;
		}
	} else {
		if (adapter->link_active == 1) {
			adapter->link_speed = 0;
			adapter->link_duplex = 0;
			device_printf(adapter->dev, "Link is Down\n");
			adapter->link_active = 0;
		}
	}
}

/*********************************************************************
 *
 *  This routine disables all traffic on the adapter by issuing a
 *  global reset on the MAC and deallocates TX/RX buffers. 
 *
 **********************************************************************/

static void
em_stop(void *arg)
{
	struct ifnet   *ifp;
	struct adapter * adapter = arg;
	ifp = &adapter->interface_data.ac_if;

	INIT_DEBUGOUT("em_stop: begin");
	em_disable_intr(adapter);
	em_reset_hw(&adapter->hw);
	callout_stop(&adapter->timer);
	callout_stop(&adapter->tx_fifo_timer);
	em_free_transmit_structures(adapter);
	em_free_receive_structures(adapter);

	/* Tell the stack that the interface is no longer active */
	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;
}

/*********************************************************************
 *
 *  Determine hardware revision.
 *
 **********************************************************************/
static void
em_identify_hardware(struct adapter * adapter)
{
	device_t dev = adapter->dev;

	/* Make sure our PCI config space has the necessary stuff set */
	adapter->hw.pci_cmd_word = pci_read_config(dev, PCIR_COMMAND, 2);
	if (!((adapter->hw.pci_cmd_word & PCIM_CMD_BUSMASTEREN) &&
	      (adapter->hw.pci_cmd_word & PCIM_CMD_MEMEN))) {
		device_printf(dev, "Memory Access and/or Bus Master bits were not set!\n");
		adapter->hw.pci_cmd_word |= 
		(PCIM_CMD_BUSMASTEREN | PCIM_CMD_MEMEN);
		pci_write_config(dev, PCIR_COMMAND, adapter->hw.pci_cmd_word, 2);
	}

	/* Save off the information about this board */
	adapter->hw.vendor_id = pci_get_vendor(dev);
	adapter->hw.device_id = pci_get_device(dev);
	adapter->hw.revision_id = pci_get_revid(dev);
	adapter->hw.subsystem_vendor_id = pci_get_subvendor(dev);
	adapter->hw.subsystem_id = pci_get_subdevice(dev);

	/* Identify the MAC */
	if (em_set_mac_type(&adapter->hw))
		device_printf(dev, "Unknown MAC Type\n");

	if (adapter->hw.mac_type == em_82541 ||
	    adapter->hw.mac_type == em_82541_rev_2 ||
	    adapter->hw.mac_type == em_82547 ||
	    adapter->hw.mac_type == em_82547_rev_2)
		adapter->hw.phy_init_script = TRUE;
}

/*********************************************************************
 *
 *  Initialize the hardware to a configuration as specified by the
 *  adapter structure. The controller is reset, the EEPROM is
 *  verified, the MAC address is set, then the shared initialization
 *  routines are called.
 *
 **********************************************************************/
static int
em_hardware_init(struct adapter *adapter)
{
	INIT_DEBUGOUT("em_hardware_init: begin");
	/* Issue a global reset */
	em_reset_hw(&adapter->hw);

	/* When hardware is reset, fifo_head is also reset */
	adapter->tx_fifo_head = 0;

	/* Make sure we have a good EEPROM before we read from it */
	if (em_validate_eeprom_checksum(&adapter->hw) < 0) {
		device_printf(adapter->dev, "The EEPROM Checksum Is Not Valid\n");
		return(EIO);
	}

	if (em_read_part_num(&adapter->hw, &(adapter->part_num)) < 0) {
		device_printf(adapter->dev, "EEPROM read error while reading part number\n");
		return(EIO);
	}

	if (em_init_hw(&adapter->hw) < 0) {
		device_printf(adapter->dev, "Hardware Initialization Failed");
		return(EIO);
	}

	em_check_for_link(&adapter->hw);
	if (E1000_READ_REG(&adapter->hw, STATUS) & E1000_STATUS_LU)
		adapter->link_active = 1;
	else
		adapter->link_active = 0;

	if (adapter->link_active) {
		em_get_speed_and_duplex(&adapter->hw, 
					&adapter->link_speed, 
					&adapter->link_duplex);
	} else {
		adapter->link_speed = 0;
		adapter->link_duplex = 0;
	}

	return(0);
}

/*********************************************************************
 *
 *  Setup networking device structure and register an interface.
 *
 **********************************************************************/
static void
em_setup_interface(device_t dev, struct adapter *adapter)
{
	struct ifnet   *ifp;
	INIT_DEBUGOUT("em_setup_interface: begin");

	ifp = &adapter->interface_data.ac_if;
	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_mtu = ETHERMTU;
	ifp->if_baudrate = 1000000000;
	ifp->if_init =  em_init;
	ifp->if_softc = adapter;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = em_ioctl;
	ifp->if_start = em_start;
#ifdef DEVICE_POLLING
	ifp->if_poll = em_poll;
#endif
	ifp->if_watchdog = em_watchdog;
	ifq_set_maxlen(&ifp->if_snd, adapter->num_tx_desc - 1);
	ifq_set_ready(&ifp->if_snd);

	if (adapter->hw.mac_type >= em_82543)
		ifp->if_capabilities |= IFCAP_HWCSUM;

	ifp->if_capenable = ifp->if_capabilities;

	ether_ifattach(ifp, adapter->hw.mac_addr);

	/*
	 * Tell the upper layer(s) we support long frames.
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);
        ifp->if_capabilities |= IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_MTU;

	/* 
	 * Specify the media types supported by this adapter and register
	 * callbacks to update media and link information
	 */
	ifmedia_init(&adapter->media, IFM_IMASK, em_media_change,
		     em_media_status);
	if (adapter->hw.media_type == em_media_type_fiber) {
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_1000_SX | IFM_FDX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_1000_SX, 
			    0, NULL);
	} else {
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_10_T, 0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_10_T | IFM_FDX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_100_TX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_100_TX | IFM_FDX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_1000_T | IFM_FDX, 
			    0, NULL);
		ifmedia_add(&adapter->media, IFM_ETHER | IFM_1000_T, 0, NULL);
	}
	ifmedia_add(&adapter->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&adapter->media, IFM_ETHER | IFM_AUTO);
}

/*********************************************************************
 *
 *  Workaround for SmartSpeed on 82541 and 82547 controllers
 *
 **********************************************************************/        
static void
em_smartspeed(struct adapter *adapter)
{
	uint16_t phy_tmp;

	if (adapter->link_active || (adapter->hw.phy_type != em_phy_igp) || 
	    !adapter->hw.autoneg ||
	    !(adapter->hw.autoneg_advertised & ADVERTISE_1000_FULL))
		return;

	if (adapter->smartspeed == 0) {
		/*
		 * If Master/Slave config fault is asserted twice,
		 * we assume back-to-back.
		 */
		em_read_phy_reg(&adapter->hw, PHY_1000T_STATUS, &phy_tmp);
		if (!(phy_tmp & SR_1000T_MS_CONFIG_FAULT))
			return;
		em_read_phy_reg(&adapter->hw, PHY_1000T_STATUS, &phy_tmp);
		if (phy_tmp & SR_1000T_MS_CONFIG_FAULT) {
			em_read_phy_reg(&adapter->hw, PHY_1000T_CTRL,
					&phy_tmp);
			if (phy_tmp & CR_1000T_MS_ENABLE) {
				phy_tmp &= ~CR_1000T_MS_ENABLE;
				em_write_phy_reg(&adapter->hw,
						 PHY_1000T_CTRL, phy_tmp);
				adapter->smartspeed++;
				if (adapter->hw.autoneg &&
				    !em_phy_setup_autoneg(&adapter->hw) &&
				    !em_read_phy_reg(&adapter->hw, PHY_CTRL,
						     &phy_tmp)) {
					phy_tmp |= (MII_CR_AUTO_NEG_EN |  
						    MII_CR_RESTART_AUTO_NEG);
					em_write_phy_reg(&adapter->hw,
							 PHY_CTRL, phy_tmp);
				}
			}
		}
                return;
	} else if (adapter->smartspeed == EM_SMARTSPEED_DOWNSHIFT) {
		/* If still no link, perhaps using 2/3 pair cable */
		em_read_phy_reg(&adapter->hw, PHY_1000T_CTRL, &phy_tmp);
		phy_tmp |= CR_1000T_MS_ENABLE;
		em_write_phy_reg(&adapter->hw, PHY_1000T_CTRL, phy_tmp);
		if (adapter->hw.autoneg &&
		    !em_phy_setup_autoneg(&adapter->hw) &&
		    !em_read_phy_reg(&adapter->hw, PHY_CTRL, &phy_tmp)) {
			phy_tmp |= (MII_CR_AUTO_NEG_EN |
				    MII_CR_RESTART_AUTO_NEG);
			em_write_phy_reg(&adapter->hw, PHY_CTRL, phy_tmp);
		}
	}
	/* Restart process after EM_SMARTSPEED_MAX iterations */
	if (adapter->smartspeed++ == EM_SMARTSPEED_MAX)
		adapter->smartspeed = 0;
}

/*
 * Manage DMA'able memory.
 */
static void
em_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{ 
	if (error)
		return;
	*(bus_addr_t*) arg = segs->ds_addr;
}

static int
em_dma_malloc(struct adapter *adapter, bus_size_t size,
	      struct em_dma_alloc *dma, int mapflags)
{
	int r;
	device_t dev = adapter->dev;

	r = bus_dma_tag_create(NULL,                    /* parent */
			       PAGE_SIZE, 0,            /* alignment, bounds */
			       BUS_SPACE_MAXADDR,       /* lowaddr */
			       BUS_SPACE_MAXADDR,       /* highaddr */
			       NULL, NULL,              /* filter, filterarg */
			       size,                    /* maxsize */
			       1,                       /* nsegments */
			       size,                    /* maxsegsize */
			       BUS_DMA_ALLOCNOW,        /* flags */
			       &dma->dma_tag);
	if (r != 0) {
		device_printf(dev, "em_dma_malloc: bus_dma_tag_create failed; "
			      "error %u\n", r);
		goto fail_0;
	}

	r = bus_dmamem_alloc(dma->dma_tag, (void**) &dma->dma_vaddr,
			     BUS_DMA_NOWAIT, &dma->dma_map);
	if (r != 0) {
		device_printf(dev, "em_dma_malloc: bus_dmammem_alloc failed; "
			      "size %llu, error %d\n", (uintmax_t)size, r);
		goto fail_2;
	}

	r = bus_dmamap_load(dma->dma_tag, dma->dma_map, dma->dma_vaddr,
			    size,
			    em_dmamap_cb,
			    &dma->dma_paddr,
			    mapflags | BUS_DMA_NOWAIT);
	if (r != 0) {
		device_printf(dev, "em_dma_malloc: bus_dmamap_load failed; "
			      "error %u\n", r);
		goto fail_3;
	}

	dma->dma_size = size;
	return(0);

fail_3:
	bus_dmamap_unload(dma->dma_tag, dma->dma_map);
fail_2:
	bus_dmamem_free(dma->dma_tag, dma->dma_vaddr, dma->dma_map);
	bus_dma_tag_destroy(dma->dma_tag);
fail_0:
	dma->dma_map = NULL;
	dma->dma_tag = NULL;
	return(r);
}

static void
em_dma_free(struct adapter *adapter, struct em_dma_alloc *dma)
{
	bus_dmamap_unload(dma->dma_tag, dma->dma_map);
	bus_dmamem_free(dma->dma_tag, dma->dma_vaddr, dma->dma_map);
	bus_dma_tag_destroy(dma->dma_tag);
}

/*********************************************************************
 *
 *  Allocate memory for tx_buffer structures. The tx_buffer stores all 
 *  the information needed to transmit a packet on the wire. 
 *
 **********************************************************************/
static int
em_allocate_transmit_structures(struct adapter * adapter)
{
	adapter->tx_buffer_area = malloc(sizeof(struct em_buffer) *
	    adapter->num_tx_desc, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->tx_buffer_area == NULL) {
		device_printf(adapter->dev, "Unable to allocate tx_buffer memory\n");
		return(ENOMEM);
	}

	return(0);
}

/*********************************************************************
 *
 *  Allocate and initialize transmit structures. 
 *
 **********************************************************************/
static int
em_setup_transmit_structures(struct adapter * adapter)
{
	/*
	 * Setup DMA descriptor areas.
	 */
	if (bus_dma_tag_create(NULL,                    /* parent */
			       1, 0,			/* alignment, bounds */
			       BUS_SPACE_MAXADDR,       /* lowaddr */ 
			       BUS_SPACE_MAXADDR,       /* highaddr */
			       NULL, NULL,              /* filter, filterarg */
			       MCLBYTES * 8,            /* maxsize */
			       EM_MAX_SCATTER,          /* nsegments */
			       MCLBYTES * 8,            /* maxsegsize */
			       BUS_DMA_ALLOCNOW,        /* flags */ 
			       &adapter->txtag)) {
		device_printf(adapter->dev, "Unable to allocate TX DMA tag\n");
		return(ENOMEM);
	}

	if (em_allocate_transmit_structures(adapter))
		return(ENOMEM);

        bzero((void *) adapter->tx_desc_base,
              (sizeof(struct em_tx_desc)) * adapter->num_tx_desc);

        adapter->next_avail_tx_desc = 0;
	adapter->oldest_used_tx_desc = 0;

	/* Set number of descriptors available */
	adapter->num_tx_desc_avail = adapter->num_tx_desc;

	/* Set checksum context */
	adapter->active_checksum_context = OFFLOAD_NONE;

	return(0);
}

/*********************************************************************
 *
 *  Enable transmit unit.
 *
 **********************************************************************/
static void
em_initialize_transmit_unit(struct adapter * adapter)
{
	uint32_t reg_tctl;
	uint32_t reg_tipg = 0;
	uint64_t bus_addr;

	INIT_DEBUGOUT("em_initialize_transmit_unit: begin");

	/* Setup the Base and Length of the Tx Descriptor Ring */
	bus_addr = adapter->txdma.dma_paddr;
	E1000_WRITE_REG(&adapter->hw, TDBAL, (uint32_t)bus_addr);
	E1000_WRITE_REG(&adapter->hw, TDBAH, (uint32_t)(bus_addr >> 32));
	E1000_WRITE_REG(&adapter->hw, TDLEN, 
			adapter->num_tx_desc * sizeof(struct em_tx_desc));

	/* Setup the HW Tx Head and Tail descriptor pointers */
	E1000_WRITE_REG(&adapter->hw, TDH, 0);
	E1000_WRITE_REG(&adapter->hw, TDT, 0);

	HW_DEBUGOUT2("Base = %x, Length = %x\n", 
		     E1000_READ_REG(&adapter->hw, TDBAL),
		     E1000_READ_REG(&adapter->hw, TDLEN));

	/* Set the default values for the Tx Inter Packet Gap timer */
	switch (adapter->hw.mac_type) {
	case em_82542_rev2_0:
	case em_82542_rev2_1:
		reg_tipg = DEFAULT_82542_TIPG_IPGT;
		reg_tipg |= DEFAULT_82542_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
		reg_tipg |= DEFAULT_82542_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
		break;
	default:
		if (adapter->hw.media_type == em_media_type_fiber)
			reg_tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
		else
			reg_tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
		reg_tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
		reg_tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
	}

	E1000_WRITE_REG(&adapter->hw, TIPG, reg_tipg);
	E1000_WRITE_REG(&adapter->hw, TIDV, adapter->tx_int_delay.value);
	if (adapter->hw.mac_type >= em_82540)
		E1000_WRITE_REG(&adapter->hw, TADV,
				adapter->tx_abs_int_delay.value);

	/* Program the Transmit Control Register */
	reg_tctl = E1000_TCTL_PSP | E1000_TCTL_EN |
		   (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);
	if (adapter->link_duplex == 1)
		reg_tctl |= E1000_FDX_COLLISION_DISTANCE << E1000_COLD_SHIFT;
	else
		reg_tctl |= E1000_HDX_COLLISION_DISTANCE << E1000_COLD_SHIFT;
	E1000_WRITE_REG(&adapter->hw, TCTL, reg_tctl);

	/* Setup Transmit Descriptor Settings for this adapter */   
	adapter->txd_cmd = E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;

	if (adapter->tx_int_delay.value > 0)
		adapter->txd_cmd |= E1000_TXD_CMD_IDE;
}

/*********************************************************************
 *
 *  Free all transmit related data structures.
 *
 **********************************************************************/
static void
em_free_transmit_structures(struct adapter * adapter)
{
	struct em_buffer *tx_buffer;
	int i;

	INIT_DEBUGOUT("free_transmit_structures: begin");

	if (adapter->tx_buffer_area != NULL) {
		tx_buffer = adapter->tx_buffer_area;
		for (i = 0; i < adapter->num_tx_desc; i++, tx_buffer++) {
			if (tx_buffer->m_head != NULL) {
				bus_dmamap_unload(adapter->txtag, tx_buffer->map);
				bus_dmamap_destroy(adapter->txtag, tx_buffer->map);
				m_freem(tx_buffer->m_head);
			}
			tx_buffer->m_head = NULL;
		}
	}
	if (adapter->tx_buffer_area != NULL) {
		free(adapter->tx_buffer_area, M_DEVBUF);
		adapter->tx_buffer_area = NULL;
	}
	if (adapter->txtag != NULL) {
		bus_dma_tag_destroy(adapter->txtag);
		adapter->txtag = NULL;
	}
}

/*********************************************************************
 *
 *  The offload context needs to be set when we transfer the first
 *  packet of a particular protocol (TCP/UDP). We change the
 *  context only if the protocol type changes.
 *
 **********************************************************************/
static void
em_transmit_checksum_setup(struct adapter * adapter,
			   struct mbuf *mp,
			   uint32_t *txd_upper,
			   uint32_t *txd_lower) 
{
	struct em_context_desc *TXD;
	struct em_buffer *tx_buffer;
	int curr_txd;

	if (mp->m_pkthdr.csum_flags) {
		if (mp->m_pkthdr.csum_flags & CSUM_TCP) {
			*txd_upper = E1000_TXD_POPTS_TXSM << 8;
			*txd_lower = E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D;
			if (adapter->active_checksum_context == OFFLOAD_TCP_IP)
				return;
			else
				adapter->active_checksum_context = OFFLOAD_TCP_IP;
		} else if (mp->m_pkthdr.csum_flags & CSUM_UDP) {
			*txd_upper = E1000_TXD_POPTS_TXSM << 8;
			*txd_lower = E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D;
			if (adapter->active_checksum_context == OFFLOAD_UDP_IP)
				return;
			else
				adapter->active_checksum_context = OFFLOAD_UDP_IP;
		} else {
			*txd_upper = 0;
			*txd_lower = 0;
			return;
		}
	} else {
		*txd_upper = 0;
		*txd_lower = 0;
		return;
	}

	/* If we reach this point, the checksum offload context
	 * needs to be reset.
	 */
	curr_txd = adapter->next_avail_tx_desc;
	tx_buffer = &adapter->tx_buffer_area[curr_txd];
	TXD = (struct em_context_desc *) &adapter->tx_desc_base[curr_txd];

	TXD->lower_setup.ip_fields.ipcss = ETHER_HDR_LEN;
	TXD->lower_setup.ip_fields.ipcso =
	    ETHER_HDR_LEN + offsetof(struct ip, ip_sum);
	TXD->lower_setup.ip_fields.ipcse =
	    htole16(ETHER_HDR_LEN + sizeof(struct ip) - 1);

	TXD->upper_setup.tcp_fields.tucss = 
	    ETHER_HDR_LEN + sizeof(struct ip);
	TXD->upper_setup.tcp_fields.tucse = htole16(0);

	if (adapter->active_checksum_context == OFFLOAD_TCP_IP) {
		TXD->upper_setup.tcp_fields.tucso =
		    ETHER_HDR_LEN + sizeof(struct ip) +
		    offsetof(struct tcphdr, th_sum);
	} else if (adapter->active_checksum_context == OFFLOAD_UDP_IP) {
		TXD->upper_setup.tcp_fields.tucso =
			ETHER_HDR_LEN + sizeof(struct ip) +
			offsetof(struct udphdr, uh_sum);
	}

	TXD->tcp_seg_setup.data = htole32(0);
	TXD->cmd_and_length = htole32(adapter->txd_cmd | E1000_TXD_CMD_DEXT);

	tx_buffer->m_head = NULL;

	if (++curr_txd == adapter->num_tx_desc)
		curr_txd = 0;

	adapter->num_tx_desc_avail--;
	adapter->next_avail_tx_desc = curr_txd;
}

/**********************************************************************
 *
 *  Examine each tx_buffer in the used queue. If the hardware is done
 *  processing the packet then free associated resources. The
 *  tx_buffer is put back on the free queue.
 *
 **********************************************************************/

static void
em_clean_transmit_interrupts(struct adapter *adapter)
{
	int i, num_avail;
	struct em_buffer *tx_buffer;
	struct em_tx_desc *tx_desc;
	struct ifnet *ifp = &adapter->interface_data.ac_if;

	if (adapter->num_tx_desc_avail == adapter->num_tx_desc)
		return;

#ifdef DBG_STATS
	adapter->clean_tx_interrupts++;
#endif
	num_avail = adapter->num_tx_desc_avail;	
	i = adapter->oldest_used_tx_desc;

	tx_buffer = &adapter->tx_buffer_area[i];
	tx_desc = &adapter->tx_desc_base[i];

	bus_dmamap_sync(adapter->txdma.dma_tag, adapter->txdma.dma_map,
			BUS_DMASYNC_POSTREAD);

	while(tx_desc->upper.fields.status & E1000_TXD_STAT_DD) {
		tx_desc->upper.data = 0;
		num_avail++;                        

		if (tx_buffer->m_head) {
			ifp->if_opackets++;
			bus_dmamap_sync(adapter->txtag, tx_buffer->map,
					BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(adapter->txtag, tx_buffer->map);
			bus_dmamap_destroy(adapter->txtag, tx_buffer->map);

			m_freem(tx_buffer->m_head);
			tx_buffer->m_head = NULL;
		}

		if (++i == adapter->num_tx_desc)
			i = 0;

		tx_buffer = &adapter->tx_buffer_area[i];
		tx_desc = &adapter->tx_desc_base[i];
	}

	bus_dmamap_sync(adapter->txdma.dma_tag, adapter->txdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	adapter->oldest_used_tx_desc = i;

	/*
	 * If we have enough room, clear IFF_OACTIVE to tell the stack
	 * that it is OK to send packets.
	 * If there are no pending descriptors, clear the timeout. Otherwise,
	 * if some descriptors have been freed, restart the timeout.
	 */
	if (num_avail > EM_TX_CLEANUP_THRESHOLD) {
		ifp->if_flags &= ~IFF_OACTIVE;
		if (num_avail == adapter->num_tx_desc)
			ifp->if_timer = 0;
		else if (num_avail == adapter->num_tx_desc_avail)
			ifp->if_timer = EM_TX_TIMEOUT;
	}
	adapter->num_tx_desc_avail = num_avail;
}

/*********************************************************************
 *
 *  Get a buffer from system mbuf buffer pool.
 *
 **********************************************************************/
static int
em_get_buf(int i, struct adapter *adapter, struct mbuf *nmp, int how)
{
	struct mbuf *mp = nmp;
	struct em_buffer *rx_buffer;
	struct ifnet *ifp;
	bus_addr_t paddr;
	int error;

	ifp = &adapter->interface_data.ac_if;

	if (mp == NULL) {
		mp = m_getcl(how, MT_DATA, M_PKTHDR);
		if (mp == NULL) {
			adapter->mbuf_cluster_failed++;
			return(ENOBUFS);
		}
		mp->m_len = mp->m_pkthdr.len = MCLBYTES;
	} else {
		mp->m_len = mp->m_pkthdr.len = MCLBYTES;
		mp->m_data = mp->m_ext.ext_buf;
		mp->m_next = NULL;
	}
	if (ifp->if_mtu <= ETHERMTU)
		m_adj(mp, ETHER_ALIGN);

	rx_buffer = &adapter->rx_buffer_area[i];

	/*
	 * Using memory from the mbuf cluster pool, invoke the
	 * bus_dma machinery to arrange the memory mapping.
	 */
	error = bus_dmamap_load(adapter->rxtag, rx_buffer->map,
				mtod(mp, void *), mp->m_len,
				em_dmamap_cb, &paddr, 0);
	if (error) {
		m_free(mp);
		return(error);
	}
	rx_buffer->m_head = mp;
	adapter->rx_desc_base[i].buffer_addr = htole64(paddr);
	bus_dmamap_sync(adapter->rxtag, rx_buffer->map, BUS_DMASYNC_PREREAD);

	return(0);
}

/*********************************************************************
 *
 *  Allocate memory for rx_buffer structures. Since we use one 
 *  rx_buffer per received packet, the maximum number of rx_buffer's 
 *  that we'll need is equal to the number of receive descriptors 
 *  that we've allocated.
 *
 **********************************************************************/
static int
em_allocate_receive_structures(struct adapter *adapter)
{
	int i, error, size;
	struct em_buffer *rx_buffer;

	size = adapter->num_rx_desc * sizeof(struct em_buffer);
	adapter->rx_buffer_area = malloc(size, M_DEVBUF, M_WAITOK | M_ZERO);

	error = bus_dma_tag_create(NULL,		/* parent */
				   1, 0,		/* alignment, bounds */
				   BUS_SPACE_MAXADDR,	/* lowaddr */
				   BUS_SPACE_MAXADDR,	/* highaddr */
				   NULL, NULL,		/* filter, filterarg */
				   MCLBYTES,		/* maxsize */
				   1,			/* nsegments */
				   MCLBYTES,		/* maxsegsize */
				   BUS_DMA_ALLOCNOW,	/* flags */
				   &adapter->rxtag);
	if (error != 0) {
		device_printf(adapter->dev, "em_allocate_receive_structures: "
			      "bus_dma_tag_create failed; error %u\n", error);
		goto fail_0;
	}
 
	rx_buffer = adapter->rx_buffer_area;
	for (i = 0; i < adapter->num_rx_desc; i++, rx_buffer++) {
		error = bus_dmamap_create(adapter->rxtag, BUS_DMA_NOWAIT,
					  &rx_buffer->map);
		if (error != 0) {
			device_printf(adapter->dev,
				      "em_allocate_receive_structures: "
				      "bus_dmamap_create failed; error %u\n",
				      error);
			goto fail_1;
		}
	}

	for (i = 0; i < adapter->num_rx_desc; i++) {
		error = em_get_buf(i, adapter, NULL, MB_WAIT);
		if (error != 0) {
			adapter->rx_buffer_area[i].m_head = NULL;
			adapter->rx_desc_base[i].buffer_addr = 0;
			return(error);
		}
	}

	bus_dmamap_sync(adapter->rxdma.dma_tag, adapter->rxdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	return(0);

fail_1:
	bus_dma_tag_destroy(adapter->rxtag);
fail_0:
	adapter->rxtag = NULL;
	free(adapter->rx_buffer_area, M_DEVBUF);
	adapter->rx_buffer_area = NULL;
	return(error);
}

/*********************************************************************
 *
 *  Allocate and initialize receive structures.
 *  
 **********************************************************************/
static int
em_setup_receive_structures(struct adapter *adapter)
{
	bzero((void *) adapter->rx_desc_base,
	      (sizeof(struct em_rx_desc)) * adapter->num_rx_desc);

	if (em_allocate_receive_structures(adapter))
		return(ENOMEM);

	/* Setup our descriptor pointers */
	adapter->next_rx_desc_to_check = 0;
	return(0);
}

/*********************************************************************
 *
 *  Enable receive unit.
 *  
 **********************************************************************/
static void
em_initialize_receive_unit(struct adapter *adapter)
{
	uint32_t reg_rctl;
	uint32_t reg_rxcsum;
	struct ifnet *ifp;
	uint64_t bus_addr;
 
	INIT_DEBUGOUT("em_initialize_receive_unit: begin");

	ifp = &adapter->interface_data.ac_if;

	/* Make sure receives are disabled while setting up the descriptor ring */
	E1000_WRITE_REG(&adapter->hw, RCTL, 0);

	/* Set the Receive Delay Timer Register */
	E1000_WRITE_REG(&adapter->hw, RDTR, 
			adapter->rx_int_delay.value | E1000_RDT_FPDB);

	if(adapter->hw.mac_type >= em_82540) {
		E1000_WRITE_REG(&adapter->hw, RADV,
				adapter->rx_abs_int_delay.value);

		/* Set the interrupt throttling rate in 256ns increments */  
		if (em_int_throttle_ceil) {
			E1000_WRITE_REG(&adapter->hw, ITR,
				1000000000 / 256 / em_int_throttle_ceil);
		} else {
			E1000_WRITE_REG(&adapter->hw, ITR, 0);
		}
	}

	/* Setup the Base and Length of the Rx Descriptor Ring */
	bus_addr = adapter->rxdma.dma_paddr;
	E1000_WRITE_REG(&adapter->hw, RDBAL, (uint32_t)bus_addr);
	E1000_WRITE_REG(&adapter->hw, RDBAH, (uint32_t)(bus_addr >> 32));
	E1000_WRITE_REG(&adapter->hw, RDLEN, adapter->num_rx_desc *
			sizeof(struct em_rx_desc));

	/* Setup the HW Rx Head and Tail Descriptor Pointers */
	E1000_WRITE_REG(&adapter->hw, RDH, 0);
	E1000_WRITE_REG(&adapter->hw, RDT, adapter->num_rx_desc - 1);

	/* Setup the Receive Control Register */
	reg_rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO |
		   E1000_RCTL_RDMTS_HALF |
		   (adapter->hw.mc_filter_type << E1000_RCTL_MO_SHIFT);

	if (adapter->hw.tbi_compatibility_on == TRUE)
		reg_rctl |= E1000_RCTL_SBP;

	switch (adapter->rx_buffer_len) {
	default:
	case EM_RXBUFFER_2048:
		reg_rctl |= E1000_RCTL_SZ_2048;
		break;
	case EM_RXBUFFER_4096:
		reg_rctl |= E1000_RCTL_SZ_4096 | E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;            
	case EM_RXBUFFER_8192:
		reg_rctl |= E1000_RCTL_SZ_8192 | E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;
	case EM_RXBUFFER_16384:
		reg_rctl |= E1000_RCTL_SZ_16384 | E1000_RCTL_BSEX | E1000_RCTL_LPE;
		break;
	}

	if (ifp->if_mtu > ETHERMTU)
		reg_rctl |= E1000_RCTL_LPE;

	/* Enable 82543 Receive Checksum Offload for TCP and UDP */
	if ((adapter->hw.mac_type >= em_82543) && 
	    (ifp->if_capenable & IFCAP_RXCSUM)) {
		reg_rxcsum = E1000_READ_REG(&adapter->hw, RXCSUM);
		reg_rxcsum |= (E1000_RXCSUM_IPOFL | E1000_RXCSUM_TUOFL);
		E1000_WRITE_REG(&adapter->hw, RXCSUM, reg_rxcsum);
	}

	/* Enable Receives */
	E1000_WRITE_REG(&adapter->hw, RCTL, reg_rctl);	
}

/*********************************************************************
 *
 *  Free receive related data structures.
 *
 **********************************************************************/
static void
em_free_receive_structures(struct adapter *adapter)
{
	struct em_buffer *rx_buffer;
	int i;

	INIT_DEBUGOUT("free_receive_structures: begin");

	if (adapter->rx_buffer_area != NULL) {
		rx_buffer = adapter->rx_buffer_area;
		for (i = 0; i < adapter->num_rx_desc; i++, rx_buffer++) {
			if (rx_buffer->map != NULL) {
				bus_dmamap_unload(adapter->rxtag, rx_buffer->map);
				bus_dmamap_destroy(adapter->rxtag, rx_buffer->map);
			}
			if (rx_buffer->m_head != NULL)
				m_freem(rx_buffer->m_head);
			rx_buffer->m_head = NULL;
		}
	}
	if (adapter->rx_buffer_area != NULL) {
		free(adapter->rx_buffer_area, M_DEVBUF);
		adapter->rx_buffer_area = NULL;
	}
	if (adapter->rxtag != NULL) {
		bus_dma_tag_destroy(adapter->rxtag);
		adapter->rxtag = NULL;
	}
}

/*********************************************************************
 *
 *  This routine executes in interrupt context. It replenishes
 *  the mbufs in the descriptor and sends data which has been
 *  dma'ed into host memory to upper layer.
 *
 *  We loop at most count times if count is > 0, or until done if
 *  count < 0.
 *
 *********************************************************************/
static void
em_process_receive_interrupts(struct adapter *adapter, int count)
{
	struct ifnet *ifp;
	struct mbuf *mp;
	uint8_t accept_frame = 0;
	uint8_t eop = 0;
	uint16_t len, desc_len, prev_len_adj;
	int i;

	/* Pointer to the receive descriptor being examined. */
	struct em_rx_desc *current_desc;

	ifp = &adapter->interface_data.ac_if;
	i = adapter->next_rx_desc_to_check;
	current_desc = &adapter->rx_desc_base[i];

	bus_dmamap_sync(adapter->rxdma.dma_tag, adapter->rxdma.dma_map,
			BUS_DMASYNC_POSTREAD);

	if (!((current_desc->status) & E1000_RXD_STAT_DD)) {
#ifdef DBG_STATS
		adapter->no_pkts_avail++;
#endif
		return;
	}
	while ((current_desc->status & E1000_RXD_STAT_DD) && (count != 0)) {
		mp = adapter->rx_buffer_area[i].m_head;
		bus_dmamap_sync(adapter->rxtag, adapter->rx_buffer_area[i].map,
				BUS_DMASYNC_POSTREAD);

		accept_frame = 1;
		prev_len_adj = 0;
		desc_len = le16toh(current_desc->length);
		if (current_desc->status & E1000_RXD_STAT_EOP) {
			count--;
			eop = 1;
			if (desc_len < ETHER_CRC_LEN) {
				len = 0;
				prev_len_adj = ETHER_CRC_LEN - desc_len;
			}
			else {
				len = desc_len - ETHER_CRC_LEN;
			}
		} else {
			eop = 0;
			len = desc_len;
		}

		if (current_desc->errors & E1000_RXD_ERR_FRAME_ERR_MASK) {
			uint8_t last_byte;
			uint32_t pkt_len = desc_len;

			if (adapter->fmp != NULL)
				pkt_len += adapter->fmp->m_pkthdr.len; 

			last_byte = *(mtod(mp, caddr_t) + desc_len - 1);

			if (TBI_ACCEPT(&adapter->hw, current_desc->status, 
				       current_desc->errors, 
				       pkt_len, last_byte)) {
				em_tbi_adjust_stats(&adapter->hw, 
						    &adapter->stats, 
						    pkt_len, 
						    adapter->hw.mac_addr);
				if (len > 0)
					len--;
			}
			else {
				accept_frame = 0;
			}
		}

		if (accept_frame) {
			if (em_get_buf(i, adapter, NULL, MB_DONTWAIT) == ENOBUFS) {
				adapter->dropped_pkts++;
				em_get_buf(i, adapter, mp, MB_DONTWAIT);
				if (adapter->fmp != NULL) 
					m_freem(adapter->fmp);
				adapter->fmp = NULL;
				adapter->lmp = NULL;
				break;
			}

			/* Assign correct length to the current fragment */
			mp->m_len = len;

			if (adapter->fmp == NULL) {
				mp->m_pkthdr.len = len;
				adapter->fmp = mp;	 /* Store the first mbuf */
				adapter->lmp = mp;
			} else {
				/* Chain mbuf's together */
				/* 
				 * Adjust length of previous mbuf in chain if we 
				 * received less than 4 bytes in the last descriptor.
				 */
				if (prev_len_adj > 0) {
					adapter->lmp->m_len -= prev_len_adj;
					adapter->fmp->m_pkthdr.len -= prev_len_adj;
				}
				adapter->lmp->m_next = mp;
				adapter->lmp = adapter->lmp->m_next;
				adapter->fmp->m_pkthdr.len += len;
			}

			if (eop) {
				adapter->fmp->m_pkthdr.rcvif = ifp;
				ifp->if_ipackets++;

				em_receive_checksum(adapter, current_desc,
						    adapter->fmp);
				if (current_desc->status & E1000_RXD_STAT_VP)
					VLAN_INPUT_TAG(adapter->fmp,
						       (current_desc->special & 
							E1000_RXD_SPC_VLAN_MASK));
				else
					(*ifp->if_input)(ifp, adapter->fmp);
				adapter->fmp = NULL;
				adapter->lmp = NULL;
			}
		} else {
			adapter->dropped_pkts++;
			em_get_buf(i, adapter, mp, MB_DONTWAIT);
			if (adapter->fmp != NULL) 
				m_freem(adapter->fmp);
			adapter->fmp = NULL;
			adapter->lmp = NULL;
		}

		/* Zero out the receive descriptors status  */
		current_desc->status = 0;

		/* Advance the E1000's Receive Queue #0  "Tail Pointer". */
		E1000_WRITE_REG(&adapter->hw, RDT, i);

		/* Advance our pointers to the next descriptor */
		if (++i == adapter->num_rx_desc) {
			i = 0;
			current_desc = adapter->rx_desc_base;
		} else
			current_desc++;
	}

	bus_dmamap_sync(adapter->rxdma.dma_tag, adapter->rxdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	adapter->next_rx_desc_to_check = i;
}

/*********************************************************************
 *
 *  Verify that the hardware indicated that the checksum is valid. 
 *  Inform the stack about the status of checksum so that stack
 *  doesn't spend time verifying the checksum.
 *
 *********************************************************************/
static void
em_receive_checksum(struct adapter *adapter,
		    struct em_rx_desc *rx_desc,
		    struct mbuf *mp)
{
	/* 82543 or newer only */
	if ((adapter->hw.mac_type < em_82543) ||
	    /* Ignore Checksum bit is set */
	    (rx_desc->status & E1000_RXD_STAT_IXSM)) {
		mp->m_pkthdr.csum_flags = 0;
		return;
	}

	if (rx_desc->status & E1000_RXD_STAT_IPCS) {
		/* Did it pass? */
		if (!(rx_desc->errors & E1000_RXD_ERR_IPE)) {
			/* IP Checksum Good */
			mp->m_pkthdr.csum_flags = CSUM_IP_CHECKED;
			mp->m_pkthdr.csum_flags |= CSUM_IP_VALID;
		} else {
			mp->m_pkthdr.csum_flags = 0;
		}
	}

	if (rx_desc->status & E1000_RXD_STAT_TCPCS) {
		/* Did it pass? */        
		if (!(rx_desc->errors & E1000_RXD_ERR_TCPE)) {
			mp->m_pkthdr.csum_flags |= 
			(CSUM_DATA_VALID | CSUM_PSEUDO_HDR);
			mp->m_pkthdr.csum_data = htons(0xffff);
		}
	}
}


static void 
em_enable_vlans(struct adapter *adapter)
{
	uint32_t ctrl;

	E1000_WRITE_REG(&adapter->hw, VET, ETHERTYPE_VLAN);

	ctrl = E1000_READ_REG(&adapter->hw, CTRL);
	ctrl |= E1000_CTRL_VME; 
	E1000_WRITE_REG(&adapter->hw, CTRL, ctrl);
}

/*
 * note: we must call bus_enable_intr() prior to enabling the hardware
 * interrupt and bus_disable_intr() after disabling the hardware interrupt
 * in order to avoid handler execution races from scheduled interrupt
 * threads.
 */
static void
em_enable_intr(struct adapter *adapter)
{
	struct ifnet *ifp = &adapter->interface_data.ac_if;
	
	if ((ifp->if_flags & IFF_POLLING) == 0) {
		lwkt_serialize_handler_enable(&adapter->serializer);
		E1000_WRITE_REG(&adapter->hw, IMS, (IMS_ENABLE_MASK));
	}
}

static void
em_disable_intr(struct adapter *adapter)
{
	/*
	 * The first version of 82542 had an errata where when link was
	 * forced it would stay up even up even if the cable was disconnected.
	 * Sequence errors were used to detect the disconnect and then the
	 * driver would unforce the link.  This code in the in the ISR.  For
	 * this to work correctly the Sequence error interrupt had to be
	 * enabled all the time.
	 */
	if (adapter->hw.mac_type == em_82542_rev2_0) {
		E1000_WRITE_REG(&adapter->hw, IMC,
				(0xffffffff & ~E1000_IMC_RXSEQ));
	} else {
		E1000_WRITE_REG(&adapter->hw, IMC, 0xffffffff);
	}

	lwkt_serialize_handler_disable(&adapter->serializer);
}

static int
em_is_valid_ether_addr(uint8_t *addr)
{
	char zero_addr[6] = { 0, 0, 0, 0, 0, 0 };

	if ((addr[0] & 1) || (!bcmp(addr, zero_addr, ETHER_ADDR_LEN)))
		return(FALSE);
	else
		return(TRUE);
}

void 
em_write_pci_cfg(struct em_hw *hw, uint32_t reg, uint16_t *value)
{
	pci_write_config(((struct em_osdep *)hw->back)->dev, reg, *value, 2);
}

void 
em_read_pci_cfg(struct em_hw *hw, uint32_t reg, uint16_t *value)
{
	*value = pci_read_config(((struct em_osdep *)hw->back)->dev, reg, 2);
}

void
em_pci_set_mwi(struct em_hw *hw)
{
	pci_write_config(((struct em_osdep *)hw->back)->dev, PCIR_COMMAND,
			 (hw->pci_cmd_word | CMD_MEM_WRT_INVALIDATE), 2);
}

void
em_pci_clear_mwi(struct em_hw *hw)
{
	pci_write_config(((struct em_osdep *)hw->back)->dev, PCIR_COMMAND,
			 (hw->pci_cmd_word & ~CMD_MEM_WRT_INVALIDATE), 2);
}

uint32_t
em_read_reg_io(struct em_hw *hw, uint32_t offset)
{
	bus_space_write_4(hw->reg_io_tag, hw->reg_io_handle, 0, offset);
	return(bus_space_read_4(hw->reg_io_tag, hw->reg_io_handle, 4));
}

void
em_write_reg_io(struct em_hw *hw, uint32_t offset, uint32_t value)
{
	bus_space_write_4(hw->reg_io_tag, hw->reg_io_handle, 0, offset);
	bus_space_write_4(hw->reg_io_tag, hw->reg_io_handle, 4, value);
}

/*********************************************************************
 * 82544 Coexistence issue workaround.
 *    There are 2 issues.
 *	1. Transmit Hang issue.
 *    To detect this issue, following equation can be used...
 *          SIZE[3:0] + ADDR[2:0] = SUM[3:0].
 *          If SUM[3:0] is in between 1 to 4, we will have this issue.
 *
 *	2. DAC issue.
 *    To detect this issue, following equation can be used...
 *          SIZE[3:0] + ADDR[2:0] = SUM[3:0].
 *          If SUM[3:0] is in between 9 to c, we will have this issue.
 *
 *
 *    WORKAROUND:
 *          Make sure we do not have ending address as 1,2,3,4(Hang) or
 *          9,a,b,c (DAC)
 *
*************************************************************************/
static uint32_t
em_fill_descriptors(uint64_t address, uint32_t length, PDESC_ARRAY desc_array)
{
	/* Since issue is sensitive to length and address.*/
	/* Let us first check the address...*/
	uint32_t safe_terminator;
	if (length <= 4) {
		desc_array->descriptor[0].address = address;
		desc_array->descriptor[0].length = length;
		desc_array->elements = 1;
		return(desc_array->elements);
	}
	safe_terminator = (uint32_t)((((uint32_t)address & 0x7) + (length & 0xF)) & 0xF);
	/* if it does not fall between 0x1 to 0x4 and 0x9 to 0xC then return */ 
	if (safe_terminator == 0 ||
	    (safe_terminator > 4 && safe_terminator < 9) || 
	    (safe_terminator > 0xC && safe_terminator <= 0xF)) {
		desc_array->descriptor[0].address = address;
		desc_array->descriptor[0].length = length;
		desc_array->elements = 1;
		return(desc_array->elements);
	}

	desc_array->descriptor[0].address = address;
	desc_array->descriptor[0].length = length - 4;
	desc_array->descriptor[1].address = address + (length - 4);
	desc_array->descriptor[1].length = 4;
	desc_array->elements = 2;
	return(desc_array->elements);
}

/**********************************************************************
 *
 *  Update the board statistics counters. 
 *
 **********************************************************************/
static void
em_update_stats_counters(struct adapter *adapter)
{
	struct ifnet   *ifp;

	if (adapter->hw.media_type == em_media_type_copper ||
	    (E1000_READ_REG(&adapter->hw, STATUS) & E1000_STATUS_LU)) {
		adapter->stats.symerrs += E1000_READ_REG(&adapter->hw, SYMERRS);
		adapter->stats.sec += E1000_READ_REG(&adapter->hw, SEC);
	}
	adapter->stats.crcerrs += E1000_READ_REG(&adapter->hw, CRCERRS);
	adapter->stats.mpc += E1000_READ_REG(&adapter->hw, MPC);
	adapter->stats.scc += E1000_READ_REG(&adapter->hw, SCC);
	adapter->stats.ecol += E1000_READ_REG(&adapter->hw, ECOL);

	adapter->stats.mcc += E1000_READ_REG(&adapter->hw, MCC);
	adapter->stats.latecol += E1000_READ_REG(&adapter->hw, LATECOL);
	adapter->stats.colc += E1000_READ_REG(&adapter->hw, COLC);
	adapter->stats.dc += E1000_READ_REG(&adapter->hw, DC);
	adapter->stats.rlec += E1000_READ_REG(&adapter->hw, RLEC);
	adapter->stats.xonrxc += E1000_READ_REG(&adapter->hw, XONRXC);
	adapter->stats.xontxc += E1000_READ_REG(&adapter->hw, XONTXC);
	adapter->stats.xoffrxc += E1000_READ_REG(&adapter->hw, XOFFRXC);
	adapter->stats.xofftxc += E1000_READ_REG(&adapter->hw, XOFFTXC);
	adapter->stats.fcruc += E1000_READ_REG(&adapter->hw, FCRUC);
	adapter->stats.prc64 += E1000_READ_REG(&adapter->hw, PRC64);
	adapter->stats.prc127 += E1000_READ_REG(&adapter->hw, PRC127);
	adapter->stats.prc255 += E1000_READ_REG(&adapter->hw, PRC255);
	adapter->stats.prc511 += E1000_READ_REG(&adapter->hw, PRC511);
	adapter->stats.prc1023 += E1000_READ_REG(&adapter->hw, PRC1023);
	adapter->stats.prc1522 += E1000_READ_REG(&adapter->hw, PRC1522);
	adapter->stats.gprc += E1000_READ_REG(&adapter->hw, GPRC);
	adapter->stats.bprc += E1000_READ_REG(&adapter->hw, BPRC);
	adapter->stats.mprc += E1000_READ_REG(&adapter->hw, MPRC);
	adapter->stats.gptc += E1000_READ_REG(&adapter->hw, GPTC);

	/* For the 64-bit byte counters the low dword must be read first. */
	/* Both registers clear on the read of the high dword */

	adapter->stats.gorcl += E1000_READ_REG(&adapter->hw, GORCL); 
	adapter->stats.gorch += E1000_READ_REG(&adapter->hw, GORCH);
	adapter->stats.gotcl += E1000_READ_REG(&adapter->hw, GOTCL);
	adapter->stats.gotch += E1000_READ_REG(&adapter->hw, GOTCH);

	adapter->stats.rnbc += E1000_READ_REG(&adapter->hw, RNBC);
	adapter->stats.ruc += E1000_READ_REG(&adapter->hw, RUC);
	adapter->stats.rfc += E1000_READ_REG(&adapter->hw, RFC);
	adapter->stats.roc += E1000_READ_REG(&adapter->hw, ROC);
	adapter->stats.rjc += E1000_READ_REG(&adapter->hw, RJC);

	adapter->stats.torl += E1000_READ_REG(&adapter->hw, TORL);
	adapter->stats.torh += E1000_READ_REG(&adapter->hw, TORH);
	adapter->stats.totl += E1000_READ_REG(&adapter->hw, TOTL);
	adapter->stats.toth += E1000_READ_REG(&adapter->hw, TOTH);

	adapter->stats.tpr += E1000_READ_REG(&adapter->hw, TPR);
	adapter->stats.tpt += E1000_READ_REG(&adapter->hw, TPT);
	adapter->stats.ptc64 += E1000_READ_REG(&adapter->hw, PTC64);
	adapter->stats.ptc127 += E1000_READ_REG(&adapter->hw, PTC127);
	adapter->stats.ptc255 += E1000_READ_REG(&adapter->hw, PTC255);
	adapter->stats.ptc511 += E1000_READ_REG(&adapter->hw, PTC511);
	adapter->stats.ptc1023 += E1000_READ_REG(&adapter->hw, PTC1023);
	adapter->stats.ptc1522 += E1000_READ_REG(&adapter->hw, PTC1522);
	adapter->stats.mptc += E1000_READ_REG(&adapter->hw, MPTC);
	adapter->stats.bptc += E1000_READ_REG(&adapter->hw, BPTC);

	if (adapter->hw.mac_type >= em_82543) {
		adapter->stats.algnerrc += 
		    E1000_READ_REG(&adapter->hw, ALGNERRC);
		adapter->stats.rxerrc += 
		    E1000_READ_REG(&adapter->hw, RXERRC);
		adapter->stats.tncrs += 
		    E1000_READ_REG(&adapter->hw, TNCRS);
		adapter->stats.cexterr += 
		    E1000_READ_REG(&adapter->hw, CEXTERR);
		adapter->stats.tsctc += 
		    E1000_READ_REG(&adapter->hw, TSCTC);
		adapter->stats.tsctfc += 
		    E1000_READ_REG(&adapter->hw, TSCTFC);
	}
	ifp = &adapter->interface_data.ac_if;

	/* Fill out the OS statistics structure */
	ifp->if_ibytes = adapter->stats.gorcl;
	ifp->if_obytes = adapter->stats.gotcl;
	ifp->if_imcasts = adapter->stats.mprc;
	ifp->if_collisions = adapter->stats.colc;

	/* Rx Errors */
	ifp->if_ierrors = adapter->dropped_pkts + adapter->stats.rxerrc +
	    adapter->stats.crcerrs + adapter->stats.algnerrc +
	    adapter->stats.rlec + adapter->stats.mpc + adapter->stats.cexterr;

	/* Tx Errors */
	ifp->if_oerrors = adapter->stats.ecol + adapter->stats.latecol;
}


/**********************************************************************
 *
 *  This routine is called only when em_display_debug_stats is enabled.
 *  This routine provides a way to take a look at important statistics
 *  maintained by the driver and hardware.
 *
 **********************************************************************/
static void
em_print_debug_info(struct adapter *adapter)
{
	device_t dev= adapter->dev;
	uint8_t *hw_addr = adapter->hw.hw_addr;

	device_printf(dev, "Adapter hardware address = %p \n", hw_addr);
	device_printf(dev, "CTRL  = 0x%x\n",
		      E1000_READ_REG(&adapter->hw, CTRL)); 
	device_printf(dev, "RCTL  = 0x%x PS=(0x8402)\n",
		      E1000_READ_REG(&adapter->hw, RCTL)); 
	device_printf(dev, "tx_int_delay = %d, tx_abs_int_delay = %d\n",
		      E1000_READ_REG(&adapter->hw, TIDV),
		      E1000_READ_REG(&adapter->hw, TADV));
	device_printf(dev, "rx_int_delay = %d, rx_abs_int_delay = %d\n",
		      E1000_READ_REG(&adapter->hw, RDTR),
		      E1000_READ_REG(&adapter->hw, RADV));
#ifdef DBG_STATS
	device_printf(dev, "Packets not Avail = %ld\n", adapter->no_pkts_avail);
	device_printf(dev, "CleanTxInterrupts = %ld\n",
		      adapter->clean_tx_interrupts);
#endif
	device_printf(dev, "fifo workaround = %lld, fifo_reset = %lld\n",
		      (long long)adapter->tx_fifo_wrk_cnt,
		      (long long)adapter->tx_fifo_reset_cnt);
	device_printf(dev, "hw tdh = %d, hw tdt = %d\n",
		      E1000_READ_REG(&adapter->hw, TDH),
		      E1000_READ_REG(&adapter->hw, TDT));
	device_printf(dev, "Num Tx descriptors avail = %d\n",
		      adapter->num_tx_desc_avail);
	device_printf(dev, "Tx Descriptors not avail1 = %ld\n",
		      adapter->no_tx_desc_avail1);
	device_printf(dev, "Tx Descriptors not avail2 = %ld\n",
		      adapter->no_tx_desc_avail2);
	device_printf(dev, "Std mbuf failed = %ld\n",
		      adapter->mbuf_alloc_failed);
	device_printf(dev, "Std mbuf cluster failed = %ld\n",
		      adapter->mbuf_cluster_failed);
	device_printf(dev, "Driver dropped packets = %ld\n",
		      adapter->dropped_pkts);
}

static void
em_print_hw_stats(struct adapter *adapter)
{
	device_t dev= adapter->dev;

	device_printf(dev, "Adapter: %p\n", adapter);

	device_printf(dev, "Excessive collisions = %lld\n",
		      (long long)adapter->stats.ecol);
	device_printf(dev, "Symbol errors = %lld\n",
		      (long long)adapter->stats.symerrs);
	device_printf(dev, "Sequence errors = %lld\n",
		      (long long)adapter->stats.sec);
	device_printf(dev, "Defer count = %lld\n",
		      (long long)adapter->stats.dc);

	device_printf(dev, "Missed Packets = %lld\n",
		      (long long)adapter->stats.mpc);
	device_printf(dev, "Receive No Buffers = %lld\n",
		      (long long)adapter->stats.rnbc);
	device_printf(dev, "Receive length errors = %lld\n",
		      (long long)adapter->stats.rlec);
	device_printf(dev, "Receive errors = %lld\n",
		      (long long)adapter->stats.rxerrc);
	device_printf(dev, "Crc errors = %lld\n",
		      (long long)adapter->stats.crcerrs);
	device_printf(dev, "Alignment errors = %lld\n",
		      (long long)adapter->stats.algnerrc);
	device_printf(dev, "Carrier extension errors = %lld\n",
		      (long long)adapter->stats.cexterr);

	device_printf(dev, "XON Rcvd = %lld\n",
		      (long long)adapter->stats.xonrxc);
	device_printf(dev, "XON Xmtd = %lld\n",
		      (long long)adapter->stats.xontxc);
	device_printf(dev, "XOFF Rcvd = %lld\n",
		      (long long)adapter->stats.xoffrxc);
	device_printf(dev, "XOFF Xmtd = %lld\n",
		      (long long)adapter->stats.xofftxc);

	device_printf(dev, "Good Packets Rcvd = %lld\n",
		      (long long)adapter->stats.gprc);
	device_printf(dev, "Good Packets Xmtd = %lld\n",
		      (long long)adapter->stats.gptc);
}

static int
em_sysctl_debug_info(SYSCTL_HANDLER_ARGS)
{
	int error;
	int result;
	struct adapter *adapter;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);

	if (error || !req->newptr)
		return(error);

	if (result == 1) {
		adapter = (struct adapter *)arg1;
		em_print_debug_info(adapter);
	}

	return(error);
}

static int
em_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	int error;
	int result;
	struct adapter *adapter;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);

	if (error || !req->newptr)
		return(error);

	if (result == 1) {
		adapter = (struct adapter *)arg1;
		em_print_hw_stats(adapter);
	}

	return(error);
}

static int
em_sysctl_int_delay(SYSCTL_HANDLER_ARGS)
{
	struct em_int_delay_info *info;
	struct adapter *adapter;
	uint32_t regval;
	int error;
	int usecs;
	int ticks;

	info = (struct em_int_delay_info *)arg1;
	adapter = info->adapter;
	usecs = info->value;
	error = sysctl_handle_int(oidp, &usecs, 0, req);
	if (error != 0 || req->newptr == NULL)
		return(error);
	if (usecs < 0 || usecs > E1000_TICKS_TO_USECS(65535))
		return(EINVAL);
	info->value = usecs;
	ticks = E1000_USECS_TO_TICKS(usecs);

	lwkt_serialize_enter(&adapter->serializer);
	regval = E1000_READ_OFFSET(&adapter->hw, info->offset);
	regval = (regval & ~0xffff) | (ticks & 0xffff);
	/* Handle a few special cases. */
	switch (info->offset) {
	case E1000_RDTR:
	case E1000_82542_RDTR:
		regval |= E1000_RDT_FPDB;
		break;
	case E1000_TIDV:
	case E1000_82542_TIDV:
		if (ticks == 0) {
			adapter->txd_cmd &= ~E1000_TXD_CMD_IDE;
			/* Don't write 0 into the TIDV register. */
			regval++;
		} else
			adapter->txd_cmd |= E1000_TXD_CMD_IDE;
		break;
	}
	E1000_WRITE_OFFSET(&adapter->hw, info->offset, regval);
	lwkt_serialize_exit(&adapter->serializer);
	return(0);
}

static void
em_add_int_delay_sysctl(struct adapter *adapter, const char *name,
			const char *description, struct em_int_delay_info *info,
			int offset, int value)
{
	info->adapter = adapter;
	info->offset = offset;
	info->value = value;
	SYSCTL_ADD_PROC(&adapter->sysctl_ctx,
			SYSCTL_CHILDREN(adapter->sysctl_tree),
			OID_AUTO, name, CTLTYPE_INT|CTLFLAG_RW,
			info, 0, em_sysctl_int_delay, "I", description);
}

static int
em_sysctl_int_throttle(SYSCTL_HANDLER_ARGS)
{
	struct adapter *adapter = (void *)arg1;
	int error;
	int throttle;

	throttle = em_int_throttle_ceil;
	error = sysctl_handle_int(oidp, &throttle, 0, req);
	if (error || req->newptr == NULL)
		return error;
	if (throttle < 0 || throttle > 1000000000 / 256)
		return EINVAL;
	if (throttle) {
		/*
		 * Set the interrupt throttling rate in 256ns increments,
		 * recalculate sysctl value assignment to get exact frequency.
		 */
		throttle = 1000000000 / 256 / throttle;
		lwkt_serialize_enter(&adapter->serializer);
		em_int_throttle_ceil = 1000000000 / 256 / throttle;
		E1000_WRITE_REG(&adapter->hw, ITR, throttle);
		lwkt_serialize_exit(&adapter->serializer);
	} else {
		lwkt_serialize_enter(&adapter->serializer);
		em_int_throttle_ceil = 0;
		E1000_WRITE_REG(&adapter->hw, ITR, 0);
		lwkt_serialize_exit(&adapter->serializer);
	}
	device_printf(adapter->dev, "Interrupt moderation set to %d/sec\n", 
			em_int_throttle_ceil);
	return 0;
}

