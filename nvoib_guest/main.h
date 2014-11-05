#define RING_SIZE 12800
#define NAPI_POLL_WEIGHT 64
#define IB_UD_GRH 40
#define IB_MTU 4096

#ifdef NET_IP_ALIGN
#undef NET_IP_ALIGN
#endif
#define NET_IP_ALIGN 2

/*temp*/
extern struct net_device *ip_dev;
extern struct kvm_ivshmem_device ivs_info;

netdev_tx_t nvoib_tx(struct sk_buff *skb, struct net_device *dev);
int nvoib_rx(struct napi_struct *napi, int weight);
void nvoib_eth_addr(unsigned char *dev_addr);
void nvoib_irq_enable(void);
void nvoib_irq_disable(void);

struct kvm_ivshmem_device {
        void __iomem * regs;
        unsigned int regaddr;
        unsigned int reg_size;

        struct pci_dev *dev;
        char (*msix_names)[256];
        struct msix_entry *msix_entries;
        int nvectors;
        void *shared_region;

	int mtu;
	int ip_align;
};

#define ENTRY_AVAILABLE 2
#define ENTRY_INFLIGHT 1
#define ENTRY_COMPLETE 0

struct buf_data {
        volatile uint64_t       skb;
        volatile uint64_t       data_ptr;
        volatile uint32_t       size;
        volatile uint32_t       flag;
};

struct ring_buf {
        struct buf_data buf[RING_SIZE];
        volatile uint32_t       interruptible;
};

struct shared_region {
	struct ring_buf tx;
	struct ring_buf rx;
};
