/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _SPRD_ION_H
#define _SPRD_ION_H

#include <uapi/linux/sprd_ion.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>

struct ion_client *sprd_ion_client_create(const char *name);

int sprd_ion_is_reserved(int fd, struct dma_buf *dmabuf,
					bool *reserved);

int sprd_ion_get_sg_table(int fd, struct dma_buf *dmabuf,
				struct sg_table **table, size_t *size);

int sprd_ion_get_buffer(int fd, struct dma_buf *dmabuf,
			  void **buf, size_t *size);

int sprd_ion_get_sg(void *buf, struct sg_table **table);

void sprd_ion_set_dma(void *buf, int id);

void sprd_ion_put_dma(void *buf, int id);

void sprd_ion_unmap_dma(void *buffer);

int sprd_ion_get_phys_addr(int fd, struct dma_buf *dmabuf,
				unsigned long *phys_addr, size_t *size);

int sprd_ion_check_phys_addr(struct dma_buf *dmabuf);

int sprd_ion_get_phys_addr_bydmabuf(int fd_buffer, struct dma_buf *dmabuf,
				unsigned long *phys_addr,size_t *size);

struct ion_client *sprd_ion_client_get(int fd);

void sprd_ion_client_put(struct ion_client *client);
#endif /* _SPRD_ION_H */
