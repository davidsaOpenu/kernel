// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/exofs/ialloc.c
 */

#include <linux/quotaops.h>
#include <linux/sched.h>
#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include <linux/random.h>
#include "exofs.h"

struct inode *exofs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct exofs_inode_info *ei;
	struct inode* const inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	
	ei = EXOFS_I(inode);
	spin_lock(&EXOFS_SB(sb)->lock);
	inode->i_ino = EXOFS_OBJ_OFF + (EXOFS_SB(sb)->nextid++);
	EXOFS_SB(sb)->numfiles++;
	EXOFS_SB(sb)->dirty = true;
	spin_unlock(&EXOFS_SB(sb)->lock);

	exofs_pr_debug("exofs_new_inode: new inode %lu\n", inode->i_ino);

	inode->i_mode = mode;
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	set_nlink(inode, 1);
	simple_inode_init_ts(inode);
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	return inode;
}
