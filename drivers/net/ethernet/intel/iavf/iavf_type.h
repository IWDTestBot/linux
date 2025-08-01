/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#ifndef _IAVF_TYPE_H_
#define _IAVF_TYPE_H_

#include "iavf_status.h"
#include "iavf_osdep.h"
#include "iavf_register.h"
#include "iavf_adminq.h"
#include "iavf_devids.h"

/* IAVF_MASK is a macro used on 32 bit registers */
#define IAVF_MASK(mask, shift) ((u32)(mask) << (shift))

#define IAVF_MAX_VSI_QP			16
#define IAVF_MAX_VF_VSI			3
#define IAVF_MAX_CHAINED_RX_BUFFERS	5

/* forward declaration */
struct iavf_hw;
typedef void (*IAVF_ADMINQ_CALLBACK)(struct iavf_hw *, struct libie_aq_desc *);

/* Data type manipulation macros. */

#define IAVF_DESC_UNUSED(R)	\
	((((R)->next_to_clean > (R)->next_to_use) ? 0 : (R)->count) + \
	(R)->next_to_clean - (R)->next_to_use - 1)

/* bitfields for Tx queue mapping in QTX_CTL */
#define IAVF_QTX_CTL_VF_QUEUE	0x0
#define IAVF_QTX_CTL_VM_QUEUE	0x1
#define IAVF_QTX_CTL_PF_QUEUE	0x2

/* debug masks - set these bits in hw->debug_mask to control output */
enum iavf_debug_mask {
	IAVF_DEBUG_INIT			= 0x00000001,
	IAVF_DEBUG_RELEASE		= 0x00000002,

	IAVF_DEBUG_LINK			= 0x00000010,
	IAVF_DEBUG_PHY			= 0x00000020,
	IAVF_DEBUG_HMC			= 0x00000040,
	IAVF_DEBUG_NVM			= 0x00000080,
	IAVF_DEBUG_LAN			= 0x00000100,
	IAVF_DEBUG_FLOW			= 0x00000200,
	IAVF_DEBUG_DCB			= 0x00000400,
	IAVF_DEBUG_DIAG			= 0x00000800,
	IAVF_DEBUG_FD			= 0x00001000,
	IAVF_DEBUG_PACKAGE		= 0x00002000,

	IAVF_DEBUG_AQ_MESSAGE		= 0x01000000,
	IAVF_DEBUG_AQ_DESCRIPTOR	= 0x02000000,
	IAVF_DEBUG_AQ_DESC_BUFFER	= 0x04000000,
	IAVF_DEBUG_AQ_COMMAND		= 0x06000000,
	IAVF_DEBUG_AQ			= 0x0F000000,

	IAVF_DEBUG_USER			= 0xF0000000,

	IAVF_DEBUG_ALL			= 0xFFFFFFFF
};

/* These are structs for managing the hardware information and the operations.
 * The structures of function pointers are filled out at init time when we
 * know for sure exactly which hardware we're working with.  This gives us the
 * flexibility of using the same main driver code but adapting to slightly
 * different hardware needs as new parts are developed.  For this architecture,
 * the Firmware and AdminQ are intended to insulate the driver from most of the
 * future changes, but these structures will also do part of the job.
 */
enum iavf_vsi_type {
	IAVF_VSI_MAIN	= 0,
	IAVF_VSI_VMDQ1	= 1,
	IAVF_VSI_VMDQ2	= 2,
	IAVF_VSI_CTRL	= 3,
	IAVF_VSI_FCOE	= 4,
	IAVF_VSI_MIRROR	= 5,
	IAVF_VSI_SRIOV	= 6,
	IAVF_VSI_FDIR	= 7,
	IAVF_VSI_TYPE_UNKNOWN
};

enum iavf_queue_type {
	IAVF_QUEUE_TYPE_RX = 0,
	IAVF_QUEUE_TYPE_TX,
	IAVF_QUEUE_TYPE_PE_CEQ,
	IAVF_QUEUE_TYPE_UNKNOWN
};

#define IAVF_HW_CAP_MAX_GPIO		30
/* Capabilities of a PF or a VF or the whole device */
struct iavf_hw_capabilities {
	bool dcb;
	bool fcoe;
	u32 num_vsis;
	u32 num_rx_qp;
	u32 num_tx_qp;
	u32 base_queue;
	u32 num_msix_vectors_vf;
};

