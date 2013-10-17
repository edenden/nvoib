#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <asm/io.h>
#include <linux/skbuff.h>
#include "main.h"
#include "netdev.h"

#define RING_SIZE 256

struct net_device *ip_dev;
uint32_t tx_balancer = 0;
uint32_t rx_balancer = 0;

static struct pci_device_id kvm_ivshmem_id_table[] = {
        { 0x1af4, 0x1120, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_ivshmem_id_table);

enum {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,    /* Doorbell */
};

struct kvm_ivshmem_device {
        void __iomem * regs;
        void * base_addr;

        unsigned int regaddr;
        unsigned int reg_size;

        unsigned int ioaddr;
        unsigned int ioaddr_size;

        struct pci_dev *dev;
        char (*msix_names)[256];
        struct msix_entry *msix_entries;
        int nvectors;
};

struct buf_data {
	void		*skb;
	uint64_t	data_ptr;
	uint32_t	size;
	uint32_t	flag;
};

struct ring_buf {
	struct buf_data buf[RING_SIZE];
	uint32_t	ring_empty;
};

struct shared_region {
	struct ring_buf	rx_avail;	/* Guest prepares empty buffer to 'rx_avail' from ZONE_DMA */
	struct ring_buf	rx;		/* Host writes received data to 'rx' using RDMA */
	struct ring_buf	tx;		/* Guest chains TX buffer to 'tx' */
	struct ring_buf tx_used;	/* Host chains processed buffer to 'tx_used' */
};

struct kvm_ivshmem_device ivs_info;

static int kick_host(struct kvm_ivshmem_device *ivs_info);
static void kvm_ivshmem_txused(struct shared_region *sr);
static irqreturn_t kvm_ivshmem_rx (int irq, void *dev_instance);
static void kvm_ivshmem_rxavail(struct shared_region *sr);
static int kvm_ivshmem_probe_device (struct pci_dev *pdev, const struct pci_device_id * ent);
static int request_msix_vectors(struct kvm_ivshmem_device *ivs_info, int nvectors);
static void free_msix_vectors(struct kvm_ivshmem_device *ivs_info, const int max_vector);
static void kvm_ivshmem_remove_device(struct pci_dev* pdev);


static struct pci_driver kvm_ivshmem_pci_driver = {
	.name		= "kvm-shmem",
	.id_table	= kvm_ivshmem_id_table,
	.probe		= kvm_ivshmem_probe_device,
	.remove		= kvm_ivshmem_remove_device,
};

static int kick_host(struct kvm_ivshmem_device *ivs_info){
        void __iomem *plx_intscr = ivs_info->regs + Doorbell;

printk(KERN_INFO "kicking host...\n");
        writel(0, plx_intscr);
	return 0;
}

static void kvm_ivshmem_txused(struct shared_region *sr){
	static uint32_t next_consume = 0;

	/* release processed buffer */
	sr->tx_used.ring_empty = 0;
	while(!sr->tx_used.ring_empty){
		struct sk_buff *skb = NULL;
		uint32_t flag = 0;
		int index = next_consume;

		skb = sr->tx_used.buf[index].skb;
		flag = sr->tx_used.buf[index].flag;
		if(flag){
			next_consume = (index + 1) % RING_SIZE;
			sr->tx_used.buf[index].skb = NULL;
			sr->tx_used.buf[index].data_ptr = 0;
			sr->tx_used.buf[index].size = 0;
			sr->tx_used.buf[index].flag = 0;

			tx_balancer--;
			kfree_skb(skb);
		}

		sr->tx_used.ring_empty = !sr->tx_used.buf[next_consume].flag;
	}

	return;
}

int kvm_ivshmem_tx(struct sk_buff *skb){
	struct shared_region *sr = ivs_info.base_addr;
	static uint32_t next_prepare = 0;
	int ret = 0;

	/* add skb to tx ring buffer */
	if(!sr->tx.buf[next_prepare].flag && tx_balancer < RING_SIZE){
		uint64_t ptr = (uint64_t)virt_to_phys((volatile void *)skb->head);
		int index = next_prepare;

		next_prepare = (index + 1) % RING_SIZE;
		sr->tx.buf[index].data_ptr = ptr;
		sr->tx.buf[index].skb = (void *)skb;
		sr->tx.buf[index].size = skb->len;
		sr->tx.buf[index].flag = 1;

		if(sr->tx.ring_empty){
			/* wake up host OS */
			kick_host(&ivs_info);
		}

		tx_balancer++;
	}else{
		ret = -1;
	}

	kvm_ivshmem_txused(sr);

	return ret;
}

static void kvm_ivshmem_rxavail(struct shared_region *sr){
	static uint32_t next_prepare = 0;

	/* prepare receive buffer */
	if(!sr->rx_avail.buf[next_prepare].flag && rx_balancer < RING_SIZE){
		struct sk_buff *skb = NULL;
		uint64_t data_ptr = 0;
		int index = next_prepare;

		skb = alloc_skb(PAGE_SIZE, GFP_KERNEL);
		if(skb == NULL){
			printk(KERN_ERR "failed to get buffer\n");
			return;
		}

		next_prepare = (index + 1) % RING_SIZE;
		data_ptr = (uint64_t)virt_to_phys((volatile void *)skb->head);
		sr->rx_avail.buf[index].data_ptr = data_ptr;
		sr->rx_avail.buf[index].skb = (void *)skb;
		sr->rx_avail.buf[index].size = PAGE_SIZE;
		sr->rx_avail.buf[index].flag = 1;

		rx_balancer++;
	}

	return;
}

static irqreturn_t kvm_ivshmem_rx (int irq, void *dev_instance){
        struct kvm_ivshmem_device *ivs_info = dev_instance;
	struct shared_region *sr = ivs_info->base_addr;
	static uint32_t next_consume = 0;
	int ret = IRQ_HANDLED;

	/* process received buffer */
	sr->rx.ring_empty = 0;
	while(!sr->rx.ring_empty){
		struct sk_buff *skb = NULL;
		uint32_t flag = 0;
		int index = next_consume;

		skb = sr->rx.buf[index].skb;
		flag = sr->rx.buf[index].flag;
		if(flag){
			next_consume = (index + 1) % RING_SIZE;
			sr->rx.buf[index].skb = NULL;
			sr->rx.buf[index].data_ptr = 0;
			sr->rx.buf[index].size = 0;
			sr->rx.buf[index].flag = 0;

			/* packet injection process */
			rx_balancer--;
			kfree_skb(skb);
			/* here */

			kvm_ivshmem_rxavail(sr);
		}

		sr->rx.ring_empty = !sr->rx.buf[next_consume].flag;
	}

        return ret;
}

static int prepare_shared_region(struct kvm_ivshmem_device *ivs_info){
	struct shared_region *sr = ivs_info->base_addr;
	int i;

	memset(ivs_info->base_addr, 0, sizeof(struct shared_region));
	sr->rx.ring_empty	= 1;
	sr->rx_avail.ring_empty	= 1;
	sr->tx.ring_empty	= 1;
	sr->tx_used.ring_empty	= 1;

	for(i = 0; i < RING_SIZE; i++){
		struct sk_buff *skb = NULL;
		uint64_t data_ptr = 0;
		
		skb = alloc_skb(PAGE_SIZE, GFP_KERNEL);
		if(skb == NULL){
			printk(KERN_ERR "failed to get buffer\n");
			return -1;
		}

		data_ptr = (uint64_t)virt_to_phys((volatile void *)skb->head);
		sr->rx_avail.buf[i].data_ptr = data_ptr;
		sr->rx_avail.buf[i].skb = (void *)skb;
		sr->rx_avail.buf[i].size = PAGE_SIZE;
		sr->rx_avail.buf[i].flag = 1;
	}

	rx_balancer = RING_SIZE;

	return 0;
}

static int kvm_ivshmem_probe_device (struct pci_dev *pdev, const struct pci_device_id * ent) {
	int result;

	printk(KERN_INFO "IVSHMEM_NIC: Probing for PCI Device\n");

	result = pci_enable_device(pdev);
	if (result) {
		printk(KERN_ERR "IVSHMEM_NIC: Cannot probe PCI device %s: error %d\n",
			pci_name(pdev), result);
		return result;
	}

	result = pci_request_regions(pdev, "kvm_ivshmem");
	if (result < 0) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot request regions\n");
		goto pci_disable;
	}

