#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/route.h>
#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>

#include "main.h"
#include "netdev.h"

static int netdev_up(struct net_device *dev);
static int netdev_down(struct net_device *dev);
static void netdev_setup(struct net_device *dev);
static void nvoib_net_mclist(struct net_device *dev);

struct napi_struct napi;

static const struct net_device_ops ip_netdev_ops = {
//      .ndo_init       = ,			// Called at register_netdev
//      .ndo_uninit     = ,			// Called at unregister_netdev
        .ndo_open       = netdev_up,            // Called at ifconfig up
        .ndo_stop       = netdev_down,          // Called at ifconfig down
        .ndo_start_xmit = nvoib_tx,       // REQUIRED, must return NETDEV_TX_OK
//      .ndo_change_rx_flags = ,		// Called when setting promisc or multicast flags.
//      .ndo_change_mtu = ,
//      .net_device_stats = ,			// Called for usage statictics
	.ndo_set_rx_mode = nvoib_net_mclist,
	.ndo_set_mac_address    = eth_mac_addr,
	.ndo_validate_addr      = eth_validate_addr,
};

irqreturn_t nvoib_interrupt(int irq, void *dev){
	int ret = IRQ_HANDLED;

	nvoib_irq_disable();
	napi_schedule(&napi);
	return ret;
}

static void nvoib_net_mclist(struct net_device *dev){
	/*
	 * This callback is supposed to deal with mc filter in
	 * _rx_ path and has nothing to do with the _tx_ path.
	 * In rx path we always accept everything userspace gives us.
	 */
	return;
}

static int netdev_up(struct net_device *dev){
	napi_enable(&napi);
	netif_start_queue(dev);
	nvoib_irq_enable();
	return 0;
}

static int netdev_down(struct net_device *dev){
	nvoib_irq_disable();
	netif_stop_queue(dev);
	return 0;
}

static void netdev_setup(struct net_device *dev){
	dev->netdev_ops = &ip_netdev_ops;
	ether_setup(dev);
	nvoib_eth_addr(dev->dev_addr);

/*
	dev->type = ARPHRD_NONE;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = 1500;
	dev->features = NETIF_F_NETNS_LOCAL | NETIF_F_NO_CSUM;
	dev->flags = IFF_NOARP | IFF_POINTOPOINT;
*/
	dev->mtu = ivs_info.mtu - sizeof(struct ethhdr);
	dev->tx_queue_len = 12800;
}

int netdev_create(struct net_device **dev){
	int ret = 0;

	*dev = alloc_netdev(0, NETDEV_NAME, netdev_setup);
	if (!*dev) {
		printk(KERN_ERR "IVSHMEM_NIC: Unable to allocate ip device.\n");
		return -ENOMEM;
	}

	netif_napi_add(*dev, &napi, nvoib_rx, NAPI_POLL_WEIGHT);

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

