void server_start_listen(struct rdma_event_channel *ec, char *recv_port);
void server_set_mr(struct rdma_cm_id *id);
void server_conn_estab(struct rdma_cm_id *id);
