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

double gettimeofday_sec(void){
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec * 1e-6;
}

void nvoib_kick_enable(struct nvoib_dev *dev){
        struct shared_region *sr = dev->shared_region;

        sr->tx.interruptible = 1;
        return;
}

void nvoib_kick_disable(struct nvoib_dev *dev){
        struct shared_region *sr = dev->shared_region;

        sr->tx.interruptible = 0;
        return;
}

void nvoib_set_timer(int tm_fd, int interval){
        struct itimerspec val;

        val.it_value.tv_sec     = 0;
        val.it_value.tv_nsec    = interval;
        val.it_interval.tv_sec  = 0;
        val.it_interval.tv_nsec = interval;

        timerfd_settime(tm_fd, 0, &val, NULL);
        return;
}

void nvoib_unset_timer(int tm_fd){
        struct itimerspec val;

        val.it_value.tv_sec     = 0;
        val.it_value.tv_nsec    = 0;
        val.it_interval.tv_sec  = 0;
        val.it_interval.tv_nsec = 0;

        timerfd_settime(tm_fd, 0, &val, NULL);
        return;
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

uint64_t nvoib_event_clear(int fd){
        uint64_t val;

        if(read(fd, &val, sizeof(uint64_t)) < 0){
                printf("failed to read eventfd\n");
                exit(EXIT_FAILURE);
        }

        return val;
}