	ivs_info.ioaddr = pci_resource_start(pdev, 2);
	ivs_info.ioaddr_size = pci_resource_len(pdev, 2);
	ivs_info.base_addr = ioremap_cache(ivs_info.ioaddr, ivs_info.ioaddr_size);

	if (!ivs_info.base_addr) {
		printk(KERN_ERR "IVSHMEM_NIC: Cannot iomap region of size %d\n", ivs_info.ioaddr_size);
		goto pci_release;
	}

	prepare_shared_region(&ivs_info);

	ivs_info.regaddr =  pci_resource_start(pdev, 0);
	ivs_info.reg_size = pci_resource_len(pdev, 0);
	ivs_info.regs = pci_ioremap_bar(pdev, 0);

	ivs_info.dev = pdev;

	if (!ivs_info.regs) {
		printk(KERN_ERR "IVSHMEM_NIC: Cannot ioremap registers of size %d\n", ivs_info.reg_size);
		goto reg_release;
	}

	if (request_msix_vectors(&ivs_info, 4) != 0) {
		printk(KERN_INFO "IVSHMEM_NIC: MSI-X disabled\n");
		goto reg_release;
	}

	pci_set_drvdata(pdev, &ivs_info);
	return 0;

reg_release:
	pci_iounmap(pdev, ivs_info.base_addr);
pci_release:
	pci_release_regions(pdev);
pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;

}

