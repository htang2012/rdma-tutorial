#include <hlthunk.h>
#include <infiniband/verbs.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Structure to hold Gaudi DMA-BUF and RDMA context
typedef struct {
    int gaudi_fd;
    uint64_t gaudi_device_va;
    uint32_t gaudi_handle;
    int dmabuf_fd;
    size_t buffer_size;
    void *host_mapped_ptr;
    
    // RDMA context
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_mr *dmabuf_mr;
    struct ibv_qp *qp;
    struct ibv_cq *cq;
} gaudi_dmabuf_ctx_t;

int open_gaudi_device(gaudi_dmabuf_ctx_t *ctx) {
    // Open Gaudi device
    ctx->gaudi_fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, NULL);
    if (ctx->gaudi_fd < 0) {
        fprintf(stderr, "Failed to open Gaudi device: %s\n", strerror(errno));
        return -1;
    }
    
    printf("Gaudi device opened successfully (fd: %d)\n", ctx->gaudi_fd);
    
    // Get device information
    struct hlthunk_hw_ip_info hw_ip;
    if (hlthunk_get_hw_ip_info(ctx->gaudi_fd, &hw_ip) != 0) {
        fprintf(stderr, "Failed to get Gaudi hardware info\n");
        return -1;
    }
    
    printf("Gaudi device: %d, DRAM size: %lu MB\n", 
           hw_ip.device_id, hw_ip.dram_size / (1024 * 1024));
    
    return 0;
}

int allocate_gaudi_memory_with_dmabuf(gaudi_dmabuf_ctx_t *ctx, size_t size) {
    struct hlthunk_memory_alloc_args alloc_args;
    struct hlthunk_export_dmabuf_args export_args;
    int ret;
    
    ctx->buffer_size = size;
    
    // Allocate memory on Gaudi device
    memset(&alloc_args, 0, sizeof(alloc_args));
    alloc_args.mem_size = size;
    alloc_args.flags = HLTHUNK_MEM_OP_FLAGS_DRAM_ALLOC;
    
    ret = hlthunk_memory_alloc(ctx->gaudi_fd, &alloc_args);
    if (ret != 0) {
        fprintf(stderr, "Failed to allocate Gaudi memory: %s\n", strerror(errno));
        return -1;
    }
    
    ctx->gaudi_device_va = alloc_args.device_virt_addr;
    ctx->gaudi_handle = alloc_args.mem_handle;
    
    printf("Gaudi memory allocated: VA=0x%lx, handle=0x%x, size=%zu\n",
           ctx->gaudi_device_va, ctx->gaudi_handle, size);
    
    // Export Gaudi memory as DMA-BUF
    memset(&export_args, 0, sizeof(export_args));
    export_args.mem_handle = ctx->gaudi_handle;
    export_args.flags = 0;
    
    ret = hlthunk_export_dmabuf(ctx->gaudi_fd, &export_args);
    if (ret != 0) {
        fprintf(stderr, "Failed to export Gaudi memory as DMA-BUF: %s\n", 
                strerror(errno));
        return -1;
    }
    
    ctx->dmabuf_fd = export_args.dmabuf_fd;
    
    printf("Gaudi memory exported as DMA-BUF (fd: %d)\n", ctx->dmabuf_fd);
    
    return 0;
}

int map_dmabuf_to_host(gaudi_dmabuf_ctx_t *ctx) {
    // Map DMA-BUF for host access
    ctx->host_mapped_ptr = mmap(NULL, ctx->buffer_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->dmabuf_fd, 0);
    
    if (ctx->host_mapped_ptr == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap DMA-BUF to host: %s\n", strerror(errno));
        ctx->host_mapped_ptr = NULL;
        return -1;
    }
    
    printf("DMA-BUF mapped to host address: %p\n", ctx->host_mapped_ptr);
    return 0;
}