struct iavf_mac_info {
	u8 addr[ETH_ALEN];
	u8 perm_addr[ETH_ALEN];
};

/* PCI bus types */
enum iavf_bus_type {
	iavf_bus_type_unknown = 0,
	iavf_bus_type_pci,
	iavf_bus_type_pcix,
	iavf_bus_type_pci_express,
	iavf_bus_type_reserved
};

/* PCI bus speeds */
enum iavf_bus_speed {
	iavf_bus_speed_unknown	= 0,
	iavf_bus_speed_33	= 33,
	iavf_bus_speed_66	= 66,
	iavf_bus_speed_100	= 100,
	iavf_bus_speed_120	= 120,
	iavf_bus_speed_133	= 133,
	iavf_bus_speed_2500	= 2500,
	iavf_bus_speed_5000	= 5000,
	iavf_bus_speed_8000	= 8000,
	iavf_bus_speed_reserved
};

/* PCI bus widths */
enum iavf_bus_width {
	iavf_bus_width_unknown	= 0,
	iavf_bus_width_pcie_x1	= 1,
	iavf_bus_width_pcie_x2	= 2,
	iavf_bus_width_pcie_x4	= 4,
	iavf_bus_width_pcie_x8	= 8,
	iavf_bus_width_32	= 32,
	iavf_bus_width_64	= 64,
	iavf_bus_width_reserved
};

/* Bus parameters */
struct iavf_bus_info {
	enum iavf_bus_speed speed;
	enum iavf_bus_width width;
	enum iavf_bus_type type;

	u16 func;
	u16 device;
	u16 lan_id;
	u16 bus_id;
};

#define IAVF_MAX_USER_PRIORITY		8
/* Port hardware description */
struct iavf_hw {
	u8 __iomem *hw_addr;
	void *back;

	/* subsystem structs */
	struct iavf_mac_info mac;
	struct iavf_bus_info bus;

	/* pci info */
	u16 device_id;
	u16 vendor_id;
	u16 subsystem_device_id;
	u16 subsystem_vendor_id;
	u8 revision_id;

	/* capabilities for entire device and PCI func */
	struct iavf_hw_capabilities dev_caps;

	/* Admin Queue info */
	struct iavf_adminq_info aq;

	/* debug mask */
	u32 debug_mask;
	char err_str[16];
};

/**
 * struct iavf_rx_desc - Receive descriptor (both legacy and flexible)
 * @qw0: quad word 0 fields:
 *	 Legacy: Descriptor Type; Mirror ID; L2TAG1P (S-TAG); Filter Status
 *	 Flex: Descriptor Type; Mirror ID; UMBCAST; Packet Type; Flexible Flags
 *	       Section 0; Packet Length; Header Length; Split Header Flag;
 *	       Flexible Flags section 1 / Extended Status
 * @qw1: quad word 1 fields:
 *	 Legacy: Status Field; Error Field; Packet Type; Packet Length (packet,
 *		 header, Split Header Flag)
 *	 Flex: Status / Error 0 Field; L2TAG1P (S-TAG); Flexible Metadata
 *	       Container #0; Flexible Metadata Container #1
 * @qw2: quad word 2 fields:
 *	 Legacy: Extended Status; 1st L2TAG2P (C-TAG); 2nd L2TAG2P (C-TAG)
 *	 Flex: Status / Error 1 Field; Flexible Flags section 2; Timestamp Low;
 *	       1st L2TAG2 (C-TAG); 2nd L2TAG2 (C-TAG)
 * @qw3: quad word 3 fields:
 *	 Legacy: FD Filter ID / Flexible Bytes
 *	 Flex: Flexible Metadata Container #2; Flexible Metadata Container #3;
 *	       Flexible Metadata Container #4 / Timestamp High 0; Flexible
 *	       Metadata Container #5 / Timestamp High 1;
 */
