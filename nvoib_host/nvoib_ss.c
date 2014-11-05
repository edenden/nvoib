#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <mqueue.h>

#include "debug.h"
#include "nvoib_pci.h"
#include "nvoib.h"

static int session_set_mr(struct session *ss, struct nvoib_dev *dev);
static void session_prepare_multicast(struct session *ss, struct nvoib_dev *dev);
static void session_start_rx(struct session *ss, struct nvoib_dev *dev);
static void session_start_tx(struct session *ss);

struct session *session_init(struct nvoib_dev *dev){
	struct session *ss;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct ibv_qp_init_attr qp_init_attr;
	struct ibv_qp_attr qp_attr;

	ss = malloc(sizeof(struct session));
	memset(ss, 0, sizeof(struct session));

        dev_list = ibv_get_device_list(NULL);
        if (!dev_list) {
                printf("Failed to get IB devices list");
		exit(EXIT_FAILURE);
        }

	ib_dev = *dev_list;
	if (!ib_dev) {
		printf("No IB devices found\n");
		exit(EXIT_FAILURE);
	}

        ss->ibverbs = ibv_open_device(ib_dev);
        if(!ss->ibverbs) {
		printf("failed to open device\n");
		exit(EXIT_FAILURE);
        }

	if(ibv_query_port(ss->ibverbs, NVOIB_PORT, &ss->portinfo)){
		printf("failed to query port\n");
		exit(EXIT_FAILURE);
	}

        ss->pd = ibv_alloc_pd(ss->ibverbs);
        if (!ss->pd) {
		printf("failed to alloc pd\n");
		exit(EXIT_FAILURE);
        }

	if(session_set_mr(ss, dev)){
		printf("failed to set mr\n");
                exit(EXIT_FAILURE);
	}

	/* TX completion queue init */
        ss->tx_cc = ibv_create_comp_channel(ss->ibverbs);
        if (!ss->tx_cc) {
                printf("failed to create comp tx comp channel");
                exit(EXIT_FAILURE);
        }

        ss->tx_cq = ibv_create_cq(ss->ibverbs, RING_SIZE, NULL, ss->tx_cc, TX_CPU_AFFINITY);
        if (!ss->tx_cq) {
		printf("failed to create tx completion queue\n");
		exit(EXIT_FAILURE);
        }

        if(ibv_req_notify_cq(ss->tx_cq, 0) != 0){
		printf("failed to request notifying tx cq\n");
                exit(EXIT_FAILURE);
        }

	/* RX completion queue init */
        ss->rx_cc = ibv_create_comp_channel(ss->ibverbs);
        if (!ss->rx_cc) {
		printf("failed to create rx comp channel\n");
		exit(EXIT_FAILURE);
        }

        ss->rx_cq = ibv_create_cq(ss->ibverbs, RING_SIZE, NULL, ss->rx_cc, RX_CPU_AFFINITY);
        if (!ss->rx_cq) {
		printf("failed to create rx completion queue\n");
		exit(EXIT_FAILURE);
        }

        if(ibv_req_notify_cq(ss->rx_cq, 0) != 0){
                printf("failed to request notifying rx cq\n");
                exit(EXIT_FAILURE);
        }

	/* Create QP */
	memset(&qp_init_attr, 0, sizeof(struct ibv_qp_init_attr));
	qp_init_attr.send_cq = ss->tx_cq;
	qp_init_attr.recv_cq = ss->rx_cq;
	qp_init_attr.cap.max_send_wr = RING_SIZE;
	qp_init_attr.cap.max_recv_wr = RING_SIZE;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_UD;

	ss->qp = ibv_create_qp(ss->pd, &qp_init_attr);
	if (!ss->qp)  {
		printf("failed to create qp\n");
		exit(EXIT_FAILURE);
	}

	printf("MAIN: local lid = %x, local qpn = %x\n", ss->portinfo.lid, ss->qp->qp_num);

	/* Set Qkey to QP */
	memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
	qp_attr.qp_state	= IBV_QPS_INIT;
	qp_attr.pkey_index	= 0; /* partition key's index (like vlan) */
	qp_attr.port_num	= NVOIB_PORT;
	qp_attr.qkey		= dev->tenant_id; 

	if(ibv_modify_qp(ss->qp, &qp_attr,
	IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)){
		printf("failed to modify qp\n");
		exit(EXIT_FAILURE);
	}

	session_prepare_multicast(ss, dev);

	session_start_rx(ss, dev);
	session_start_tx(ss);

	return ss;
}

static int session_set_mr(struct session *ss, struct nvoib_dev *dev){
        ss->guest_memory_mr = ibv_reg_mr(ss->pd, dev->guest_memory, dev->ram_size,
                IBV_ACCESS_LOCAL_WRITE);
        if(ss->guest_memory_mr == NULL){
		return -1;
        }

	return 0;
}

static void session_prepare_multicast(struct session *ss, struct nvoib_dev *dev){
	union ibv_gid mgid;
	struct ibv_ah_attr ah_attr;
	struct forward_entry *entry;
	volatile struct forward_db *fdb;

	dprintf("MAIN: multicast init tenant_id = %d\n", dev->tenant_id);

        /* attach qp to multicast group */
        inet_pton(AF_INET6, MCAST_BASE, mgid.raw);
        *(uint32_t *)(&mgid.raw[12]) = htonl(dev->tenant_id);
        if (ibv_attach_mcast(ss->qp, &mgid, 0xc000 + dev->tenant_id)){
                printf("failed to attach qp to multicast group\n");
                exit(EXIT_FAILURE);
        }

	entry = malloc(sizeof(struct forward_entry));

	memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
        ah_attr.is_global	= 1;
	ah_attr.grh.dgid	= mgid;
        ah_attr.dlid		= 0xc000 + dev->tenant_id;
        ah_attr.port_num	= NVOIB_PORT;
        entry->ah = ibv_create_ah(ss->pd, &ah_attr);
        if(!entry->ah) {
                printf("failed to create ah\n");
                exit(EXIT_FAILURE);
        }

	entry->qpn = 0xffffff;

	fdb = (volatile struct forward_db *)&ss->fdb;
	fdb->entry[0] = entry;
	return;
}

static void session_start_rx(struct session *ss, struct nvoib_dev *dev){
	struct ibv_qp_attr qp_attr;

	/* move qp state to Ready to Receive */
	qp_attr.qp_state = IBV_QPS_RTR;

        if(ibv_modify_qp(ss->qp, &qp_attr, IBV_QP_STATE)) {
		printf("failed to move qp state to Ready to Receive\n");
		exit(EXIT_FAILURE);
        }

	ring_rx_avail(ss, dev);
}

static void session_start_tx(struct session *ss){
	struct ibv_qp_attr qp_attr;

        qp_attr.qp_state       = IBV_QPS_RTS;
	/* set initial packet sequence number */
        qp_attr.sq_psn         = lrand48() & 0xffffff;

        if (ibv_modify_qp(ss->qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
		printf("failed to move qp state to Ready to Send\n");
		exit(EXIT_FAILURE);
        }
}
