// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/exofs/inode.c
 */

#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include "exofs.h"

struct inode *exofs_iget(struct super_block *const sb, const unsigned long ino)
{
	struct inode *const inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;

	long ret;

	struct exofs_fcb fcb;
	ret = nvme_submit_sync_kv_read(EXOFS_SB(sb)->ns,
				       exofs_nvme_metadata(ino), &fcb,
				       sizeof(fcb), 0);
	if (ret != sizeof(fcb))
		exofs_dbg("exofs iget read size: %ld, fcbsize: %ld\n", ret,
			  (long)(sizeof(fcb)));
	if (ret < 0)
		goto bad_inode;

	inode->i_mode = le16_to_cpu(fcb.i_mode);
	i_uid_write(inode, le32_to_cpu(fcb.i_uid));
	i_gid_write(inode, le32_to_cpu(fcb.i_gid));
	set_nlink(inode, le16_to_cpu(fcb.i_links_count));
	i_size_write(inode, le64_to_cpu(fcb.i_size));
	inode_set_ctime(inode, (time64_t)le64_to_cpu(fcb.i_ctime), 0);
	inode_set_atime(inode, (time64_t)le64_to_cpu(fcb.i_atime), 0);
	inode_set_mtime(inode, (time64_t)le64_to_cpu(fcb.i_mtime), 0);
	inode->i_generation = le32_to_cpu(fcb.i_generation);

	if (inode->i_nlink == 0 && inode->i_mode == 0) {
		ret = -ESTALE;
		goto bad_inode;
	}

	if (i_size_read(inode) < 0) {
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		if (fcb.i_data[0])
			inode->i_rdev =
				old_decode_dev(le32_to_cpu(fcb.i_data[0]));
		else
			inode->i_rdev =
				new_decode_dev(le32_to_cpu(fcb.i_data[1]));
	} else {
		memcpy(EXOFS_I(inode)->i_data, fcb.i_data, sizeof(fcb.i_data));
	}

	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &exofs_file_inode_operations;
		inode->i_fop = &exofs_file_operations;
		inode->i_mapping->a_ops = &exofs_aops;
		break;
	case S_IFDIR:
		inode->i_op = &exofs_dir_inode_operations;
		inode->i_fop = &exofs_dir_operations;
		inode->i_mapping->a_ops = &exofs_aops;
		break;
	case S_IFLNK:
		if (exofs_inode_is_fast_symlink(inode)) {
			inode->i_op = &simple_symlink_inode_operations;
			inode->i_link = (char *)EXOFS_I(inode)->i_data;
		} else {
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			inode->i_mapping->a_ops = &exofs_aops;
		}
		break;
	default:
		inode->i_op = &exofs_special_inode_operations;
		if (fcb.i_data[0])
			init_special_inode(
				inode, inode->i_mode,
				old_decode_dev(le32_to_cpu(fcb.i_data[0])));
		else
			init_special_inode(
				inode, inode->i_mode,
				new_decode_dev(le32_to_cpu(fcb.i_data[1])));
		break;
	}
	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}

void exofs_evict_inode(struct inode *inode)
{
	exofs_dbg("EXOFS EVICT ino=%lu nlink=%u count=%d\n", inode->i_ino,
		  inode->i_nlink, atomic_read(&inode->i_count));

	truncate_inode_pages_final(&inode->i_data);

	if (!inode->i_nlink && !is_bad_inode(inode)) {
		exofs_dbg("EXOFS EVICT DELETE ino=%lu nlink=%u count=%d\n",
			  inode->i_ino, inode->i_nlink, atomic_read(&inode->i_count));
		spin_lock(&EXOFS_SB(inode->i_sb)->lock);
		EXOFS_SB(inode->i_sb)->numfiles--;
		EXOFS_SB(inode->i_sb)->dirty = true;
		spin_unlock(&EXOFS_SB(inode->i_sb)->lock);

		nvme_submit_sync_kv_delete(EXOFS_SB(inode->i_sb)->ns,
					   exofs_nvme_metadata(inode->i_ino));
		nvme_submit_sync_kv_delete(EXOFS_SB(inode->i_sb)->ns,
					   exofs_nvme_data(inode->i_ino));
	}

	clear_inode(inode);
}

