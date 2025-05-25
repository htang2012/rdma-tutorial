#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>
#include "hlthunk.h"



int main()
{
    int device_fd, dmabuf_fd;
    uint64_t device_addr;  // Example device address
    size_t buffer_size = 4096;         // 4KB buffer
    void* mapped_memory;
    int ctrl_fd, fd;
    enum hlthunk_device_name actual_asic_type;


    
    const char *busid = "0000:33:00.0"; // Example bus ID

    /* Open control device first in order to compare against asic_mask_for_testing */
	ctrl_fd = hlthunk_open_control_by_name(HLTHUNK_DEVICE_DONT_CARE, busid);
	if (ctrl_fd < 0) return -1;
    hlthunk_close(ctrl_fd);

    // Open control device to get the actual ASIC type
    fd = hlthunk_open(HLTHUNK_DEVICE_DONT_CARE, busid);  
    if (fd < 0) {
        perror("Failed to open device");
        close(ctrl_fd);
        return 1;
    }


    device_addr = (uint64_t) hlthunk_malloc(buffer_size);
    if (!device_addr) {
        perror("Failed to allocate device memory");
        close(fd);
        return 1;
    }

    
    // Export device memory as DMA-buf
    dmabuf_fd = hlthunk_device_memory_export_dmabuf_fd(device_fd, device_addr, 
                                                       buffer_size, 0);

    if (dmabuf_fd < 0) {
        // Handle error in exporting DMA-buf
        switch (dmabuf_fd) {
            case -EINVAL:
                fprintf(stderr, "Invalid parameters\n");
                break;
            case -ENOMEM:
                fprintf(stderr, "Out of memory\n");
                break;
            case -ENODEV:
                fprintf(stderr, "Device not available\n");
                break;
            default:
                fprintf(stderr, "Unknown error: %d\n", dmabuf_fd);
        }
        close(device_fd);
        return -1;
    }
    
    // Map DMA-buf into userspace
    mapped_memory = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, 
                        MAP_SHARED, dmabuf_fd, 0);
    if (mapped_memory == MAP_FAILED) {
        perror("Failed to map DMA-buf");
        close(dmabuf_fd);
        close(device_fd);
        return 1;
    }
    
    // Use the mapped memory
    printf("Successfully mapped DMA-buf at %p\n", mapped_memory);
    
    // Example: Write some data
    *((uint32_t*)mapped_memory) = 0xDEADBEEF;
    
    // Cleanup
    munmap(mapped_memory, buffer_size);
    close(dmabuf_fd);
    close(device_fd);
    
    return 0;
}