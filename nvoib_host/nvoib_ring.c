#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include <mqueue.h>

#include "hw/pci/msix.h"

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

void ring_tx_comp(struct nvoib_dev *dev){
	struct shared_region *sr = dev->shared_region;
	static uint32_t next_tx_comp = 0;

#ifdef DEBUG
	/* add used buffer to tx_used ring */
        if(sr->tx.buf[next_tx_comp].flag != ENTRY_INFLIGHT){
		/* TODO: queue next tx used */
		printf("BUG: tx race condition\n");
		exit(EXIT_FAILURE);
        }
#endif

	int index = next_tx_comp;
	next_tx_comp = (index + 1) % RING_SIZE;

	sr->tx.buf[index].flag	= ENTRY_COMPLETE;
}

void ring_rx_comp(struct nvoib_dev *dev, uint32_t size, int badget){
	struct shared_region *sr = dev->shared_region;
	static uint32_t next_rx_comp = 0;
	int index;

#ifdef DEBUG
	/* add skb to rx ring buffer */
	if(sr->rx.buf[next_rx_comp].flag != ENTRY_INFLIGHT){
		/* TODO: queue next rx avail */
		printf("BUG: rx race condition\n");
		exit(EXIT_FAILURE);
	}
#endif

	index = next_rx_comp;
	next_rx_comp = (index + 1) % RING_SIZE;
	sr->rx.buf[index].size		= size;
	smp_wmb();
	sr->rx.buf[index].flag		= ENTRY_COMPLETE;

	dev->rx_remain++;
	if(dev->rx_remain > badget){
		/* wake up guest OS immediately because many packets are pended... */
		smp_wmb();
		if(sr->rx.interruptible){
			event_notifier_set(&dev->rx_event);
		}
		dev->rx_remain = 0;
	}
}

int ring_rx_avail(struct session *ss, struct nvoib_dev *dev){
	struct shared_region *sr = dev->shared_region;
	static uint32_t next_rx_avail = 0;
	int ret = 0;

	smp_rmb();
	while(sr->rx.buf[next_rx_avail].flag == ENTRY_AVAILABLE){
		uint64_t data_ptr;
		uint32_t size;
		int index;

		index = next_rx_avail;
		next_rx_avail = (index + 1) % RING_SIZE;

		smp_rmb();
		data_ptr		= sr->rx.buf[index].data_ptr;
		size			= sr->rx.buf[index].size;
		sr->rx.buf[index].flag  = ENTRY_INFLIGHT;

		nvoib_request_recv(ss, dev, data_ptr, size);

		ret = 1;
	}
	smp_wmb();

	return ret;
}

int ring_tx_avail(struct session *ss, struct nvoib_dev *dev, int badget){
	struct shared_region *sr = dev->shared_region;
	static uint32_t next_tx_avail = 0;
	int ret = 0;
	int work_done = 0;

	smp_rmb();
	while(sr->tx.buf[next_tx_avail].flag == ENTRY_AVAILABLE && work_done < badget){
		uint64_t data_ptr;
		uint32_t size;
		int index;

		work_done++;
		index = next_tx_avail;
		next_tx_avail = (index + 1) % RING_SIZE;

		smp_rmb();
		data_ptr		= sr->tx.buf[index].data_ptr;
		size			= sr->tx.buf[index].size;
		sr->tx.buf[index].flag  = ENTRY_INFLIGHT;

		nvoib_request_send(ss, dev, data_ptr, size);

		ret = 1;
	}
	smp_wmb();

	return ret;
}

