#include <sys/mman.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <rdma/rdma_cma.h>
#include <pthread.h>
#include <mqueue.h>

#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/msix.h"
#include "sysemu/kvm.h"
#include "migration/migration.h"
#include "qapi/qmp/qerror.h"
#include "exec/cpu-common.h"

#include "nvoib.h"
#include "rdma_event.h"

static void nvoib_io_write(void *opaque, hwaddr addr, uint64_t reg_val, unsigned size){
	struct nvoib_dev *pci_dev = opaque;
	uint64_t ev_val = 1;

	addr &= 0xff;

	switch (addr){
		case Doorbell:
			/* check that dest VM ID is reasonable */
			if((reg_val & 0xffff) == 0){
				if(write(pci_dev->tx_fd, &ev_val, sizeof(uint64_t)) < 0){
					printf("failed to send eventfd\n");
				}
				dprintf("Doorbell occured\n");
			}
			break;

		case Init:
			if((reg_val & 0xffff) == 0){
				pci_nvoib_thread(pci_dev);
				dprintf("Init occured\n");
			}
			break;

		default:
			printf("Invalid VM Doorbell VM\n");
			break;
	}
}

static uint64_t nvoib_io_read(void *opaque, hwaddr addr, unsigned size){
	//struct nvoib_dev *s = opaque;
	uint32_t ret;

	switch (addr){
		default:
			printf("why are we reading " TARGET_FMT_plx "\n", addr);
			ret = 0;
	}

	return ret;
}

static const MemoryRegionOps nvoib_mmio_ops = {
	.read = nvoib_io_read,
	.write = nvoib_io_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.impl = {
		.min_access_size = 4,
		.max_access_size = 4,
	},
};

static int check_shm_size(struct nvoib_dev *s, int fd) {
	/* check that the guest isn't going to try and map more memory than the
	 * the object has allocated return -1 to indicate error */

	struct stat buf;

	fstat(fd, &buf);

	if (s->nvoib_size > buf.st_size) {
		fprintf(stderr, "NVOIB ERROR: Requested memory size is greater than shared object size\n");
		return -1;
	} else {
		return 0;
	}
}

/* create the shared memory BAR when we are not using the server, so we can
 * create the BAR and map the memory immediately */
static void create_shared_memory_BAR(struct nvoib_dev *s, void *ptr) {

	memory_region_init_ram_ptr(&s->nvoib, OBJECT(s), "nvoib_dev.bar2", s->nvoib_size, ptr);
	vmstate_register_ram(&s->nvoib, DEVICE(s));
	memory_region_add_subregion(&s->bar, 0, &s->nvoib);

	/* region for shared memory */
	pci_register_bar(PCI_DEVICE(s), 2, s->nvoib_attr, &s->bar);
}

/* Select the MSI-X vectors used by device.
 * nvoib maps events to vectors statically, so
 * we just enable all vectors on init and after reset. */
static void nvoib_use_msix(struct nvoib_dev * s){
	PCIDevice *d = PCI_DEVICE(s);
	int i;

	if (!msix_present(d)) {
		return;
	}

	for (i = 0; i < s->vectors; i++) {
		msix_vector_use(d, i);
	}
}

static void nvoib_reset(DeviceState *d){
	struct nvoib_dev *s = NVOIBDEV(d);
	nvoib_use_msix(s);
}

static inline bool is_power_of_two(uint64_t x) {
        return (x & (x - 1)) == 0;
}

static uint64_t nvoib_get_size(char *sizestr) {
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
			fprintf(stderr, "qemu: invalid ram size: %s\n", sizestr);
			exit(1);
	}

	/* BARs must be a power of 2 */
	if (!is_power_of_two(value)) {
		fprintf(stderr, "nvoib: size must be power of 2\n");
		exit(1);
	}

	return value;
}

static void nvoib_setup_msi(struct nvoib_dev * s){
	if (msix_init_exclusive_bar(PCI_DEVICE(s), s->vectors, 1)) {
		printf("msix initialization failed\n");
		exit(1);
	}

	printf("msix initialized (%d vectors)\n", s->vectors);

	nvoib_use_msix(s);
}

static void nvoib_save(QEMUFile* f, void *opaque){
	struct nvoib_dev *proxy = opaque;
	PCIDevice *pci_dev = PCI_DEVICE(proxy);

	printf("nvoib_save\n");
	pci_device_save(pci_dev, f);
	msix_save(pci_dev, f);

}

static int nvoib_load(QEMUFile* f, void *opaque, int version_id){
	printf("nvoib_load\n");

	struct nvoib_dev *proxy = opaque;
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
	nvoib_use_msix(proxy);

	return 0;
}

static void nvoib_write_config(PCIDevice *pci_dev, uint32_t address, uint32_t val, int len){
	pci_default_write_config(pci_dev, address, val, len);
	msix_write_config(pci_dev, address, val, len);
}

static int pci_nvoib_thread(struct nvoib_dev *s){
	pthread_t rxwait_thread;
	pthread_t txwait_thread;

	if(pthread_create(&rxwait_thread, NULL, rx_wait, s) != 0){
		return -1;
	}

        if(pthread_create(&txwait_thread, NULL, tx_wait, s) != 0){
                return -1;
        }

	return 0;
}

static void pci_nvoib_set_memory(void *host_addr, ram_addr_t offset, ram_addr_t length, void *opaque){
	struct nvoib_dev *s = (struct nvoib_dev *)opaque;

	if(offset == 0){
		s->guest_memory = host_addr;
		s->ram_size = length;
	}
}

