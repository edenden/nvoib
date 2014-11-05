#include <sys/mman.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include <netinet/ether.h>
#include <mqueue.h>

#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "sysemu/kvm.h"
#include "migration/migration.h"
#include "qapi/qmp/qerror.h"
#include "exec/cpu-common.h"

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

static int pci_nvoib_thread(struct nvoib_dev *dev);

static void nvoib_io_write(void *opaque, hwaddr addr, uint64_t reg_val, unsigned size){
	struct nvoib_dev *pci_dev = opaque;

	addr &= 0xff;

	switch (addr){
		case Init:
			if((reg_val & 0xffff) == 0){
				nvoib_kick_enable(pci_dev);
				pci_nvoib_thread(pci_dev);
				dprintf("MAIN: Initialization request from guest\n");
			}
			break;

		case SregionTop:
			pci_dev->sr_guest_physical = 0;
			((uint32_t *)&pci_dev->sr_guest_physical)[0] = (uint32_t)reg_val;
			break;

		case SregionBottom:
			((uint32_t *)&pci_dev->sr_guest_physical)[1] = (uint32_t)reg_val;
			pci_dev->shared_region =
				(void *)(pci_dev->sr_guest_physical + (uint64_t)pci_dev->guest_memory);
			dprintf("MAIN: shared_region = %p\n", (void *)pci_dev->sr_guest_physical);
			break;

		default:
			dprintf("MAIN: Invalid MMIO write address = " TARGET_FMT_plx "\n", addr);
			break;
	}
}

