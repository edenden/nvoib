#define TIMEOUT_IN_MS 500
#define MAX_EVENTS 16
#define DEST_HOST "192.168.0.2"
#define DEST_PORT "12345"
#define RING_SIZE 1024

typedef void (*comp_f)(struct rdma_cm_id*, struct nvoib_dev *pci_dev, struct ibv_wc *);

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

/* Connection & Event related methods (rdma_event.c) */
int event_alloc_context(struct rdma_cm_id *id);
void nvoib_epoll_add(int new_fd, int ep_fd);
void event_switch(struct rdma_event_channel *ec, struct ibv_comp_channel **cc,
        struct nvoib_dev *pci_dev, int ep_fd);

/* Completion queue related methods (rdma_comp.c) */
void comp_pull(struct ibv_comp_channel *cc, struct nvoib_dev *pci_dev, comp_f func);
void comp_server_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc);
void comp_client_work_completed(struct rdma_cm_id *id, struct nvoib_dev *pci_dev, struct ibv_wc *wc);
void nvoib_request_recv(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
	uint64_t offset, uint32_t size, struct inflight *info);
void nvoib_request_send(struct rdma_cm_id *id, struct nvoib_dev *pci_dev,
	uint64_t offset, uint32_t size, struct inflight *info);

/* Ring buffer related methods (rdma_ring.c) */
void ring_tx_used(struct nvoib_dev *pci_dev, struct inflight *info, int size);
void ring_rx(struct nvoib_dev *pci_dev, struct inflight *info, int size);
void ring_rx_avail(struct rdma_cm_id *id, struct nvoib_dev *pci_dev);
void ring_tx(struct rdma_event_channel *ec, struct nvoib_dev *pci_dev, int ev_fd);

/* TX process related methods (rdma_tx.c) */
void *tx_wait(void *arg);
struct rdma_cm_id *tx_start_connect(struct rdma_event_channel *ec,
        const char *dest_host, const char *dest_port);
void tx_schedule_process(int ev_fd);

/* RX process related methods (rdma_rx.c) */
void *rx_wait(void *arg);
void rx_kick_guest(int ev_fd);

