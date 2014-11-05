#define MAX_EVENTS 16
#define RING_SIZE 12800
#define TX_POLL_INTERVAL 50000
#define RX_POLL_INTERVAL 50000
#define TX_POLL_RETRY 1000
#define RX_POLL_RETRY 1000
#define TX_POLL_BADGET 4096
#define RX_POLL_BADGET 128

#define MCAST_BASE "ff05::"
#define NVOIB_PORT 1

#define RX_CPU_AFFINITY 2
#define TX_CPU_AFFINITY 3

#define IS_ARP(buffer) \
(((struct ethhdr *)(buffer))->h_proto == htons(ETH_P_ARP) ? 1 : 0)

struct thread_param {
	struct session *ss;
	struct nvoib_dev *dev;
};

struct forward_entry {
        struct ibv_ah   *ah;
        uint32_t        qpn;
};

struct forward_db {
        struct forward_entry *entry[65536];
};

struct forward_message {
	struct forward_entry *entry;
	uint16_t hash_key;
};

struct session {
        struct ibv_context      *ibverbs;
        struct ibv_pd           *pd;
        struct ibv_qp           *qp;
        struct ibv_port_attr    portinfo;
        struct forward_db       fdb;
	mqd_t			mq_fd;

        struct ibv_comp_channel *rx_cc;
        struct ibv_comp_channel *tx_cc;

        struct ibv_cq           *rx_cq;
        struct ibv_cq           *tx_cq;

	struct ibv_mr		*guest_memory_mr;
};

#define ENTRY_AVAILABLE 2
#define ENTRY_INFLIGHT 1
#define ENTRY_COMPLETE 0

struct buf_data {
	volatile uint64_t	skb;
	volatile uint64_t	data_ptr;
	volatile uint32_t	size;
	volatile uint32_t	flag;
};

struct ring_buf {
	struct buf_data buf[RING_SIZE];
	volatile uint32_t	interruptible;
};

struct shared_region {
	struct ring_buf	tx;		/* Guest chains TX buffer to 'tx' */
	struct ring_buf rx;
};

typedef void (*comp_f)(struct session *ss, struct nvoib_dev *dev, struct ibv_wc *);

double gettimeofday_sec(void);

/* Common methods (nvoib_common.c) */
void nvoib_kick_enable(struct nvoib_dev *dev);
void nvoib_kick_disable(struct nvoib_dev *dev);
void nvoib_set_timer(int tm_fd, int interval);
void nvoib_unset_timer(int tm_fd);
void nvoib_epoll_add(int new_fd, int ep_fd);
uint64_t nvoib_event_clear(int fd);

/* Session related methods (nvoib_ss.c) */
struct session *session_init(struct nvoib_dev *dev);

/* Completion queue related methods (nvoib_wc.c) */
void comp_pull(struct session *ss, struct ibv_comp_channel *cc,
        struct nvoib_dev *dev, comp_f func);
void comp_rx_work_completed(struct session *ss, struct nvoib_dev *dev, struct ibv_wc *wc);
void comp_tx_work_completed(struct session *ss, struct nvoib_dev *dev, struct ibv_wc *wc);
void nvoib_request_recv(struct session *ss, struct nvoib_dev *dev,
        uint64_t data_ptr, uint32_t size);
void nvoib_request_send(struct session *ss, struct nvoib_dev *dev,
        uint64_t data_ptr, uint32_t size);

/* Ring buffer related methods (nvoib_ring.c) */
void ring_tx_comp(struct nvoib_dev *dev);
void ring_rx_comp(struct nvoib_dev *dev, uint32_t size, int badget);
int ring_rx_avail(struct session *ss, struct nvoib_dev *dev);
int ring_tx_avail(struct session *ss, struct nvoib_dev *dev, int badget);

/* TX process related methods (nvoib_tx.c) */
void *tx_wait(void *arg);
struct forward_entry *tx_fdb_lookup(struct forward_db *fdb, void *buffer);

/* RX process related methods (nvoib_rx.c) */
void *rx_wait(void *arg);
void rx_fdb_learn(struct session *ss, struct ibv_wc *wc, void *buffer);

