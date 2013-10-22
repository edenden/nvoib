#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "debug.h"
#include "rdma_event.h"
#include "rdma_cq.h"
#include "rdma_client.h"

void cq_pull(struct ibv_comp_channel *cc, struct nvoib_dev *pci_dev, comp_f func){
	struct ibv_cq *cq;
	struct rdma_cm_id *id;
	struct ibv_wc wc;

	if(ibv_get_cq_event(cc, &cq, (void **)&id) != 0){
		exit(EXIT_FAILURE);
	}

	ibv_ack_cq_events(cq, 1);

	if(ibv_req_notify_cq(cq, 0) != 0){
		exit(EXIT_FAILURE);
	}

	while(ibv_poll_cq(cq, 1, &wc)){
		if (wc.status == IBV_WC_SUCCESS){
			dprintf("status is IBV_WC_SUCCESS\n");
			func(id, pci_dev, &wc);
		}else{
			dprintf("poll_cq: status(%d) is not IBV_WC_SUCCESS\n", wc.status);
			exit(EXIT_FAILURE);
		}
	}

	return;
}

void cq_server_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc){
	uint32_t size;
	struct inflight *info;

	dprintf("RX completion occured\n");
	if(wc->opcode == IBV_WC_RECV){
		size = ntohl(wc->imm_data);
		info = (struct inflight *)(wc->wr_id);

		dprintf("IB RECV arrived size = %d\n", size);
		comp_queue_to_rx_ring(pci_dev, info, size);
                dprintf("writed packet to rx ring\n");

		rxavail_ring_to_receive_queue(id, pci_dev);
		dprintf("IBV RECV completed\n");
	}
}

void cq_client_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc){
	uint32_t size;
	struct inflight *info;

	dprintf("TX completion occured\n");
	if(wc->opcode == IBV_WC_SEND){
		size = ntohl(wc->imm_data);
		info = (struct inflight *)(wc->wr_id);

		dprintf("send size = %d\n", size);
		comp_queue_to_txused_ring(pci_dev, info, size);
		dprintf("IBV SEND completed\n");
	}
}

void comp_queue_to_txused_ring(struct nvoib_dev *pci_dev, struct inflight, int size){
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

void comp_queue_to_rx_ring(struct nvoib_dev *pci_dev, struct inflight *info, int size){
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
			kick_guest(pci_dev->rx_fd);
		}
	}else{
		/* Here is not reached because guest driver keeps number of buffer RING_SIZE */
		exit(EXIT_FAILURE);
	}

	free(info);
}

void rxavail_ring_to_recv_queue(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
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
			struct inflight_info *info;

			next_consume = (index + 1) % RING_SIZE;
			sr->rx_avail.buf[index].skb = NULL;
			sr->rx_avail.buf[index].data_ptr = 0;
			sr->rx_avail.buf[index].size = 0;
			sr->rx_avail.buf[index].flag = 0;

			info = (struct inflight *)malloc(sizeof(struct inflight));
			info->skb = skb;
			info->data_ptr = data_ptr;
			rdma_request_recv(id, pci_dev, data_ptr, size, info);
		}

		sr->rx_avail.ring_empty = !sr->rx_avail.buf[next_consume].flag;
	}

}

void tx_ring_to_send_queue(struct rdma_event_channel *ec, struct nvoib_dev *pci_dev, int ev_fd){
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	static uint32_t next_consume = 0;

        /* temporary for debugging... */
        static struct rdma_cm_id *id_test = NULL;
	static struct temp_buffer temp_start = {0};
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
				id_test = client_start_connect(ec, DEST_HOST, DEST_PORT);
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
				rdma_request_send(id, pci_dev, data_ptr, size, info);
			}
		}

		sr->tx.ring_empty = !sr->tx.buf[next_consume].flag;
		if(!sr->tx.ring_empty){
			schedule_tx_process(ev_fd);
		}
	}
}

static void rdma_request_recv(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
	uint64_t offset, uint32_t size, struct inflight *info){

        struct context *ctx = (struct context *)id->context;
        struct ibv_recv_wr wr, *bad_wr = NULL;
        struct ibv_sge sge;

        memset(&wr, 0, sizeof(wr));

        wr.wr_id = (uintptr_t)info;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        sge.addr = ((uintptr_t)pci_dev->guest_memory) + offset;
        sge.length = size;
        sge.lkey = ctx->guest_memory_mr->lkey;

        if(ibv_post_recv(id->qp, &wr, &bad_wr) != 0){
                exit(EXIT_FAILURE);
        }
}

void rdma_request_send(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
	uint64_t offset, uint32_t size, struct inflight *info){

	struct context *ctx = (struct context *)id->context;
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = (uintptr_t)info;
	wr.imm_data = htonl(size);
	wr.opcode = IBV_WR_SEND_WITH_IMM;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	sge.addr = ((uintptr_t)pci_dev->guest_memory) + offset;
	sge.length = size;
	sge.lkey = ctx->guest_memory_mr->lkey;

	if(ibv_post_send(id->qp, &wr, &bad_wr) != 0){
		dprintf("failed to ibv_post_send\n");
		exit(EXIT_FAILURE);
	}
}