int setup_rdma_with_gaudi_dmabuf(gaudi_dmabuf_ctx_t *ctx) {
    struct ibv_device **dev_list;
    struct ibv_device_attr device_attr;
    
    // Get InfiniBand devices
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB device list\n");
        return -1;
    }
    
    // Open first available device
    ctx->ib_ctx = ibv_open_device(dev_list[0]);
    if (!ctx->ib_ctx) {
        fprintf(stderr, "Failed to open IB device\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    // Query device capabilities
    if (ibv_query_device(ctx->ib_ctx, &device_attr) != 0) {
        fprintf(stderr, "Failed to query IB device\n");
        return -1;
    }
    
    printf("RDMA device: %s, max MR size: %llu\n",
           ibv_get_device_name(dev_list[0]), device_attr.max_mr_size);
    
    ibv_free_device_list(dev_list);
    
    // Allocate Protection Domain
    ctx->pd = ibv_alloc_pd(ctx->ib_ctx);
    if (!ctx->pd) {
        fprintf(stderr, "Failed to allocate protection domain\n");
        return -1;
    }
    
    // Register DMA-BUF with RDMA
    // Note: Direct DMA-BUF registration may require specific driver support
    // For now, we'll register the mapped host pointer
    if (ctx->host_mapped_ptr) {
        ctx->dmabuf_mr = ibv_reg_mr(ctx->pd, ctx->host_mapped_ptr, ctx->buffer_size,
                                   IBV_ACCESS_LOCAL_WRITE | 
                                   IBV_ACCESS_REMOTE_WRITE |
                                   IBV_ACCESS_REMOTE_READ);
        
        if (!ctx->dmabuf_mr) {
            fprintf(stderr, "Failed to register DMA-BUF memory: %s\n", 
                    strerror(errno));
            return -1;
        }
        
        printf("DMA-BUF registered with RDMA: lkey=0x%x, rkey=0x%x\n",
               ctx->dmabuf_mr->lkey, ctx->dmabuf_mr->rkey);
    }
    
    // Create Completion Queue
    ctx->cq = ibv_create_cq(ctx->ib_ctx, 16, NULL, NULL, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Failed to create completion queue\n");
        return -1;
    }
    
    // Create Queue Pair
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = ctx->cq;
    qp_init_attr.recv_cq = ctx->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    
    ctx->qp = ibv_create_qp(ctx->pd, &qp_init_attr);
    if (!ctx->qp) {
        fprintf(stderr, "Failed to create queue pair\n");
        return -1;
    }
    
    printf("RDMA Queue Pair created (QPN: %d)\n", ctx->qp->qp_num);
    return 0;
}
#if 0
int gaudi_compute_and_rdma_example(gaudi_dmabuf_ctx_t *ctx) {
    // Example: Submit work to Gaudi and then use RDMA
    struct hlthunk_cs_in cs;
    uint32_t *host_buffer;
    int i;
    
    // Initialize some test data in host mapped buffer
    if (ctx->host_mapped_ptr) {
        host_buffer = (uint32_t*)ctx->host_mapped_ptr;
        for (i = 0; i < ctx->buffer_size / sizeof(uint32_t); i++) {
            host_buffer[i] = i;
        }
        printf("Initialized test data in DMA-BUF\n");
    }
    
    // Submit command to Gaudi (simplified example)
    memset(&cs, 0, sizeof(cs));
    cs.queue_index = 0;
    cs.num_chunks_restore = 0;
    cs.num_chunks_execute = 0; // Would contain actual command buffers
    
    // For a real application, you would:
    // 1. Create command buffers with TPC/MME operations
    // 2. Set up proper synchronization
    // 3. Submit the command stream

    int rc = hlthunk_command_submission(ctx->gaudi_fd, &cs);
    if (rc != 0) {
        fprintf(stderr, "Failed to submit command to Gaudi: %s\n", strerror(errno));
        return -1;
    }
    
    printf("Gaudi computation phase complete\n");
    
    // Now the DMA-BUF contains processed data and can be used with RDMA
    return 0;
}

#endif

int perform_rdma_with_gaudi_buffer(gaudi_dmabuf_ctx_t *ctx) {
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int ne;
    
    if (!ctx->dmabuf_mr) {
        fprintf(stderr, "No memory region registered\n");
        return -1;
    }
    
    // Prepare scatter-gather entry
    sge.addr = (uintptr_t)ctx->host_mapped_ptr;
    sge.length = ctx->buffer_size;
    sge.lkey = ctx->dmabuf_mr->lkey;
    
    // Prepare RDMA SEND work request
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    
    // Post send request
    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post RDMA send: %s\n", strerror(errno));
        return -1;
    }
    
    printf("RDMA operation posted using Gaudi DMA-BUF\n");
    
    // Poll for completion
    do {
        ne = ibv_poll_cq(ctx->cq, 1, &wc);
    } while (ne == 0);
    
    if (ne < 0) {
        fprintf(stderr, "Failed to poll CQ: %s\n", strerror(errno));
        return -1;
    }
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "RDMA operation failed: %s\n", 
                ibv_wc_status_str(wc.status));
        return -1;
    }
    
    printf("RDMA operation completed successfully\n");
    return 0;
}

