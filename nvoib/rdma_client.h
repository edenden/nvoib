void *client_wait_txring(void *arg);
struct rdma_cm_id *client_start_connect(struct rdma_event_channel *ec,
					const char *dest_host, const char *dest_port);
void client_set_mr(struct rdma_cm_id *id);

struct write_remote_info {
	void *skb;
	uint64_t data_ptr;
	uint32_t size;
};