struct iavf_rx_desc {
	aligned_le64 qw0;
/* The hash signature (RSS) */
#define IAVF_RXD_LEGACY_RSS_M			GENMASK_ULL(63, 32)
/* Stripped C-TAG VLAN from the receive packet */
#define IAVF_RXD_LEGACY_L2TAG1_M		GENMASK_ULL(33, 16)
/* Packet type */
#define IAVF_RXD_FLEX_PTYPE_M			GENMASK_ULL(25, 16)
/* Packet length */
#define IAVF_RXD_FLEX_PKT_LEN_M			GENMASK_ULL(45, 32)

	aligned_le64 qw1;
/* Descriptor done indication flag. */
#define IAVF_RXD_LEGACY_DD_M			BIT(0)
/* End of packet. Set to 1 if this descriptor is the last one of the packet */
#define IAVF_RXD_LEGACY_EOP_M			BIT(1)
/* L2 TAG 1 presence indication */
#define IAVF_RXD_LEGACY_L2TAG1P_M		BIT(2)
/* Detectable L3 and L4 integrity check is processed by the HW */
#define IAVF_RXD_LEGACY_L3L4P_M			BIT(3)
/* Set when an IPv6 packet contains a Destination Options Header or a Routing
 * Header.
 */
#define IAVF_RXD_LEGACY_IPV6EXADD_M		BIT(15)
/* Receive MAC Errors: CRC; Alignment; Oversize; Undersizes; Length error */
#define IAVF_RXD_LEGACY_RXE_M			BIT(19)
/* Checksum reports:
 * - IPE: IP checksum error
 * - L4E: L4 integrity error
 * - EIPE: External IP header (tunneled packets)
 */
#define IAVF_RXD_LEGACY_IPE_M			BIT(22)
#define IAVF_RXD_LEGACY_L4E_M			BIT(23)
#define IAVF_RXD_LEGACY_EIPE_M			BIT(24)
/* Set for packets that skip checksum calculation in pre-parser */
#define IAVF_RXD_LEGACY_PPRS_M			BIT(26)
/* Indicates the content in the Filter Status field */
#define IAVF_RXD_LEGACY_FLTSTAT_M		GENMASK_ULL(13, 12)
/* Packet type */
#define IAVF_RXD_LEGACY_PTYPE_M			GENMASK_ULL(37, 30)
/* Packet length */
#define IAVF_RXD_LEGACY_LENGTH_M		GENMASK_ULL(51, 38)
/* Descriptor done indication flag */
#define IAVF_RXD_FLEX_DD_M			BIT(0)
/* End of packet. Set to 1 if this descriptor is the last one of the packet */
#define IAVF_RXD_FLEX_EOP_M			BIT(1)
/* Detectable L3 and L4 integrity check is processed by the HW */
#define IAVF_RXD_FLEX_L3L4P_M			BIT(3)
/* Checksum reports:
 * - IPE: IP checksum error
 * - L4E: L4 integrity error
 * - EIPE: External IP header (tunneled packets)
 * - EUDPE: External UDP checksum error (tunneled packets)
 */
#define IAVF_RXD_FLEX_XSUM_IPE_M		BIT(4)
#define IAVF_RXD_FLEX_XSUM_L4E_M		BIT(5)
#define IAVF_RXD_FLEX_XSUM_EIPE_M		BIT(6)
#define IAVF_RXD_FLEX_XSUM_EUDPE_M		BIT(7)
/* Set when an IPv6 packet contains a Destination Options Header or a Routing
 * Header.
 */
#define IAVF_RXD_FLEX_IPV6EXADD_M		BIT(9)
/* Receive MAC Errors: CRC; Alignment; Oversize; Undersizes; Length error */
#define IAVF_RXD_FLEX_RXE_M			BIT(10)
/* Indicates that the RSS/HASH result is valid */
#define IAVF_RXD_FLEX_RSS_VALID_M		BIT(12)
/* L2 TAG 1 presence indication */
#define IAVF_RXD_FLEX_L2TAG1P_M			BIT(13)
/* Stripped L2 Tag from the receive packet */
#define IAVF_RXD_FLEX_L2TAG1_M			GENMASK_ULL(31, 16)
/* The hash signature (RSS) */
#define IAVF_RXD_FLEX_RSS_HASH_M		GENMASK_ULL(63, 32)

