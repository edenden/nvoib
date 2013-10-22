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
#include "nvoib.h"
#include "rdma.h"

void *tx_wait(void *arg){
	struct nvoib_dev *pci_dev = (struct nvoib_dev *)arg;
	struct rdma_event_channel *ec = NULL;
	struct ibv_comp_channel *cc = NULL;
        struct epoll_event ev_ret[MAX_EVENTS];
        int i, fd_num, fd;
	int ep_fd, ev_fd, ec_fd, cc_fd = 0;

        if((ep_fd = epoll_create(MAX_EVENTS)) < 0){
                exit(EXIT_FAILURE);
        }

        ec = rdma_create_event_channel();
        if(ec == NULL){
                exit(EXIT_FAILURE);
        }

        ec_fd = ec->fd;
	nvoib_epoll_add(ec_fd, ep_fd);

	ev_fd = pci_dev->tx_fd;
	nvoib_epoll_add(ev_fd, ep_fd);

	while(1){
                if((fd_num = epoll_wait(ep_fd, ev_ret, MAX_EVENTS, -1)) < 0){
                        /* 'interrupted syscall error' occurs when using gdb */
                        continue;
                }

		for(i = 0; i < fd_num; i++){
			fd = ev_ret[i].data.fd;

			if(fd == ec_fd){
                                event_switch(ec, &cc, pci_dev, ep_fd);
                                if(cc != NULL){
                                        cc_fd = cc->fd;
					nvoib_epoll_add(cc_fd, ep_fd);
                                }
			}else if(fd == cc_fd){
				comp_pull(cc, pci_dev, comp_client_work_completed);
			}else if(fd == ev_fd){
				ring_tx(ec, pci_dev, ev_fd);
			}
		}
	} 

	rdma_destroy_event_channel(ec);
	return NULL;
}

struct rdma_cm_id *tx_start_connect(struct rdma_event_channel *ec,
	const char *dest_host, const char *dest_port){

        struct addrinfo *addr;
        struct rdma_cm_id *id = NULL;

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

	if(event_alloc_context(id) != 0){
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

void tx_schedule_process(int ev_fd){
	uint64_t val = 1;

	if(write(ev_fd, &val, sizeof(uint64_t)) < 0){
		printf("failed to send eventfd\n");
	}
}
