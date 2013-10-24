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
#include <linux/if_ether.h>
#include <linux/netdevice.h>

#include "main.h"
#include "netdev.h"

#define RING_SIZE 1024
#define MTU 2048

struct net_device *ip_dev;
uint32_t tx_inflight = 0;
uint32_t rx_inflight = 0;

static struct pci_device_id kvm_ivshmem_id_table[] = {
        { 0x1af4, 0x1120, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_ivshmem_id_table);

enum {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,		/* Doorbell */
	Init		= 0x04,		/* Initialize */
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
	volatile void		*skb;
	volatile uint64_t	data_ptr;
	volatile uint32_t	size;
	volatile uint32_t	flag;
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

        writel(0, plx_intscr);
	return 0;
}

static int init_host(struct kvm_ivshmem_device *ivs_info){
        void __iomem *plx_intscr = ivs_info->regs + Init;

	printk(KERN_INFO "Notifying init completion to host...\n");
        writel(0, plx_intscr);
        return 0;
}

static void kvm_ivshmem_txused(struct shared_region *sr){
	static uint32_t next_consume = 0;

	/* release processed buffer */
	sr->tx_used.ring_empty = 0;
	while(!sr->tx_used.ring_empty){
		struct sk_buff *skb = NULL;
		uint32_t size = 0;
		uint32_t flag = 0;
		int index = next_consume;

		flag	= sr->tx_used.buf[index].flag;
		__sync_synchronize();
		skb	= (struct sk_buff *)sr->tx_used.buf[index].skb;
		size	= sr->tx_used.buf[index].size;

		if(flag){
			next_consume = (index + 1) % RING_SIZE;
			sr->tx_used.buf[index].skb	= NULL;
			sr->tx_used.buf[index].data_ptr	= 0;
			sr->tx_used.buf[index].size	= 0;
			__sync_synchronize();
			sr->tx_used.buf[index].flag	= 0;

			tx_inflight--;
			kfree_skb(skb);

			ip_dev->stats.tx_packets++;
			ip_dev->stats.tx_bytes += size;
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
	if(!sr->tx.buf[next_prepare].flag && tx_inflight < RING_SIZE){
		uint64_t ptr = (uint64_t)virt_to_phys((volatile void *)skb->data);
		int index = next_prepare;

		next_prepare = (index + 1) % RING_SIZE;
		sr->tx.buf[index].data_ptr	= ptr;
		sr->tx.buf[index].skb		= (void *)skb;
		sr->tx.buf[index].size		= skb->len;
		__sync_synchronize();
		sr->tx.buf[index].flag		= 1;

		if(sr->tx.ring_empty){
			/* wake up host OS */
			kick_host(&ivs_info);
		}

		tx_inflight++;
	}else{
		printk(KERN_ERR "NVOIB_FATAL: tx buffer is full or inflight == RING_SIZE\n");
		//ret = -1;
		ret = 0;
	}

	kvm_ivshmem_txused(sr);

	return ret;
}

static void kvm_ivshmem_rxavail(struct shared_region *sr){
	static uint32_t next_prepare = 0;

	/* prepare receive buffer */
	if(!sr->rx_avail.buf[next_prepare].flag && rx_inflight < RING_SIZE){
		struct sk_buff *skb = NULL;
		uint64_t data_ptr = 0;
		int index = next_prepare;

		skb = dev_alloc_skb(MTU + NET_IP_ALIGN);
		if(unlikely(!skb)){
			printk(KERN_ERR "NVOIB_FATAL: failed to get buffer\n");
			return;
		}

		next_prepare = (index + 1) % RING_SIZE;
		skb_reserve(skb, NET_IP_ALIGN);
		data_ptr = (uint64_t)virt_to_phys((volatile void *)skb->data);
		sr->rx_avail.buf[index].data_ptr	= data_ptr;
		sr->rx_avail.buf[index].skb		= (void *)skb;
		sr->rx_avail.buf[index].size		= MTU;
		__sync_synchronize();
		sr->rx_avail.buf[index].flag		= 1;

		rx_inflight++;
	}

	return;
}

static irqreturn_t kvm_ivshmem_rx (int irq, void *dev_instance){
        struct kvm_ivshmem_device *ivs_info = dev_instance;
	struct shared_region *sr = ivs_info->base_addr;
	static uint32_t next_consume = 0;
	int ret = IRQ_HANDLED;
uint64_t test;
static int count = 0;

	/* process received buffer */
	sr->rx.ring_empty = 0;
	while(!sr->rx.ring_empty){
		struct sk_buff *skb = NULL;
		uint32_t size = 0;
		uint32_t flag = 0;
		int index = next_consume;

		flag	= sr->rx.buf[index].flag;
		__sync_synchronize();
		skb	= (struct sk_buff *)sr->rx.buf[index].skb;
		size	= sr->rx.buf[index].size;

		if(flag){
			next_consume = (index + 1) % RING_SIZE;
			sr->rx.buf[index].skb		= NULL;
			sr->rx.buf[index].data_ptr	= 0;
			sr->rx.buf[index].size		= 0;
			__sync_synchronize();
			sr->rx.buf[index].flag		= 0;

			rx_inflight--;

			/* packet injection process */
test = (uint64_t)skb;
if(test == 0){
printk(KERN_INFO "skb = %p, count = %d, size = %d\n", (void *)skb, count, size);
}
count++;
			//skb_put(skb, size);
/*
			switch(skb->data[0] & 0xf0){
				case 0x40:
					skb->protocol = htons(ETH_P_IP);
					break;
				case 0x60:
					skb->protocol = htons(ETH_P_IPV6);
					break;
				default:
					printk(KERN_ERR "NVOIB_FATAL: unknown protocol\n");
					break;
			}
*/

/*
			skb_reset_mac_header(skb);
			skb_reset_network_header(skb);

			skb->dev = ip_dev;
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			netif_rx(skb);
*/
dev_kfree_skb(skb);

			ip_dev->stats.rx_packets++;
			ip_dev->stats.rx_bytes += size;

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
		
		skb = dev_alloc_skb(MTU + NET_IP_ALIGN);
		if(unlikely(!skb)){
			printk(KERN_ERR "NVOIB_FATAL: failed to get buffer\n");
			return -1;
		}

		skb_reserve(skb, NET_IP_ALIGN);
		data_ptr = (uint64_t)virt_to_phys((volatile void *)skb->data);

		sr->rx_avail.buf[i].data_ptr = data_ptr;
		sr->rx_avail.buf[i].skb = (void *)skb;
		sr->rx_avail.buf[i].size = MTU;
		sr->rx_avail.buf[i].flag = 1;
	}

	rx_inflight = RING_SIZE;

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
	init_host(&ivs_info);
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
		printk(KERN_INFO "vector = %d requested\n", ivs_info->msix_entries[i].vector);
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
