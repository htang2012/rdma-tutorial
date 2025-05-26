#include "rdma_common.h"


//#define ALL_FALLBACK // Fallback to posix_memalign


struct config_t config = {
    .dev_name = NULL,
    .server_name = NULL,
    .tcp_port = 20000,
    .ib_port = 1,
    .gid_idx = -1
};

int sock_connect(const char *server_name, int port) {
    struct addrinfo hints = {.ai_flags = AI_PASSIVE, .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *resolved_addr = NULL, *iterator;
    char service[6];
    int sockfd = -1, listenfd = 0;

    sprintf(service, "%d", port);
    CHECK(getaddrinfo(server_name, service, &hints, &resolved_addr));

    for (iterator = resolved_addr; iterator != NULL; iterator = iterator->ai_next) {
        sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
        if (sockfd < 0) ERR_DIE("socket failed: %s\n", strerror(errno));

        if (server_name == NULL) {
            listenfd = sockfd;
            CHECK(bind(listenfd, iterator->ai_addr, iterator->ai_addrlen));
            CHECK(listen(listenfd, 1));
            sockfd = accept(listenfd, NULL, 0);
        } else {
            CHECK(connect(sockfd, iterator->ai_addr, iterator->ai_addrlen));
        }
        break;
    }

    freeaddrinfo(resolved_addr);
    return sockfd;
}

int sock_sync_data(int sockfd, int xfer_size, char *local_data, char *remote_data) {
    int write_bytes = write(sockfd, local_data, xfer_size);
    if (write_bytes != xfer_size) ERR_DIE("write failed: %s\n", strerror(errno));
    int read_bytes = read(sockfd, remote_data, xfer_size);
    if (read_bytes != xfer_size) ERR_DIE("read failed: %s\n", strerror(errno));
    return 0;
}

void print_config(void) {
    INFO("Device name:          %s\n", config.dev_name);
    INFO("IB port:              %d\n", config.ib_port);
    if (config.server_name)
        INFO("IP:                   %s\n", config.server_name);
    INFO("TCP port:             %u\n", config.tcp_port);
    if (config.gid_idx >= 0)
        INFO("GID index:            %u\n", config.gid_idx);
}

void print_usage(const char *progname) {
    printf("Usage:\n");
    printf("%s          start a server and wait for connection\n", progname);
    printf("%s <host>   connect to server at <host>\n\n", progname);
    printf("Options:\n");
    printf("-p, --port <port>           listen on / connect to port <port> (default 20000)\n");
    printf("-d, --ib-dev <dev>          use IB device <dev> (default first device found)\n");
    printf("-i, --ib-port <port>        use port <port> of IB device (default 1)\n");
    printf("-g, --gid_idx <gid index>   gid index to be used in GRH (default not used)\n");
    printf("-h, --help                  this message\n");
}

void resources_init(struct resources *res) {
    memset(res, 0, sizeof(*res));
    res->sock = -1;
    res->dma_fd = -1; // Initialize DMA-BUF file descriptor
}

int resources_create(struct resources *res) {
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    int i, mr_flags = 0, cq_size = 1, num_devices;

    res->sock = sock_connect(config.server_name, config.tcp_port);
    if (res->sock < 0)
        ERR_DIE("Failed to establish TCP connection\n");

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0)
        ERR_DIE("No IB devices found\n");

    for (i = 0; i < num_devices; i++) {
        if (!config.dev_name) {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
        }
        if (strcmp(ibv_get_device_name(dev_list[i]), config.dev_name) == 0) {
            ib_dev = dev_list[i];
            break;
        }
    }

    if (!ib_dev)
        ERR_DIE("IB device %s wasn't found\n", config.dev_name);

    res->ib_ctx = ibv_open_device(ib_dev);
    if (!res->ib_ctx)
        ERR_DIE("Failed to open device %s\n", config.dev_name);

    CHECK(ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr));

    res->pd = ibv_alloc_pd(res->ib_ctx);
    if (!res->pd)
        ERR_DIE("Failed to allocate PD: %s\n", strerror(errno));

    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
    if (!res->cq)
        ERR_DIE("Failed to create CQ: %s\n", strerror(errno));

    // Try DMA-BUF allocation
    long page_size = sysconf(_SC_PAGESIZE);
    int heap_fd = open(DMA_HEAP_PATH, O_RDWR | O_CLOEXEC);
    res->buf = NULL;
    res->dma_fd = -1;
    if (heap_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", DMA_HEAP_PATH, strerror(errno));
    } else {
        struct dma_heap_allocation_data alloc_data = {
            .len = BUFFER_SIZE,
            .fd_flags = O_RDWR | O_CLOEXEC,
            .heap_flags = 0,
        };
        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
            fprintf(stderr, "DMA heap allocation failed: %s\n", strerror(errno));
            close(heap_fd);
        } else {
            INFO("Allocated DMA-BUF: fd=%d, size=%llu\n", alloc_data.fd, (unsigned long long)alloc_data.len);
            res->dma_fd = alloc_data.fd;
            res->buf = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, res->dma_fd, 0);
            if (res->buf == MAP_FAILED) {
                fprintf(stderr, "mmap failed: %s (errno=%d)\n", strerror(errno), errno);
                close(res->dma_fd);
                res->buf = NULL;
            } else {
                INFO("Mapped DMA-BUF: addr=%p\n", res->buf);
            }
        }
    }
    
    INFO("Register memory region (DMA-BUF)-------->\n ");
    // Register memory region (DMA-BUF)
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    if (res->buf && res->dma_fd >= 0) {
        res->mr = ibv_reg_dmabuf_mr(res->pd, 0, BUFFER_SIZE, (uint64_t)res->buf, res->dma_fd, mr_flags);
        if (!res->mr) {
            fprintf(stderr, "ibv_reg_dmabuf_mr failed: %s (errno=%d)\n", strerror(errno), errno);
        } else {
            INFO("DMA-BUF memory region registered: lkey=%u, rkey=%u\n", res->mr->lkey, res->mr->rkey);
        }
    }

