#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/epoll.h>
#include <mqueue.h>

#include "main.h"
#include "rdma.h"

static void rdma_build_context(struct ibv_context *verbs);
static void rdma_set_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void event_loop(struct rdma_event_channel *ec, int exit_on_disconnect);
static void * poll_cq(void *);

struct context *rdma_rdma_build_context(struct rdma_cm_id *id, struct rdmanic *pci_dev,
	int ep_fd, mqd_t sl_mq, int flag){

	struct context *ctx;
	struct epoll_event ev;
	ctx = (struct context *)malloc(sizeof(struct context));
	id->context = ctx;

	ctx->pci_dev = pci_dev;
	ctx->offset = 0
	ctx->recv_count = 0;
	ctx->ep_fd = ep_fd;
	ctx->rx_flag = flag;
	ctx->msg_mutex = PTHREAD_MUTEX_INITIALIZER;

	ctx->pd = ibv_alloc_pd(id->verbs);
	if(ctx->pd == NULL){
		exit(EXIT_FAILURE);
	}

	ctx->comp_channel = ibv_create_comp_channel(id->verbs);
	if(ctx->comp_channel == NULL){
		exit(EXIT_FAILURE);
	}

	/* cqe = 10 is arbitrary */
	ctx->cq = ibv_create_cq(id->verbs, 10, NULL, ctx->comp_channel, 0);
	if(ctx->cq == NULL){
		exit(EXIT_FAILURE);
	}

	if(ibv_req_notify_cq(ctx->cq, 0) != 0){
		exit(EXIT_FAILURE);
	}

	ctx->slot_mutex = PTHREAD_MUTEX_INITIALIZER;
	ctx->next_slot = 0;
	ctx->next_slot_assign = 0;

	memset(&ev, 0, sizeof(struct epoll_event));
	ev.events = EPOLLIN;
	ev.data.ptr = id;

	if(epoll_ctl(ctx->ep_fd, EPOLL_CTL_ADD, ctx->comp_channel->fd, &ev) != 0){
		exit(EXIT_FAILURE);
	}

	return ctx;
}

void rdma_build_connection(struct rdma_cm_id *id){
	struct ibv_qp_init_attr qp_attr;
	struct context *ctx = (struct context *)id->context;

	rdma_set_qp_attr(ctx, &qp_attr);
	if(rdma_create_qp(id, ctx->pd, &qp_attr) != 0){
		exit(EXIT_FAILURE);
	}

	return;
}

void rdma_set_conn_params(struct rdma_conn_param *params)
{
	memset(params, 0, sizeof(*params));

	params->initiator_depth = params->responder_resources = 1;
	params->rnr_retry_count = 7; /* infinite retry */
}

void rdma_set_qp_attr(struct context *ctx, struct ibv_qp_init_attr *qp_attr)
{
	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->send_cq = ctx->cq;
	qp_attr->recv_cq = ctx->cq;
	qp_attr->qp_type = IBV_QPT_RC;

	qp_attr->cap.max_send_wr = 10;
	qp_attr->cap.max_recv_wr = 10;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
}

static void rdma_request_msg(struct rdma_cm_id *id){
        struct context *ctx = (struct context *)id->context;

        struct ibv_recv_wr wr, *bad_wr = NULL;
        struct ibv_sge sge;

        memset(&wr, 0, sizeof(wr));

        wr.wr_id = (uintptr_t)id;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        sge.addr = (uintptr_t)ctx->msg;
        sge.length = sizeof(*ctx->msg);
        sge.lkey = ctx->msg_mr->lkey;

        if(ibv_post_recv(id->qp, &wr, &bad_wr) != 0){
                exit(EXIT_FAILURE);
        }
}

static void rdma_request_write_with_imm(struct rdma_cm_id *id){
        struct ibv_recv_wr wr, *bad_wr = NULL;

        memset(&wr, 0, sizeof(wr));

        wr.wr_id = (uintptr_t)id;
        wr.sg_list = NULL;
        wr.num_sge = 0;

        if(ibv_post_recv(id->qp, &wr, &bad_wr) != 0){
                exit(EXIT_FAILURE);
        }
}

static void rdma_cleanup_context(struct rdma_cm_id *id){
        struct context *ctx = (struct context *)id->context;

        ibv_dereg_mr(ctx->guest_memory_mr);
        ibv_dereg_mr(ctx->msg_mr);

	if(epoll_ctl(ctx->ep_fd, EPOLL_CTL_DEL, ctx->comp_channel->fd, NULL) != 0){
		err(EXIT_FAILURE, "failed to remove epoll");
	}

        free(ctx->msg);
        free(ctx);
}