static uint64_t nvoib_io_read(void *opaque, hwaddr addr, unsigned size){
	struct nvoib_dev *dev = opaque;
	uint32_t ret = 0;

	addr &= 0xff;

	switch (addr){
		case EthaddrTop:
			memcpy(&ret, &(dev->eth_addr->ether_addr_octet[0]), 3);
			break;

		case EthaddrBottom:
			memcpy(&ret, &(dev->eth_addr->ether_addr_octet[3]), 3);
			break;

		default:
			dprintf("MAIN: Invalid MMIO read address = " TARGET_FMT_plx "\n", addr);
			break;
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

static int nvoib_msix_vector_use(PCIDevice *pdev, unsigned int vector, MSIMessage msg){
	struct nvoib_dev *dev;

	dprintf("MAIN: nvoib_msix_vector_use called\n");
	dev = NVOIB_DEV(pdev);
	msix_vector_use(pdev, vector);

	dev->virq = kvm_irqchip_add_msi_route(kvm_state, msg);
	if(kvm_irqchip_add_irqfd_notifier(kvm_state, &dev->rx_event, dev->virq) < 0){
		kvm_irqchip_release_virq(kvm_state, dev->virq);
	}

	return 0;
}

static void nvoib_msix_vector_release(PCIDevice *pdev, unsigned int vector){
	struct nvoib_dev *dev;

	dprintf("MAIN: nvoib_msix_vector_release called\n");
	dev = NVOIB_DEV(pdev);
	msix_vector_unuse(pdev, vector);

        kvm_irqchip_remove_irqfd_notifier(kvm_state, &dev->rx_event, dev->virq);
        kvm_irqchip_release_virq(kvm_state, dev->virq);

	return;
}

static void nvoib_enable_msix(PCIDevice *pdev){
	struct nvoib_dev *dev = NVOIB_DEV(pdev);

        if (msix_init_exclusive_bar(pdev, dev->vectors, 1)) {
                printf("MAIN: msix initialization failed\n");
                exit(EXIT_FAILURE);
        }

	if (msix_set_vector_notifiers(pdev, nvoib_msix_vector_use,
		nvoib_msix_vector_release, NULL)) {
		printf("MAIN: failed to msix_set_vector_notifiers\n");
		exit(EXIT_FAILURE);
	}

	dprintf("MAIN: nvoib_enable_msix called\n");
	return;
}

static void nvoib_reset(DeviceState *dev_state){
	struct nvoib_dev *dev = NVOIB_DEV(dev_state);
	nvoib_enable_msix(PCI_DEVICE(dev));
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
	nvoib_enable_msix(pci_dev);

	return 0;
}

static int pci_nvoib_thread(struct nvoib_dev *dev){
	struct session *ss;
	struct thread_param *param;
	pthread_t rxwait_thread;
	pthread_t txwait_thread;
	char mq_path[256];

	if(dev->shared_region == NULL){
		printf("shared region is not initialized\n");
		return -1;
	}

	ss = session_init(dev);

	sprintf(mq_path, "/%s", dev->eth_addr_str);
	mq_unlink(mq_path);
	ss->mq_fd = mq_open(mq_path, O_RDWR | O_CREAT, S_IRWXU | S_IRWXO, NULL);
	dprintf("MAIN: mq_fd = %d, mq_path = %s\n", (int)ss->mq_fd, mq_path);

        param = malloc(sizeof(struct thread_param));
        param->ss	= ss;
        param->dev	= dev;

	if(pthread_create(&rxwait_thread, NULL, rx_wait, param) != 0){
		return -1;
	}
	printf("MAIN: RX waiting thread created\n");

        if(pthread_create(&txwait_thread, NULL, tx_wait, param) != 0){
                return -1;
        }
	printf("MAIN: TX waiting thread created\n");

	return 0;
}

static void pci_nvoib_set_memory(void *host_addr, ram_addr_t offset, ram_addr_t length, void *opaque){
	struct nvoib_dev *s = (struct nvoib_dev *)opaque;

	if(offset == 0){
		s->guest_memory = host_addr;
		s->ram_size = length;
	}
}

static int pci_nvoib_init(PCIDevice *pdev){
	struct nvoib_dev *dev = NVOIB_DEV(pdev);
	uint8_t *pci_conf;

	register_savevm(DEVICE(pdev), "nvoib_dev", 0, 0, nvoib_save, nvoib_load, pdev);

	pci_conf = pdev->config;
	pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&dev->nvoib_mmio, OBJECT(dev), &nvoib_mmio_ops, dev,
						  "nvoib_dev-mmio", NVOIB_REG_BAR_SIZE);

	/* region for registers*/
	pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &dev->nvoib_mmio);

	dev->shared_region = NULL;
	dev->rx_remain = 0;

	dev->eth_addr = ether_aton((const char *)dev->eth_addr_str);
	if(dev->eth_addr == NULL){
		printf("MAIN: could not parse eth_addr\n");
		exit(EXIT_FAILURE);
	}

	qemu_ram_foreach_block(pci_nvoib_set_memory, dev);
	if(dev->guest_memory == NULL){
		printf("MAIN: could not map guest physical to host virtual\n");
		exit(EXIT_FAILURE);
	}

	printf("MAIN: guest physical 0x0 is host virtual %p, size = %llu\n",
		dev->guest_memory, (long long unsigned)dev->ram_size);

        if(event_notifier_init(&dev->tx_event, 0)){
                printf("MAIN: could not init event_notifier\n");
		exit(EXIT_FAILURE);
        }

	memory_region_add_eventfd(&dev->nvoib_mmio, Doorbell, 4, false, 0, &dev->tx_event);

	if(event_notifier_init(&dev->rx_event, 0)){
		printf("MAIN: could not init event_notifier\n");
		exit(EXIT_FAILURE);
	}

	dev->vectors = 1; /* currently only 1 msix vector is used */
	nvoib_enable_msix(pdev);
	pdev->config_write = pci_default_write_config;

	return 0;
}

static void pci_nvoib_uninit(PCIDevice *dev){
	struct nvoib_dev *s = NVOIB_DEV(dev);

	event_notifier_cleanup(&s->rx_event);

	memory_region_destroy(&s->nvoib_mmio);
	unregister_savevm(DEVICE(dev), "nvoib_dev", s);
}

static Property nvoib_properties[] = {
	DEFINE_PROP_HEX32("tenant", struct nvoib_dev, tenant_id, 1),
	DEFINE_PROP_STRING("ethaddr", struct nvoib_dev, eth_addr_str),
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
