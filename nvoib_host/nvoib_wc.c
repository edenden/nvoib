#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include <mqueue.h>
#include <netinet/ether.h>

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

void comp_pull(struct session *ss, struct ibv_comp_channel *cc,
	struct nvoib_dev *dev, comp_f func){

	struct ibv_cq *cq;
	struct ibv_wc wc;
	void *cq_context;

	if(ibv_get_cq_event(cc, &cq, (void **)&cq_context) != 0){
		exit(EXIT_FAILURE);
	}

	ibv_ack_cq_events(cq, 1);

	if(ibv_req_notify_cq(cq, 0) != 0){
		exit(EXIT_FAILURE);
	}

	while(ibv_poll_cq(cq, 1, &wc)){
		if (wc.status == IBV_WC_SUCCESS){
			func(ss, dev, &wc);
		}else{
			printf("poll_cq: status(%d) is not IBV_WC_SUCCESS\n", wc.status);
			exit(EXIT_FAILURE);
		}
	}

	return;
}

void comp_rx_work_completed(struct session *ss, struct nvoib_dev *dev, struct ibv_wc *wc){

	dprintf("RX: wc status is IBV_WC_SUCCESS\n");
	if(wc->opcode == IBV_WC_RECV){
		dprintf("RX: arrived size (including GRH) = %d\n", wc->byte_len);

		if(IS_ARP(wc->wr_id + sizeof(struct ibv_grh))){
			rx_fdb_learn(ss, wc, (void *)(wc->wr_id));
		}

		ring_rx_comp(dev, wc->byte_len, RX_POLL_BADGET);
		dprintf("RX: completed\n");
	}
}

void comp_tx_work_completed(struct session *ss, struct nvoib_dev *dev, struct ibv_wc *wc){

	dprintf("TX: wc status is IBV_WC_SUCCESS\n");
	if(wc->opcode == IBV_WC_SEND){
		ring_tx_comp(dev);
		dprintf("TX: completed\n");
	}
}

void nvoib_request_recv(struct session *ss, struct nvoib_dev *dev,
	uint64_t data_ptr, uint32_t size){

        struct ibv_recv_wr wr, *bad_wr = NULL;
        struct ibv_sge sge;
	uintptr_t buffer;

        memset(&wr, 0, sizeof(wr));
	buffer = (uintptr_t)dev->guest_memory + data_ptr;

        wr.wr_id = buffer;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        sge.addr = buffer;
        sge.length = size;
        sge.lkey = ss->guest_memory_mr->lkey;

        if(ibv_post_recv(ss->qp, &wr, &bad_wr) != 0){
                exit(EXIT_FAILURE);
        }
}

void nvoib_request_send(struct session *ss, struct nvoib_dev *dev,
	uint64_t data_ptr, uint32_t size){

	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	uintptr_t buffer;
	struct forward_entry *entry;

	memset(&wr, 0, sizeof(wr));
	buffer = (uintptr_t)dev->guest_memory + data_ptr;

	wr.wr_id = (uintptr_t)NULL;
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	entry = tx_fdb_lookup(&ss->fdb, (void *)buffer);
        wr.wr.ud.ah = entry->ah;
        wr.wr.ud.remote_qpn = entry->qpn;
        wr.wr.ud.remote_qkey = dev->tenant_id;

	sge.addr = buffer;
	sge.length = size;
	sge.lkey = ss->guest_memory_mr->lkey;

	if(ibv_post_send(ss->qp, &wr, &bad_wr) != 0){
		printf("failed to ibv_post_send\n");
		exit(EXIT_FAILURE);
	}

	dprintf("TX: request_send: dest_qpn = 0x%x, dest_qkey(tenant ID) = 0x%x\n",
        entry->qpn, dev->tenant_id);
}
