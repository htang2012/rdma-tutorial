// rdma_client.c
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
    if (optind < argc) config.server_name = argv[optind];
    if (!config.server_name) {
        print_usage(argv[0]);
        return 1;
    }
    resources_init(&res);
    print_config();
    CHECK(resources_create(&res));
    CHECK(connect_qp(&res));
    CHECK(post_receive(&res));
    CHECK(poll_completion(&res));
    INFO("Received message: %s\n", res.buf);
    CHECK(resources_destroy(&res));
    return 0;
}