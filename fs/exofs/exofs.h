/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>
#include "exofs_fs.h"
#include <linux/blockgroup_lock.h>
#include <linux/percpu_counter.h>
#include <linux/rbtree.h>
#include <linux/mm.h>
#include <linux/highmem.h>

#include "nvme.h"
#include "debug.h"

struct exofs_sb_info {
	struct nvme_ns* ns; 
	spinlock_t lock;
	uint64_t nextid;
	uint64_t numfiles;
	bool dirty;
};

/*
 * Define EXOFSFS_DEBUG to produce debug messages
 */
#undef EXOFSFS_DEBUG

/*
 * Debug code
 */
#ifdef EXOFSFS_DEBUG
#	define exofs_debug(f, a...)	{ \
					printk ("EXOFS-fs DEBUG file://%s:%d:%s:", \
						__FILE__, __LINE__, __func__); \
				  	printk (f, ## a); \
					}
#else
#	define exofs_debug(f, a...)	/**/
#endif

/*
 * Special inode numbers
 */

static inline struct exofs_sb_info *EXOFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}
/*
 * Mount flags
 */
#define EXOFS_MOUNT_PID			0x000001
#define EXOFS_MOUNT_TIMEOUT		0x000002


#define clear_opt(o, opt)		o &= ~EXOFS_MOUNT_##opt
#define set_opt(o, opt)			o |= EXOFS_MOUNT_##opt
#define test_opt(sb, opt)		(EXOFS_SB(sb)->mount_opt & \
					 EXOFS_MOUNT_##opt)


/*
 * exofs mount options
 */
struct exofs_mount_options {
	unsigned long mount_opt;
	int timeout;
	unsigned long pid;
};

/*
 * second extended file system inode data in memory
 */
struct exofs_inode_info {
	struct inode	vfs_inode;
	uint32_t       i_data[EXOFS_IDATA];/*short symlink names and device #s*/
};

struct exofs_dir_lookup {
	struct exofs_dir_entry *de;
	struct folio *folio;
	void *kaddr;
};

/*
 * Function prototypes
 */

/*
 * Ok, these declarations are also in <linux/kernel.h> but none of the
 * exofs source programs needs to include it so they are duplicated here.
 */

static inline struct exofs_inode_info *EXOFS_I(struct inode *inode)
{
	return container_of(inode, struct exofs_inode_info, vfs_inode);
}

/*
 * Test whether an inode is a fast symlink.
 */
static inline int exofs_inode_is_fast_symlink(struct inode *inode)
{
	return S_ISLNK(inode->i_mode) && (EXOFS_I(inode)->i_data[0] != 0);
}

/* dir.c */
int exofs_add_link(struct dentry *, struct inode *);
int exofs_make_empty(struct inode *, struct inode *);
int exofs_find_entry(struct inode *dir, const char *name,
			    unsigned namelen, struct exofs_dir_lookup *res);
int exofs_delete_entry(struct inode *dir, struct dentry *dentry);
int exofs_empty_dir(struct inode *);
struct exofs_dir_entry *exofs_dotdot(struct inode *dir, struct folio **foliop);
int exofs_set_link(struct inode *dir, struct exofs_dir_entry *de,
		struct folio *folio, struct inode *inode);
void folio_release_kmap(struct folio *folio, void *kaddr);

/* ialloc.c */
struct inode * exofs_new_inode (struct inode *, umode_t);

/* inode.c */
struct inode *exofs_iget (struct super_block *, unsigned long);
int exofs_write_inode (struct inode *, struct writeback_control *);
void exofs_evict_inode(struct inode *);
int exofs_setattr (struct mnt_idmap *, struct dentry *, struct iattr *);
int exofs_getattr (struct mnt_idmap *, const struct path *,
			 struct kstat *, u32, unsigned int);

/* dir.c */
extern const struct file_operations exofs_dir_operations;

/* file.c */
extern const struct inode_operations exofs_file_inode_operations;
extern const struct file_operations exofs_file_operations;

/* inode.c */
extern void exofs_set_file_ops(struct inode *inode);
extern const struct address_space_operations exofs_aops;

/* namei.c */
extern const struct inode_operations exofs_dir_inode_operations;
extern const struct inode_operations exofs_special_inode_operations;
