// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/exofs/namei.c
 */

#include <linux/pagemap.h>
#include "exofs.h"

static int exofs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	int err;

	inode = exofs_new_inode(dir, mode | S_IFREG);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &exofs_file_inode_operations;
	inode->i_fop = &exofs_file_operations;
	inode->i_mapping->a_ops = &exofs_aops;

	err = exofs_add_link(dentry, inode);
	if (err)
		goto fail;

	mark_inode_dirty(inode);

	d_instantiate(dentry, inode);
	return 0;

fail:
	iput(inode);
	return err;
}

static struct dentry *exofs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct exofs_dir_lookup lookup;
	struct inode *inode;
	int err;

	if (dentry->d_name.len > EXOFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	err = exofs_find_entry(dir, dentry->d_name.name, dentry->d_name.len,
			       &lookup);

	if (err == -ENOENT) {
		/*
		 * Negative dentry.
		 */
		d_add(dentry, NULL);
		return NULL;
	}

	if (err)
		return ERR_PTR(err);

	inode = exofs_iget(dir->i_sb, le64_to_cpu(lookup.de->inode_no));

	folio_release_kmap(lookup.folio, lookup.kaddr);

	if (IS_ERR(inode))
		return ERR_CAST(inode);

	d_add(dentry, inode);

	return NULL;
}


static int exofs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	inode_set_ctime_current(inode);
	inc_nlink(inode);

	ihold(inode);

	err = exofs_add_link(dentry, inode);
	if (err) {
		drop_nlink(inode);
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);

	mark_inode_dirty(inode);

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

	return 0;
}


static int exofs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	exofs_dbg("UNLINK BEFORE: ino=%lu nlink=%u count=%d dentry=%px\n",
       inode->i_ino,
       inode->i_nlink,
       atomic_read(&inode->i_count),
       dentry);

	err = exofs_delete_entry(dir, dentry);

	if (err)
		return err;

	exofs_dbg("UNLINK AFTER DELETE: ino=%lu nlink=%u count=%d\n",
       inode->i_ino,
       inode->i_nlink,
       atomic_read(&inode->i_count));

	drop_nlink(inode);


	exofs_dbg("UNLINK AFTER DROP: ino=%lu nlink=%u count=%d\n",
       inode->i_ino,
       inode->i_nlink,
       atomic_read(&inode->i_count));

	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);

	return 0;
}

static int exofs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	size_t len = strlen(symname) + 1;
	int err;

	inode = exofs_new_inode(dir, S_IFLNK | 0777);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	/*
	 * Fast symlink:
	 * Store the string directly inside inode private data.
	 */
	if (len <= sizeof(EXOFS_I(inode)->i_data)) {
		exofs_dbg("exofs_symlink: fast symlink, len=%zu, symname=%s\n", len, symname);
		memcpy(EXOFS_I(inode)->i_data, symname, len);

		inode->i_link = (char *)EXOFS_I(inode)->i_data;

		inode->i_op = &simple_symlink_inode_operations;
		i_size_write(inode, len - 1);

	} else {
		/*
		 * Slow symlink:
		 * Data lives in page cache and is written
		 * through exofs_aops.
		 */
		exofs_dbg("exofs_symlink: slow symlink, len=%zu, symname=%s\n", len, symname);
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &exofs_aops;

		err = page_symlink(inode, symname, len);

		if (err)
			goto out_fail;
	}

	err = exofs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	mark_inode_dirty(inode);

	d_instantiate(dentry, inode);

	return 0;

out_fail:
	iput(inode);
	return err;
}

static struct dentry* exofs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	inode = exofs_new_inode(dir, S_IFDIR | mode);

	if (IS_ERR(inode))
		return ERR_CAST(inode);

	inode->i_op = &exofs_dir_inode_operations;
	inode->i_fop = &exofs_dir_operations;
	inode->i_mapping->a_ops = &exofs_aops;

	err = exofs_make_empty(inode, dir);

	if (err)
		goto out_fail;

	err = exofs_add_link(dentry, inode);

	if (err)
		goto out_fail;

	inc_nlink(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	mark_inode_dirty(dir);
	mark_inode_dirty(inode);

	d_instantiate(dentry, inode);

	return ERR_PTR(err);

out_fail:
	clear_nlink(inode);
	iput(inode);

	return ERR_PTR(err);
}

static int exofs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	err = exofs_empty_dir(inode);
	if (err)
		return err;

	err = exofs_delete_entry(dir, dentry);
	if (err)
		return err;

	drop_nlink(dir);
	clear_nlink(inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	mark_inode_dirty(inode);
	mark_inode_dirty(dir);

	return 0;
}

static int exofs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode;
	int err;

	inode = exofs_new_inode(dir, mode);

	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, mode, dev);

	err = exofs_add_link(dentry, inode);

	if (err) {
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);

	return 0;
}

static int exofs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			 struct file *file, umode_t mode)
{
	struct inode *inode;

	inode = exofs_new_inode(dir, S_IFREG | mode);

	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &exofs_file_inode_operations;
	inode->i_fop = &exofs_file_operations;
	inode->i_mapping->a_ops = &exofs_aops;

	d_tmpfile(file, inode);

	return 0;
}

static int exofs_rename(struct mnt_idmap *idmap,
			struct inode *old_dir,
			struct dentry *old_dentry,
			struct inode *new_dir,
			struct dentry *new_dentry,
			unsigned int flags)
{
	struct inode *inode = d_inode(old_dentry);
	struct inode *target = d_inode(new_dentry);
	struct exofs_dir_lookup old_lookup;
	struct exofs_dir_lookup new_lookup;
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/* Find source. */
	err = exofs_find_entry(old_dir,
			       old_dentry->d_name.name,
			       old_dentry->d_name.len,
			       &old_lookup);
	if (err)
		return err;

	/*
	 * Make sure the entry we found still points to the inode
	 * that VFS is asking us to rename.
	 */
	if (le64_to_cpu(old_lookup.de->inode_no) != inode->i_ino) {
		err = -ENOENT;
		goto out_old;
	}

	if (target) {
		if (flags & RENAME_NOREPLACE) {
			err = -EEXIST;
			goto out_old;
		}

		err = exofs_find_entry(new_dir,
				       new_dentry->d_name.name,
				       new_dentry->d_name.len,
				       &new_lookup);
		if (err)
			goto out_old;

		err = exofs_set_link(new_dir, new_lookup.de,
				     new_lookup.folio, inode);

		folio_release_kmap(new_lookup.folio, new_lookup.kaddr);

		if (err)
			goto out_old;

		/*
		 * The replaced inode loses its directory link.
		 */
		drop_nlink(target);
	} else {
		err = exofs_add_link(new_dentry, inode);
		if (err)
			goto out_old;
	}

	err = exofs_delete_entry(old_dir, old_dentry);
	if (err)
		goto out_old;

	/*
	 * The inode's ctime changes because its directory entry
	 * changed.
	 */
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);

out_old:
	folio_release_kmap(old_lookup.folio, old_lookup.kaddr);

	return err;
}

const struct inode_operations exofs_dir_inode_operations = {
	.create = exofs_create,
	.lookup = exofs_lookup,
	.link = exofs_link,
	.unlink = exofs_unlink,
	.symlink = exofs_symlink,
	.mkdir = exofs_mkdir,
	.rmdir = exofs_rmdir,
	.mknod = exofs_mknod,
	.rename = exofs_rename,
	.setattr = exofs_setattr,
	.tmpfile = exofs_tmpfile,
};

const struct inode_operations exofs_special_inode_operations = {
	.setattr = exofs_setattr,
};
