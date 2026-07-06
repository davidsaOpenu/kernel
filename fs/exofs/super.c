// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/exofs/super.c
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/uaccess.h>
#include <linux/iversion.h>
#include "exofs.h"
#include "submodule.h"

static int exofs_statfs(struct dentry *dentry, struct kstatfs *buf);
static int exofs_sync_fs(struct super_block *sb, int wait);
static int exofs_freeze(struct super_block *sb);
static int exofs_unfreeze(struct super_block *sb);

static void exofs_put_super(struct super_block *sb)
{
	struct exofs_sb_info *sbi = EXOFS_SB(sb);
	kfree(sbi);
}

static struct kmem_cache *exofs_inode_cachep;

static struct inode *exofs_alloc_inode(struct super_block *sb)
{
	struct exofs_inode_info *ei;
	struct exofs_sb_info *sbi = EXOFS_SB(sb);

	sbi = EXOFS_SB(sb);
	ei = alloc_inode_sb(sb, exofs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	inode_set_iversion(&ei->vfs_inode, 1);
	return &ei->vfs_inode;
}

static void exofs_free_in_core_inode(struct inode *inode)
{
	//do not access EXOFS_SB(inode->i_sb), i.e. inode->i_sb->s_fs_info because inodes are freed defered with RCU and sbi is set to NULL in fill_super
	kmem_cache_free(exofs_inode_cachep, EXOFS_I(inode));
}

static void init_once(void *foo)
{
	struct exofs_inode_info *ei = (struct exofs_inode_info *)foo;

	inode_init_once(&ei->vfs_inode);
}

static int __init init_inodecache(void)
{
	exofs_inode_cachep = kmem_cache_create_usercopy(
		"exofs_inode_cache", sizeof(struct exofs_inode_info), 0,
		SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
		offsetof(struct exofs_inode_info, i_data),
		sizeof_field(struct exofs_inode_info, i_data), init_once);
	if (exofs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(exofs_inode_cachep);
}

static int exofs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct exofs_sb_info *__maybe_unused sbi = EXOFS_SB(sb);
	seq_puts(seq, "TODO\n");
	return 0;
}

static const struct super_operations exofs_sops = {
	.alloc_inode = exofs_alloc_inode,
	.free_inode = exofs_free_in_core_inode,
	.write_inode = exofs_write_inode,
	.evict_inode = exofs_evict_inode,
	.put_super = exofs_put_super,
	.sync_fs = exofs_sync_fs,
	.freeze_fs = exofs_freeze,
	.unfreeze_fs = exofs_unfreeze,
	.statfs = exofs_statfs,
	.show_options = exofs_show_options,
};

static const struct fs_parameter_spec exofs_param_spec[] = { {} };

static int exofs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, exofs_param_spec, param, &result);
	if (opt < 0)
		return opt;
	return 0;
}

static int exofs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct exofs_sb_info *sbi;
	struct inode *root;
	struct exofs_fscb fscb;
	ssize_t sb_read_res;
	long ret = -ENOMEM;
	pr_info("exofs: sb: %p\n", sb);

	sbi = kzalloc_obj(*sbi);
	if (!sbi)
		return -ENOMEM;
	
	spin_lock_init(&sbi->lock);

	sbi->ns = sb->s_bdev->bd_disk->private_data;

	sb->s_fs_info = sbi;
	pr_info("exofs: sbi: %p\n", sbi);

	ret = -EINVAL;

	sb->s_op = &exofs_sops;

	sb_read_res = nvme_submit_sync_kv_read(EXOFS_SB(sb)->ns, EXOFS_SUPER_ID,
					       &fscb, sizeof(fscb), 0);
	if (sb_read_res < 0) {
		ret = sb_read_res;
		exofs_pr_err("error: failed to read superblock");
		goto cleanup_sb;
	}

	if (le16_to_cpu(fscb.s_magic) != EXOFS_SUPER_MAGIC) {
		exofs_pr_err("error: bad magic value %hu, read_res: %ld\n", le16_to_cpu(fscb.s_magic), sb_read_res);
		ret = -EINVAL;
		goto cleanup_sb;
	}

	if (le32_to_cpu(fscb.s_version) != EXOFS_FSCB_VER) {
		exofs_pr_err("error: unsupported fscb version %d\n",
			  le32_to_cpu(fscb.s_version));
		ret = -EINVAL;
		goto cleanup_sb;
	}

	sb->s_magic = le16_to_cpu(fscb.s_magic);
	sbi->nextid = le64_to_cpu(fscb.s_nextid);
	sbi->numfiles = le64_to_cpu(fscb.s_numfiles);
	sbi->dirty = false;

	root = exofs_iget(sb, EXOFS_ROOT_ID);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		exofs_pr_err("error: failed to get root inode\n");
		goto cleanup_sb;
	}

	if (!S_ISDIR(root->i_mode)) {
		ret = -ENOTDIR;
		exofs_pr_err("error: corrupt root inode, run e2fsck\n");
		goto cleanup_root;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		exofs_pr_err("error: get root inode failed\n");
		ret = -ENOMEM;
		goto cleanup_root;
	}

	return 0;

