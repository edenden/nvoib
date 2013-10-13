void rdma_request_next_msg(struct rdma_cm_id *id);
void rdma_request_next_write(struct rdma_cm_id *id);
void *rdma_event_handling(void *arg);

#define TIMEOUT_IN_MS 500
#define MAX_EVENTS 16
#define RDMA_SLOT 8

enum message_id {
	MSG_MR = 0,
	MSG_ASSIGN,
};

struct thread_args {
	int ep_fd;
	mqd_t sl_mq;
	struct nvoib_dev *pci_dev;
	struct rdma_event_channel *ec;
}

struct message {
	uint32_t id;
	uint64_t addr;
	uint32_t rkey;
	uint32_t slot_num;
	uint64_t data_ptr[RDMA_SLOT];
};

struct rdma_slot {
        void            *skb;
        uint64_t        data_ptr;
};

struct context {
        struct ibv_pd *pd;
        struct ibv_cq *cq;
        struct ibv_comp_channel *comp_channel;
        struct nvoib_dev *pci_dev;
	int ep_fd;

	pthread_mutex_t msg_mutex;
        struct message *msg;
        struct ibv_mr *msg_mr;
        struct ibv_mr *guest_memory_mr;
	int rx_flag;

	pthread_mutex_t slot_mutex;
	int next_slot;
	int remain_slot;
	int next_slot_assign;
	int slot_assign_num;
	struct rdma_slot slot[RDMA_SLOT];

        /* Used only on tx side */
        uint64_t peer_addr;
        uint32_t peer_rkey;
};

struct buf_data {
	void		*skb;
	uint64_t	data_ptr;
	uint32_t	size;
	uint32_t	flag;
};

struct ring_buf {
	struct buf_data buf[RING_SIZE];
	uint32_t	ring_empty;
};

struct shared_region {
	struct ring_buf	rx_avail;	/* Guest prepares empty buffer to 'rx_avail' from ZONE_DMA */
	struct ring_buf	rx;		/* Host writes received data to 'rx' using RDMA */
	struct ring_buf	tx;		/* Guest chains TX buffer to 'tx' */
	struct ring_buf tx_used;	/* Host chains processed buffer to 'tx_used' */
};

