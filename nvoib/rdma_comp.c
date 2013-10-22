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

void comp_pull(struct ibv_comp_channel *cc, struct nvoib_dev *pci_dev, comp_f func){
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

void comp_server_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc){
	uint32_t size;
	struct inflight *info;

	dprintf("RX completion occured\n");
	if(wc->opcode == IBV_WC_RECV){
		size = ntohl(wc->imm_data);
		info = (struct inflight *)(wc->wr_id);

		dprintf("IB RECV arrived size = %d\n", size);
		ring_rx(pci_dev, info, size);
                dprintf("writed packet to rx ring\n");

		ring_rx_avail(id, pci_dev);
		dprintf("IBV RECV completed\n");
	}
}

void comp_client_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc){
	uint32_t size;
	struct inflight *info;

	dprintf("TX completion occured\n");
	if(wc->opcode == IBV_WC_SEND){
		size = ntohl(wc->imm_data);
		info = (struct inflight *)(wc->wr_id);

		dprintf("send size = %d\n", size);
		ring_tx_used(pci_dev, info, size);
		dprintf("IBV SEND completed\n");
	}
}

void nvoib_request_recv(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
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

void nvoib_request_send(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
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
