#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include "debug.h"
#include "rdma_event.h"
#include "rdma_client.h"
#include "rdma_cq.h"

static struct rdma_cm_id *client_start_connect(struct rdma_event_channel *ec,
	const char *dest_host, const char *dest_port);
static void client_context_prepare(struct rdma_cm_id *id);
static void client_send_remote(struct rdma_cm_id *id, uint64_t offset,
        uint32_t size, struct txbuf_info *info);
static void client_tun_init(int tun_fd, int ep_fd);

void *tx_wait(void *arg){
	struct nvoib_dev *pci_dev = (struct nvoib_dev *)arg;
	struct rdma_event_channel *ec = NULL;
	struct ibv_comp_channel *cc = NULL;
        struct epoll_event ev_ret[MAX_EVENTS];
        int i, fd_num, fd;
	int ep_fd;
	int ev_fd;
	int ec_fd;
	int cc_fd;

        if((ep_fd = epoll_create(MAX_EVENTS)) < 0){
                exit(EXIT_FAILURE);
        }

        ec = rdma_create_event_channel();
        if(ec == NULL){
                exit(EXIT_FAILURE);
        }

        ec_fd = ec->fd;
	add_fd_to_epoll(ec_fd, ep_fd);

	ev_fd = pci_dev->tx_fd;
	add_fd_to_epoll(ev_fd, ep_fd);

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
				cq_pull(cc, pci_dev, cq_client_work_completed);
			}else if(fd == ev_fd){
				tx_ring_to_send_queue(ec, pci_dev, ev_fd);
			}
		}
	} 

	rdma_destroy_event_channel(ec);
	return NULL;
}

static struct rdma_cm_id *client_start_connect(struct rdma_event_channel *ec,
	const char *dest_host, const char *dest_port){

        struct addrinfo *addr;
        struct rdma_cm_id *id = NULL;

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

	if(rdma_alloc_context(id) != 0){
		exit(EXIT_FAILURE);
	}

        if(getaddrinfo(dest_host, dest_port, NULL, &addr) != 0){
                exit(EXIT_FAILURE);
        }

        if(rdma_resolve_addr(id, NULL, addr->ai_addr, TIMEOUT_IN_MS) != 0){
                exit(EXIT_FAILURE);
        }

        freeaddrinfo(addr);

	return id;
}

void schedule_tx_process(int ev_fd){
	uint64_t val = 1;

	if(write(ev_fd, &val, sizeof(uint64_t)) < 0){
		printf("failed to send eventfd\n");
	}
}