void cleanup_gaudi_dmabuf_ctx(gaudi_dmabuf_ctx_t *ctx) {
    struct hlthunk_memory_free_args free_args;
    
    // Cleanup RDMA resources
    if (ctx->dmabuf_mr) {
        ibv_dereg_mr(ctx->dmabuf_mr);
    }
    if (ctx->qp) {
        ibv_destroy_qp(ctx->qp);
    }
    if (ctx->cq) {
        ibv_destroy_cq(ctx->cq);
    }
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
    }
    if (ctx->ib_ctx) {
        ibv_close_device(ctx->ib_ctx);
    }
    
    // Cleanup host mapping
    if (ctx->host_mapped_ptr && ctx->host_mapped_ptr != MAP_FAILED) {
        munmap(ctx->host_mapped_ptr, ctx->buffer_size);
    }
    
    // Close DMA-BUF file descriptor
    if (ctx->dmabuf_fd > 0) {
        close(ctx->dmabuf_fd);
    }
    
    // Free Gaudi memory
    if (ctx->gaudi_handle) {
        memset(&free_args, 0, sizeof(free_args));
        free_args.mem_handle = ctx->gaudi_handle;
        hlthunk_memory_free(ctx->gaudi_fd, &free_args);
    }
    
    // Close Gaudi device
    if (ctx->gaudi_fd >= 0) {
        hlthunk_close(ctx->gaudi_fd);
    }
}

int main() {
    gaudi_dmabuf_ctx_t ctx = {0};
    const size_t buffer_size = 4 * 1024 * 1024; // 4MB buffer
    
    printf("Setting up Intel Gaudi DMA-BUF connection...\n");
    
    // Open Gaudi device
    if (open_gaudi_device(&ctx) != 0) {
        fprintf(stderr, "Failed to open Gaudi device\n");
        return -1;
    }
    
    // Allocate Gaudi memory and export as DMA-BUF
    if (allocate_gaudi_memory_with_dmabuf(&ctx, buffer_size) != 0) {
        fprintf(stderr, "Failed to allocate Gaudi memory with DMA-BUF\n");
        cleanup_gaudi_dmabuf_ctx(&ctx);
        return -1;
    }
    
    // Map DMA-BUF to host for CPU access
    if (map_dmabuf_to_host(&ctx) != 0) {
        fprintf(stderr, "Failed to map DMA-BUF to host\n");
        cleanup_gaudi_dmabuf_ctx(&ctx);
        return -1;
    }
    
    // Setup RDMA with DMA-BUF
    if (setup_rdma_with_gaudi_dmabuf(&ctx) != 0) {
        fprintf(stderr, "Failed to setup RDMA with Gaudi DMA-BUF\n");
        cleanup_gaudi_dmabuf_ctx(&ctx);
        return -1;
    }

    #if 0
    
    // Example computation and RDMA usage
    if (gaudi_compute_and_rdma_example(&ctx) != 0) {
        fprintf(stderr, "Failed Gaudi computation example\n");
        cleanup_gaudi_dmabuf_ctx(&ctx);
        return -1;
    }
    #endif

    
    printf("Intel Gaudi DMA-BUF setup completed successfully\n");
    printf("Buffer can be used for Gaudi compute -> RDMA transfers\n");
    
    cleanup_gaudi_dmabuf_ctx(&ctx);
    return 0;
}