#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <mqueue.h>

#include "rdma.h"
#include "recv.h"

void rx_start_listen(struct rdma_event_channel *ec, char *recv_port){
        struct sockaddr_in addr;
        struct rdma_cm_id *id = NULL;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(atoi(port));

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

        if(rdma_bind_addr(id, (struct sockaddr *)&addr) != 0){
                exit(EXIT_FAILURE);
        }

        /* backlog=10 is arbitrary */
        if(rdma_listen(id, 10) != 0){
                exit(EXIT_FAILURE);
        }

	return;
}

static void rx_set_memory_region(struct rdma_cm_id *id){
	struct context *ctx = (struct context *)id->context;

	ctx->guest_memory_mr = ibv_reg_mr(ctx->pd, ctx->pci_dev.guest_memory, ctx->pci_dev.ram_size,
				IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if(ctx->guest_memory_mr == NULL){
		exit(EXIT_FAILURE);
	} 

	ctx->msg = malloc(sizeof(struct message));
	ctx->msg_mr = ibv_reg_mr(ctx->pd, ctx->msg, sizeof(struct message),
				IBV_ACCESS_LOCAL_WRITE);
	if(ctx->msg_mr == NULL){
		exit(EXIT_FAILURE);
	}
}

static void rx_on_connection(struct rdma_cm_id *id){
	struct context *ctx = (struct context *)id->context;

	ctx->msg->id = MSG_MR;
	ctx->msg->addr = (uintptr_t)ctx->guest_memory_mr->addr;
	ctx->msg->rkey = ctx->guest_memory_mr->rkey;

	ctx->next_slot_assign = 0;
	ctx->slot_assign_num = RDMA_SLOT;

	if(mq_send(sl_mq, &id, sizeof(struct rdma_cm_id *), 10) != 0){
		exit(EXIT_FAILURE);
	}
}

static void rxcon_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc){
	struct context *ctx = (struct context *)id->context;
	static uint32_t next_prepare = 0;
	struct shared_region *sr = (struct shared_region *)ctx->pci_dev->shm_ptr;

	if(wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM){
		uint32_t size = ntohl(wc->imm_data);
		int slot_index = ctx->next_slot;
		ctx->next_slot = (slot_index + 1) % RDMA_SLOT;
		ctx->remain_slot--;

		rdma_request_write_with_imm(id);

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

		if(remain_slot == (RDMA_SLOT / 2)){
			ctx->msg->id = MSG_ASSIGN;
			ctx->slot_assign_num = RDMA_SLOT / 2;
			if(mq_send(sl_mq, &id, sizeof(struct rdma_cm_id *), 10) != 0){
				exit(EXIT_FAILURE);
			}
		}
	}else if(wc->opcode == IBV_WC_SEND){
		pthread_mutex_unlock(ctx->msg_mutex);
	}
}

void *rx_wait_assign(void *arg){
	struct thread_args *tharg = (struct thread_args *)arg;
	mqd_t sl_mq = tharg->sl_mq;
        struct mq_attr attr;
        void *buf = NULL;
        int read_len;

        if(mq_getattr(ctx->sl_mq, &attr) < 0){
                exit(EXIT_FAILURE);
        }

        buf = malloc(attr.mq_msgsize);
        if(buf == NULL){
                exit(EXIT_FAILURE);
        }

	while(read_len = mq_receive(sl_mq, buf, attr.mq_msgsize, NULL)){
		struct rdma_cm_id *id = *(struct rdma_cm_id *)buf;
		struct context *ctx = (struct context *)id->context;
		pthread_mutex_lock(ctx->msg_mutex);
		rx_assign_slot_to_peer(id, ctx->slot_assign_num);
	} 

}

void rx_assign_slot_to_peer(struct rdma_cm_id *id, int slot){
	struct context *ctx = (struct context *)id->context;
	struct shared_region *sr = (struct shared_region *)ctx->pci_dev->shm_ptr;
	static uint32_t next_consume = 0;
	int i;
	int allocated_count = 0;

	while(allocated_count < slot){
		sr->rx_avail.ring_empty = 0;
		for(i = 0; i < slot && !sr->rx_avail.ring_empty; i++){
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
				next_consume = (index + 1) % RING_SIZE;
				sr->rx_avail.buf[index].skb = NULL;
				sr->rx_avail.buf[index].data_ptr = 0;
				sr->rx_avail.buf[index].size = 0;
				sr->rx_avail.buf[index].flag = 0;

				ctx->slot[ctx->next_slot_assign + allocated_count].skb = skb;
				ctx->slot[ctx->next_slot_assign + allocated_count].data_ptr = data_ptr;
				ctx->msg->data_ptr[allocated_count] = data_ptr;
				allocated_count++;
			}

			sr->rx_avail.ring_empty = !sr->rx_avail.buf[next_consume].flag;
		}
	}

	ctx->remain_slot += allocated_count;
	ctx->next_slot_assign = (ctx->next_slot_assign + allocated_count) % RDMA_SLOT;
	ctx->msg->slot_num = allocated_count;

	rdma_send_slot(id);
}

static void rdma_send_slot(struct rdma_cm_id *id){
	struct context *ctx = (struct context *)id->context;
	struct ibv_send_wr wr, *bad_wr = NULL;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id = NULL;
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;

	sge.addr = (uintptr_t)ctx->msg;
	sge.length = sizeof(*ctx->msg);
	sge.lkey = ctx->msg_mr->lkey;

	if(ibv_post_send(id->qp, &wr, &bad_wr) != 0){
		exit(EXIT_FAILURE);
	}
}

