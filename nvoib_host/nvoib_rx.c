#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sched.h>
#include <netinet/ether.h>
#include <infiniband/verbs.h>
#include <mqueue.h>

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

void *rx_wait(void *arg){
	struct thread_param *param;
	struct nvoib_dev *dev;
	struct shared_region *sr;
	struct session *ss;
	int ep_fd, cc_fd, tm_fd;
	struct epoll_event ev_ret[MAX_EVENTS];
	int i, fd_num, fd, timer_set = 0, miss_count = 0;
        cpu_set_t cpu_mask;

        CPU_ZERO(&cpu_mask);
        CPU_SET(RX_CPU_AFFINITY, &cpu_mask);
        if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_mask) != 0){
                printf("Failed to set CPU affinity\n");
                exit(EXIT_FAILURE);
        }

	param	= (struct thread_param *)arg;
	ss	= param->ss;
	dev	= param->dev;

        if((ep_fd = epoll_create(MAX_EVENTS)) < 0){
                exit(EXIT_FAILURE);
        }

	cc_fd = ss->rx_cc->fd;
	nvoib_epoll_add(cc_fd, ep_fd);

        tm_fd = timerfd_create(CLOCK_MONOTONIC, 0);
        nvoib_epoll_add(tm_fd, ep_fd);

	sr = dev->shared_region;

	while(1){
		if((fd_num = epoll_wait(ep_fd, ev_ret, MAX_EVENTS, -1)) < 0){
                        /* 'interrupted syscall error' occurs when using gdb */
                        continue;
                }

		for(i = 0; i < fd_num; i++){
			fd = ev_ret[i].data.fd;

			if(fd == cc_fd){
                                comp_pull(ss, ss->rx_cc, dev, comp_rx_work_completed);

				if(!timer_set){
					nvoib_set_timer(tm_fd, RX_POLL_INTERVAL);
					timer_set = 1;
				}
			}else if(fd == tm_fd){
				nvoib_event_clear(tm_fd);

				if(dev->rx_remain){
					smp_wmb();
					if(sr->rx.interruptible){
						event_notifier_set(&dev->rx_event);
					}

					dev->rx_remain = 0;
					miss_count = 0;
					dprintf("RX: packet interrupt completed\n");
				}else{
					miss_count++;
				}

				if(ring_rx_avail(ss, dev)){
					miss_count = 0;
				}

				if(miss_count > RX_POLL_RETRY){
					dprintf("RX: polling time out\n");
					nvoib_unset_timer(tm_fd);
					timer_set = 0;
					miss_count = 0;
				}
			}
		}
	}

	return 0;
}

void rx_fdb_learn(struct session *ss, struct ibv_wc *wc, void *buffer){
	struct ethhdr *eth;
	struct ibv_grh *grh;
	struct ibv_ah_attr ah_attr;
	struct forward_entry *entry;
	struct forward_message message;

	if(!(wc->wc_flags & IBV_WC_GRH)){
		printf("RX: ARP packet arrived, but there is no GRH.\n");
		return;
	}

	grh = (struct ibv_grh *)buffer;
	eth = (struct ethhdr *)(buffer + sizeof(struct ibv_grh));
	entry = malloc(sizeof(struct forward_entry));

	memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
	ah_attr.is_global       = 1;
	ah_attr.grh.dgid        = grh->sgid;
	ah_attr.dlid            = wc->slid;
	ah_attr.port_num        = NVOIB_PORT;

	entry->ah = ibv_create_ah(ss->pd, &ah_attr);
	if(!entry->ah) {
		printf("failed to create ah\n");
		exit(EXIT_FAILURE);
	}

	entry->qpn = wc->src_qp;

	message.entry = entry;
	message.hash_key = *(uint16_t *)&eth->h_source[4];

	if(mq_send(ss->mq_fd, (const char *)&message, sizeof(struct forward_message), 0) != 0){
		printf("RX: failed to send message queue\n");
		exit(EXIT_FAILURE);
	}

	dprintf("RX: requested fdb register (lid = 0x%x, qpn = 0x%x)\n", wc->slid, wc->src_qp);

        return;
}

