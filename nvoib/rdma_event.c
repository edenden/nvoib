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

static void event_build_context(struct rdma_cm_id *id, struct ibv_comp_channel *cc);
static void event_build_connection(struct rdma_cm_id *id);
static void event_set_mr(struct rdma_cm_id *id, struct nvoib_dev *pci_dev);
static struct ibv_comp_channel *event_comp_channel_init(struct rdma_cm_id *id, int ep_fd);
static void event_set_conn_params(struct rdma_conn_param *params);
static void event_set_qp_attr(struct context *ctx, struct ibv_qp_init_attr *qp_attr);
static void event_context_init_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev);
static void event_cleanup_context(struct rdma_cm_id *id);

int event_alloc_context(struct rdma_cm_id *id){
	struct context *ctx;

	ctx = (struct context *)malloc(sizeof(struct context));
	ctx->initialized = 0;

	/* following is temporary for debug */
	ctx->temp_start.next = NULL;
	ctx->temp_last_ptr = &(ctx->temp_start);
	/* here */

	id->context = ctx;
        return 0;
}

static void event_build_context(struct rdma_cm_id *id, struct ibv_comp_channel *cc){
	struct context *ctx;

	ctx = (struct context *)id->context;

	ctx->pd = ibv_alloc_pd(id->verbs);
	if(ctx->pd == NULL){
		exit(EXIT_FAILURE);
	}

	ctx->cq = ibv_create_cq(id->verbs, RING_SIZE, id, cc, 0);
	if(ctx->cq == NULL){
		exit(EXIT_FAILURE);
	}

	if(ibv_req_notify_cq(ctx->cq, 0) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

static void event_build_connection(struct rdma_cm_id *id){
	struct ibv_qp_init_attr qp_attr;
	struct context *ctx = (struct context *)id->context;

	event_set_qp_attr(ctx, &qp_attr);
	if(rdma_create_qp(id, ctx->pd, &qp_attr) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

static void event_set_mr(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
	struct context *ctx = (struct context *)id->context;

	ctx->guest_memory_mr = ibv_reg_mr(ctx->pd, pci_dev->guest_memory, pci_dev->ram_size,
		IBV_ACCESS_LOCAL_WRITE);
	if(ctx->guest_memory_mr == NULL){
		exit(EXIT_FAILURE);
	}
}

static struct ibv_comp_channel *event_comp_channel_init(struct rdma_cm_id *id, int ep_fd){
	struct ibv_comp_channel *cc;

	cc = ibv_create_comp_channel(id->verbs);
	return cc;
}

void nvoib_epoll_add(int new_fd, int ep_fd){
	struct epoll_event ev;

	memset(&ev, 0, sizeof(struct epoll_event));
	ev.events = EPOLLIN;
	ev.data.fd = new_fd;

	if(epoll_ctl(ep_fd, EPOLL_CTL_ADD, new_fd, &ev) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

static void event_set_conn_params(struct rdma_conn_param *params){
	memset(params, 0, sizeof(*params));

	params->initiator_depth = params->responder_resources = 1;
	params->rnr_retry_count = 7; /* infinite retry */
}

static void event_set_qp_attr(struct context *ctx, struct ibv_qp_init_attr *qp_attr){
	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->send_cq = ctx->cq;
	qp_attr->recv_cq = ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = RING_SIZE;
	qp_attr->cap.max_recv_wr = RING_SIZE;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

static void event_context_init_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
        struct context *ctx = (struct context *)id->context;

	ctx->initialized = 1;

	/* following is temporary for debug */
	struct temp_buffer *temp;
	struct inflight *info;
	temp = ctx->temp_start.next;

	while(temp != NULL){
		info = (struct inflight *)malloc(sizeof(struct inflight));
		info->skb	= temp->info.skb;
		info->data_ptr	= temp->info.data_ptr;
		nvoib_request_send(id, pci_dev, temp->info.data_ptr, temp->size, info);
		temp = temp->next;
	}
	/* here */

        return;
}

static void event_cleanup_context(struct rdma_cm_id *id){
        struct context *ctx = (struct context *)id->context;

        ibv_dereg_mr(ctx->guest_memory_mr);
        free(ctx);
	return;
}

void event_switch(struct rdma_event_channel *ec, struct ibv_comp_channel **cc,
	struct nvoib_dev *pci_dev, int ep_fd){

	struct rdma_cm_event *event = NULL;
	struct rdma_conn_param cm_params;

	if(rdma_get_cm_event(ec, &event) == 0){
		struct rdma_cm_event event_copy;
		struct context *ctx;

		memcpy(&event_copy, event, sizeof(struct rdma_cm_event));
		rdma_ack_cm_event(event);

		switch(event_copy.event){
			case RDMA_CM_EVENT_ADDR_RESOLVED:
				dprintf("test: RDMA_CM_EVENT_ADDR_RESOLVED occured\n");
				if(*cc == NULL){
					*cc = event_comp_channel_init(event_copy.id, ep_fd);
				}

				event_build_context(event_copy.id, *cc);
				event_build_connection(event_copy.id);
				event_set_mr(event_copy.id, pci_dev);

				if(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ROUTE_RESOLVED:
				dprintf("test: RDMA_CM_EVENT_ROUTE_RESOLVED occured\n");
				event_set_conn_params(&cm_params);
				if(rdma_connect(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}

				break;

			case RDMA_CM_EVENT_CONNECT_REQUEST:
				dprintf("test: RDMA_CM_EVENT_CONNECT_REQUEST occured\n");
				if(*cc == NULL){
					*cc = event_comp_channel_init(event_copy.id, ep_fd);
				}

                                event_build_context(event_copy.id, *cc);
                                event_build_connection(event_copy.id);
				event_set_mr(event_copy.id, pci_dev);
				ring_rx_avail(event_copy.id, pci_dev);

				event_set_conn_params(&cm_params);
				if(rdma_accept(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ESTABLISHED:
				dprintf("test: RDMA_CM_EVENT_ESTABLISHED occured\n");
				ctx = (struct context *)event_copy.id->context;
				event_context_init_completed(event_copy.id, pci_dev);
				break;

			case RDMA_CM_EVENT_DISCONNECTED:
				dprintf("test: RDMA_CM_EVENT_DISCONNECTED occured\n");
				rdma_destroy_qp(event_copy.id);
				event_cleanup_context(event_copy.id);
				rdma_destroy_id(event_copy.id);
				break;

			default:
				dprintf("unknown event = %d\n", event_copy.event);
				exit(EXIT_FAILURE);
				break;
		}
	}
}