static int exofs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	loff_t pos = folio_pos(folio);
	loff_t isize = i_size_read(inode);
	size_t len;
	void *kaddr;
	ssize_t readlen;
	exofs_dbg("exofs read folio: %p\n", folio);

	if (pos >= isize) {
		folio_zero_segment(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		return 0;
	}

	len = min_t(loff_t, folio_size(folio), isize - pos);

	kaddr = kmap_local_folio(folio, 0);

	readlen = nvme_submit_sync_kv_read(EXOFS_SB(inode->i_sb)->ns,
					   exofs_nvme_data(inode->i_ino), kaddr,
					   len, pos);

	exofs_dbg("READ: page=%lu readlen=%ld first_rec_len=%u "
		  "first_inode=%llu\n",
		  folio->index, readlen,
		  le16_to_cpu(((struct exofs_dir_entry *)kaddr)->rec_len),
		  le64_to_cpu(((struct exofs_dir_entry *)kaddr)->inode_no));

	if (readlen >= 0) {
		if (readlen < folio_size(folio))
			memset(kaddr + readlen, 0, folio_size(folio) - readlen);

		folio_mark_uptodate(folio);
	}

	kunmap_local(kaddr);

	folio_unlock(folio);

	return readlen >= 0 ? 0 : -EIO;
}

static int exofs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct inode *const inode = mapping->host;
	struct super_block *const sb = inode->i_sb;

	ssize_t writelen = 0;
	int err = 0;
	struct folio *folio = NULL;

	// DO NOT break out if the loop writeback_iter expects to be called until it returns NULL
	while ((folio = writeback_iter(mapping, wbc, folio, &err))) {
		loff_t pos = folio_pos(folio);
		size_t len = folio_size(folio);

		void *kaddr = kmap_local_folio(folio, 0);

		folio_clear_dirty_for_io(folio);

		folio_start_writeback(folio);

		writelen = nvme_submit_sync_kv_write(
			EXOFS_SB(sb)->ns, exofs_nvme_data(inode->i_ino), kaddr,
			len, pos);

		if (writelen < 0) {
			err = writelen;
			folio_redirty_for_writepage(wbc, folio);
		}

		kunmap_local(kaddr);
		folio_end_writeback(folio);
		folio_unlock(folio);
	}

	return err;
}

