#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/epoll.h>

#include "debug.h"
#include "nvoib.h"
#include "rdma.h"

static void rx_start_listen(struct rdma_event_channel *ec, const char *recv_port);

void *rx_wait(void *arg){
	struct nvoib_dev *pci_dev = (struct nvoib_dev *)arg;
        struct rdma_event_channel *ec = NULL;
	struct ibv_comp_channel *cc = NULL;
	int ep_fd, ec_fd, cc_fd = 0;
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
	nvoib_epoll_add(ec_fd, ep_fd);

	rx_start_listen(ec, "12345");

	/* waiting event on RDMA... */
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
				comp_pull(cc, pci_dev, comp_server_work_completed);
			}
		}
	}

        rdma_destroy_event_channel(ec);
	return 0;
}

static void rx_start_listen(struct rdma_event_channel *ec, const char *recv_port){
        struct sockaddr_in addr;
        struct rdma_cm_id *id = NULL;

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

	if(event_alloc_context(id) != 0){
		exit(EXIT_FAILURE);
	}

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(atoi(recv_port));

        if(rdma_bind_addr(id, (struct sockaddr *)&addr) != 0){
                exit(EXIT_FAILURE);
        }

        /* backlog=10 is arbitrary */
        if(rdma_listen(id, 10) != 0){
                exit(EXIT_FAILURE);
        }

	return;
}

void rx_kick_guest(int ev_fd){
	uint64_t val = 1;

	if(write(ev_fd, &val, sizeof(uint64_t)) < 0){
		printf("failed to send eventfd\n");
	}
}
