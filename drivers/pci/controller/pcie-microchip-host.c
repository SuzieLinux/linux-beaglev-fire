// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip AXI PCIe Bridge host controller driver
 *
 * Copyright (c) 2018 - 2020 Microchip Corporation. All rights reserved.
 *
 * Author: Daire McNamara <daire.mcnamara@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>

#include "../pci.h"

/* Number of MSI IRQs */
#define MC_MAX_NUM_MSI_IRQS			32

#define MC_MAX_NUM_INBOUND_WINDOWS		8
#define MC_ATT_MASK				GENMASK_ULL(63, 31)

/* PCIe Bridge Phy and Controller Phy offsets */
#define MC_PCIE1_BRIDGE_ADDR			0x00004000u
#define MC_PCIE1_CTRL_ADDR			0x00006000u

/* PCIe Bridge Phy Regs */
#define PCIE_PCI_IRQ_DW0			0xa8
#define  MSIX_CAP_MASK				BIT(31)
#define  NUM_MSI_MSGS_MASK			GENMASK(6, 4)
#define  NUM_MSI_MSGS_SHIFT			4

#define IMASK_LOCAL				0x180
#define  DMA_END_ENGINE_0_MASK			0x00000000u
#define  DMA_END_ENGINE_0_SHIFT			0
#define  DMA_END_ENGINE_1_MASK			0x00000000u
#define  DMA_END_ENGINE_1_SHIFT			1
#define  DMA_ERROR_ENGINE_0_MASK		0x00000100u
#define  DMA_ERROR_ENGINE_0_SHIFT		8
#define  DMA_ERROR_ENGINE_1_MASK		0x00000200u
#define  DMA_ERROR_ENGINE_1_SHIFT		9
#define  A_ATR_EVT_POST_ERR_MASK		0x00010000u
#define  A_ATR_EVT_POST_ERR_SHIFT		16
#define  A_ATR_EVT_FETCH_ERR_MASK		0x00020000u
#define  A_ATR_EVT_FETCH_ERR_SHIFT		17
#define  A_ATR_EVT_DISCARD_ERR_MASK		0x00040000u
#define  A_ATR_EVT_DISCARD_ERR_SHIFT		18
#define  A_ATR_EVT_DOORBELL_MASK		0x00000000u
#define  A_ATR_EVT_DOORBELL_SHIFT		19
#define  P_ATR_EVT_POST_ERR_MASK		0x00100000u
#define  P_ATR_EVT_POST_ERR_SHIFT		20
#define  P_ATR_EVT_FETCH_ERR_MASK		0x00200000u
#define  P_ATR_EVT_FETCH_ERR_SHIFT		21
#define  P_ATR_EVT_DISCARD_ERR_MASK		0x00400000u
#define  P_ATR_EVT_DISCARD_ERR_SHIFT		22
#define  P_ATR_EVT_DOORBELL_MASK		0x00000000u
#define  P_ATR_EVT_DOORBELL_SHIFT		23
#define  PM_MSI_INT_INTA_MASK			0x01000000u
#define  PM_MSI_INT_INTA_SHIFT			24
#define  PM_MSI_INT_INTB_MASK			0x02000000u
#define  PM_MSI_INT_INTB_SHIFT			25
#define  PM_MSI_INT_INTC_MASK			0x04000000u
#define  PM_MSI_INT_INTC_SHIFT			26
#define  PM_MSI_INT_INTD_MASK			0x08000000u
#define  PM_MSI_INT_INTD_SHIFT			27
#define  PM_MSI_INT_INTX_MASK			0x0f000000u
#define  PM_MSI_INT_INTX_SHIFT			24
#define  PM_MSI_INT_MSI_MASK			0x10000000u
#define  PM_MSI_INT_MSI_SHIFT			28
#define  PM_MSI_INT_AER_EVT_MASK		0x20000000u
#define  PM_MSI_INT_AER_EVT_SHIFT		29
#define  PM_MSI_INT_EVENTS_MASK			0x40000000u
#define  PM_MSI_INT_EVENTS_SHIFT		30
#define  PM_MSI_INT_SYS_ERR_MASK		0x80000000u
#define  PM_MSI_INT_SYS_ERR_SHIFT		31
#define  NUM_LOCAL_EVENTS			15
#define ISTATUS_LOCAL				0x184
#define IMASK_HOST				0x188
#define ISTATUS_HOST				0x18c
#define IMSI_ADDR				0x190
#define ISTATUS_MSI				0x194

#define ATR_WINDOW_DESC_SIZE			32
#define ATR_SIZE_SHIFT				1
#define ATR_IMPL_ENABLE				1

#define ATR_PCIE_WIN0_SRCADDR			0x80000000
#define ATR_PCIE_ATR_SIZE			(512 * 1024 * 1024ul)
#define ATR_PCIE_NUM_WINDOWS			8

/* PCIe Master table init defines */
#define ATR0_PCIE_WIN0_SRCADDR_PARAM		0x600u
#define ATR0_PCIE_WIN0_SRC_ADDR			0x604u
#define ATR0_PCIE_WIN0_TRSL_ADDR_LSB		0x608u
#define ATR0_PCIE_WIN0_TRSL_ADDR_UDW		0x60cu
#define ATR0_PCIE_WIN0_TRSL_PARAM		0x610u

enum {
	TRSL_ID_PCIE_TXRX,
	TRSL_ID_PCIE_CONFIG,
	TRSL_ID_AXI4_LITE_MASTER,
	TRSL_ID_AXI4_MASTER_0 = 4,
	TRSL_ID_AXI4_MASTER_1,
	TRSL_ID_AXI4_MASTER_2,
	TRSL_ID_AXI4_MASTER_3,
	TRSL_ID_AXI4_STREAM_0,
	TRSL_ID_AXI4_STREAM_1,
	TRSL_ID_AXI4_STREAM_2,
	TRSL_ID_AXI4_STREAM_3,
	TRSL_ID_INTERNAL_BRIDGE_REGISTERS
};

#define ATR0_PCIE_WIN0_TRSL_MASK_LSB		0x618u
#define ATR0_PCIE_WIN0_TRSL_MASK_UDW		0x61cu

/* PCIe AXI slave table init defines */
#define ATR0_AXI4_SLV0_SRCADDR_PARAM		0x800u
#define ATR0_AXI4_SLV0_SRC_ADDR			0x804u
#define ATR0_AXI4_SLV0_TRSL_ADDR_LSB		0x808u
#define ATR0_AXI4_SLV0_TRSL_ADDR_UDW		0x80cu
#define ATR0_AXI4_SLV0_TRSL_PARAM		0x810u