static int request_msix_vectors(struct kvm_ivshmem_device *ivs_info, int nvectors){
	int i, err;
	const char *name = "ivshmem";

	ivs_info->nvectors = nvectors;
	ivs_info->msix_entries = kmalloc(nvectors * sizeof(struct msix_entry), GFP_KERNEL);
        if (ivs_info->msix_entries == NULL){
                return -ENOSPC;
	}

	ivs_info->msix_names = kmalloc(nvectors * 256, GFP_KERNEL);
        if (ivs_info->msix_names == NULL) {
                kfree(ivs_info->msix_entries);
                return -ENOSPC;
        }

	memset(ivs_info->msix_entries, 0, nvectors * sizeof(struct msix_entry));
	memset(ivs_info->msix_names, 0, nvectors * 256);

	for (i = 0; i < nvectors; ++i){
		ivs_info->msix_entries[i].entry = i;
	}

	err = pci_enable_msix(ivs_info->dev, ivs_info->msix_entries, ivs_info->nvectors);
	if (err > 0) {
                ivs_info->nvectors = err; /* msi-x positive error code returns the number available */
                err = pci_enable_msix(ivs_info->dev, ivs_info->msix_entries, ivs_info->nvectors);
                if(err){
                        printk(KERN_INFO "IVSHMEM_NIC: no MSI (%d).\n", err);
                        goto error;
                }
	}

	printk(KERN_INFO "IVSHMEM_NIC: Succeed to enable MSI-X.\n");

	for (i = 0; i < ivs_info->nvectors; i++) {
		snprintf(ivs_info->msix_names[i], sizeof(*ivs_info->msix_names), "%s-config", name);
		err = request_irq(ivs_info->msix_entries[i].vector, kvm_ivshmem_rx, 0,
			ivs_info->msix_names[i], ivs_info);

		if (err) {
			printk(KERN_INFO "IVSHMEM_NIC: Unable to get irq = %d.\n", err);
			free_msix_vectors(ivs_info, i);
                        goto error;
		}
	}

	printk(KERN_INFO "IVSHMEM_NIC: Succeed to get IRQ.\n");

	return 0;

error:
        kfree(ivs_info->msix_entries);
        kfree(ivs_info->msix_names);
        return err;
}

static void free_msix_vectors(struct kvm_ivshmem_device *ivs_info, const int max_vector){
        int i;

        for (i = 0; i < max_vector; i++){
                free_irq(ivs_info->msix_entries[i].vector, ivs_info);
	}
}

static void kvm_ivshmem_remove_device(struct pci_dev* pdev){
	struct kvm_ivshmem_device *dev_info;

	dev_info = pci_get_drvdata(pdev);
	pci_set_drvdata(pdev, NULL);
	printk(KERN_INFO "IVSHMEM_NIC: Unregister kvm_ivshmem device.\n");

        pci_iounmap(pdev, dev_info->regs);
        pci_iounmap(pdev, dev_info->base_addr);
	free_msix_vectors(dev_info, dev_info->nvectors);
	pci_disable_msix(pdev);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
}

static void __exit kvm_ivshmem_cleanup_module (void){
	netdev_destroy(ip_dev);
        pci_unregister_driver (&kvm_ivshmem_pci_driver);
}

static int __init kvm_ivshmem_init_module (void){
        int err = -ENOMEM;

        err = pci_register_driver(&kvm_ivshmem_pci_driver);
        if (err < 0) {
		return -1;
        }

       	/* netdev is created here */
	err = netdev_create(&ip_dev);
	if(err){
		return -1;
	}

        return 0;
}

module_init(kvm_ivshmem_init_module);
module_exit(kvm_ivshmem_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yukito Ueno <eden@sfc.wide.ad.jp>");
MODULE_DESCRIPTION("KVM ivshmem based NIC driver");
MODULE_VERSION("1.0");
