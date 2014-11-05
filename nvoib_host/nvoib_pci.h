#include "hw/pci/pci.h"

#define PCI_VENDOR_ID_NVOIB PCI_VENDOR_ID_REDHAT_QUMRANET
#define PCI_DEVICE_ID_NVOIB 0x1120
#define NVOIB_REG_BAR_SIZE 0x100

#define TYPE_NVOIB "nvoib"
#define NVOIB_DEV(obj) \
	OBJECT_CHECK(struct nvoib_dev, (obj), TYPE_NVOIB)

struct nvoib_dev {
	PCIDevice		parent_obj;

	MemoryRegion		nvoib_mmio;

	EventNotifier		rx_event;
	EventNotifier		tx_event;

	int			rx_remain;

	void			*shared_region;
	uint64_t		sr_guest_physical;
	uint32_t		vectors;
	uint32_t		tenant_id;
	void			*guest_memory;
	uint64_t		ram_size;

	char			*eth_addr_str;
	struct ether_addr	*eth_addr;

	/* EXPERIMENTAL */
	int			virq; /* KVM irqchip route for QEMU bypass */
};

/* registers for the Inter-VM shared memory device */
enum nvoib_registers {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,		/* Doorbell */
	Init		= 0x04,		/* Initialize */
	SregionTop	= 0x08,		/* Top-half of shared Region */
	SregionBottom	= 0x0c,		/* Bottom-half of shared region */
	EthaddrTop	= 0x10,		/* Top-half of ether address */
	EthaddrBottom	= 0x14,		/* Bottom-half of ether address */
};