/* PCIe Controller Phy Regs */
#define SEC_ERROR_EVENT_CNT			0x20
#define DED_ERROR_EVENT_CNT			0x24
#define SEC_ERROR_INT				0x28
#define  SEC_ERROR_INT_TX_RAM_SEC_ERR_INT	GENMASK(3, 0)
#define  SEC_ERROR_INT_RX_RAM_SEC_ERR_INT	GENMASK(7, 4)
#define  SEC_ERROR_INT_PCIE2AXI_RAM_SEC_ERR_INT	GENMASK(11, 8)
#define  SEC_ERROR_INT_AXI2PCIE_RAM_SEC_ERR_INT	GENMASK(15, 12)
#define  SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT	GENMASK(15, 0)
#define  NUM_SEC_ERROR_INTS			(4)
#define SEC_ERROR_INT_MASK			0x2c
#define DED_ERROR_INT				0x30
#define  DED_ERROR_INT_TX_RAM_DED_ERR_INT	GENMASK(3, 0)
#define  DED_ERROR_INT_RX_RAM_DED_ERR_INT	GENMASK(7, 4)
#define  DED_ERROR_INT_PCIE2AXI_RAM_DED_ERR_INT	GENMASK(11, 8)
#define  DED_ERROR_INT_AXI2PCIE_RAM_DED_ERR_INT	GENMASK(15, 12)
#define  DED_ERROR_INT_ALL_RAM_DED_ERR_INT	GENMASK(15, 0)
#define  NUM_DED_ERROR_INTS			(4)
#define DED_ERROR_INT_MASK			0x34
#define ECC_CONTROL				0x38
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_0		BIT(0)
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_1		BIT(1)
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_2		BIT(2)
#define  ECC_CONTROL_TX_RAM_INJ_ERROR_3		BIT(3)
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_0		BIT(4)
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_1		BIT(5)
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_2		BIT(6)
#define  ECC_CONTROL_RX_RAM_INJ_ERROR_3		BIT(7)
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_0	BIT(8)
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_1	BIT(9)
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_2	BIT(10)
#define  ECC_CONTROL_PCIE2AXI_RAM_INJ_ERROR_3	BIT(11)
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_0	BIT(12)
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_1	BIT(13)
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_2	BIT(14)
#define  ECC_CONTROL_AXI2PCIE_RAM_INJ_ERROR_3	BIT(15)
#define  ECC_CONTROL_TX_RAM_ECC_BYPASS		BIT(24)
#define  ECC_CONTROL_RX_RAM_ECC_BYPASS		BIT(25)
#define  ECC_CONTROL_PCIE2AXI_RAM_ECC_BYPASS	BIT(26)
#define  ECC_CONTROL_AXI2PCIE_RAM_ECC_BYPASS	BIT(27)
#define PCIE_EVENT_INT				0x14c
#define  PCIE_EVENT_INT_L2_EXIT_INT		BIT(0)
#define  PCIE_EVENT_INT_HOTRST_EXIT_INT		BIT(1)
#define  PCIE_EVENT_INT_DLUP_EXIT_INT		BIT(2)
#define  PCIE_EVENT_INT_MASK			GENMASK(2, 0)
#define  PCIE_EVENT_INT_L2_EXIT_INT_MASK	BIT(16)
#define  PCIE_EVENT_INT_HOTRST_EXIT_INT_MASK	BIT(17)
#define  PCIE_EVENT_INT_DLUP_EXIT_INT_MASK	BIT(18)
#define  PCIE_EVENT_INT_ENB_MASK		GENMASK(18, 16)
#define  PCIE_EVENT_INT_ENB_SHIFT		16
#define  NUM_PCIE_EVENTS			(3)

/* PCIe Config space MSI capability structure */
#define MC_MSI_CAP_CTRL_OFFSET			0xe0u

/* Events */
#define EVENT_PCIE_L2_EXIT			0
#define EVENT_PCIE_HOTRST_EXIT			1
#define EVENT_PCIE_DLUP_EXIT			2
#define EVENT_SEC_TX_RAM_SEC_ERR		3
#define EVENT_SEC_RX_RAM_SEC_ERR		4
#define EVENT_SEC_PCIE2AXI_RAM_SEC_ERR		5
#define EVENT_SEC_AXI2PCIE_RAM_SEC_ERR		6
#define EVENT_DED_TX_RAM_DED_ERR		7
#define EVENT_DED_RX_RAM_DED_ERR		8
#define EVENT_DED_PCIE2AXI_RAM_DED_ERR		9
#define EVENT_DED_AXI2PCIE_RAM_DED_ERR		10
#define EVENT_LOCAL_DMA_END_ENGINE_0		11
#define EVENT_LOCAL_DMA_END_ENGINE_1		12
#define EVENT_LOCAL_DMA_ERROR_ENGINE_0		13
#define EVENT_LOCAL_DMA_ERROR_ENGINE_1		14
#define EVENT_LOCAL_A_ATR_EVT_POST_ERR		15
#define EVENT_LOCAL_A_ATR_EVT_FETCH_ERR		16
#define EVENT_LOCAL_A_ATR_EVT_DISCARD_ERR	17
#define EVENT_LOCAL_A_ATR_EVT_DOORBELL		18
#define EVENT_LOCAL_P_ATR_EVT_POST_ERR		19
#define EVENT_LOCAL_P_ATR_EVT_FETCH_ERR		20
#define EVENT_LOCAL_P_ATR_EVT_DISCARD_ERR	21
#define EVENT_LOCAL_P_ATR_EVT_DOORBELL		22
#define EVENT_LOCAL_PM_MSI_INT_INTX		23
#define EVENT_LOCAL_PM_MSI_INT_MSI		24
#define EVENT_LOCAL_PM_MSI_INT_AER_EVT		25
#define EVENT_LOCAL_PM_MSI_INT_EVENTS		26
#define EVENT_LOCAL_PM_MSI_INT_SYS_ERR		27
#define NUM_EVENTS				28

