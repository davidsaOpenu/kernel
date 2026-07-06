// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/exofs/file.c
 */

#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/filelock.h>
#include <linux/quotaops.h>
#include <linux/iomap.h>
#include <linux/uio.h>
#include <linux/buffer_head.h>
#include "exofs.h"


const struct file_operations exofs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.open		= generic_file_open,
	.fsync		= generic_file_fsync,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.setlease	= generic_setlease,
};

const struct inode_operations exofs_file_inode_operations = {
	.getattr	= exofs_getattr,
	.setattr	= exofs_setattr,
};
