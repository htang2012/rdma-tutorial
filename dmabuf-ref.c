// SPDX-License-Identifier: MIT

/*
 * Copyright 2021 HabanaLabs, Ltd.
 * All Rights Reserved.
 */

#define _GNU_SOURCE

#define HLTESTS_LIB_MODE 1
#include "hlthunk_tests.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef min
#define min(a, b)	(((a) < (b)) ? (a) : (b))
#endif



static uint64_t hl_reg_dmabuf(int fd, uint64_t length, uint32_t dmabuf_fd)
{
	union hl_mem_args ioctl_args;
	int rc;

	memset(&ioctl_args, 0, sizeof(ioctl_args));
	ioctl_args.in.reg_dmabuf_fd.fd = dmabuf_fd;
	ioctl_args.in.reg_dmabuf_fd.length = length;
	ioctl_args.in.op = HL_MEM_OP_REG_DMABUF_FD;

	rc = ioctl(fd, DRM_IOCTL_HL_MEMORY, &ioctl_args);
	if (rc)
		return 0;

	return ioctl_args.out.device_virt_addr;
}

static int hl_unmap_dmabuf(int fd, uint64_t device_virt_addr)
{
	union hl_mem_args ioctl_args;

	memset(&ioctl_args, 0, sizeof(ioctl_args));
	ioctl_args.in.unmap.device_virt_addr = device_virt_addr;
	ioctl_args.in.op = HL_MEM_OP_UNMAP;

	return ioctl(fd, DRM_IOCTL_HL_MEMORY, &ioctl_args);
}