#define PCIE_EVENT_CAUSE(x, s)	\
	[EVENT_PCIE_ ## x] = { __stringify(x), s }

#define SEC_ERROR_CAUSE(x, s) \
	[EVENT_SEC_ ## x] = { __stringify(x), s }

#define DED_ERROR_CAUSE(x, s) \
	[EVENT_DED_ ## x] = { __stringify(x), s }

#define LOCAL_EVENT_CAUSE(x, s) \
	[EVENT_LOCAL_ ## x] = { __stringify(x), s }

#define PCIE_EVENT(x) \
	.offset = PCIE_EVENT_INT, \
	.mask_offset = PCIE_EVENT_INT, \
	.mask_high = 1, \
	.mask = PCIE_EVENT_INT_ ## x ## _INT, \
	.enb_mask = PCIE_EVENT_INT_ENB_MASK

#define SEC_EVENT(x) \
	.offset = SEC_ERROR_INT, \
	.mask_offset = SEC_ERROR_INT_MASK, \
	.mask = SEC_ERROR_INT_ ## x ## _INT, \
	.mask_high = 1, \
	.enb_mask = 0

#define DED_EVENT(x) \
	.offset = DED_ERROR_INT, \
	.mask_offset = DED_ERROR_INT_MASK, \
	.mask_high = 1, \
	.mask = DED_ERROR_INT_ ## x ## _INT, \
	.enb_mask = 0

#define LOCAL_EVENT(x) \
	.offset = ISTATUS_LOCAL, \
	.mask_offset = IMASK_LOCAL, \
	.mask_high = 0, \
	.mask = x ## _MASK, \
	.enb_mask = 0

#define PCIE_EVENT_TO_EVENT_MAP(x) \
	{ PCIE_EVENT_INT_ ## x ## _INT, EVENT_PCIE_ ## x }

#define SEC_ERROR_TO_EVENT_MAP(x) \
	{ SEC_ERROR_INT_ ## x ## _INT, EVENT_SEC_ ## x }

#define DED_ERROR_TO_EVENT_MAP(x) \
	{ DED_ERROR_INT_ ## x ## _INT, EVENT_DED_ ## x }

#define LOCAL_STATUS_TO_EVENT_MAP(x) \
	{ x ## _MASK, EVENT_LOCAL_ ## x }

struct event_map {
	u32 reg_mask;
	u32 event_bit;
};

struct mc_msi {
	struct mutex lock;		/* Protect used bitmap */
	struct irq_domain *msi_domain;
	struct irq_domain *dev_domain;
	u32 num_vectors;
	u64 vector_phy;
	DECLARE_BITMAP(used, MC_MAX_NUM_MSI_IRQS);
};

struct inbound_windows {
	u64 axi_addr;
	u64 pci_addr;
	u64 size;
};

struct mc_pcie {
	void __iomem *bridge_base_addr;
	void __iomem *ctrl_base_addr;
	struct device *dev;
	struct irq_domain *intx_domain;
	struct irq_domain *event_domain;
	raw_spinlock_t lock;
	struct mc_msi msi;
	u64 outbound_range_offset;
	u32 num_inbound_windows;
	struct inbound_windows inbound_windows[MC_MAX_NUM_INBOUND_WINDOWS];
};

struct cause {
	const char *sym;
	const char *str;
};

static const struct cause event_cause[NUM_EVENTS] = {
	PCIE_EVENT_CAUSE(L2_EXIT, "L2 exit event"),
	PCIE_EVENT_CAUSE(HOTRST_EXIT, "Hot reset exit event"),
	PCIE_EVENT_CAUSE(DLUP_EXIT, "DLUP exit event"),
	SEC_ERROR_CAUSE(TX_RAM_SEC_ERR,  "sec error in tx buffer"),
	SEC_ERROR_CAUSE(RX_RAM_SEC_ERR,  "sec error in rx buffer"),
	SEC_ERROR_CAUSE(PCIE2AXI_RAM_SEC_ERR,  "sec error in pcie2axi buffer"),
	SEC_ERROR_CAUSE(AXI2PCIE_RAM_SEC_ERR,  "sec error in axi2pcie buffer"),
	DED_ERROR_CAUSE(TX_RAM_DED_ERR,  "ded error in tx buffer"),
	DED_ERROR_CAUSE(RX_RAM_DED_ERR,  "ded error in rx buffer"),
	DED_ERROR_CAUSE(PCIE2AXI_RAM_DED_ERR,  "ded error in pcie2axi buffer"),
	DED_ERROR_CAUSE(AXI2PCIE_RAM_DED_ERR,  "ded error in axi2pcie buffer"),
	LOCAL_EVENT_CAUSE(DMA_ERROR_ENGINE_0, "dma engine 0 error"),
	LOCAL_EVENT_CAUSE(DMA_ERROR_ENGINE_1, "dma engine 1 error"),
	LOCAL_EVENT_CAUSE(A_ATR_EVT_POST_ERR, "axi write request error"),
	LOCAL_EVENT_CAUSE(A_ATR_EVT_FETCH_ERR, "axi read request error"),
	LOCAL_EVENT_CAUSE(A_ATR_EVT_DISCARD_ERR, "axi read timeout"),
	LOCAL_EVENT_CAUSE(P_ATR_EVT_POST_ERR, "pcie write request error"),
	LOCAL_EVENT_CAUSE(P_ATR_EVT_FETCH_ERR, "pcie read request error"),
	LOCAL_EVENT_CAUSE(P_ATR_EVT_DISCARD_ERR, "pcie read timeout"),
	LOCAL_EVENT_CAUSE(PM_MSI_INT_AER_EVT, "aer event"),
	LOCAL_EVENT_CAUSE(PM_MSI_INT_EVENTS, "pm/ltr/hotplug event"),
	LOCAL_EVENT_CAUSE(PM_MSI_INT_SYS_ERR, "system error"),
};

static struct event_map pcie_event_to_event[] = {
	PCIE_EVENT_TO_EVENT_MAP(L2_EXIT),
	PCIE_EVENT_TO_EVENT_MAP(HOTRST_EXIT),
	PCIE_EVENT_TO_EVENT_MAP(DLUP_EXIT),
};

static struct event_map sec_error_to_event[] = {
	SEC_ERROR_TO_EVENT_MAP(TX_RAM_SEC_ERR),
	SEC_ERROR_TO_EVENT_MAP(RX_RAM_SEC_ERR),
	SEC_ERROR_TO_EVENT_MAP(PCIE2AXI_RAM_SEC_ERR),
	SEC_ERROR_TO_EVENT_MAP(AXI2PCIE_RAM_SEC_ERR),
};

static struct event_map ded_error_to_event[] = {
	DED_ERROR_TO_EVENT_MAP(TX_RAM_DED_ERR),
	DED_ERROR_TO_EVENT_MAP(RX_RAM_DED_ERR),
	DED_ERROR_TO_EVENT_MAP(PCIE2AXI_RAM_DED_ERR),
	DED_ERROR_TO_EVENT_MAP(AXI2PCIE_RAM_DED_ERR),
};

static struct event_map local_status_to_event[] = {
	LOCAL_STATUS_TO_EVENT_MAP(DMA_END_ENGINE_0),
	LOCAL_STATUS_TO_EVENT_MAP(DMA_END_ENGINE_1),
	LOCAL_STATUS_TO_EVENT_MAP(DMA_ERROR_ENGINE_0),
	LOCAL_STATUS_TO_EVENT_MAP(DMA_ERROR_ENGINE_1),
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_POST_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_FETCH_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_DISCARD_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(A_ATR_EVT_DOORBELL),
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_POST_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_FETCH_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_DISCARD_ERR),
	LOCAL_STATUS_TO_EVENT_MAP(P_ATR_EVT_DOORBELL),
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_INTX),
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_MSI),
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_AER_EVT),
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_EVENTS),
	LOCAL_STATUS_TO_EVENT_MAP(PM_MSI_INT_SYS_ERR),
};

static struct {
	u32 offset;
	u32 mask;
	u32 shift;
	u32 enb_mask;
	u32 mask_high;
	u32 mask_offset;
} event_descs[] = {
	{ PCIE_EVENT(L2_EXIT) },
	{ PCIE_EVENT(HOTRST_EXIT) },
	{ PCIE_EVENT(DLUP_EXIT) },
	{ SEC_EVENT(TX_RAM_SEC_ERR) },
	{ SEC_EVENT(RX_RAM_SEC_ERR) },
	{ SEC_EVENT(PCIE2AXI_RAM_SEC_ERR) },
	{ SEC_EVENT(AXI2PCIE_RAM_SEC_ERR) },
	{ DED_EVENT(TX_RAM_DED_ERR) },
	{ DED_EVENT(RX_RAM_DED_ERR) },
	{ DED_EVENT(PCIE2AXI_RAM_DED_ERR) },
	{ DED_EVENT(AXI2PCIE_RAM_DED_ERR) },
	{ LOCAL_EVENT(DMA_END_ENGINE_0) },
	{ LOCAL_EVENT(DMA_END_ENGINE_1) },
	{ LOCAL_EVENT(DMA_ERROR_ENGINE_0) },
	{ LOCAL_EVENT(DMA_ERROR_ENGINE_1) },
	{ LOCAL_EVENT(A_ATR_EVT_POST_ERR) },
	{ LOCAL_EVENT(A_ATR_EVT_FETCH_ERR) },
	{ LOCAL_EVENT(A_ATR_EVT_DISCARD_ERR) },
	{ LOCAL_EVENT(A_ATR_EVT_DOORBELL) },
	{ LOCAL_EVENT(P_ATR_EVT_POST_ERR) },
	{ LOCAL_EVENT(P_ATR_EVT_FETCH_ERR) },
	{ LOCAL_EVENT(P_ATR_EVT_DISCARD_ERR) },
	{ LOCAL_EVENT(P_ATR_EVT_DOORBELL) },
	{ LOCAL_EVENT(PM_MSI_INT_INTX) },
	{ LOCAL_EVENT(PM_MSI_INT_MSI) },
	{ LOCAL_EVENT(PM_MSI_INT_AER_EVT) },
	{ LOCAL_EVENT(PM_MSI_INT_EVENTS) },
	{ LOCAL_EVENT(PM_MSI_INT_SYS_ERR) },
};

static char poss_clks[][5] = { "fic0", "fic1", "fic2", "fic3" };

static struct mc_pcie *port;

static void mc_pcie_enable_msi(struct mc_pcie *port, void __iomem *ecam)
{
	struct mc_msi *msi = &port->msi;
	u16 reg;
	u8 queue_size;

	/* Fixup MSI enable flag */
	reg = readw_relaxed(ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);
	reg |= PCI_MSI_FLAGS_ENABLE;
	writew_relaxed(reg, ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);

	/* Fixup PCI MSI queue flags */
	queue_size = FIELD_GET(PCI_MSI_FLAGS_QMASK, reg);
	reg |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, queue_size);
	writew_relaxed(reg, ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_FLAGS);

	/* Fixup MSI addr fields */
	writel_relaxed(lower_32_bits(msi->vector_phy),
		       ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_ADDRESS_LO);
	writel_relaxed(upper_32_bits(msi->vector_phy),
		       ecam + MC_MSI_CAP_CTRL_OFFSET + PCI_MSI_ADDRESS_HI);
}

static void mc_handle_msi(struct irq_desc *desc)
{
	struct mc_pcie *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct device *dev = port->dev;
	struct mc_msi *msi = &port->msi;
	unsigned long status;
	u32 bit;
	int ret;

	chained_irq_enter(chip, desc);

	status = readl_relaxed(port->bridge_base_addr + ISTATUS_LOCAL);
	if (status & PM_MSI_INT_MSI_MASK) {
		writel_relaxed(status & PM_MSI_INT_MSI_MASK,
			       port->bridge_base_addr + ISTATUS_LOCAL);
		status = readl_relaxed(port->bridge_base_addr + ISTATUS_MSI);
		for_each_set_bit(bit, &status, msi->num_vectors) {
			ret = generic_handle_domain_irq(msi->dev_domain, bit);
			if (ret)
				dev_err_ratelimited(dev, "bad MSI IRQ %d\n",
						    bit);
		}
	}

	chained_irq_exit(chip, desc);
}

static void mc_msi_bottom_irq_ack(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	u32 bitpos = data->hwirq;

	writel_relaxed(BIT(bitpos), port->bridge_base_addr + ISTATUS_MSI);
}

static void mc_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	phys_addr_t addr = port->msi.vector_phy;

	msg->address_lo = lower_32_bits(addr);
	msg->address_hi = upper_32_bits(addr);
	msg->data = data->hwirq;

	dev_dbg(port->dev, "msi#%x address_hi %#x address_lo %#x\n",
		(int)data->hwirq, msg->address_hi, msg->address_lo);
}

static int mc_msi_set_affinity(struct irq_data *irq_data,
			       const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static struct irq_chip mc_msi_bottom_irq_chip = {
	.name = "Microchip MSI",
	.irq_ack = mc_msi_bottom_irq_ack,
	.irq_compose_msi_msg = mc_compose_msi_msg,
	.irq_set_affinity = mc_msi_set_affinity,
};

static int mc_irq_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	struct mc_pcie *port = domain->host_data;
	struct mc_msi *msi = &port->msi;
	unsigned long bit;

	mutex_lock(&msi->lock);
	bit = find_first_zero_bit(msi->used, msi->num_vectors);
	if (bit >= msi->num_vectors) {
		mutex_unlock(&msi->lock);
		return -ENOSPC;
	}

	set_bit(bit, msi->used);

	irq_domain_set_info(domain, virq, bit, &mc_msi_bottom_irq_chip,
			    domain->host_data, handle_edge_irq, NULL, NULL);

	mutex_unlock(&msi->lock);

	return 0;
}

static void mc_irq_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct mc_pcie *port = irq_data_get_irq_chip_data(d);
	struct mc_msi *msi = &port->msi;

	mutex_lock(&msi->lock);

	if (test_bit(d->hwirq, msi->used))
		__clear_bit(d->hwirq, msi->used);
	else
		dev_err(port->dev, "trying to free unused MSI%lu\n", d->hwirq);

	mutex_unlock(&msi->lock);
}

