#define NETDEV_NAME "ip0"

int netdev_create(struct net_device **dev);
void netdev_destroy(struct net_device *dev);