	aligned_le64 qw2;
/* L2 Tag 2 Presence */
#define IAVF_RXD_LEGACY_L2TAG2P_M		BIT(0)
/* Stripped S-TAG VLAN from the receive packet */
#define IAVF_RXD_LEGACY_L2TAG2_M		GENMASK_ULL(63, 32)
/* Stripped S-TAG VLAN from the receive packet */
#define IAVF_RXD_FLEX_L2TAG2_2_M		GENMASK_ULL(63, 48)
/* The packet is a UDP tunneled packet */
#define IAVF_RXD_FLEX_NAT_M			BIT(4)
/* L2 Tag 2 Presence */
#define IAVF_RXD_FLEX_L2TAG2P_M			BIT(11)
	aligned_le64 qw3;
#define IAVF_RXD_FLEX_QW3_TSTAMP_HIGH_M		GENMASK_ULL(63, 32)
} __aligned(4 * sizeof(__le64));
static_assert(sizeof(struct iavf_rx_desc) == 32);

#define IAVF_RXD_QW1_STATUS_TSYNINDX_SHIFT IAVF_RX_DESC_STATUS_TSYNINDX_SHIFT
#define IAVF_RXD_QW1_STATUS_TSYNINDX_MASK  (0x3UL << \
					    IAVF_RXD_QW1_STATUS_TSYNINDX_SHIFT)

#define IAVF_RXD_QW1_STATUS_TSYNVALID_SHIFT IAVF_RX_DESC_STATUS_TSYNVALID_SHIFT
#define IAVF_RXD_QW1_STATUS_TSYNVALID_MASK \
				    BIT_ULL(IAVF_RXD_QW1_STATUS_TSYNVALID_SHIFT)

enum iavf_rx_desc_fltstat_values {
	IAVF_RX_DESC_FLTSTAT_NO_DATA	= 0,
	IAVF_RX_DESC_FLTSTAT_RSV_FD_ID	= 1, /* 16byte desc? FD_ID : RSV */
	IAVF_RX_DESC_FLTSTAT_RSV	= 2,
	IAVF_RX_DESC_FLTSTAT_RSS_HASH	= 3,
};

enum iavf_rx_desc_error_l3l4e_fcoe_masks {
	IAVF_RX_DESC_ERROR_L3L4E_NONE		= 0,
	IAVF_RX_DESC_ERROR_L3L4E_PROT		= 1,
	IAVF_RX_DESC_ERROR_L3L4E_FC		= 2,
	IAVF_RX_DESC_ERROR_L3L4E_DMAC_ERR	= 3,
	IAVF_RX_DESC_ERROR_L3L4E_DMAC_WARN	= 4
};

#define IAVF_RXD_QW1_LENGTH_HBUF_SHIFT	52
#define IAVF_RXD_QW1_LENGTH_HBUF_MASK	(0x7FFULL << \
					 IAVF_RXD_QW1_LENGTH_HBUF_SHIFT)

#define IAVF_RXD_QW1_LENGTH_SPH_SHIFT	63
#define IAVF_RXD_QW1_LENGTH_SPH_MASK	BIT_ULL(IAVF_RXD_QW1_LENGTH_SPH_SHIFT)

enum iavf_rx_desc_ext_status_bits {
	/* Note: These are predefined bit offsets */
	IAVF_RX_DESC_EXT_STATUS_L2TAG2P_SHIFT	= 0,
	IAVF_RX_DESC_EXT_STATUS_L2TAG3P_SHIFT	= 1,
	IAVF_RX_DESC_EXT_STATUS_FLEXBL_SHIFT	= 2, /* 2 BITS */
	IAVF_RX_DESC_EXT_STATUS_FLEXBH_SHIFT	= 4, /* 2 BITS */
	IAVF_RX_DESC_EXT_STATUS_FDLONGB_SHIFT	= 9,
	IAVF_RX_DESC_EXT_STATUS_FCOELONGB_SHIFT	= 10,
	IAVF_RX_DESC_EXT_STATUS_PELONGB_SHIFT	= 11,
};

#define IAVF_RX_DESC_EXT_STATUS_L2TAG2P_M	BIT(IAVF_RX_DESC_EXT_STATUS_L2TAG2P_SHIFT)

