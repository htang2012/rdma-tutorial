// rdma_common.h
#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <assert.h>
#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>

#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "This is alice, how are you?"
#define RDMAMSGR "RDMA read operation"
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE (strlen(MSG) + 1)
#define DMA_HEAP_PATH "/dev/dma_heap/system"
#define BUFFER_SIZE ((MSG_SIZE + 4095) & ~4095) // Round up MSG_SIZE to page size (4KB)

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is not defined
#endif

#define ERROR(fmt, args...)                                                    \
    { fprintf(stderr, "ERROR: %s(): " fmt, __func__, ##args); }
#define ERR_DIE(fmt, args...)                                                  \
    {                                                                          \
        ERROR(fmt, ##args);                                                    \
        exit(EXIT_FAILURE);                                                \
    }
#define INFO(fmt, args...)                                                     \
    { printf("INFO: %s(): " fmt, __func__, ##args); }
#define WARN(fmt, args...)                                                     \
    { printf("WARN: %s(): " fmt, __func__, ##args); }

#define CHECK(expr)                                                            \
    {                                                                          \
        int rc = (expr);                                                       \
        if (rc != 0) {                                                         \
            perror(strerror(errno));                                           \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    }

struct config_t {
    const char *dev_name;
    char *server_name;
    uint32_t tcp_port;
    int ib_port;
    int gid_idx;
};

struct cm_con_data_t {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
} __attribute__((packed));

struct resources {
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;
    struct cm_con_data_t remote_props;
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    int sock;
    int dma_fd; // Added for DMA-BUF
};

extern struct config_t config;

// Function declarations
int sock_connect(const char *server_name, int port);
int sock_sync_data(int sockfd, int xfer_size, char *local_data, char *remote_data);
void print_config(void);
void print_usage(const char *progname);
void resources_init(struct resources *res);
int resources_create(struct resources *res);
int resources_destroy(struct resources *res);
int connect_qp(struct resources *res);
int post_send(struct resources *res, int opcode);
int post_receive(struct resources *res);
int poll_completion(struct resources *res);

#endif