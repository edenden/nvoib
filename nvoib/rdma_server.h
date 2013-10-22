void server_start_listen(struct rdma_event_channel *ec, const char *recv_port);
void server_context_prepare(struct rdma_cm_id *id);
void server_set_mr(struct rdma_cm_id *id);
void server_init_recv(struct rdma_cm_id *id);
