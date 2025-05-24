#include "rdma_common.h"

int main(int argc, char *argv[]) {
    struct resources res;
    int opt;
    while ((opt = getopt(argc, argv, "p:d:i:g:h")) != -1) {
        switch (opt) {
            case 'p': config.tcp_port = atoi(optarg); break;
            case 'd': config.dev_name = strdup(optarg); break;
            case 'i': config.ib_port = atoi(optarg); break;
            case 'g': config.gid_idx = atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }
    resources_init(&res);
    print_config();
    CHECK(resources_create(&res));
    CHECK(connect_qp(&res));
    CHECK(post_send(&res, IBV_WR_SEND));
    CHECK(poll_completion(&res));
    CHECK(resources_destroy(&res));
    return 0;
}