enum iavf_rx_desc_pe_status_bits {
	/* Note: These are predefined bit offsets */
	IAVF_RX_DESC_PE_STATUS_QPID_SHIFT	= 0, /* 18 BITS */
	IAVF_RX_DESC_PE_STATUS_L4PORT_SHIFT	= 0, /* 16 BITS */
	IAVF_RX_DESC_PE_STATUS_IPINDEX_SHIFT	= 16, /* 8 BITS */
	IAVF_RX_DESC_PE_STATUS_QPIDHIT_SHIFT	= 24,
	IAVF_RX_DESC_PE_STATUS_APBVTHIT_SHIFT	= 25,
	IAVF_RX_DESC_PE_STATUS_PORTV_SHIFT	= 26,
	IAVF_RX_DESC_PE_STATUS_URG_SHIFT	= 27,
	IAVF_RX_DESC_PE_STATUS_IPFRAG_SHIFT	= 28,
	IAVF_RX_DESC_PE_STATUS_IPOPT_SHIFT	= 29
};

#define IAVF_RX_PROG_STATUS_DESC_LENGTH_SHIFT		38
#define IAVF_RX_PROG_STATUS_DESC_LENGTH			0x2000000

#define IAVF_RX_PROG_STATUS_DESC_QW1_PROGID_SHIFT	2
#define IAVF_RX_PROG_STATUS_DESC_QW1_PROGID_MASK	(0x7UL << \
				IAVF_RX_PROG_STATUS_DESC_QW1_PROGID_SHIFT)

#define IAVF_RX_PROG_STATUS_DESC_QW1_ERROR_SHIFT	19
#define IAVF_RX_PROG_STATUS_DESC_QW1_ERROR_MASK		(0x3FUL << \
				IAVF_RX_PROG_STATUS_DESC_QW1_ERROR_SHIFT)

enum iavf_rx_prog_status_desc_status_bits {
	/* Note: These are predefined bit offsets */
	IAVF_RX_PROG_STATUS_DESC_DD_SHIFT	= 0,
	IAVF_RX_PROG_STATUS_DESC_PROG_ID_SHIFT	= 2 /* 3 BITS */
};

enum iavf_rx_prog_status_desc_prog_id_masks {
	IAVF_RX_PROG_STATUS_DESC_FD_FILTER_STATUS	= 1,
	IAVF_RX_PROG_STATUS_DESC_FCOE_CTXT_PROG_STATUS	= 2,
	IAVF_RX_PROG_STATUS_DESC_FCOE_CTXT_INVL_STATUS	= 4,
};

enum iavf_rx_prog_status_desc_error_bits {
	/* Note: These are predefined bit offsets */
	IAVF_RX_PROG_STATUS_DESC_FD_TBL_FULL_SHIFT	= 0,
	IAVF_RX_PROG_STATUS_DESC_NO_FD_ENTRY_SHIFT	= 1,
	IAVF_RX_PROG_STATUS_DESC_FCOE_TBL_FULL_SHIFT	= 2,
	IAVF_RX_PROG_STATUS_DESC_FCOE_CONFLICT_SHIFT	= 3
};

/* TX Descriptor */
struct iavf_tx_desc {
	__le64 buffer_addr; /* Address of descriptor's data buf */
	__le64 cmd_type_offset_bsz;
};

#define IAVF_TXD_QW1_DTYPE_SHIFT	0
#define IAVF_TXD_QW1_DTYPE_MASK		(0xFUL << IAVF_TXD_QW1_DTYPE_SHIFT)

enum iavf_tx_desc_dtype_value {
	IAVF_TX_DESC_DTYPE_DATA		= 0x0,
	IAVF_TX_DESC_DTYPE_NOP		= 0x1, /* same as Context desc */
	IAVF_TX_DESC_DTYPE_CONTEXT	= 0x1,
	IAVF_TX_DESC_DTYPE_FCOE_CTX	= 0x2,
	IAVF_TX_DESC_DTYPE_FILTER_PROG	= 0x8,
	IAVF_TX_DESC_DTYPE_DDP_CTX	= 0x9,
	IAVF_TX_DESC_DTYPE_FLEX_DATA	= 0xB,
	IAVF_TX_DESC_DTYPE_FLEX_CTX_1	= 0xC,
	IAVF_TX_DESC_DTYPE_FLEX_CTX_2	= 0xD,
	IAVF_TX_DESC_DTYPE_DESC_DONE	= 0xF
};