static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= mc_irq_msi_domain_alloc,
	.free	= mc_irq_msi_domain_free,
};

static struct irq_chip mc_msi_irq_chip = {
	.name = "Microchip PCIe MSI",
	.irq_ack = irq_chip_ack_parent,
	.irq_mask = pci_msi_mask_irq,
	.irq_unmask = pci_msi_unmask_irq,
};

static struct msi_domain_info mc_msi_domain_info = {
	.flags = (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_PCI_MSIX),
	.chip = &mc_msi_irq_chip,
};

static int mc_allocate_msi_domains(struct mc_pcie *port)
{
	struct device *dev = port->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);
	struct mc_msi *msi = &port->msi;

	mutex_init(&port->msi.lock);

	msi->dev_domain = irq_domain_add_linear(NULL, msi->num_vectors,
						&msi_domain_ops, port);
	if (!msi->dev_domain) {
		dev_err(dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}

	msi->msi_domain = pci_msi_create_irq_domain(fwnode, &mc_msi_domain_info,
						    msi->dev_domain);
	if (!msi->msi_domain) {
		dev_err(dev, "failed to create MSI domain\n");
		irq_domain_remove(msi->dev_domain);
		return -ENOMEM;
	}

	return 0;
}

static void mc_handle_intx(struct irq_desc *desc)
{
	struct mc_pcie *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct device *dev = port->dev;
	unsigned long status;
	u32 bit;
	int ret;

	chained_irq_enter(chip, desc);

	status = readl_relaxed(port->bridge_base_addr + ISTATUS_LOCAL);
	if (status & PM_MSI_INT_INTX_MASK) {
		status &= PM_MSI_INT_INTX_MASK;
		status >>= PM_MSI_INT_INTX_SHIFT;
		for_each_set_bit(bit, &status, PCI_NUM_INTX) {
			ret = generic_handle_domain_irq(port->intx_domain, bit);
			if (ret)
				dev_err_ratelimited(dev, "bad INTx IRQ %d\n",
						    bit);
		}
	}

	chained_irq_exit(chip, desc);
}