void rdma_event_loop(struct rdma_event_channel *ec, struct rdmanic *pci_dev, int ep_fd){
	struct rdma_cm_event *event = NULL;
	struct rdma_conn_param cm_params;
	struct context *ctx = NULL;

	rdma_set_conn_params(&cm_params);

	while(rdma_get_cm_event(ec, &event) == 0){
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(struct rdma_cm_event));
		rdma_ack_cm_event(event);

		switch(event_copy.event){
			case RDMA_CM_EVENT_ADDR_RESOLVED:
				ctx = rdma_build_context(event_copy.id, pci_dev, ep_fd, 0);
				rdma_build_connection(event_copy.id);
				tx_set_memory_region(event_copy.id);
				rdma_request_msg(id);

				if(rdma_resolve_route(event_copy.id, TIMEOUT_IN_MS) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ROUTE_RESOLVED:
				if(rdma_connect(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_CONNECT_REQUEST:
                                ctx = rdma_build_context(event_copy.id, pci_dev, ep_fd, 1);
                                rdma_build_connection(event_copy.id);
				rx_set_memory_region(event_copy.id);
				rdma_request_write_with_imm(id);

				if(rdma_accept(event_copy.id, &cm_params) != 0){
					exit(EXIT_FAILURE);
				}
				break;

			case RDMA_CM_EVENT_ESTABLISHED:
				rx_on_connection(event_copy.id);
				break;

			case RDMA_CM_EVENT_DISCONNECTED:
				rdma_destroy_qp(event_copy.id);
				rdma_cleanup_context(event_copy.id);
				rdma_destroy_id(event_copy.id);
				break;

			default:
				printf("unknown event\n");
				exit(EXIT_FAILURE);
				break;
		}
	}
}

void *rdma_event_handling(void *arg){
	struct rdmanic *pci_dev = (struct rdmanic *)args;
        struct rdma_event_channel *ec = NULL;
	pthread_t tx_wait_thread;
	pthraed_t rx_assign_thread;
	pthread_t cq_poller_thread;
	struct thread_args tharg;
	int ep_fd;
	mqd_t sl_mq;

        ec = rdma_create_event_channel();
        if(ec == NULL){
                exit(EXIT_FAILURE);
        }

	if((ep_fd = epoll_create(MAX_EVENTS)) < 0){
		exit(EXIT_FAILURE);
	}

        sl_mq = mq_open("/sl_mq", O_RDWR | O_CREAT);
        if(sl_mq < 0){
                exit(EXIT_FAILURE);
        }

	rx_start_listen(ec, "12345");

	tharg.ep_fd = ep_fd;
	tharg.sl_mq = sl_mq;
	tharg.pci_dev = pci_dev;
	tharg.ec = ec;

	if(pthread_create(&rx_assign_thread, NULL, rx_wait_assign, &tharg) != 0){
		exit(EXIT_FAILURE);
	}

	if(pthread_create(&cq_poller_thread, NULL, rdma_cq_handling, &tharg) != 0){
                exit(EXIT_FAILURE);
        }

        if(pthread_create(&tx_wait_thread, NULL, tx_wait_ring, &tharg) != 0){
                exit(EXIT_FAILURE);
        }

	/* waiting event on RDMA... */
        event_loop(ec, pci_dev, ep_fd);

        rdma_destroy_event_channel(ec);
}

void *rdma_cq_handling(void *arg){
	struct ibv_cq *cq;
	struct ibv_wc wc;
	struct rdma_cm_id *id;
	struct context *ctx;
	struct thread_args *tharg = (struct thread_args *)arg;
	int ep_fd = tharg->ep_fd;
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

			if(ibv_get_cq_event(ctx->comp_channel, &cq, &ctx) != 0){
				exit(EXIT_FAILURE);
			}

			ibv_ack_cq_events(cq, 1);

			if(ibv_req_notify_cq(cq, 0) != 0){
				exit(EXIT_FAILURE);
			}

			while(ibv_poll_cq(cq, 1, &wc)){
				if (wc.status == IBV_WC_SUCCESS){
					if(ctx->rx_flag){
						rxcon_work_completed(id, &wc);
					}else{
						txcon_work_completed(id, &wc);
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



