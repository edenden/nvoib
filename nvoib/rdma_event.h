#define TIMEOUT_IN_MS 500
#define MAX_EVENTS 16
#define DEST_HOST "192.168.0.2"
#define DEST_PORT "12345"

struct inflight {
	void	*skb;
	uint64_t data_ptr;
};

struct temp_buffer {
	struct inflight info;
	int	size;
	struct temp_buffer *next;
};

struct context {
        struct ibv_pd	*pd;
        struct ibv_cq	*cq;
        struct ibv_mr	*guest_memory_mr;

	int		initialized;

	/* temporary for debug */
	struct temp_buffer temp_start;
	struct temp_buffer *temp_last_ptr;
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

void rdma_event(struct rdma_event_channel *ec, struct ibv_comp_channel **cc,
	struct nvoib_dev *pci_dev, int ep_fd);
void rdma_event_channel_init(struct rdma_event_channel *ec, int ep_fd);
int rdma_alloc_context(struct rdma_cm_id *id);
