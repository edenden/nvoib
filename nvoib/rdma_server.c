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

#include "hw/pci/pci.h"
#include "hw/pci/msix.h"

#include "nvoib.h"
#include "rdma_event.h"
#include "rdma_server.h"

void server_start_listen(struct rdma_event_channel *ec, const char *recv_port){
        struct sockaddr_in addr;
        struct rdma_cm_id *id = NULL;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(atoi(recv_port));

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

void server_set_mr(struct rdma_cm_id *id){
	struct context *ctx = (struct context *)id->context;

	ctx->guest_memory_mr = ibv_reg_mr(ctx->pd, ctx->pci_dev->guest_memory, ctx->pci_dev->ram_size,
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

void server_conn_estab(struct rdma_cm_id *id, mqd_t sl_mq){
	struct context *ctx = (struct context *)id->context;

	ctx->msg->id = MSG_MR;
	ctx->msg->addr = (uintptr_t)ctx->guest_memory_mr->addr;
	ctx->msg->rkey = ctx->guest_memory_mr->rkey;

	ctx->next_slot_assign = 0;
	ctx->slot_assign_num = RDMA_SLOT;

	if(mq_send(sl_mq, (const char *)&id, sizeof(struct rdma_cm_id *), 10) != 0){
		exit(EXIT_FAILURE);
	}
}

