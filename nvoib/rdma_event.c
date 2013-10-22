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
#include "rdma_server.h"
#include "rdma_client.h"

static void rdma_build_context(struct rdma_cm_id *id, struct ibv_comp_channel *cc);
static void rdma_build_connection(struct rdma_cm_id *id);
static struct ibv_comp_channel *rdma_comp_channel_init(struct rdma_cm_id *id, int ep_fd);
static void rdma_set_conn_params(struct rdma_conn_param *params);
static void rdma_set_qp_attr(struct context *ctx, struct ibv_qp_init_attr *qp_attr);
static void rdma_context_init_completed(struct rdma_cm_id *id);
static void rdma_cleanup_context(struct rdma_cm_id *id);

int rdma_alloc_context(struct rdma_cm_id *id){
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

static void rdma_build_context(struct rdma_cm_id *id, struct ibv_comp_channel *cc){
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

static void rdma_build_connection(struct rdma_cm_id *id){
	struct ibv_qp_init_attr qp_attr;
	struct context *ctx = (struct context *)id->context;

	rdma_set_qp_attr(ctx, &qp_attr);
	if(rdma_create_qp(id, ctx->pd, &qp_attr) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

static void rdma_set_mr(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
	struct context *ctx = (struct context *)id->context;

	ctx->guest_memory_mr = ibv_reg_mr(ctx->pd, pci_dev->guest_memory, pci_dev->ram_size,
		IBV_ACCESS_LOCAL_WRITE);
	if(ctx->guest_memory_mr == NULL){
		exit(EXIT_FAILURE);
	}
}

static struct ibv_comp_channel *rdma_comp_channel_init(struct rdma_cm_id *id, int ep_fd){
	struct ibv_comp_channel *cc;

	cc = ibv_create_comp_channel(id->verbs);
	return cc;
}

void add_fd_to_epoll(int new_fd, int ep_fd){
	struct epoll_event ev;

	memset(&ev, 0, sizeof(struct epoll_event));
	ev.events = EPOLLIN;
	ev.data.fd = new_fd;

	if(epoll_ctl(ep_fd, EPOLL_CTL_ADD, new_fd, &ev) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

static void rdma_set_conn_params(struct rdma_conn_param *params){
	memset(params, 0, sizeof(*params));

	params->initiator_depth = params->responder_resources = 1;
	params->rnr_retry_count = 7; /* infinite retry */
}

static void rdma_set_qp_attr(struct context *ctx, struct ibv_qp_init_attr *qp_attr){
	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->send_cq = ctx->cq;
	qp_attr->recv_cq = ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = RING_SIZE;
	qp_attr->cap.max_recv_wr = RING_SIZE;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

static void rdma_context_init_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev){
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
		rdma_request_send(id, pci_dev, temp->info.data_ptr, temp->size, info);
		temp = temp->next;
	}
	/* here */

        return;
}

static void rdma_cleanup_context(struct rdma_cm_id *id){
        struct context *ctx = (struct context *)id->context;

        ibv_dereg_mr(ctx->guest_memory_mr);
        free(ctx);
	return;
}

void rdma_event(struct rdma_event_channel *ec, struct ibv_comp_channel **cc,
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
					*cc = rdma_comp_channel_init(event_copy.id, ep_fd);
				}

				rdma_build_context(event_copy.id, *cc);
				rdma_build_connection(event_copy.id);
				rdma_set_mr(event_copy.id, pci_dev);

				if(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ROUTE_RESOLVED:
				dprintf("test: RDMA_CM_EVENT_ROUTE_RESOLVED occured\n");
				rdma_set_conn_params(&cm_params);
				if(rdma_connect(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}

				break;

			case RDMA_CM_EVENT_CONNECT_REQUEST:
				dprintf("test: RDMA_CM_EVENT_CONNECT_REQUEST occured\n");
				if(*cc == NULL){
					*cc = rdma_comp_channel_init(event_copy.id, ep_fd);
				}

                                rdma_build_context(event_copy.id, *cc);
                                rdma_build_connection(event_copy.id);
				rdma_set_mr(event_copy.id, pci_dev);
				rxavail_ring_to_recv_queue(id, pci_dev);

				rdma_set_conn_params(&cm_params);
				if(rdma_accept(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ESTABLISHED:
				dprintf("test: RDMA_CM_EVENT_ESTABLISHED occured\n");
				ctx = (struct context *)event_copy.id->context;
				rdma_context_init_completed(event_copy.id, pci_dev);
				break;

			case RDMA_CM_EVENT_DISCONNECTED:
				dprintf("test: RDMA_CM_EVENT_DISCONNECTED occured\n");
				rdma_destroy_qp(event_copy.id);
				rdma_cleanup_context(event_copy.id);
				rdma_destroy_id(event_copy.id);
				break;

			default:
				dprintf("unknown event = %d\n", event_copy.event);
				exit(EXIT_FAILURE);
				break;
		}
	}
}

void *rx_wait(void *arg){
	struct nvoib_dev *pci_dev = (struct nvoib_dev *)arg;
        struct rdma_event_channel *ec = NULL;
	struct ibv_comp_channel *cc = NULL;
	int tun_fd;
	int ep_fd;
	int ec_fd;
	int cc_fd;
	struct epoll_event ev_ret[MAX_EVENTS];
	int i, fd_num, fd;

        if((ep_fd = epoll_create(MAX_EVENTS)) < 0){
                exit(EXIT_FAILURE);
        }

        ec = rdma_create_event_channel();
        if(ec == NULL){
                exit(EXIT_FAILURE);
        }

        ec_fd = ec->fd;
	add_fd_to_epoll(ec_fd, ep_fd);

	server_start_listen(ec, "12345");

	/* waiting event on RDMA... */
	while(1){
		if((fd_num = epoll_wait(ep_fd, ev_ret, MAX_EVENTS, -1)) < 0){
                        /* 'interrupted syscall error' occurs when using gdb */
                        continue;
                }

		for(i = 0; i < fd_num; i++){
			fd = ev_ret[i].data.fd;

			if(fd == ec_fd){
				rdma_event(ec, &cc, pci_dev, ep_fd);
				if(cc != NULL){
					cc_fd = cc->fd;
					add_fd_to_epoll(cc_fd, ep_fd);
				}
			}else if(fd == cc_fd){
				cq_pull(cc, pci_dev, cq_server_work_completed);
			}
		}
	}

        rdma_destroy_event_channel(ec);
	return 0;
}