#ifdef ALL_FALLBACK 

    INFO("Fallback to posix_memalign if DMA-BUF fails")
    // Fallback to posix_memalign if DMA-BUF fails
    if (!res->mr) {
        fprintf(stderr, "Falling back to posix_memalign allocation\n");
        if (res->buf) {
            munmap(res->buf, BUFFER_SIZE);
            close(res->dma_fd);
            close(heap_fd);
            res->buf = NULL;
            res->dma_fd = -1;
        }
        if (posix_memalign((void **)&res->buf, page_size, BUFFER_SIZE) != 0)
            ERR_DIE("posix_memalign failed: %s\n", strerror(errno));
        INFO("Allocated posix_memalign: addr=%p\n", res->buf);
        res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE, mr_flags);
        if (!res->mr)
            ERR_DIE("ibv_reg_mr (posix_memalign) failed: %s (errno=%d)\n", strerror(errno), errno);
        INFO("posix_memalign memory region registered: lkey=%u, rkey=%u\n", res->mr->lkey, res->mr->rkey);
    }

#endif

    if (!config.server_name)
        strcpy(res->buf, MSG);

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp)
        ERR_DIE("Failed to create QP: %s\n", strerror(errno));

    ibv_free_device_list(dev_list);
    return 0;
}

int resources_destroy(struct resources *res) {
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->buf) {
        if (res->dma_fd >= 0) {
            munmap(res->buf, BUFFER_SIZE);
            close(res->dma_fd);
        } else {
            free(res->buf);
        }
    }
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ib_ctx) ibv_close_device(res->ib_ctx);
    if (res->sock >= 0) close(res->sock);
    return 0;
}