static void mc_ack_intx_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);

	writel_relaxed(mask, port->bridge_base_addr + ISTATUS_LOCAL);
}

static void mc_mask_intx_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	unsigned long flags;
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	val = readl_relaxed(port->bridge_base_addr + IMASK_LOCAL);
	val &= ~mask;
	writel_relaxed(val, port->bridge_base_addr + IMASK_LOCAL);
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static void mc_unmask_intx_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	unsigned long flags;
	u32 mask = BIT(data->hwirq + PM_MSI_INT_INTX_SHIFT);
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	val = readl_relaxed(port->bridge_base_addr + IMASK_LOCAL);
	val |= mask;
	writel_relaxed(val, port->bridge_base_addr + IMASK_LOCAL);
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static struct irq_chip mc_intx_irq_chip = {
	.name = "Microchip PCIe INTx",
	.irq_ack = mc_ack_intx_irq,
	.irq_mask = mc_mask_intx_irq,
	.irq_unmask = mc_unmask_intx_irq,
};

static int mc_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			    irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &mc_intx_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = mc_pcie_intx_map,
};

static inline u32 reg_to_event(u32 reg, struct event_map field)
{
	return (reg & field.reg_mask) ? BIT(field.event_bit) : 0;
}

static u32 pcie_events(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + PCIE_EVENT_INT);
	u32 val = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(pcie_event_to_event); i++)
		val |= reg_to_event(reg, pcie_event_to_event[i]);

	return val;
}

static u32 sec_errors(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + SEC_ERROR_INT);
	u32 val = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(sec_error_to_event); i++)
		val |= reg_to_event(reg, sec_error_to_event[i]);

	return val;
}

static u32 ded_errors(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->ctrl_base_addr + DED_ERROR_INT);
	u32 val = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(ded_error_to_event); i++)
		val |= reg_to_event(reg, ded_error_to_event[i]);

	return val;
}

static u32 local_events(struct mc_pcie *port)
{
	u32 reg = readl_relaxed(port->bridge_base_addr + ISTATUS_LOCAL);
	u32 val = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(local_status_to_event); i++)
		val |= reg_to_event(reg, local_status_to_event[i]);

	return val;
}

static u32 get_events(struct mc_pcie *port)
{
	u32 events = 0;

	events |= pcie_events(port);
	events |= sec_errors(port);
	events |= ded_errors(port);
	events |= local_events(port);

	return events;
}

static irqreturn_t mc_event_handler(int irq, void *dev_id)
{
	struct mc_pcie *port = dev_id;
	struct device *dev = port->dev;
	struct irq_data *data;

	data = irq_domain_get_irq_data(port->event_domain, irq);

	if (event_cause[data->hwirq].str)
		dev_err_ratelimited(dev, "%s\n", event_cause[data->hwirq].str);
	else
		dev_err_ratelimited(dev, "bad event IRQ %ld\n", data->hwirq);

	return IRQ_HANDLED;
}

static void mc_handle_event(struct irq_desc *desc)
{
	struct mc_pcie *port = irq_desc_get_handler_data(desc);
	unsigned long events;
	u32 bit;
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);

	events = get_events(port);

	for_each_set_bit(bit, &events, NUM_EVENTS)
		generic_handle_domain_irq(port->event_domain, bit);

	chained_irq_exit(chip, desc);
}

static void mc_ack_event_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	u32 event = data->hwirq;
	void __iomem *addr;
	u32 mask;

	if (event_descs[event].offset == ISTATUS_LOCAL)
		addr = port->bridge_base_addr;
	else
		addr = port->ctrl_base_addr;

	addr += event_descs[event].offset;
	mask = event_descs[event].mask;
	mask |= event_descs[event].enb_mask;

	writel_relaxed(mask, addr);
}

static void mc_mask_event_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	u32 event = data->hwirq;
	void __iomem *addr;
	u32 mask;
	u32 val;

	if (event_descs[event].offset == ISTATUS_LOCAL)
		addr = port->bridge_base_addr;
	else
		addr = port->ctrl_base_addr;

	addr += event_descs[event].mask_offset;
	mask = event_descs[event].mask;
	if (event_descs[event].enb_mask) {
		mask <<= PCIE_EVENT_INT_ENB_SHIFT;
		mask &= PCIE_EVENT_INT_ENB_MASK;
	}

	if (!event_descs[event].mask_high)
		mask = ~mask;

	raw_spin_lock(&port->lock);
	val = readl_relaxed(addr);
	if (event_descs[event].mask_high)
		val |= mask;
	else
		val &= mask;

	writel_relaxed(val, addr);
	raw_spin_unlock(&port->lock);
}

