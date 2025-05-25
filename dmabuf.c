#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include "hlthunk.h"
#include "libhlthunk.h"

int main(void)
{
    const char *busid = "0000:33:00.0"; // Example bus ID
    // Open Gaudi device
    int gaudi_fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, busid);  
    if (gaudi_fd < 0) {
        perror("Failed to open Gaudi device");
        return -1;
    }

    struct hl_info_args info;

    hlthunk_get_info(gaudi_fd, &info);

    // Allocate memory on Gaudi device
    // Example: allocate 4KB with 4KB page size
    uint64_t page_size = 4096;
    uint64_t device_addr = hlthunk_device_memory_alloc(gaudi_fd, 4096, page_size, 0, 0);
    if (!device_addr) {
        perror("Failed to allocate device memory");
        hlthunk_close(gaudi_fd);
        return -1;
    }


    // Export device memory as DMA-buf
    int dmabuf_fd = hlthunk_device_memory_export_dmabuf_fd(gaudi_fd, device_addr, 4096, 0);
    if (dmabuf_fd < 0) {
        perror("Failed to export DMA-buf");
        hlthunk_device_memory_free(gaudi_fd, device_addr);
        hlthunk_close(gaudi_fd);
        return -1;
    }

    printf("DMA-buf exported successfully (fd: %d)\n", dmabuf_fd);

    // Map DMA-buf to host memory
    void *mapped_memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (mapped_memory == MAP_FAILED) {
        perror("Failed to mmap DMA-buf");
        close(dmabuf_fd);
        hlthunk_device_memory_free(gaudi_fd, device_addr);
        hlthunk_close(gaudi_fd);
        return -1;
    }

    printf("DMA-buf mapped to host memory at %p\n", mapped_memory);

    // Cleanup
    munmap(mapped_memory, 4096);
    close(dmabuf_fd);
    hlthunk_device_memory_free(gaudi_fd, device_addr);
    hlthunk_close(gaudi_fd);

    return 0;
}