#define IAVF_TXD_QW1_CMD_SHIFT	4
#define IAVF_TXD_QW1_CMD_MASK	(0x3FFUL << IAVF_TXD_QW1_CMD_SHIFT)

enum iavf_tx_desc_cmd_bits {
	IAVF_TX_DESC_CMD_EOP			= 0x0001,
	IAVF_TX_DESC_CMD_RS			= 0x0002,
	IAVF_TX_DESC_CMD_ICRC			= 0x0004,
	IAVF_TX_DESC_CMD_IL2TAG1		= 0x0008,
	IAVF_TX_DESC_CMD_DUMMY			= 0x0010,
	IAVF_TX_DESC_CMD_IIPT_NONIP		= 0x0000, /* 2 BITS */
	IAVF_TX_DESC_CMD_IIPT_IPV6		= 0x0020, /* 2 BITS */
	IAVF_TX_DESC_CMD_IIPT_IPV4		= 0x0040, /* 2 BITS */
	IAVF_TX_DESC_CMD_IIPT_IPV4_CSUM		= 0x0060, /* 2 BITS */
	IAVF_TX_DESC_CMD_FCOET			= 0x0080,
	IAVF_TX_DESC_CMD_L4T_EOFT_UNK		= 0x0000, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_TCP		= 0x0100, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_SCTP		= 0x0200, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_UDP		= 0x0300, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_EOF_N		= 0x0000, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_EOF_T		= 0x0100, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_EOF_NI	= 0x0200, /* 2 BITS */
	IAVF_TX_DESC_CMD_L4T_EOFT_EOF_A		= 0x0300, /* 2 BITS */
};

#define IAVF_TXD_QW1_OFFSET_SHIFT	16
#define IAVF_TXD_QW1_OFFSET_MASK	(0x3FFFFULL << \
					 IAVF_TXD_QW1_OFFSET_SHIFT)

enum iavf_tx_desc_length_fields {
	/* Note: These are predefined bit offsets */
	IAVF_TX_DESC_LENGTH_MACLEN_SHIFT	= 0, /* 7 BITS */
	IAVF_TX_DESC_LENGTH_IPLEN_SHIFT		= 7, /* 7 BITS */
	IAVF_TX_DESC_LENGTH_L4_FC_LEN_SHIFT	= 14 /* 4 BITS */
};

#define IAVF_TXD_QW1_TX_BUF_SZ_SHIFT	34
#define IAVF_TXD_QW1_TX_BUF_SZ_MASK	(0x3FFFULL << \
					 IAVF_TXD_QW1_TX_BUF_SZ_SHIFT)

#define IAVF_TXD_QW1_L2TAG1_SHIFT	48
#define IAVF_TXD_QW1_L2TAG1_MASK	(0xFFFFULL << IAVF_TXD_QW1_L2TAG1_SHIFT)

/* Context descriptors */
struct iavf_tx_context_desc {
	__le32 tunneling_params;
	__le16 l2tag2;
	__le16 rsvd;
	__le64 type_cmd_tso_mss;
};

#define IAVF_TXD_CTX_QW1_CMD_SHIFT	4
#define IAVF_TXD_CTX_QW1_CMD_MASK	(0xFFFFUL << IAVF_TXD_CTX_QW1_CMD_SHIFT)

enum iavf_tx_ctx_desc_cmd_bits {
	IAVF_TX_CTX_DESC_TSO		= 0x01,
	IAVF_TX_CTX_DESC_TSYN		= 0x02,
	IAVF_TX_CTX_DESC_IL2TAG2	= 0x04,
	IAVF_TX_CTX_DESC_IL2TAG2_IL2H	= 0x08,
	IAVF_TX_CTX_DESC_SWTCH_NOTAG	= 0x00,
	IAVF_TX_CTX_DESC_SWTCH_UPLINK	= 0x10,
	IAVF_TX_CTX_DESC_SWTCH_LOCAL	= 0x20,
	IAVF_TX_CTX_DESC_SWTCH_VSI	= 0x30,
	IAVF_TX_CTX_DESC_SWPE		= 0x40
};

