#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/route.h>
#include <linux/skbuff.h>
#include <linux/in.h>

#include "main.h"
#include "netdev.h"

static int netdev_up(struct net_device *dev);
static int netdev_down(struct net_device *dev);
static netdev_tx_t netdev_xmit(struct sk_buff *skb, struct net_device *dev);
static void netdev_setup(struct net_device *dev);

static const struct net_device_ops ip_netdev_ops = {
//      .ndo_init       = ,			// Called at register_netdev
//      .ndo_uninit     = ,			// Called at unregister_netdev
        .ndo_open       = netdev_up,            // Called at ifconfig up
        .ndo_stop       = netdev_down,          // Called at ifconfig down
        .ndo_start_xmit = netdev_xmit,          // REQUIRED, must return NETDEV_TX_OK
//      .ndo_change_rx_flags = ,		// Called when setting promisc or multicast flags.
//      .ndo_change_mtu = ,
//      .net_device_stats = ,			// Called for usage statictics
};

static int netdev_up(struct net_device *dev){
	netif_start_queue(dev);
	return 0;
}

static int netdev_down(struct net_device *dev){
	netif_stop_queue(dev);
	return 0;
}

static netdev_tx_t netdev_xmit(struct sk_buff *skb, struct net_device *dev){
	int err;

	err = kvm_ivshmem_tx(skb);
	if(err < 0){
		kfree_skb(skb);
	}

	return NETDEV_TX_OK;
}

static void netdev_setup(struct net_device *dev){
	dev->netdev_ops = &ip_netdev_ops;

	dev->type = ARPHRD_NONE;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = 1500;
	dev->features = NETIF_F_NETNS_LOCAL | NETIF_F_NO_CSUM;
	dev->flags = IFF_NOARP | IFF_POINTOPOINT;
}

int netdev_create(struct net_device **dev){
	int ret = 0;

	*dev = alloc_netdev(0, NETDEV_NAME, netdev_setup);
	if (!*dev) {
		printk(KERN_ERR "IVSHMEM_NIC: Unable to allocate ip device.\n");
		return -ENOMEM;
	}

	ret = register_netdev(*dev);
	if(ret) {
		printk(KERN_ERR "IVSHMEM_NIC: Unable to register ip device\n");
		free_netdev(*dev);
		return ret;
	}

	printk(KERN_INFO "IVSHMEM_NIC: netdevice created successfully.\n");
	return ret;
}

void netdev_destroy(struct net_device *dev){
	unregister_netdev(dev);

	printk(KERN_INFO "IVSHMEM_NIC: Destroying rloc device.\n");
}

