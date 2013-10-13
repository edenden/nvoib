#define PCI_VENDOR_ID_NVOIB PCI_VENDOR_ID_REDHAT_QUMRANET
#define PCI_DEVICE_ID_NVOIB 0x1120
#define NVOIB_REG_BAR_SIZE 0x100

#define TYPE_NVOIB "nvoib"
#define NVOIBDEV(obj) \
	OBJECT_CHECK(struct nvoib_dev, (obj), TYPE_NVOIB)

struct nvoib_dev {
	PCIDevice parent_obj;

	MemoryRegion nvoib_mmio;

	/* We might need to register the BAR before we actually have the memory.
	 * So prepare a container MemoryRegion for the BAR immediately and
	 * add a subregion when we have the memory.
	 */
	MemoryRegion bar;
	MemoryRegion nvoib;

	uint64_t nvoib_size; /* size of shared memory region */
	uint64_t ram_size;	/* size of guest system ram(for RDMA MR register) */
	uint32_t nvoib_attr;
	uint32_t nvoib_64bit;
	int shm_fd; /* shared memory file descriptor */
	void *shm_ptr;
	uint32_t vectors;
	void *guest_memory;
	mqd_t tx_mq;

	char *shmobj;
	char *sizearg;
	char *ramsizearg;
};

/* registers for the Inter-VM shared memory device */
enum nvoib_registers {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,    /* Doorbell */
};