#define IAVF_TXD_CTX_QW1_TSO_LEN_SHIFT	30
#define IAVF_TXD_CTX_QW1_TSO_LEN_MASK	(0x3FFFFULL << \
					 IAVF_TXD_CTX_QW1_TSO_LEN_SHIFT)

#define IAVF_TXD_CTX_QW1_MSS_SHIFT	50
#define IAVF_TXD_CTX_QW1_MSS_MASK	(0x3FFFULL << \
					 IAVF_TXD_CTX_QW1_MSS_SHIFT)

#define IAVF_TXD_CTX_QW1_VSI_SHIFT	50
#define IAVF_TXD_CTX_QW1_VSI_MASK	(0x1FFULL << IAVF_TXD_CTX_QW1_VSI_SHIFT)

#define IAVF_TXD_CTX_QW0_EXT_IP_SHIFT	0
#define IAVF_TXD_CTX_QW0_EXT_IP_MASK	(0x3ULL << \
					 IAVF_TXD_CTX_QW0_EXT_IP_SHIFT)

enum iavf_tx_ctx_desc_eipt_offload {
	IAVF_TX_CTX_EXT_IP_NONE		= 0x0,
	IAVF_TX_CTX_EXT_IP_IPV6		= 0x1,
	IAVF_TX_CTX_EXT_IP_IPV4_NO_CSUM	= 0x2,
	IAVF_TX_CTX_EXT_IP_IPV4		= 0x3
};

#define IAVF_TXD_CTX_QW0_EXT_IPLEN_SHIFT	2
#define IAVF_TXD_CTX_QW0_EXT_IPLEN_MASK	(0x3FULL << \
					 IAVF_TXD_CTX_QW0_EXT_IPLEN_SHIFT)

#define IAVF_TXD_CTX_QW0_NATT_SHIFT	9
#define IAVF_TXD_CTX_QW0_NATT_MASK	(0x3ULL << IAVF_TXD_CTX_QW0_NATT_SHIFT)

#define IAVF_TXD_CTX_UDP_TUNNELING	BIT_ULL(IAVF_TXD_CTX_QW0_NATT_SHIFT)
#define IAVF_TXD_CTX_GRE_TUNNELING	(0x2ULL << IAVF_TXD_CTX_QW0_NATT_SHIFT)

#define IAVF_TXD_CTX_QW0_EIP_NOINC_SHIFT	11
#define IAVF_TXD_CTX_QW0_EIP_NOINC_MASK \
				       BIT_ULL(IAVF_TXD_CTX_QW0_EIP_NOINC_SHIFT)

#define IAVF_TXD_CTX_EIP_NOINC_IPID_CONST	IAVF_TXD_CTX_QW0_EIP_NOINC_MASK

#define IAVF_TXD_CTX_QW0_NATLEN_SHIFT	12
#define IAVF_TXD_CTX_QW0_NATLEN_MASK	(0X7FULL << \
					 IAVF_TXD_CTX_QW0_NATLEN_SHIFT)

#define IAVF_TXD_CTX_QW0_DECTTL_SHIFT	19
#define IAVF_TXD_CTX_QW0_DECTTL_MASK	(0xFULL << \
					 IAVF_TXD_CTX_QW0_DECTTL_SHIFT)

#define IAVF_TXD_CTX_QW0_L4T_CS_SHIFT	23
#define IAVF_TXD_CTX_QW0_L4T_CS_MASK	BIT_ULL(IAVF_TXD_CTX_QW0_L4T_CS_SHIFT)

/* Statistics collected by each port, VSI, VEB, and S-channel */
struct iavf_eth_stats {
	u64 rx_bytes;			/* gorc */
	u64 rx_unicast;			/* uprc */
	u64 rx_multicast;		/* mprc */
	u64 rx_broadcast;		/* bprc */
	u64 rx_discards;		/* rdpc */
	u64 rx_unknown_protocol;	/* rupp */
	u64 tx_bytes;			/* gotc */
	u64 tx_unicast;			/* uptc */
	u64 tx_multicast;		/* mptc */
	u64 tx_broadcast;		/* bptc */
	u64 tx_discards;		/* tdpc */
	u64 tx_errors;			/* tepc */
};
#endif /* _IAVF_TYPE_H_ */
