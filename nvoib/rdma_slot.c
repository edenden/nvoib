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

#include "nvoib.h"
#include "rdma_event.h"
#include "rdma_slot.h"

static void slot_assign_for_peer(struct rdma_cm_id *id, int slot);
static void slot_send(struct rdma_cm_id *id);

void *slot_wait_assigning(void *arg){
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
		slot_assign_for_peer(id, ctx->slot_assign_num);
	} 

}

static void slot_assign_for_peer(struct rdma_cm_id *id, int slot){
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

	slot_send(id);
}

static void slot_send(struct rdma_cm_id *id){
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

