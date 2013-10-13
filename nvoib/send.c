#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>

#include "rdma.h"
#include "send.h"

int tx_wait_ring(void *arg){
	struct thread_args *tharg = (struct thread_args *)arg;
	struct rdmanic *pci_dev = tharg->pci_dev;
	struct rdma_event_channel *ec = tharg->ec;
	struct shared_region *sr = (struct shared_region *)pci_dev->shm_ptr;
	struct mq_attr attr;
	void *buf = NULL;
	int read_len;
	static uint32_t next_consume = 0;

	/* temporary for debugging... */
	static int connected = 0;
	static rdma_cm_id *id_test;
	/* here */

	if(mq_getattr(pci_dev->tx_mq, &attr) < 0){
		exit(EXIT_FAILURE);
	}

	buf = malloc(attr.mq_msgsize);
	if(buf == NULL){
		exit(EXIT_FAILURE);
	}

	while(read_len = mq_receive(pci_dev->tx_mq, buf, attr.mq_msgsize, NULL)){
		/* process tx buffer */
		sr->tx.ring_empty = 0;
		while(!sr->tx.ring_empty){
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
				struct write_remote_info *info;

				next_consume = (index + 1) % RING_SIZE;
				sr->tx.buf[index].skb = NULL;
				sr->tx.buf[index].data_ptr = 0;
				sr->tx.buf[index].size = 0;
				sr->tx.buf[index].flag = 0;

				/* TODO: Get rdmacm_id from IP destination */

				/* temporary test code for debug */
				if(connected){
					id = id_test;
				}else{
					connected = 1;
					id_test = tx_start_connect(ec, "192.168.0.2", "12345");
					id = id_test;
				}
				/* here */

				/* RDMA Write Now! */
				info = (struct write_remote_info *)malloc(sizeof(struct write_remote_info));
				info->skb = skb;
				info->data_ptr = data_ptr;
				info->size = size;
				tx_write_remote(id, data_ptr, size, info);
			}

			sr->tx.ring_empty = !sr->tx.buf[next_consume].flag;
		}
	} 
}

struct rdma_cm_id *tx_start_connect(struct rdma_event_channel *ec, char *dest_host, char *dest_port){
        struct addrinfo *addr;
        struct rdma_cm_id *id = NULL;

        if(getaddrinfo(host, port, NULL, &addr) != 0){
                exit(EXIT_FAILURE);
        }

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

        if(rdma_resolve_addr(id, NULL, addr->ai_addr, TIMEOUT_IN_MS) != 0){
                exit(EXIT_FAILURE);
        }

        freeaddrinfo(addr);
	return id;
}

static void tx_set_memory_region(struct rdma_cm_id *id){
	struct context *ctx = (struct context *)id->context;

	ctx->guest_memory_mr = ibv_reg_mr(ctx->pd, ctx->pci_dev.guest_memory, ctx->pci_dev.ram_size,
				IBV_ACCESS_LOCAL_WRITE);
	if(ctx->guest_memory_mr == NULL){
		exit(EXIT_FAILURE);
	}

	ctx->msg = malloc(sizeof(struct message));
	ctx->msg_mr = ibv_reg_mr(ctx->pd, ctx->msg, sizeof(*ctx->msg),
				IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if(ctx->msg_mr == NULL){
		exit(EXIT_FAILURE);
	}
}

static void txcon_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc){
	struct context *ctx = (struct context *)id->context;
	static uint32_t next_prepare = 0;

	if (wc->opcode & IBV_WC_RECV) {
		int i;

		pthread_mutex_lock(ctx->slot_mutex);
		if(ctx->msg->id == MSG_MR) {
			ctx->peer_addr = ctx->msg->addr;
			ctx->peer_rkey = ctx->msg->rkey;
			printf("received MR from Server\n");

			for(i = 0; i < ctx->msg->slot_num; i++){
				ctx->slot[ctx->slot_next_assign + i].data_ptr = ctx->msg->data_ptr[i];
			}

			ctx->remain_slot += ctx->msg->slot_num;
			ctx->slot_next_assign = (ctx->slot_next_assign + ctx->msg->slot_num) % RDMA_SLOT;
			printf("received new %d slot from Server\n", ctx->msg->slot_num);
		} else if(ctx->msg->id == MSG_ASSIGN) {
                        for(i = 0; i < ctx->msg->slot_num; i++){
                                ctx->slot[ctx->slot_next_assign + i].data_ptr = ctx->msg->data_ptr[i];
                        }

			ctx->remain_slot += ctx->msg->slot_num;
                        ctx->slot_next_assign = (ctx->slot_next_assign + ctx->msg->slot_num) % RDMA_SLOT;
			printf("received new %d slot from Server\n", ctx->msg->slot_num);
		}
		pthread_mutex_unlock(ctx->slot_mutex);

		rdma_request_msg(id);
	}else if(wc->opcode & IBV_WC_SEND_RDMA_WITH_IMM){
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

static void tx_write_remote(struct rdma_cm_id *id, uint64_t local_offset, uint32_t size,
	struct write_remote_info *info){

	struct context *ctx = (struct context *)id->context;
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;
	uint64_t remote_offset;
	uint64_t peer_addr;
	uint32_t peer_rkey;

	pthread_mutex_lock(ctx->slot_mutex);
	if(ctx->remain_slot == 0){
		printf("no RDMA slot is remaining...\n");
		pthread_mutex_unlock(ctx->slot_mutex);
		return;
	}else{
		if(ctx->peer_addr == 0 || ctx->peer_rkey == 0){
			printf("peer_addr or peer_rkey have not arrived\n");
			pthread_mutex_unlock(ctx->slot_mutex);
			return;
		}

		peer_addr = ctx->peer_addr;
		peer_rkey = ctx->peer_rkey;
		remote_offset = ctx->slot[ctx->next_slot].data_ptr;
		ctx->next_slot = (ctx->next_slot + 1) % RDMA_SLOT;
		ctx->remain_slot--;
	}
	pthread_mutex_unlock(ctx->slot_mutex);

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)id;
	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(size);
	wr.wr.rdma.remote_addr = ctx->peer_addr + remote_offset;
	wr.wr.rdma.rkey = ctx->peer_rkey;

	if (len) {
		wr.sg_list = &sge;
		wr.num_sge = 1;

		sge.addr = (uintptr_t)ctx->pci_dev->guest_memory + local_offset;
		sge.length = size;
		sge.lkey = ctx->guest_memory_mr->lkey;
	}

	if(ibv_post_send(id->qp, &wr, &bad_wr) != 0){
		exit(EXIT_FAILURE);
	}
}