static void pci_nvoib_rx(void *opaque){
	struct nvoib_dev *s = (struct nvoib_dev *)opaque;
	msix_notify(PCI_DEVICE(s), 1);
	return;
}

static int pci_nvoib_init(PCIDevice *dev){
	struct nvoib_dev *s = NVOIBDEV(dev);
	uint8_t *pci_conf;
	int fd;

	if(s->sizearg == NULL)
		s->nvoib_size = 4 << 20; /* 4 MB default */
	else {
		s->nvoib_size = nvoib_get_size(s->sizearg);
	}

	register_savevm(DEVICE(dev), "nvoib_dev", 0, 0, nvoib_save, nvoib_load, dev);

	pci_conf = dev->config;
	pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

	pci_config_set_interrupt_pin(pci_conf, 1);

	s->shm_fd = 0;

	memory_region_init_io(&s->nvoib_mmio, OBJECT(s), &nvoib_mmio_ops, s,
						  "nvoib_dev-mmio", NVOIB_REG_BAR_SIZE);

	/* region for registers*/
	pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->nvoib_mmio);

	memory_region_init(&s->bar, OBJECT(s), "nvoib_dev-bar2-container", s->nvoib_size);
	s->nvoib_attr = PCI_BASE_ADDRESS_SPACE_MEMORY |
		PCI_BASE_ADDRESS_MEM_PREFETCH;
	if (s->nvoib_64bit) {
		s->nvoib_attr |= PCI_BASE_ADDRESS_MEM_TYPE_64;
	}

	/* just map the file immediately, we're not using a server */
	if (s->shmobj == NULL) {
		fprintf(stderr, "Must specify 'chardev' or 'shm' to nvoib_dev\n");
		exit(1);
	}

	printf("using shm_open (shm object = %s)\n", s->shmobj);

	/* try opening with O_EXCL and if it succeeds zero the memory
	 * by truncating to 0 */
	if ((fd = shm_open(s->shmobj, O_CREAT|O_RDWR|O_EXCL, S_IRWXU|S_IRWXG|S_IRWXO)) > 0) {
		/* truncate file to length PCI device's memory */
		if (ftruncate(fd, s->nvoib_size) != 0) {
			fprintf(stderr, "nvoib: could not truncate shared file\n");
		}
	} else if ((fd = shm_open(s->shmobj, O_CREAT|O_RDWR,
					S_IRWXU|S_IRWXG|S_IRWXO)) < 0) {
		fprintf(stderr, "nvoib: could not open shared file\n");
		exit(-1);

	}

	if (check_shm_size(s, fd) == -1) {
		exit(-1);
	}

        s->shm_fd = fd;
        s->shm_ptr = mmap(0, s->nvoib_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	create_shared_memory_BAR(s, s->shm_ptr);

	nvoib_setup_msi(s);
	dev->config_write = nvoib_write_config;

	qemu_ram_foreach_block(pci_nvoib_set_memory, s);
	if(s->guest_memory == NULL){
		fprintf(stderr, "nvoib: could not map guest physical to host virtual\n");
		exit(-1);
	}

	printf("guest physical 0x0 is host virtual %p, size = %llu\n", s->guest_memory, (long long unsigned)s->ram_size);

	s->tx_fd = eventfd(0, 0);

	if(event_notifier_init(&s->rx_event, 0)){
		fprintf(stderr, "nvoib: could not init event_notifier\n");
		exit(-1);
	}

	s->rx_fd = event_notifier_get_fd(&s->rx_event);
	qemu_set_fd_handler(s->rx_fd, pci_nvoib_rx, NULL, s);

	return 0;
}

static void pci_nvoib_uninit(PCIDevice *dev){
	struct nvoib_dev *s = NVOIBDEV(dev);

	qemu_set_fd_handler(s->rx_fd, NULL, NULL, s);
	event_notifier_cleanup(&s->rx_event);

	memory_region_destroy(&s->nvoib_mmio);
	memory_region_del_subregion(&s->bar, &s->nvoib);
	vmstate_unregister_ram(&s->nvoib, DEVICE(dev));
	memory_region_destroy(&s->nvoib);
	memory_region_destroy(&s->bar);
	unregister_savevm(DEVICE(dev), "nvoib_dev", s);
}

static Property nvoib_properties[] = {
	DEFINE_PROP_STRING("size", struct nvoib_dev, sizearg),
	DEFINE_PROP_UINT32("vectors", struct nvoib_dev, vectors, 1),
	DEFINE_PROP_STRING("shm", struct nvoib_dev, shmobj),
	DEFINE_PROP_UINT32("use64", struct nvoib_dev, nvoib_64bit, 1),
	DEFINE_PROP_END_OF_LIST(),
};

static void nvoib_class_init(ObjectClass *klass, void *data){
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->init = pci_nvoib_init;
	k->exit = pci_nvoib_uninit;
	k->vendor_id = PCI_VENDOR_ID_NVOIB;
	k->device_id = PCI_DEVICE_ID_NVOIB;
	k->class_id = PCI_CLASS_MEMORY_RAM;
	dc->reset = nvoib_reset;
	dc->props = nvoib_properties;
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo nvoib_dev_info = {
	.name		  = TYPE_NVOIB,
	.parent		= TYPE_PCI_DEVICE,
	.instance_size = sizeof(struct nvoib_dev),
	.class_init	= nvoib_class_init,
};

static void nvoib_register_types(void){
	type_register_static(&nvoib_dev_info);
}

type_init(nvoib_register_types)