static void mc_unmask_event_irq(struct irq_data *data)
{
	struct mc_pcie *port = irq_data_get_irq_chip_data(data);
	u32 event = data->hwirq;
	void __iomem *addr;
	u32 mask;
	u32 val;

	if (event_descs[event].offset == ISTATUS_LOCAL)
		addr = port->bridge_base_addr;
	else
		addr = port->ctrl_base_addr;

	addr += event_descs[event].mask_offset;
	mask = event_descs[event].mask;

	if (event_descs[event].enb_mask)
		mask <<= PCIE_EVENT_INT_ENB_SHIFT;

	if (event_descs[event].mask_high)
		mask = ~mask;

	if (event_descs[event].enb_mask)
		mask &= PCIE_EVENT_INT_ENB_MASK;

	raw_spin_lock(&port->lock);
	val = readl_relaxed(addr);
	if (event_descs[event].mask_high)
		val &= mask;
	else
		val |= mask;
	writel_relaxed(val, addr);
	raw_spin_unlock(&port->lock);
}

static struct irq_chip mc_event_irq_chip = {
	.name = "Microchip PCIe EVENT",
	.irq_ack = mc_ack_event_irq,
	.irq_mask = mc_mask_event_irq,
	.irq_unmask = mc_unmask_event_irq,
};

static int mc_pcie_event_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &mc_event_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops event_domain_ops = {
	.map = mc_pcie_event_map,
};

static inline void mc_pcie_deinit_clk(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
}

static inline struct clk *mc_pcie_init_clk(struct device *dev, const char *id)
{
	struct clk *clk;
	int ret;

	clk = devm_clk_get_optional(dev, id);
	if (IS_ERR(clk))
		return clk;
	if (!clk)
		return clk;

	ret = clk_prepare_enable(clk);
	if (ret)
		return ERR_PTR(ret);

	devm_add_action_or_reset(dev, mc_pcie_deinit_clk, clk);

	return clk;
}

static int mc_pcie_init_clks(struct device *dev)
{
	int i;
	struct clk *fic;

	/*
	 * PCIe may be clocked via Fabric Interface using between 1 and 4
	 * clocks. Scan DT for clocks and enable them if present
	 */
	for (i = 0; i < ARRAY_SIZE(poss_clks); i++) {
		fic = mc_pcie_init_clk(dev, poss_clks[i]);
		if (IS_ERR(fic))
			return PTR_ERR(fic);
	}

	return 0;
}

