#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>

#include "debug.h"
#include "rdma_event.h"
#include "rdma_server.h"

void server_start_listen(struct rdma_event_channel *ec, const char *recv_port){
        struct sockaddr_in addr;
        struct rdma_cm_id *id = NULL;

        if(rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0){
                exit(EXIT_FAILURE);
        }

	if(rdma_alloc_context(id) != 0){
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

void kick_guest(int ev_fd){
	uint64_t val = 1;

	if(write(ev_fd, &val, sizeof(uint64_t)) < 0){
		printf("failed to send eventfd\n");
	}
}