int main(int argc, const char **argv)
{
	uint64_t host_src_device_va, host_dst_device_va, device_alloc_size, import_device_va,
		host_alloc_size;
	struct hltests_state *tests_state_export, *tests_state_import;
	void *device_addr, *host_src, *host_dst;
	int rc, fd_export, fd_import, dmabuf_fd;

	hltests_parser(argc, argv, NULL, HLTEST_DEVICE_MASK_DONT_CARE);

	hltests_set_capabilities_mask(CAP_ARC_FW_LOAD_SCHED_MASK | CAP_ARC_FW_LOAD_PDMA_MASK);

	rc = hltests_init();
	if (rc) {
		printf("Failed to initialize hlthunk tests library %d\n", rc);
		return rc;
	}

	hltests_override_parser_pciaddr(argv[0]);
	rc = hltests_setup((void **) &tests_state_export);
	if (rc) {
		printf("Failed to run setup phase of hlthunk tests for device %s\n", argv[0]);
		goto fini_tests;
	}
	fd_export = tests_state_export->fd;

	if (strncmp(argv[0], argv[1], min(strlen(argv[0]), strlen(argv[1])))) {
		hltests_override_parser_pciaddr(argv[1]);
		rc = hltests_setup((void **) &tests_state_import);
		if (rc) {
			printf("Failed to run setup phase of hlthunk tests for device %s\n",
				argv[1]);
			goto teardown_export;
		}
		fd_import = tests_state_import->fd;
	} else {
		fd_import = fd_export;
	}

	printf("%s - exporter\n", argv[0]);
	printf("%s - importer\n", argv[1]);

	/* Allocate host/device memories */
	host_alloc_size = SZ_4G + SZ_1G + SZ_1K;
	device_alloc_size = SZ_8G;

	if (host_alloc_size > device_alloc_size) {
		printf("device allocation must be >= host allocation\n");
		rc = -1;
		goto teardown_import;
	}

	host_src = hltests_allocate_host_mem(fd_export, host_alloc_size, NOT_HUGE_MAP);
	if (!host_src) {
		rc = -1;
		goto teardown_import;
	}
	host_src_device_va = hltests_get_device_va_for_host_ptr(fd_export, host_src);

	host_dst = hltests_allocate_host_mem(fd_import, host_alloc_size, NOT_HUGE_MAP);
	if (!host_dst) {
		rc = -1;
		goto free_host_src;
	}
	host_dst_device_va = hltests_get_device_va_for_host_ptr(fd_import, host_dst);

	device_addr = hltests_allocate_device_mem(fd_export, device_alloc_size, 0, NOT_CONTIGUOUS);
	if (!device_addr) {
		rc = -1;
		goto free_dst_src;
	}

	/* Export DMA-BUF and register MR */

	dmabuf_fd = hltests_device_memory_export_dmabuf_fd(fd_export, device_addr,
								device_alloc_size, 0);
	if (dmabuf_fd < 0) {
		printf("Failed to export dmabuf %d\n", dmabuf_fd);
		rc = dmabuf_fd;
		goto free_device_mem;
	}

	printf("Exported dmabuf from %s for HBM address 0x%lx, size %lu\n",
		argv[0], (uint64_t) (uintptr_t) device_addr, device_alloc_size);

#ifdef MMAP_DEBUG
	{
		void *dmabuf_ptr = mmap(NULL, device_alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED,
					dmabuf_fd, 0);
		if (dmabuf_ptr == MAP_FAILED) {
			printf("Failed to mmap dmabuf\n");
			rc = -1;
			goto close_dmabuf_fd;
		} else {
			munmap(dmabuf_ptr, device_alloc_size);
		}
	}
#endif

	import_device_va = hl_reg_dmabuf(fd_import, device_alloc_size, dmabuf_fd);
	if (!import_device_va) {
		printf("Failed to register dmabuf\n");
		rc = -1;
		goto close_dmabuf_fd;
	}

	printf("Registered dmabuf on %s with device VA for dmabuf = 0x%lx\n",
		argv[1], import_device_va);

	if (host_alloc_size > UINT32_MAX) {
		hltests_fill_rand_values(host_src, UINT32_MAX);
		hltests_fill_rand_values((char *) host_src + UINT32_MAX,
					host_alloc_size - UINT32_MAX);
	} else {
		hltests_fill_rand_values(host_src, host_alloc_size);
	}

	memset(host_dst, 0, host_alloc_size);

	if (host_alloc_size > UINT32_MAX) {
		/* DMA: host->device */
		rc = hltests_dma_transfer(fd_export, hltests_get_dma_down_qid(fd_export, STREAM0),
						EB_FALSE, MB_TRUE, host_src_device_va,
						(uint64_t) (uintptr_t) device_addr,
						UINT32_MAX, 0);
		if (rc) {
			printf("Failed to do dma host->device\n");
			rc = -1;
			goto unmap_dmabuf;
		}

		rc = hltests_dma_transfer(fd_export, hltests_get_dma_down_qid(fd_export, STREAM0),
						EB_FALSE, MB_TRUE, host_src_device_va + UINT32_MAX,
						(uint64_t) (uintptr_t) device_addr + UINT32_MAX,
						host_alloc_size - UINT32_MAX, 0);
		if (rc) {
			printf("Failed to do dma host->device\n");
			rc = -1;
			goto unmap_dmabuf;
		}
	} else {
		/* DMA: host->device */
		rc = hltests_dma_transfer(fd_export, hltests_get_dma_down_qid(fd_export, STREAM0),
						EB_FALSE, MB_TRUE, host_src_device_va,
						(uint64_t) (uintptr_t) device_addr,
						host_alloc_size, 0);
		if (rc) {
			printf("Failed to do dma host->device\n");
			rc = -1;
			goto unmap_dmabuf;
		}
	}

	printf("Copied data from host to device %s HBM memory at address 0x%lx, size %lu\n",
		argv[0], (uint64_t) (uintptr_t) device_addr, host_alloc_size);

	if (host_alloc_size > UINT32_MAX) {
		/* DMA: device->host */
		rc = hltests_dma_transfer(fd_import, hltests_get_dma_up_qid(fd_import, STREAM0),
						EB_FALSE, MB_TRUE, import_device_va,
						host_dst_device_va,
						UINT32_MAX, 0);
		if (rc) {
			printf("Failed to do dma device->host\n");
			rc = -1;
			goto unmap_dmabuf;
		}

		rc = hltests_dma_transfer(fd_import, hltests_get_dma_up_qid(fd_import, STREAM0),
						EB_FALSE, MB_TRUE, import_device_va + UINT32_MAX,
						host_dst_device_va + UINT32_MAX,
						host_alloc_size - UINT32_MAX, 0);
		if (rc) {
			printf("Failed to do dma device->host\n");
			rc = -1;
			goto unmap_dmabuf;
		}
	} else {
		/* DMA: device->host */
		rc = hltests_dma_transfer(fd_import, hltests_get_dma_up_qid(fd_import, STREAM0),
						EB_FALSE, MB_TRUE, import_device_va,
						host_dst_device_va,
						host_alloc_size, 0);
		if (rc) {
			printf("Failed to do dma device->host\n");
			rc = -1;
			goto unmap_dmabuf;
		}
	}

	printf("Copied data to host by doing P2P read from device %s to device %s\n",
		argv[0], argv[1]);

	/* Compare host memories */
	rc = hltests_mem_compare(host_src, host_dst, host_alloc_size);
	if (rc) {
		printf("Failed memory comparision\n");
		rc = -1;
		goto unmap_dmabuf;
	}

	printf("Memory Compare was successful :)\n");

	/* Cleanup */
unmap_dmabuf:
	rc = hl_unmap_dmabuf(fd_import, import_device_va);
	if (rc)
		printf("Failed to unmap dmabuf %d\n", rc);
close_dmabuf_fd:
	close(dmabuf_fd);
free_device_mem:
	hltests_free_device_mem(fd_export, device_addr);
free_dst_src:
	hltests_free_host_mem(fd_import, host_dst);
free_host_src:
	hltests_free_host_mem(fd_export, host_src);
teardown_import:
	if (fd_import != fd_export)
		hltests_teardown((void **) &tests_state_import);
teardown_export:
	hltests_teardown((void **) &tests_state_export);
fini_tests:
	hltests_fini();

	return rc;
}