static int mc_pcie_init_irq_domains(struct mc_pcie *port)
{
	struct device *dev = port->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node;

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL);
	if (!pcie_intc_node) {
		dev_err(dev, "failed to find PCIe Intc node\n");
		return -EINVAL;
	}

	port->event_domain = irq_domain_add_linear(pcie_intc_node, NUM_EVENTS,
						   &event_domain_ops, port);
	if (!port->event_domain) {
		dev_err(dev, "failed to get event domain\n");
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	irq_domain_update_bus_token(port->event_domain, DOMAIN_BUS_NEXUS);

	port->intx_domain = irq_domain_add_linear(pcie_intc_node, PCI_NUM_INTX,
						  &intx_domain_ops, port);
	if (!port->intx_domain) {
		dev_err(dev, "failed to get an INTx IRQ domain\n");
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	irq_domain_update_bus_token(port->intx_domain, DOMAIN_BUS_WIRED);

	of_node_put(pcie_intc_node);
	raw_spin_lock_init(&port->lock);

	return mc_allocate_msi_domains(port);
}

static int mc_pcie_setup_inbound_ranges(struct platform_device *pdev, struct mc_pcie *port)
{
	phys_addr_t pcie_addr;
	phys_addr_t axi_addr;
	u32 atr_size;
	u32 val;
	int i;

	for (i = 0; i < port->num_inbound_windows; i++) {
		atr_size = ilog2(port->inbound_windows[i].size) - 1;
		atr_size &= GENMASK(5, 0);

		pcie_addr = port->inbound_windows[i].pci_addr;

		val = lower_32_bits(pcie_addr) & GENMASK(31, 12);
		val |= (atr_size << ATR_SIZE_SHIFT);
		val |= ATR_IMPL_ENABLE;
		writel(val, port->bridge_base_addr +
		       ATR0_PCIE_WIN0_SRCADDR_PARAM + (i * ATR_WINDOW_DESC_SIZE));
		writel(upper_32_bits(pcie_addr), port->bridge_base_addr +
		       ATR0_PCIE_WIN0_SRC_ADDR + (i * ATR_WINDOW_DESC_SIZE));

		axi_addr = port->inbound_windows[i].axi_addr;

		writel(lower_32_bits(axi_addr), port->bridge_base_addr +
		       ATR0_PCIE_WIN0_TRSL_ADDR_LSB + (i * ATR_WINDOW_DESC_SIZE));
		writel(upper_32_bits(axi_addr), port->bridge_base_addr +
		       ATR0_PCIE_WIN0_TRSL_ADDR_UDW + (i * ATR_WINDOW_DESC_SIZE));

		writel(TRSL_ID_AXI4_MASTER_0, port->bridge_base_addr +
		       ATR0_PCIE_WIN0_TRSL_PARAM + (i * ATR_WINDOW_DESC_SIZE));
	}

	return 0;
}

static void mc_pcie_setup_window(void __iomem *bridge_base_addr, u32 index,
				 phys_addr_t axi_addr, phys_addr_t pci_addr,
				 size_t size)
{
	u32 atr_size = ilog2(size) - 1;
	u32 val;

	if (index == 0)
		val = TRSL_ID_PCIE_CONFIG;
	else
		val = TRSL_ID_PCIE_TXRX;

	writel(val, bridge_base_addr + (index * ATR_WINDOW_DESC_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_PARAM);

	val = lower_32_bits(axi_addr) | (atr_size << ATR_SIZE_SHIFT) |
			    ATR_IMPL_ENABLE;
	writel(val, bridge_base_addr + (index * ATR_WINDOW_DESC_SIZE) +
	       ATR0_AXI4_SLV0_SRCADDR_PARAM);

	val = upper_32_bits(axi_addr);
	writel(val, bridge_base_addr + (index * ATR_WINDOW_DESC_SIZE) +
	       ATR0_AXI4_SLV0_SRC_ADDR);

	val = lower_32_bits(pci_addr);
	writel(val, bridge_base_addr + (index * ATR_WINDOW_DESC_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_ADDR_LSB);

	val = upper_32_bits(pci_addr);
	writel(val, bridge_base_addr + (index * ATR_WINDOW_DESC_SIZE) +
	       ATR0_AXI4_SLV0_TRSL_ADDR_UDW);
}

static int mc_pcie_setup_windows(struct platform_device *pdev,
				 struct mc_pcie *port)
{
	struct pci_host_bridge *bridge = platform_get_drvdata(pdev);
	struct resource_entry *entry;
	u64 pci_addr;
	u32 index = 1; /* Window 0 used for config space */

	resource_list_for_each_entry(entry, &bridge->windows) {
		if (resource_type(entry->res) == IORESOURCE_MEM) {
			pci_addr = entry->res->start - entry->offset;
			mc_pcie_setup_window(port->bridge_base_addr, index,
					     entry->res->start - port->outbound_range_offset,
					     pci_addr, resource_size(entry->res));
			index++;
		}
	}

	return 0;
}

static inline void mc_clear_secs(struct mc_pcie *port)
{
	writel_relaxed(SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT, port->ctrl_base_addr +
		       SEC_ERROR_INT);
	writel_relaxed(0, port->ctrl_base_addr + SEC_ERROR_EVENT_CNT);
}

static inline void mc_clear_deds(struct mc_pcie *port)
{
	writel_relaxed(DED_ERROR_INT_ALL_RAM_DED_ERR_INT, port->ctrl_base_addr +
		       DED_ERROR_INT);
	writel_relaxed(0, port->ctrl_base_addr + DED_ERROR_EVENT_CNT);
}

static void mc_disable_interrupts(struct mc_pcie *port)
{
	u32 val;

	/* Ensure ECC bypass is enabled */
	val = ECC_CONTROL_TX_RAM_ECC_BYPASS |
	      ECC_CONTROL_RX_RAM_ECC_BYPASS |
	      ECC_CONTROL_PCIE2AXI_RAM_ECC_BYPASS |
	      ECC_CONTROL_AXI2PCIE_RAM_ECC_BYPASS;
	writel_relaxed(val, port->ctrl_base_addr + ECC_CONTROL);

	/* Disable SEC errors and clear any outstanding */
	writel_relaxed(SEC_ERROR_INT_ALL_RAM_SEC_ERR_INT, port->ctrl_base_addr +
		       SEC_ERROR_INT_MASK);
	mc_clear_secs(port);

	/* Disable DED errors and clear any outstanding */
	writel_relaxed(DED_ERROR_INT_ALL_RAM_DED_ERR_INT, port->ctrl_base_addr +
		       DED_ERROR_INT_MASK);
	mc_clear_deds(port);

	/* Disable local interrupts and clear any outstanding */
	writel_relaxed(0, port->bridge_base_addr + IMASK_LOCAL);
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_LOCAL);
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_MSI);

	/* Disable PCIe events and clear any outstanding */
	val = PCIE_EVENT_INT_L2_EXIT_INT |
	      PCIE_EVENT_INT_HOTRST_EXIT_INT |
	      PCIE_EVENT_INT_DLUP_EXIT_INT |
	      PCIE_EVENT_INT_L2_EXIT_INT_MASK |
	      PCIE_EVENT_INT_HOTRST_EXIT_INT_MASK |
	      PCIE_EVENT_INT_DLUP_EXIT_INT_MASK;
	writel_relaxed(val, port->ctrl_base_addr + PCIE_EVENT_INT);

	/* Disable host interrupts and clear any outstanding */
	writel_relaxed(0, port->bridge_base_addr + IMASK_HOST);
	writel_relaxed(GENMASK(31, 0), port->bridge_base_addr + ISTATUS_HOST);
}

static int mc_init_interrupts(struct platform_device *pdev, struct mc_pcie *port)
{
	struct device *dev = &pdev->dev;
	int irq;
	int i, intx_irq, msi_irq, event_irq;
	int ret;

	ret = mc_pcie_init_irq_domains(port);
	if (ret) {
		dev_err(dev, "failed creating IRQ domains\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -ENODEV;

	for (i = 0; i < NUM_EVENTS; i++) {
		event_irq = irq_create_mapping(port->event_domain, i);
		if (!event_irq) {
			dev_err(dev, "failed to map hwirq %d\n", i);
			return -ENXIO;
		}

		ret = devm_request_irq(dev, event_irq, mc_event_handler,
				       0, event_cause[i].sym, port);
		if (ret) {
			dev_err(dev, "failed to request IRQ %d\n", event_irq);
			return ret;
		}
	}

	intx_irq = irq_create_mapping(port->event_domain,
				      EVENT_LOCAL_PM_MSI_INT_INTX);
	if (!intx_irq) {
		dev_err(dev, "failed to map INTx interrupt\n");
		return -ENXIO;
	}

	/* Plug the INTx chained handler */
	irq_set_chained_handler_and_data(intx_irq, mc_handle_intx, port);

	msi_irq = irq_create_mapping(port->event_domain,
				     EVENT_LOCAL_PM_MSI_INT_MSI);
	if (!msi_irq)
		return -ENXIO;

	/* Plug the MSI chained handler */
	irq_set_chained_handler_and_data(msi_irq, mc_handle_msi, port);

	/* Plug the main event chained handler */
	irq_set_chained_handler_and_data(irq, mc_handle_event, port);

	return 0;
}

static int mc_check_for_parent_range_handling(struct platform_device *pdev, struct mc_pcie *port)
{
	struct device *dev = &pdev->dev;
	struct device_node *dn = dev->of_node;
	struct of_range_parser parser;
	struct of_range range;
	u64 cpu_addr;

	/* Find any pcie range */
	if (of_range_parser_init(&parser, dn)) {
		dev_err(dev, "missing ranges property\n");
		return -EINVAL;
	}

	for_each_of_range(&parser, &range) {
		cpu_addr = range.cpu_addr;
		/*
		 * First range is enough - extend if anyone ever needs more
		 * than one fabric interface
		 */
		break;
	}

	/* Check for one level up; that is enough */
	dn = of_get_parent(dn);
	if (dn) {
		of_range_parser_init(&parser, dn);
		for_each_of_range(&parser, &range) {
			/* Find the parent range that contains cpu_addr */
			if (range.cpu_addr > port->outbound_range_offset &&
			    range.cpu_addr < cpu_addr)
				port->outbound_range_offset = range.cpu_addr;
		}
	}

	return 0;
}

static int mc_check_for_parent_dma_range_handling(struct platform_device *pdev,
						  struct mc_pcie *port)
{
	struct device *dev = &pdev->dev;
	struct device_node *dn = dev->of_node;
	struct of_range_parser parser;
	struct of_range range;
	int num_parent_ranges = 0;
	int num_ranges = 0;
	struct inbound_windows ranges[MC_MAX_NUM_INBOUND_WINDOWS] = { 0 };
	u64 start_axi = GENMASK_ULL(63, 0);
	u64 end_axi = 0;
	u64 start_pci = GENMASK_ULL(63, 0);
	s64 size;
	u64 window_size;
	int i;

	/* Find all dma-ranges */
	if (of_pci_dma_range_parser_init(&parser, dn)) {
		dev_err(dev, "missing dma-ranges property\n");
		return -EINVAL;
	}

	for_each_of_range(&parser, &range) {
		if (num_ranges > MC_MAX_NUM_INBOUND_WINDOWS) {
			dev_err(dev, "too many inbound ranges; %d available tables\n",
				MC_MAX_NUM_INBOUND_WINDOWS);
			return -EINVAL;
		}
		ranges[num_ranges].axi_addr = range.cpu_addr;
		ranges[num_ranges].pci_addr = range.pci_addr;
		ranges[num_ranges].size = range.size;

		num_ranges++;
	}

	/*
	 * Check for one level up; will need to adjust address translation
	 * tables for these
	 */
	dn = of_get_parent(dn);
	if (dn) {
		of_pci_dma_range_parser_init(&parser, dn);

		for_each_of_range(&parser, &range) {
			if (num_parent_ranges > MC_MAX_NUM_INBOUND_WINDOWS) {
				dev_err(dev, "too many parent inbound ranges; %d available tables\n",
					MC_MAX_NUM_INBOUND_WINDOWS);
				return -EINVAL;
			}
			ranges[num_parent_ranges].axi_addr = range.pci_addr;
			num_parent_ranges++;
		}
	}

	if (num_parent_ranges) {
		if (num_ranges != num_parent_ranges) {
			dev_err(dev, "num parent inbound ranges must be 0 or match num inbound ranges\n");
			return -EINVAL;
		}
	}

	/* Merge ranges */
	for (i = 0; i < num_ranges; i++) {
		struct inbound_windows *range = &ranges[i];

		if (range->axi_addr < start_axi) {
			start_axi = range->axi_addr;
			start_pci = range->pci_addr;
		}

		if (range->axi_addr + range->size > end_axi)
			end_axi = range->axi_addr + range->size;
	}

	/* Move starts back as far as possible */
	start_axi &= MC_ATT_MASK;
	start_pci &= MC_ATT_MASK;

	/* Adjust size to take account of that change */
	size = end_axi - start_axi;

	/* May need to adjust size up to the next largest power of 2 */
	if (size < 1ull << ilog2(size))
		size = 1ull << (ilog2(size) + 1);

	window_size = 1ull << (ilog2(size) - 1);

	/* Divide merged range into windows */
	i = 0;
	while (size > 0 && i < MC_MAX_NUM_INBOUND_WINDOWS) {
		port->inbound_windows[i].axi_addr = start_axi;
		port->inbound_windows[i].pci_addr = start_pci;
		port->inbound_windows[i].size = window_size;

		size -= window_size;
		start_axi += window_size;
		start_pci += window_size;
		i++;
		port->num_inbound_windows = i;
	}

	if (size < 0) {
		dev_err(dev, "insufficient windows to map inbound ranges\n");
		return -EINVAL;
	}

	return 0;
}

static int mc_platform_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct platform_device *pdev = to_platform_device(dev);
	int ret;

	/*
	 * Need information about any parent bus that may be performing some
	 * of the outbound address translation to setup outbound address
	 * translation tables later
	 */
	ret = mc_check_for_parent_range_handling(pdev, port);
	if (ret)
		return ret;

	/* And similarly, check for inbound address translation */
	ret = mc_check_for_parent_dma_range_handling(pdev, port);
	if (ret)
		return ret;

	/* Configure address translation table 0 for PCIe config space */
	mc_pcie_setup_window(port->bridge_base_addr, 0,
			     cfg->res.start - port->outbound_range_offset,
			     cfg->res.start - port->outbound_range_offset,
			     resource_size(&cfg->res));

	/* Need some fixups in config space */
	mc_pcie_enable_msi(port, cfg->win);

	/* Configure non-config space outbound ranges */
	ret = mc_pcie_setup_windows(pdev, port);
	if (ret)
		return ret;

	/* Configure inbound translation tables */
	ret = mc_pcie_setup_inbound_ranges(pdev, port);
	if (ret)
		return ret;

	/* Address translation is up; safe to enable interrupts */
	ret = mc_init_interrupts(pdev, port);
	if (ret)
		return ret;

	return 0;
}

static int mc_host_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *axi_base_addr;
	int ret;
	u32 val;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->dev = dev;

	port->bridge_base_addr = devm_platform_ioremap_resource_byname(pdev, "bridge");
	port->ctrl_base_addr = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if(!IS_ERR(port->bridge_base_addr) && !IS_ERR(port->ctrl_base_addr))
		goto addrs_set;

	/*
	 * The original, incorrect, binding that lumped the control and
	 * bridge addresses together still needs to be handled by the driver.
	 */
	axi_base_addr = devm_platform_ioremap_resource_byname(pdev, "apb");
	if (IS_ERR(axi_base_addr))
		return dev_err_probe(dev, PTR_ERR(port->bridge_base_addr),
				     "both legacy apb register and ctrl/bridge regions missing");

	port->bridge_base_addr = axi_base_addr + MC_PCIE1_BRIDGE_ADDR;
	port->ctrl_base_addr = axi_base_addr + MC_PCIE1_CTRL_ADDR;

addrs_set:
	mc_disable_interrupts(port);

	/* Allow enabling MSI by disabling MSI-X */
	val = readl(port->bridge_base_addr + PCIE_PCI_IRQ_DW0);
	val &= ~MSIX_CAP_MASK;
	writel(val, port->bridge_base_addr + PCIE_PCI_IRQ_DW0);

	/* Pick num vectors from bitfile programmed onto FPGA fabric */
	val = readl(port->bridge_base_addr + PCIE_PCI_IRQ_DW0);
	val &= NUM_MSI_MSGS_MASK;
	val >>= NUM_MSI_MSGS_SHIFT;

	port->msi.num_vectors = 1 << val;

	/* Pick vector address from design */
	port->msi.vector_phy = readl_relaxed(port->bridge_base_addr+ IMSI_ADDR);

	ret = mc_pcie_init_clks(dev);
	if (ret) {
		dev_err(dev, "failed to get clock resources, error %d\n", ret);
		return -ENODEV;
	}

	return pci_host_common_probe(pdev);
}

static const struct pci_ecam_ops mc_ecam_ops = {
	.init = mc_platform_init,
	.pci_ops = {
		.map_bus = pci_ecam_map_bus,
		.read = pci_generic_config_read,
		.write = pci_generic_config_write,
	}
};

static const struct of_device_id mc_pcie_of_match[] = {
	{
		.compatible = "microchip,pcie-host-1.0",
		.data = &mc_ecam_ops,
	},
	{},
};

MODULE_DEVICE_TABLE(of, mc_pcie_of_match);

static struct platform_driver mc_pcie_driver = {
	.probe = mc_host_probe,
	.driver = {
		.name = "microchip-pcie",
		.of_match_table = mc_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(mc_pcie_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Microchip PCIe host controller driver");
MODULE_AUTHOR("Daire McNamara <daire.mcnamara@microchip.com>");
