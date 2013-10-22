#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "debug.h"
#include "nvoib.h"
#include "rdma.h"

void ring_tx_used(struct nvoib_dev *pci_dev, struct inflight *info, int size){
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	static uint32_t next_prepare = 0;

	/* add used buffer to tx_used ring */
	if(!sr->tx_used.buf[next_prepare].flag){
		int index = next_prepare;

		next_prepare = (index + 1) % RING_SIZE;
		sr->tx_used.buf[index].data_ptr = info->data_ptr;
		sr->tx_used.buf[index].skb = info->skb;
		sr->tx_used.buf[index].size = size;
		sr->tx_used.buf[index].flag = 1;

	}else{
		/* Here is not reached because guest driver keeps number of buffer RING_SIZE */
		exit(EXIT_FAILURE);
	}

	free(info);
}

void ring_rx(struct nvoib_dev *pci_dev, struct inflight *info, int size){
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	static uint32_t next_prepare = 0;

	/* add skb to rx ring buffer */
	if(!sr->rx.buf[next_prepare].flag){
		int index = next_prepare;

		next_prepare = (index + 1) % RING_SIZE;
		sr->rx.buf[index].data_ptr      = info->data_ptr;
		sr->rx.buf[index].skb           = info->skb;
		sr->rx.buf[index].size          = size;
		sr->rx.buf[index].flag          = 1;

		if(sr->rx.ring_empty){
			/* wake up guest OS */
			rx_kick_guest(pci_dev->rx_fd);
		}
	}else{
		/* Here is not reached because guest driver keeps number of buffer RING_SIZE */
		exit(EXIT_FAILURE);
	}

	free(info);
}

void ring_rx_avail(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	static uint32_t next_consume = 0;

	sr->rx_avail.ring_empty = 0;
	while(!sr->rx_avail.ring_empty){
		void *skb = NULL;
		uint64_t data_ptr = 0;
		uint32_t size = 0;
		uint32_t flag = 0;

		int index = next_consume;

		skb = sr->rx_avail.buf[index].skb;
		data_ptr = sr->rx_avail.buf[index].data_ptr;
		size = sr->rx_avail.buf[index].size;
		flag = sr->rx_avail.buf[index].flag;
		if(flag){
			struct inflight *info;

			next_consume = (index + 1) % RING_SIZE;
			sr->rx_avail.buf[index].skb = NULL;
			sr->rx_avail.buf[index].data_ptr = 0;
			sr->rx_avail.buf[index].size = 0;
			sr->rx_avail.buf[index].flag = 0;

			info = (struct inflight *)malloc(sizeof(struct inflight));
			info->skb = skb;
			info->data_ptr = data_ptr;
			nvoib_request_recv(id, pci_dev, data_ptr, size, info);
		}

		sr->rx_avail.ring_empty = !sr->rx_avail.buf[next_consume].flag;
	}

}

void ring_tx(struct rdma_event_channel *ec, struct nvoib_dev *pci_dev, int ev_fd){
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	static uint32_t next_consume = 0;

        /* temporary for debugging... */
        static struct rdma_cm_id *id_test = NULL;
        /* here */

	/* process tx buffer */
	{
		void *skb = NULL;
		uint64_t data_ptr = 0;
		uint32_t size = 0;
		uint32_t flag = 0;

		int index = next_consume;

		skb = sr->tx.buf[index].skb;
		data_ptr = sr->tx.buf[index].data_ptr;
		size = sr->tx.buf[index].size;
		flag = sr->tx.buf[index].flag;
		if(flag){
			struct rdma_cm_id *id;
			struct context *ctx;
			struct inflight *info;

			next_consume = (index + 1) % RING_SIZE;
			sr->tx.buf[index].skb = NULL;
			sr->tx.buf[index].data_ptr = 0;
			sr->tx.buf[index].size = 0;
			sr->tx.buf[index].flag = 0;

			/* TODO: Get rdmacm_id from IP destination */
			/* following is temporary test code for debug */
			if(id_test == NULL){
				id_test = tx_start_connect(ec, DEST_HOST, DEST_PORT);
			}

			id = id_test;
			ctx = (struct context *)id->context;

			if(!ctx->initialized){
				struct temp_buffer *temp = malloc(sizeof(struct temp_buffer));
				struct temp_buffer *last = ctx->temp_last_ptr;

				temp->info.skb		= skb;
				temp->info.data_ptr	= data_ptr;
				temp->size		= size;
				temp->next		= NULL;

				last->next = temp;
				ctx->temp_last_ptr = temp;
			}else{
				info = (struct inflight *)malloc(sizeof(struct inflight));
				info->skb = skb;
				info->data_ptr = data_ptr;
				nvoib_request_send(id, pci_dev, data_ptr, size, info);
			}
		}

		sr->tx.ring_empty = !sr->tx.buf[next_consume].flag;
		if(!sr->tx.ring_empty){
			tx_schedule_process(ev_fd);
		}
	}
}

