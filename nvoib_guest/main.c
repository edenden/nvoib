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
#include <linux/etherdevice.h>
#include <asm/barrier.h>

#include "main.h"
#include "netdev.h"

struct net_device *ip_dev;

static struct pci_device_id kvm_ivshmem_id_table[] = {
        { 0x1af4, 0x1120, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
        { 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_ivshmem_id_table);

enum {
        /* KVM Inter-VM shared memory device register offsets */
        Doorbell        = 0x00,		/* Doorbell */
	Init		= 0x04,		/* Initialize */
	SregionTop	= 0x08,		/* Top of shared region */
	SregionBottom	= 0x0c,		/* Bottom of shared region */
        EthaddrTop      = 0x10,         /* Top-half of ether address */
        EthaddrBottom   = 0x14,         /* Bottom-half of ether address */
};

struct kvm_ivshmem_device ivs_info;

static int kick_host(struct kvm_ivshmem_device *ivs_info);
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

static int notify_shared_region(struct kvm_ivshmem_device *ivs_info){
	void __iomem *plx_intscr;
	uint64_t offset;

	offset = (uint64_t)virt_to_phys((volatile void *)ivs_info->shared_region);
	printk(KERN_INFO "Notifying shared region buffer to host(%p)\n", (void *)offset);

	plx_intscr = ivs_info->regs + SregionTop;
	writel(((uint32_t *)&offset)[0], plx_intscr);

        plx_intscr = ivs_info->regs + SregionBottom;
        writel(((uint32_t *)&offset)[1], plx_intscr);

	return 0;
}

void nvoib_eth_addr(unsigned char *dev_addr){
	void __iomem *plx_intscr;
	uint32_t val;

	plx_intscr = ivs_info.regs + EthaddrTop;
	val = readl(plx_intscr);
	memcpy(&(dev_addr[0]), &val, 3);

	plx_intscr = ivs_info.regs + EthaddrBottom;
	val = readl(plx_intscr);
	memcpy(&(dev_addr[3]), &val, 3);

	return;
}

void nvoib_irq_enable(void){
	struct shared_region *sr = ivs_info.shared_region;
	sr->rx.interruptible = 1;
	return;
}

void nvoib_irq_disable(void){
        struct shared_region *sr = ivs_info.shared_region;
        sr->rx.interruptible = 0;
        return;
}

netdev_tx_t nvoib_tx(struct sk_buff *skb, struct net_device *dev){
	struct shared_region *sr = ivs_info.shared_region;
	static uint32_t next_index = 0;
	int flag;

	/* add skb to tx ring buffer */
	rmb();
	flag = sr->tx.buf[next_index].flag;
	if(flag == ENTRY_COMPLETE){
		struct sk_buff *skb_old;
		int index;

		index = next_index;
		next_index = (index + 1) % RING_SIZE;

		rmb();
		skb_old = (struct sk_buff *)sr->tx.buf[index].skb;
		if(skb_old != NULL){
			kfree_skb(skb_old);
		}

		/* release wmem or rmem */
		if(skb->destructor != NULL){
			skb->destructor(skb);
			skb->destructor = NULL;
		}

		sr->tx.buf[index].data_ptr	= (uint64_t)virt_to_phys((volatile void *)skb->data);
		sr->tx.buf[index].skb		= (uint64_t)skb;
		sr->tx.buf[index].size		= skb->len;
		wmb();
		sr->tx.buf[index].flag		= ENTRY_AVAILABLE;
		wmb();

		if(sr->tx.interruptible){
			/* wake up host OS */
			kick_host(&ivs_info);
			/* temporary for debugging */
			ip_dev->stats.tx_errors++;
		}

                ip_dev->stats.tx_packets++;
                ip_dev->stats.tx_bytes += skb->len;
	}else{
		if(flag == ENTRY_INFLIGHT){
			ip_dev->stats.tx_dropped++;
		}else if(flag == ENTRY_AVAILABLE){
			ip_dev->stats.tx_errors++;
		}
		kfree_skb(skb);
	}

	return NETDEV_TX_OK;
}

int nvoib_rx(struct napi_struct *napi, int badget){
	struct shared_region *sr = ivs_info.shared_region;
	static uint32_t next_index = 0;
	int work_done = 0;

	/* process received buffer */
	rmb();
	while(sr->rx.buf[next_index].flag == ENTRY_COMPLETE && work_done < badget){
                struct sk_buff *skb;
                struct sk_buff *skb_new;
		uint32_t size;
		int index;

		work_done++;

		/* buffer allocation process */
                skb_new = dev_alloc_skb(ivs_info.ip_align + ivs_info.mtu);
                if(unlikely(!skb_new)){
                        printk(KERN_ERR "NVOIB_FATAL: failed to get buffer\n");
                        break;
                }
                skb_reserve(skb_new, ivs_info.ip_align);

		index = next_index;
		next_index = (index + 1) % RING_SIZE;

		rmb();
		skb		= (struct sk_buff *)sr->rx.buf[index].skb;
		size		= sr->rx.buf[index].size;

		/* packet injection process */
		skb_put(skb, size - IB_UD_GRH);
		skb->protocol = eth_type_trans(skb, ip_dev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		netif_receive_skb(skb);

		ip_dev->stats.rx_packets++;
		ip_dev->stats.rx_bytes += size;

		/* buffer configuration process */
		sr->rx.buf[index].skb		= (uint64_t)skb_new;
                sr->rx.buf[index].data_ptr	= (uint64_t)virt_to_phys((volatile void *)skb_new->data)
                                                	- IB_UD_GRH;
                sr->rx.buf[index].size		= ivs_info.mtu + IB_UD_GRH;
		wmb();
                sr->rx.buf[index].flag		= ENTRY_AVAILABLE;
	}
	wmb();

	if(work_done < badget){
		int flag;
		nvoib_irq_enable();
		napi_complete(napi);
		flag = sr->rx.buf[next_index].flag;
		if(flag  == ENTRY_COMPLETE){
			nvoib_irq_disable();
			napi_schedule(napi);
		}else if(flag == ENTRY_INFLIGHT){
			ip_dev->stats.rx_dropped++;
		}else if(flag == ENTRY_AVAILABLE){
			ip_dev->stats.rx_errors++;
		}
	}

        return work_done;
}

static int prepare_shared_region(struct kvm_ivshmem_device *dev){
	struct shared_region *sr;
	int i;

	sr = kmalloc(sizeof(struct shared_region), GFP_KERNEL);
	if(unlikely(!sr)){
		return -1;
	}
	memset(sr, 0, sizeof(struct shared_region));

	for(i = 0; i < RING_SIZE; i++){
		struct sk_buff *skb;
		
                skb = dev_alloc_skb(dev->ip_align + dev->mtu);
                if(unlikely(!skb)){
                        printk(KERN_ERR "NVOIB_FATAL: failed to get buffer\n");
                        return -1;
                }

		skb_reserve(skb, dev->ip_align);

		sr->rx.buf[i].skb	= (uint64_t)skb;
		sr->rx.buf[i].data_ptr	= (uint64_t)virt_to_phys((volatile void *)skb->data)
						- IB_UD_GRH;
		sr->rx.buf[i].size	= dev->mtu + IB_UD_GRH;
		sr->rx.buf[i].flag	= ENTRY_AVAILABLE;
	}
	wmb();

	dev->shared_region = sr;
	return 0;
}

static void nvoib_set_mtu(struct kvm_ivshmem_device *dev){
	dev->ip_align = NET_IP_ALIGN;

	while((NET_SKB_PAD + dev->ip_align) < IB_UD_GRH){
		dev->ip_align += L1_CACHE_BYTES;
	}

	dev->mtu = IB_MTU;
	return;
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

	if(prepare_shared_region(&ivs_info) < 0){
		printk(KERN_ERR "failed to get shared region buffer\n");
		goto pci_disable;
	}

	ivs_info.regaddr =  pci_resource_start(pdev, 0);
	ivs_info.reg_size = pci_resource_len(pdev, 0);
	ivs_info.regs = pci_ioremap_bar(pdev, 0);

	if(unlikely(!ivs_info.regs)){
		printk(KERN_ERR "IVSHMEM_NIC: Cannot ioremap registers of size %d\n", ivs_info.reg_size);
		goto pci_release;
	}

	notify_shared_region(&ivs_info);

	ivs_info.dev = pdev;
	if (request_msix_vectors(&ivs_info, 4) != 0) {
		printk(KERN_INFO "IVSHMEM_NIC: MSI-X disabled\n");
		goto pci_release;
	}

	pci_set_drvdata(pdev, &ivs_info);

	init_host(&ivs_info);
	return 0;

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
		err = request_irq(ivs_info->msix_entries[i].vector, nvoib_interrupt, 0,
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

	nvoib_set_mtu(&ivs_info);

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