cleanup_root:
	iput(root);
cleanup_sb:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

static int exofs_sync_fs(struct super_block *sb, int wait)
{
	struct exofs_sb_info *sbi = EXOFS_SB(sb);
	struct exofs_fscb fscb;
	ssize_t write_res;

	if (wait)
		sync_inodes_sb(sb);

	spin_lock(&sbi->lock);
	if (!sbi->dirty) {
		spin_unlock(&sbi->lock);
		exofs_pr_debug("exofs_sync_fs: no changes to sync in superblock\n");
		return 0;
	}
	fscb.s_nextid = cpu_to_le64(sbi->nextid);
	fscb.s_numfiles = cpu_to_le64(sbi->numfiles);
	sbi->dirty = false;
	spin_unlock(&sbi->lock);

	fscb.s_magic = cpu_to_le16(EXOFS_SUPER_MAGIC);
	fscb.s_version = cpu_to_le32(EXOFS_FSCB_VER);
	fscb.s_newfs = 1;

	write_res = nvme_submit_sync_kv_write(EXOFS_SB(sb)->ns, EXOFS_SUPER_ID,
					      &fscb, sizeof(fscb), 0);
	if (write_res < 0) {
		spin_lock(&sbi->lock);
		exofs_pr_err("error: failed to write superblock");
		sbi->dirty = true;
		spin_unlock(&sbi->lock);
		return write_res;
	}

	return 0;
}

static int exofs_freeze(struct super_block *sb)
{
	return -ENOSYS;
}

static int exofs_unfreeze(struct super_block *sb)
{
	return -ENOSYS;
}

static int exofs_reconfigure(struct fs_context *fc)
{
	return -ENOSYS;
}

static int exofs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct exofs_sb_info *sbi = EXOFS_SB(sb);

	buf->f_type = EXOFS_SUPER_MAGIC;
	spin_lock(&sbi->lock);
	buf->f_files = sbi->numfiles;
	spin_unlock(&sbi->lock);

	return 0;
}

static int exofs_get_tree(struct fs_context *fc)
{
	pr_info("get_tree called\n");
	return get_tree_bdev(fc, exofs_fill_super);
}

static void exofs_free_fc(struct fs_context *fc)
{
}

static const struct fs_context_operations exofs_context_ops = {
	.parse_param = exofs_parse_param,
	.get_tree = exofs_get_tree,
	.reconfigure = exofs_reconfigure,
	.free = exofs_free_fc,
};

static int exofs_init_fs_context(struct fs_context *fc)
{
	pr_info("in exofs_init_fs_context\n");
	fc->ops = &exofs_context_ops;

	return 0;
}

static struct file_system_type exofs_fs_type = {
	.owner = THIS_MODULE,
	.name = "exofs",
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
	.init_fs_context = exofs_init_fs_context,
	.parameters = exofs_param_spec,
};
MODULE_ALIAS_FS("exofs");

static int exofs_fs_init(void)
{
	int err;

	err = init_inodecache();
	if (err)
		return err;
	err = register_filesystem(&exofs_fs_type);
	if (err)
		goto out_err;
	return 0;

out_err:
	destroy_inodecache();
	return err;
}

static void exofs_fs_exit(void)
{
	unregister_filesystem(&exofs_fs_type);
	destroy_inodecache();
}

struct exofs_submodule exofs_fs_submodule = {
	.init = exofs_fs_init,
	.exit = exofs_fs_exit,
};
