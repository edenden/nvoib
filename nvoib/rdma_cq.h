typedef void (*comp_f)(struct rdma_cm_id*, struct nvoib_dev *pci_dev, struct ibv_wc *);

void cq_pull(struct ibv_comp_channel *cc, void *opaque, comp_f func);
void rdma_request_recv(struct rdma_cm_id *id, uint64_t offset, uint32_t size, struct rxbuf_info *info);
void cq_server_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc, void *opaque);
void cq_client_work_completed(struct rdma_cm_id *id, struct ibv_wc *wc, void *opaque);
