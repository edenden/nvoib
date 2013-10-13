#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/msix.h"
#include "sysemu/kvm.h"
#include "migration/migration.h"
#include "qapi/qmp/qerror.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <pthread.h>
#include <mqueue.h>

#include "main.h"

static void ivshmem_io_write(void *opaque, hwaddr addr, uint64_t val, unsigned size){
	//struct rdmanic *s = opaque;
	addr &= 0xff;

	switch (addr){
		case Doorbell:
			/* check that dest VM ID is reasonable */
			if (val & 0xffff == 0) {
				if(mq_send(s->tx_mq, &val, sizeof(uint32_t), 10) != 0){
					IVSHMEM_DPRINTF("failed to send message queue\n");
				}
			}
			break;

		default:
			IVSHMEM_DPRINTF("Invalid VM Doorbell VM\n");
			break;
	}
}

static uint64_t ivshmem_io_read(void *opaque, hwaddr addr, unsigned size){
	//struct rdmanic *s = opaque;
	uint32_t ret;

	switch (addr){
		default:
			IVSHMEM_DPRINTF("why are we reading " TARGET_FMT_plx "\n", addr);
			ret = 0;
	}

	return ret;
}

static const MemoryRegionOps ivshmem_mmio_ops = {
	.read = ivshmem_io_read,
	.write = ivshmem_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.impl = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static int check_shm_size(struct rdmanic *s, int fd) {
	/* check that the guest isn't going to try and map more memory than the
	 * the object has allocated return -1 to indicate error */

	struct stat buf;

	fstat(fd, &buf);

	if (s->ivshmem_size > buf.st_size) {
		fprintf(stderr,
				"IVSHMEM ERROR: Requested memory size greater"
				" than shared object size (%" PRIu64 " > %" PRIu64")\n",
				s->ivshmem_size, (uint64_t)buf.st_size);
		return -1;
	} else {
		return 0;
	}
}

/* create the shared memory BAR when we are not using the server, so we can
 * create the BAR and map the memory immediately */
static void create_shared_memory_BAR(struct rdmanic *s, void *ptr) {

	memory_region_init_ram_ptr(&s->ivshmem, OBJECT(s), "rdmanic.bar2", s->ivshmem_size, ptr);
	vmstate_register_ram(&s->ivshmem, DEVICE(s));
	memory_region_add_subregion(&s->bar, 0, &s->ivshmem);

	/* region for shared memory */
	pci_register_bar(PCI_DEVICE(s), 2, s->ivshmem_attr, &s->bar);
}

/* Select the MSI-X vectors used by device.
 * ivshmem maps events to vectors statically, so
 * we just enable all vectors on init and after reset. */
static void ivshmem_use_msix(struct rdmanic * s){
	PCIDevice *d = PCI_DEVICE(s);
	int i;

	if (!msix_present(d)) {
		return;
	}

	for (i = 0; i < s->vectors; i++) {
		msix_vector_use(d, i);
	}
}

static void ivshmem_reset(DeviceState *d){
	struct rdmanic *s = IVSHMEM(d);
	ivshmem_use_msix(s);
}

static inline bool is_power_of_two(uint64_t x) {
        return (x & (x - 1)) == 0;
}

static uint64_t ivshmem_get_size(char *sizestr) {

	uint64_t value;
	char *ptr;

	value = strtoull(sizestr, &ptr, 10);
	switch (*ptr) {
		case 0: case 'M': case 'm':
			value <<= 20;
			break;
		case 'G': case 'g':
			value <<= 30;
			break;
		default:
			fprintf(stderr, "qemu: invalid ram size: %s\n", s->sizearg);
			exit(1);
	}

	/* BARs must be a power of 2 */
	if (!is_power_of_two(value)) {
		fprintf(stderr, "ivshmem: size must be power of 2\n");
		exit(1);
	}

	return value;
}

static void ivshmem_setup_msi(struct rdmanic * s){
	if (msix_init_exclusive_bar(PCI_DEVICE(s), s->vectors, 1)) {
		IVSHMEM_DPRINTF("msix initialization failed\n");
		exit(1);
	}

	IVSHMEM_DPRINTF("msix initialized (%d vectors)\n", s->vectors);

	ivshmem_use_msix(s);
}

static void ivshmem_save(QEMUFile* f, void *opaque){
	struct rdmanic *proxy = opaque;
	PCIDevice *pci_dev = PCI_DEVICE(proxy);

	IVSHMEM_DPRINTF("ivshmem_save\n");
	pci_device_save(pci_dev, f);
	msix_save(pci_dev, f);

}

static int ivshmem_load(QEMUFile* f, void *opaque, int version_id){
	IVSHMEM_DPRINTF("ivshmem_load\n");

	struct rdmanic *proxy = opaque;
	PCIDevice *pci_dev = PCI_DEVICE(proxy);
	int ret;

	if (version_id > 0) {
		return -EINVAL;
	}

	ret = pci_device_load(pci_dev, f);
	if (ret) {
		return ret;
	}

	msix_load(pci_dev, f);
	ivshmem_use_msix(proxy);

	return 0;
}

static void ivshmem_write_config(PCIDevice *pci_dev, uint32_t address, uint32_t val, int len){
	pci_default_write_config(pci_dev, address, val, len);
	msix_write_config(pci_dev, address, val, len);
}

static int pci_ivshmem_thread(struct rdmanic *s){
	pthread_t rdma_thread;

	if(pthread_create(&rdma_thread, NULL, rdma_event_handling, s) != 0){
		return -1;
	}

	return 0;
}

static int pci_ivshmem_init(PCIDevice *dev){
	struct rdmanic *s = IVSHMEM(dev);
	uint8_t *pci_conf;
	int fd;

	if(s->sizearg == NULL)
		s->ivshmem_size = 4 << 20; /* 4 MB default */
	else {
		s->ivshmem_size = ivshmem_get_size(s->sizearg);
	}

	if(s->sysmemsizearg == NULL){
		fprintf(stderr, "Must specify size of guest memory using '-ramsize' option\n");
		exit(1);
	}else{
		s->ram_size = ivshmem_get_size(s->ramsizearg);
	}

	register_savevm(DEVICE(dev), "rdmanic", 0, 0, ivshmem_save, ivshmem_load, dev);

	pci_conf = dev->config;
	pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

	pci_config_set_interrupt_pin(pci_conf, 1);

	s->shm_fd = 0;

	memory_region_init_io(&s->ivshmem_mmio, OBJECT(s), &ivshmem_mmio_ops, s,
						  "rdmanic-mmio", IVSHMEM_REG_BAR_SIZE);

	/* region for registers*/
	pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ivshmem_mmio);

	memory_region_init(&s->bar, OBJECT(s), "rdmanic-bar2-container", s->ivshmem_size);
	s->ivshmem_attr = PCI_BASE_ADDRESS_SPACE_MEMORY |
		PCI_BASE_ADDRESS_MEM_PREFETCH;
	if (s->ivshmem_64bit) {
		s->ivshmem_attr |= PCI_BASE_ADDRESS_MEM_TYPE_64;
	}

	/* just map the file immediately, we're not using a server */
	if (s->shmobj == NULL) {
		fprintf(stderr, "Must specify 'chardev' or 'shm' to rdmanic\n");
		exit(1);
	}

	IVSHMEM_DPRINTF("using shm_open (shm object = %s)\n", s->shmobj);

	/* try opening with O_EXCL and if it succeeds zero the memory
	 * by truncating to 0 */
	if ((fd = shm_open(s->shmobj, O_CREAT|O_RDWR|O_EXCL, S_IRWXU|S_IRWXG|S_IRWXO)) > 0) {
		/* truncate file to length PCI device's memory */
		if (ftruncate(fd, s->ivshmem_size) != 0) {
			fprintf(stderr, "ivshmem: could not truncate shared file\n");
		}
	} else if ((fd = shm_open(s->shmobj, O_CREAT|O_RDWR,
					S_IRWXU|S_IRWXG|S_IRWXO)) < 0) {
		fprintf(stderr, "ivshmem: could not open shared file\n");
		exit(-1);

	}

	if (check_shm_size(s, fd) == -1) {
		exit(-1);
	}

        s->shm_fd = fd;
        s->shm_ptr = mmap(0, s->ivshmem_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	create_shared_memory_BAR(s, s->shm_ptr);

	ivshmem_setup_msi(s);
	dev->config_write = ivshmem_write_config;

	s->guest_memory = cpu_physical_memory_map((hwaddr)0x0, (hwaddr *)&(s->ram_size), 1);
	if(s->guest_memory == NULL){
		fprintf(stderr, "ivshmem: could not map guest physical to host virtual\n");
		exit(-1);
	}

	IVSHMEM_DPRINTF("guest physical 0x000000 is host virtual %p\n", s->guest_memory);

	s->tx_mq = mq_open("/tx_mq", O_RDWR | O_CREAT);
	if(s->tx_mq < 0){
		fprintf(stderr, "ivshmem: could not open message queue\n");
                exit(-1);
	}

	pci_ivshmem_thread(s);

	return 0;
}

static void pci_ivshmem_uninit(PCIDevice *dev){
	struct rdmanic *s = IVSHMEM(dev);

	memory_region_destroy(&s->ivshmem_mmio);
	memory_region_del_subregion(&s->bar, &s->ivshmem);
	vmstate_unregister_ram(&s->ivshmem, DEVICE(dev));
	memory_region_destroy(&s->ivshmem);
	memory_region_destroy(&s->bar);
	unregister_savevm(DEVICE(dev), "rdmanic", s);
	mq_close(s->tx_mq);
}

static Property ivshmem_properties[] = {
	DEFINE_PROP_STRING("size", struct rdmanic, sizearg),
	DEFINE_PROP_UINT32("vectors", struct rdmanic, vectors, 1),
	DEFINE_PROP_STRING("shm", struct rdmanic, shmobj),
	DEFINE_PROP_UINT32("use64", struct rdmanic, ivshmem_64bit, 1),
	DEFINE_PROP_STRING("ramsize", struct rdmanic, ramsizearg),
	DEFINE_PROP_END_OF_LIST(),
};

static void ivshmem_class_init(ObjectClass *klass, void *data){
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->init = pci_ivshmem_init;
	k->exit = pci_ivshmem_uninit;
	k->vendor_id = PCI_VENDOR_ID_IVSHMEM;
	k->device_id = PCI_DEVICE_ID_IVSHMEM;
	k->class_id = PCI_CLASS_MEMORY_RAM;
	dc->reset = ivshmem_reset;
	dc->props = ivshmem_properties;
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo rdmanic_info = {
	.name		  = TYPE_RDMANIC,
	.parent		= TYPE_PCI_DEVICE,
	.instance_size = sizeof(struct rdmanic),
	.class_init	= ivshmem_class_init,
};

static void ivshmem_register_types(void){
	type_register_static(&rdmanic_info);
}

type_init(ivshmem_register_types)