static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = config.ib_port,
        .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    };
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    int rc = ibv_modify_qp(qp, &attr, flags);
    if (rc) { INFO("modify_qp_to_init failed: %s\n", strerror(rc)); }
    else { INFO("modify_qp_to_init succeeded\n"); }
    return rc;
}
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = remote_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 0x12,
        .ah_attr = {
            .is_global = 0,
            .dlid = dlid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = config.ib_port
        }
    };
    if (config.gid_idx >= 0) {
        attr.ah_attr.is_global = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.hop_limit = 1;
    }
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    int rc = ibv_modify_qp(qp, &attr, flags);
    if (rc) { INFO("modify_qp_to_rtr failed: %s\n", strerror(rc)); }
    else { INFO("modify_qp_to_rtr succeeded: remote_qpn=%u, dlid=%u\n", remote_qpn, dlid); }
    return rc;
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 0x12,
        .retry_cnt = 6,
        .rnr_retry = 0,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };
    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    int rc = ibv_modify_qp(qp, &attr, flags);
    if (rc) { INFO("modify_qp_to_rts failed: %s\n", strerror(rc)); }
    else { INFO("modify_qp_to_rts succeeded\n"); }
    return rc;
}


/*
static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .port_num = config.ib_port,
        .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    };
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(qp, &attr, flags);
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = remote_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 0x12,
        .ah_attr = {
            .is_global = 0,
            .dlid = dlid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = config.ib_port
        }
    };

    if (config.gid_idx >= 0) {
        attr.ah_attr.is_global = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.hop_limit = 1;
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    return ibv_modify_qp(qp, &attr, flags);
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 0x12,
        .retry_cnt = 6,
        .rnr_retry = 0,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    return ibv_modify_qp(qp, &attr, flags);
} */



int connect_qp(struct resources *res) {
    struct cm_con_data_t local_con_data = {0}, remote_con_data = {0}, tmp_con_data = {0};
    union ibv_gid my_gid = {0};
    char temp_char;

    if (config.gid_idx >= 0)
        CHECK(ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid));

    local_con_data.addr = htonll((uintptr_t)res->buf);
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp->qp_num);
    local_con_data.lid = htons(res->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);

    sock_sync_data(res->sock, sizeof(local_con_data), (char *)&local_con_data, (char *)&tmp_con_data);

    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
    res->remote_props = remote_con_data;

    modify_qp_to_init(res->qp);
    if (config.server_name)
        post_receive(res);
    modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
    modify_qp_to_rts(res->qp);
    sock_sync_data(res->sock, 1, "Q", &temp_char);

    return 0;
}

int post_send(struct resources *res, int opcode) {
    struct ibv_send_wr sr = {0}, *bad_wr = NULL;
    struct ibv_sge sge = {
        .addr = (uintptr_t)res->buf,
        .length = MSG_SIZE,
        .lkey = res->mr->lkey
    };

    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = opcode;
    sr.send_flags = IBV_SEND_SIGNALED;

    if (opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = res->remote_props.addr;
        sr.wr.rdma.rkey = res->remote_props.rkey;
    }

    return ibv_post_send(res->qp, &sr, &bad_wr);
}

int post_receive(struct resources *res) {
    struct ibv_recv_wr rr = {0}, *bad_wr;
    struct ibv_sge sge = {
        .addr = (uintptr_t)res->buf,
        .length = MSG_SIZE,
        .lkey = res->mr->lkey
    };

    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;

    return ibv_post_recv(res->qp, &rr, &bad_wr);
}

int poll_completion(struct resources *res) {
    struct ibv_wc wc;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    do {
        int poll_result = ibv_poll_cq(res->cq, 1, &wc);
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (poll_result > 0) {
            if (wc.status == IBV_WC_SUCCESS)
                return 0;
            else
                ERR_DIE("Work completion error: status 0x%x\n", wc.status);
        } else if (poll_result < 0) {
            ERR_DIE("Poll CQ failed\n");
        } else if (elapsed > MAX_POLL_CQ_TIMEOUT) {
            ERR_DIE("Poll CQ timeout\n");
        }
    } while (1);
}