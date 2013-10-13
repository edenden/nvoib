#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <mqueue.h>

#include "hw/pci/pci.h"
#include "hw/pci/msix.h"

#include "nvoib.h"
#include "rdma_event.h"
#include "rdma_cq.h"
#include "rdma_client.h"

static void cq_server_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc, mqd_t sl_mq);
static void cq_client_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc);

void *cq_wait_poll(void *arg){
	struct ibv_cq *cq;
	struct ibv_wc wc;
	struct rdma_cm_id *id;
	struct context *ctx;
	struct thread_args *tharg = (struct thread_args *)arg;
	int ep_fd = tharg->ep_fd;
	mqd_t sl_mq = tharg->sl_mq;
	int i, fd_num;
	struct epoll_event ev_ret[MAX_EVENTS];

	while(1){
		if((fd_num = epoll_wait(ep_fd, ev_ret, MAX_EVENTS, -1)) < 0){
			/* 'interrupted syscall error' occurs when using gdb */
			continue;
		}

		for(i = 0; i < fd_num; i++){
			id = ev_ret[i].data.ptr;
			ctx = (struct context *)id->context;

			if(ibv_get_cq_event(ctx->comp_channel, &cq, (void **)&ctx) != 0){
				exit(EXIT_FAILURE);
			}

			ibv_ack_cq_events(cq, 1);

			if(ibv_req_notify_cq(cq, 0) != 0){
				exit(EXIT_FAILURE);
			}

			while(ibv_poll_cq(cq, 1, &wc)){
				if (wc.status == IBV_WC_SUCCESS){
					if(ctx->rx_flag){
						cq_server_work_completed(id, &wc, sl_mq);
					}else{
						cq_client_work_completed(id, &wc);
					}
				}else{
					printf("poll_cq: status is not IBV_WC_SUCCESS\n");
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	return NULL;
}

static void cq_server_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc, mqd_t sl_mq){
	struct context *ctx = (struct context *)id->context;
	static uint32_t next_prepare = 0;
	struct shared_region *sr = (struct shared_region *)ctx->pci_dev->shm_ptr;

	if(wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM){
		uint32_t size = ntohl(wc->imm_data);
		int slot_index = ctx->next_slot;
		ctx->next_slot = (slot_index + 1) % RDMA_SLOT;
		ctx->remain_slot--;

		rdma_request_next_write(id);

		/* add skb to rx ring buffer */
		if(!sr->rx.buf[next_prepare].flag){
			int index = next_prepare;

			next_prepare = (index + 1) % RING_SIZE;
			sr->rx.buf[index].data_ptr	= ctx->slot[slot_index].data_ptr;
			sr->rx.buf[index].skb		= ctx->slot[slot_index].skb;
			sr->rx.buf[index].size		= size;
			sr->rx.buf[index].flag		= 1;

			if(sr->rx.ring_empty){
				/* wake up guest OS */
				msix_notify(PCI_DEVICE(ctx->pci_dev), 1);
			}
		}else{
			/* TODO: should wait for guest processing RX queue? */
		}

		if(ctx->remain_slot == (RDMA_SLOT / 2)){
			ctx->msg->id = MSG_ASSIGN;
			ctx->slot_assign_num = RDMA_SLOT / 2;
			if(mq_send(sl_mq, (const char *)&id, sizeof(struct rdma_cm_id *), 10) != 0){
				exit(EXIT_FAILURE);
			}
		}
	}else if(wc->opcode == IBV_WC_SEND){
		pthread_mutex_unlock(&ctx->msg_mutex);
	}
}

static void cq_client_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc){
	struct context *ctx = (struct context *)id->context;
	static uint32_t next_prepare = 0;

	if (wc->opcode & IBV_WC_RECV) {
		int i;

		pthread_mutex_lock(&ctx->slot_mutex);
		if(ctx->msg->id == MSG_MR) {
			ctx->peer_addr = ctx->msg->addr;
			ctx->peer_rkey = ctx->msg->rkey;
			printf("received MR from Server\n");

			for(i = 0; i < ctx->msg->slot_num; i++){
				ctx->slot[ctx->next_slot_assign + i].data_ptr = ctx->msg->data_ptr[i];
			}

			ctx->remain_slot += ctx->msg->slot_num;
			ctx->next_slot_assign = (ctx->next_slot_assign + ctx->msg->slot_num) % RDMA_SLOT;
			printf("received new %d slot from Server\n", ctx->msg->slot_num);
		} else if(ctx->msg->id == MSG_ASSIGN) {
                        for(i = 0; i < ctx->msg->slot_num; i++){
                                ctx->slot[ctx->next_slot_assign + i].data_ptr = ctx->msg->data_ptr[i];
                        }

			ctx->remain_slot += ctx->msg->slot_num;
                        ctx->next_slot_assign = (ctx->next_slot_assign + ctx->msg->slot_num) % RDMA_SLOT;
			printf("received new %d slot from Server\n", ctx->msg->slot_num);
		}
		pthread_mutex_unlock(&ctx->slot_mutex);

		rdma_request_next_msg(id);
	}else if(wc->opcode & IBV_WR_RDMA_WRITE_WITH_IMM){
		struct shared_region *sr = (struct shared_region *)ctx->pci_dev->shm_ptr;
		struct write_remote_info *info = (struct write_remote_info *)wc->wr_id;

		/* add used buffer to tx_used ring */
		if(!sr->tx_used.buf[next_prepare].flag){
			int index = next_prepare;

			next_prepare = (index + 1) % RING_SIZE;
			sr->tx_used.buf[index].data_ptr = info->data_ptr;
			sr->tx_used.buf[index].skb = info->skb;
			sr->tx_used.buf[index].size = info->size;
			sr->tx_used.buf[index].flag = 1;

		}else{
			/* TODO: should wait for guest processing TX_used queue? */
		}

		free(info);
	}
}