static int exofs_write_begin(const struct kiocb *iocb,
			     struct address_space *mapping, loff_t pos,
			     unsigned len, struct folio **foliop, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct folio *folio;
	loff_t i_size;
	loff_t f_pos;
	size_t read_len;
	int ret;

	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_WRITEBEGIN,
				    mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	*foliop = folio;

	/*
	 * The folio already contains the complete contents of this
	 * portion of the file.
	 */
	if (folio_test_uptodate(folio))
		return 0;

	i_size = i_size_read(inode);
	f_pos = folio_pos(folio);

	/*
	 * Nothing exists in the backing KV for this folio.
	 * The entire folio is beyond EOF, so it is logically zero.
	 */
	if (f_pos >= i_size) {
		folio_zero_range(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
		return 0;
	}

	/*
	 * Read the part of the folio which already belongs to the file.
	 */
	read_len = min_t(loff_t, folio_size(folio), i_size - f_pos);

	ret = nvme_submit_sync_kv_read(EXOFS_SB(inode->i_sb)->ns,
				       exofs_nvme_data(inode->i_ino),
				       folio_address(folio), read_len, f_pos);
	if (ret)
		goto error;

	/*
	 * The rest of the folio is beyond EOF and must read as zero.
	 */
	if (read_len < folio_size(folio))
		folio_zero_range(folio, read_len, folio_size(folio) - read_len);

	folio_mark_uptodate(folio);

	return 0;

error:
	folio_unlock(folio);
	folio_put(folio);
	*foliop = NULL;

	return ret;
}

static int exofs_write_end(const struct kiocb *iocb,
			   struct address_space *mapping, loff_t pos,
			   unsigned len, unsigned copied, struct folio *folio,
			   void *fsdata)
{
	struct inode *inode = mapping->host;
	void *buf;
	loff_t end;
	int ret;

	if (!copied)
		goto out;

	buf = kmap_local_folio(folio, offset_in_folio(folio, pos));

	ret = nvme_submit_sync_kv_write(EXOFS_SB(inode->i_sb)->ns,
					exofs_nvme_data(inode->i_ino), buf,
					copied, pos);

	kunmap_local(buf);

	if (ret)
		goto out_err;

	folio_mark_uptodate(folio);

	end = pos + copied;

	if (end > i_size_read(inode))
		i_size_write(inode, end);

	mark_inode_dirty(inode);

out:
	folio_unlock(folio);
	folio_put(folio);

	return copied;

out_err:
	folio_clear_uptodate(folio);

	folio_unlock(folio);
	folio_put(folio);

	return ret;
}

const struct address_space_operations exofs_aops = {
	.dirty_folio = filemap_dirty_folio,
	.read_folio = exofs_read_folio,
	.writepages = exofs_writepages,
	.error_remove_folio = generic_error_remove_folio,
	.write_begin = exofs_write_begin, //modeled after jffs2
	.write_end = exofs_write_end, //modeled after jffs2
};

void exofs_set_file_ops(struct inode *inode)
{
	inode->i_op = &exofs_file_inode_operations;
	inode->i_fop = &exofs_file_operations;
	inode->i_mapping->a_ops = &exofs_aops;
}

int exofs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct exofs_fcb fcb;

	memset(&fcb, 0, sizeof(fcb));
	fcb.i_mode = cpu_to_le16(inode->i_mode);
	fcb.i_uid = cpu_to_le32(i_uid_read(inode));
	fcb.i_gid = cpu_to_le32(i_gid_read(inode));
	fcb.i_links_count = cpu_to_le16(inode->i_nlink);
	fcb.i_ctime = cpu_to_le64(inode->i_ctime_sec);
	fcb.i_atime = cpu_to_le64(inode->i_atime_sec);
	fcb.i_mtime = cpu_to_le64(inode->i_mtime_sec);
	fcb.i_size = cpu_to_le64(i_size_read(inode));
	fcb.i_generation = cpu_to_le32(inode->i_generation);

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		if (old_valid_dev(inode->i_rdev)) {
			fcb.i_data[0] =
				cpu_to_le32(old_encode_dev(inode->i_rdev));
			fcb.i_data[1] = 0;
		} else {
			fcb.i_data[0] = 0;
			fcb.i_data[1] =
				cpu_to_le32(new_encode_dev(inode->i_rdev));
			fcb.i_data[2] = 0;
		}
	} else {
		memcpy(fcb.i_data, EXOFS_I(inode)->i_data, sizeof(fcb.i_data));
	}

	const ssize_t ret = nvme_submit_sync_kv_write(
		EXOFS_SB(inode->i_sb)->ns, exofs_nvme_metadata(inode->i_ino),
		&fcb, sizeof(fcb), 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int exofs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(idmap, dentry, attr);
	if (error)
		return error;

	if (attr->ia_valid & ATTR_SIZE) {
		error = inode_newsize_ok(inode, attr->ia_size);
		if (error)
			return error;
		truncate_setsize(inode, attr->ia_size);

		//TODO truncate the data in the backing KV store if the new size is smaller than the old size
	}

	setattr_copy(idmap, inode, attr);

	mark_inode_dirty(inode);

	return 0;
}

int exofs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	return 0;
}
