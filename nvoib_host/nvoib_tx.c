#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sched.h>
#include <sys/timerfd.h>
#include <netinet/ether.h>
#include <infiniband/verbs.h>
#include <mqueue.h>

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

static void tx_fdb_register(struct forward_db *fdb, struct forward_message *message);

void *tx_wait(void *arg){
	struct thread_param *param;
	struct session *ss;
	struct nvoib_dev *dev;
        struct epoll_event ev_ret[MAX_EVENTS];
        int i, fd_num, fd, timer_set = 0, miss_count = 0;
	int ep_fd, tm_fd, ev_fd, cc_fd, mq_fd;
        char *mq_buf;
        struct mq_attr mq_attr;
	cpu_set_t cpu_mask;

	CPU_ZERO(&cpu_mask);
	CPU_SET(TX_CPU_AFFINITY, &cpu_mask);
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

	ev_fd = event_notifier_get_fd(&dev->tx_event);
	nvoib_epoll_add(ev_fd, ep_fd);

	tm_fd = timerfd_create(CLOCK_MONOTONIC, 0);
	nvoib_epoll_add(tm_fd, ep_fd);

        cc_fd = ss->tx_cc->fd;
        nvoib_epoll_add(cc_fd, ep_fd);

	mq_getattr(ss->mq_fd, &mq_attr);
	mq_buf = malloc(mq_attr.mq_msgsize);

	mq_fd = (int)ss->mq_fd;
	nvoib_epoll_add(mq_fd, ep_fd);

	while(1){
                if((fd_num = epoll_wait(ep_fd, ev_ret, MAX_EVENTS, -1)) < 0){
                        /* 'interrupted syscall error' occurs when using gdb */
                        continue;
                }

		for(i = 0; i < fd_num; i++){
			fd = ev_ret[i].data.fd;

			if(fd == ev_fd){
				dprintf("TX: host kick\n");
				event_notifier_test_and_clear(&dev->tx_event);

				if(!timer_set){
					nvoib_kick_disable(dev);
					nvoib_set_timer(tm_fd, TX_POLL_INTERVAL);
					ring_tx_avail(ss, dev, TX_POLL_BADGET);
					miss_count = 0;
					timer_set = 1;
				}
			}else if(fd == tm_fd){
                                nvoib_event_clear(tm_fd);

				if(ring_tx_avail(ss, dev, TX_POLL_BADGET)){
					dprintf("TX: packet sending completed\n");
					miss_count = 0;
				}else{
					miss_count++;
				}

				if(miss_count > TX_POLL_RETRY){
					dprintf("TX: polling time out\n");
					nvoib_unset_timer(tm_fd);
					nvoib_kick_enable(dev);
					timer_set = 0;
				}
                        }else if(fd == cc_fd){
                                dprintf("TX: completion occured\n");
                                comp_pull(ss, ss->tx_cc, dev, comp_tx_work_completed);
				smp_wmb();
			}else if(fd == mq_fd){
				struct forward_message *message;

				if(mq_receive(ss->mq_fd, mq_buf, mq_attr.mq_msgsize, NULL) < 0){
					printf("TX: failed to receive message queue\n");
					exit(EXIT_FAILURE);
				}

				message = (struct forward_message *)mq_buf;
				tx_fdb_register(&ss->fdb, message);
				dprintf("TX: registered new fdb entry\n");
			}
		}
	} 

	return NULL;
}

static void tx_fdb_register(struct forward_db *fdb, struct forward_message *message){

	if(fdb->entry[message->hash_key] != NULL){
		free(fdb->entry[message->hash_key]);
	}

	fdb->entry[message->hash_key] = message->entry;
	return;
}

struct forward_entry *tx_fdb_lookup(struct forward_db *fdb, void *buffer){
	struct forward_entry *entry;
	struct ethhdr *eth;
	uint16_t src_hash;

	eth = (struct ethhdr *)buffer;
	src_hash = *(uint16_t *)&eth->h_dest[4];
	entry = fdb->entry[src_hash];

	if(unlikely(!entry)){
		/* did not learn */
		dprintf("TX: Unknown destination. flooding...\n");
		return fdb->entry[0];
	}

	return entry;
}

