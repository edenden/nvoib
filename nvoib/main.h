#define PCI_VENDOR_ID_IVSHMEM   PCI_VENDOR_ID_REDHAT_QUMRANET
#define PCI_DEVICE_ID_IVSHMEM   0x1120
#define IVSHMEM_REG_BAR_SIZE 0x100

#define TYPE_RDMANIC "rdmanic"
#define IVSHMEM(obj) \
	OBJECT_CHECK(struct rdmanic, (obj), TYPE_RDMANIC)

#define IVSHMEM_DPRINTF(fmt, ...)        \
	do {printf("IVSHMEM: " fmt, ## __VA_ARGS__); } while (0)

struct rdmanic {
	PCIDevice parent_obj;

	MemoryRegion ivshmem_mmio;

	/* We might need to register the BAR before we actually have the memory.
	 * So prepare a container MemoryRegion for the BAR immediately and
	 * add a subregion when we have the memory.
	 */
	MemoryRegion bar;
	MemoryRegion ivshmem;

	uint64_t ivshmem_size; /* size of shared memory region */
	uint64_t ram_size;	/* size of guest system ram(for RDMA MR register) */
	uint32_t ivshmem_attr;
	uint32_t ivshmem_64bit;
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
enum ivshmem_registers {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,    /* Doorbell */
};

