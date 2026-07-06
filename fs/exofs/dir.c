// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/exofs/dir.c
 *
 * Exofs directory handling
 *
 * Directory data is stored through the page cache.
 * Persistence is handled by dirty folio writeback.
 */

#include "exofs.h"

#include <linux/filelock.h>
#include <linux/pagemap.h>
#include <linux/iversion.h>
#include <linux/namei.h>

/*
 * Return number of valid bytes in directory page.
 */
static unsigned exofs_last_byte(struct inode *inode, unsigned long page_nr)
{
	loff_t size = i_size_read(inode);
	loff_t offset = (loff_t)page_nr << PAGE_SHIFT;

	if (offset >= size)
		return 0;

	size -= offset;

	if (size > PAGE_SIZE)
		size = PAGE_SIZE;

	return (unsigned)size;
}

/*
 * Clear an unused directory record.
 */
static inline void exofs_clear_entry(struct exofs_dir_entry *de)
{
	exofs_pr_debug("exofs_clear_entry: de=%p, rec_len=%hu\n", de, le16_to_cpu(de->rec_len));
	de->inode_no = 0;
	memset(de->name, 0, de->name_len);
	de->name_len = 0;
	de->file_type = 0;
}

/*
 * Return next directory entry.
 */
static inline struct exofs_dir_entry *
exofs_next_entry(struct exofs_dir_entry *de)
{
	return (struct exofs_dir_entry *)((char *)de +
					  le16_to_cpu(de->rec_len));
}

static inline int exofs_entry_valid(struct exofs_dir_entry *de, unsigned remain)
{
	unsigned rec_len;

	rec_len = le16_to_cpu(de->rec_len);

	if (rec_len < EXOFS_DIR_REC_LEN(1))
		return -EFSCORRUPTED;

	if (rec_len & 3)
		return -EFSCORRUPTED;

	if (rec_len > remain)
		return -EFSCORRUPTED;

	if (de->name_len > EXOFS_NAME_LEN)
		return -EFSCORRUPTED;

	if (rec_len < EXOFS_DIR_REC_LEN(de->name_len))
		return -EFSCORRUPTED;

	return 0;
}

/*
 * Compare directory entry name.
 */
static inline bool exofs_match(unsigned int len, const char *name,
			       struct exofs_dir_entry *de)
{
	if (!de->inode_no)
		return false;

	if (de->name_len != len)
		return false;

	return !memcmp(name, de->name, len);
}

/*
 * Validate one directory page.
 */
static bool exofs_check_folio(struct folio *folio, void *kaddr)
{
	struct inode *dir = folio->mapping->host;
	const char *reason = NULL;

	unsigned limit;
	unsigned offset;

	limit = exofs_last_byte(dir, folio->index);

	if (!limit)
		return true;

	for (offset = 0; offset + EXOFS_DIR_REC_LEN(1) <= limit;) {
		struct exofs_dir_entry *de;
		unsigned rec_len;

		de = (struct exofs_dir_entry *)((char *)kaddr + offset);

		rec_len = le16_to_cpu(de->rec_len);
		exofs_pr_debug(
			"exofs_check_folio: de=%p, offset=%u, limit=%u, inode_no: %llu, rec_len: %hu\n",
			de, offset, limit, le64_to_cpu(de->inode_no), rec_len);

		if (rec_len < EXOFS_DIR_REC_LEN(1)) {
			exofs_pr_debug(
				"exofs_check_folio: rec_len=%u < EXOFS_DIR_REC_LEN(1)=%lu\n",
				rec_len, EXOFS_DIR_REC_LEN(1));
			reason = "short record length";
			goto bad;
		}

		if (rec_len & 3) {
			exofs_pr_debug("exofs_check_folio: rec_len=%u & 3 != 0\n",
				  rec_len);
			reason = "unaligned record length";
			goto bad;
		}

		if (rec_len < EXOFS_DIR_REC_LEN(de->name_len)) {
			exofs_pr_debug(
				"exofs_check_folio: rec_len=%u < EXOFS_DIR_REC_LEN(name_len=%hu)=%lu\n",
				rec_len, de->name_len,
				EXOFS_DIR_REC_LEN(de->name_len));
			reason = "record length too small for name length";
			goto bad;
		}

		if (((offset + rec_len - 1) ^ offset) & PAGE_MASK) {
			reason = "record length spans chunk boundary";
			goto bad;
		}

		offset += rec_len;
	}

	if (offset != limit) {
		reason = "last record length does not reach end of chunk";
		goto bad;
	}

	folio_set_checked(folio);
	return true;

bad:

	exofs_pr_err("corrupt directory inode %lu page %lu (%s)", dir->i_ino, folio->index, reason);

	return false;
}

/*
 * Get directory folio.
 *
 * Returned folio is locked.
 * Returned address is locally mapped.
 *
 * Caller must call folio_release_kmap().
 */
static void *exofs_get_folio(struct inode *inode, pgoff_t index,
			     struct folio **foliop)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio = read_mapping_folio(mapping, index, NULL);
	void *kaddr;

	if (IS_ERR(folio))
		return ERR_CAST(folio);

	kaddr = kmap_local_folio(folio, 0);
	if (unlikely(!folio_test_checked(folio))) {
		if (!exofs_check_folio(folio, kaddr)) {
			folio_release_kmap(folio, kaddr);
			return ERR_PTR(-EIO);
		}
	}

	*foliop = folio;
	return kaddr;
}

/*
 * Finish directory modification.
 */
static void exofs_commit_chunk(struct inode *inode, struct folio *folio)
{
	folio_mark_dirty(folio);

	inode_inc_iversion(inode);

	mark_inode_dirty(inode);
}

/*
 * Validate offset after directory modification.
 */
static unsigned exofs_validate_entry(void *base, unsigned offset, unsigned mask)
{
	struct exofs_dir_entry *de;
	struct exofs_dir_entry *p;

	de = (struct exofs_dir_entry *)((char *)base + offset);

	p = (struct exofs_dir_entry *)((char *)base + (offset & mask));

	while ((void *)p < (void *)de) {
		if (!le16_to_cpu(p->rec_len))
			break;

		p = exofs_next_entry(p);
	}

	return (char *)p - (char *)base;
}

/*
 * Iterate directory entries.
 */
static int exofs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);

	loff_t pos = ctx->pos;

	exofs_pr_debug("exofs readdir pos: %ld\n", (long)pos);
	unsigned long page_nr = pos >> PAGE_SHIFT;
	unsigned offset = pos & ~PAGE_MASK;

	unsigned long npages = dir_pages(inode);

	bool need_revalidate;

	need_revalidate = !inode_eq_iversion(inode, *(u64 *)file->private_data);

	if (pos >= i_size_read(inode))
		return 0;

	for (; page_nr < npages; page_nr++, offset = 0) {
		struct folio *folio;
		struct exofs_dir_entry *de;
		void *kaddr;
		unsigned limit;

		kaddr = exofs_get_folio(inode, page_nr, &folio);

		if (IS_ERR(kaddr)) {
			exofs_pr_err("cannot read directory page %lu: %ld", page_nr, PTR_ERR(kaddr));

			return PTR_ERR(kaddr);
		}

		limit = exofs_last_byte(inode, page_nr);

		if (unlikely(need_revalidate)) {
			if (offset)
				offset = exofs_validate_entry(
					kaddr, offset, (unsigned)PAGE_MASK);

			*(u64 *)file->private_data =
				inode_query_iversion(inode);

			need_revalidate = false;
		}

		de = (struct exofs_dir_entry *)((char *)kaddr + offset);

		while ((char *)de + EXOFS_DIR_REC_LEN(1) <=
		       (char *)kaddr + limit) {
			unsigned rec_len;

			rec_len = le16_to_cpu(de->rec_len);

			if (!rec_len) {
				exofs_pr_err(
					"exofs zero length dir: page=%lu offset=%u, limit=%u, inode_no: %llu, rec_len: %hu\n",
					page_nr, (unsigned)((char*)de - (char*)kaddr), limit,
					le64_to_cpu(de->inode_no), rec_len);

				folio_release_kmap(folio, kaddr);

				return -EFSCORRUPTED;
			}

			if (de->inode_no) {
				unsigned char type;

				type = fs_ftype_to_dtype(de->file_type);

				if (!dir_emit(ctx, de->name, de->name_len,
					      le64_to_cpu(de->inode_no),
					      type)) {
					folio_release_kmap(folio, kaddr);

					return 0;
				}
			}

			ctx->pos += rec_len;

			de = exofs_next_entry(de);
		}

		folio_release_kmap(folio, kaddr);
	}

	return 0;
}

static int exofs_dir_open(struct inode *inode, struct file *file)
{
	u64 *cookie;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);

	if (!cookie)
		return -ENOMEM;

	*cookie = inode_query_iversion(inode);

	file->private_data = cookie;

	return 0;
}

static int exofs_dir_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);

	return 0;
}

static loff_t exofs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_llseek_cookie(file, offset, whence, file->private_data);
}

const struct file_operations exofs_dir_operations = {

	.open = exofs_dir_open,
	.release = exofs_dir_release,
	.llseek = exofs_dir_llseek,

	.read = generic_read_dir,
	.iterate_shared = exofs_readdir,

	.fsync = generic_file_fsync,
	.setlease = generic_setlease,
};

/*
 * Insert a fully prepared directory entry.
 */
static void exofs_insert_entry(struct exofs_dir_entry *de, unsigned reclen,
			       const char *name, unsigned namelen,
			       struct inode *inode)
{
	exofs_pr_debug(
		"exofs_insert_entry: de=%p, reclen=%u, name=%s, namelen=%u, inode_no=%lu\n",
		de, reclen, name, namelen, inode->i_ino);
	memset(de, 0, reclen);

	de->inode_no = cpu_to_le64(inode->i_ino);

	de->rec_len = cpu_to_le16(reclen);

	de->name_len = namelen;

	de->file_type = fs_umode_to_ftype(inode->i_mode);

	memcpy(de->name, name, namelen);
}

/*
 * Allocate a new empty directory page.
 */
static int exofs_extend_dir(struct inode *dir, struct folio **foliop,
			    void **kaddrp)
{
	unsigned long index;

	struct folio *folio;
	void *kaddr;

	struct exofs_dir_entry *de;

	index = dir_pages(dir);

	folio = filemap_grab_folio(dir->i_mapping, index);

	if (IS_ERR(folio))
		return PTR_ERR(folio);

	kaddr = kmap_local_folio(folio, 0);
	if (IS_ERR(kaddr)) {
		folio_unlock(folio);
		folio_put(folio);
		return PTR_ERR(kaddr);
	}

	memset(kaddr, 0, folio_size(folio));

	de = kaddr;

	de->inode_no = 0;
	de->name_len = 0;
	de->file_type = 0;
	de->rec_len = cpu_to_le16(PAGE_SIZE);

	i_size_write(dir, max_t(loff_t, i_size_read(dir),
				folio_pos(folio) + PAGE_SIZE));

	*foliop = folio;
	*kaddrp = kaddr;

	return 0;
}

static int exofs_add_link_folio(struct inode *dir, unsigned long n,
				const char *name, unsigned namelen,
				unsigned reclen, struct inode *inode)
{
	struct folio *folio;
	void *kaddr;
	struct exofs_dir_entry *de;
	unsigned limit;
	unsigned offset;
	int err = -ENOSPC;

	kaddr = exofs_get_folio(dir, n, &folio);
	if (IS_ERR(kaddr))
		return PTR_ERR(kaddr);

	folio_lock(folio);

	limit = exofs_last_byte(dir, n);

	for (offset = 0; offset + EXOFS_DIR_REC_LEN(1) <= limit;
	     offset += le16_to_cpu(de->rec_len)) {
		unsigned current_reclen;
		unsigned current_used;

		de = (struct exofs_dir_entry *)((char *)kaddr + offset);

		current_reclen = le16_to_cpu(de->rec_len);

		if (current_reclen < EXOFS_DIR_REC_LEN(1)) {
			err = -EFSCORRUPTED;
			break;
		}

		if (exofs_match(namelen, name, de)) {
			err = -EEXIST;
			break;
		}

		current_used = EXOFS_DIR_REC_LEN(de->name_len);

		if (!de->inode_no && current_reclen >= reclen) {
			exofs_insert_entry(de, current_reclen, name, namelen, inode);
			exofs_commit_chunk(dir, folio);

			err = 0;
			break;
		}

		if (de->inode_no && current_reclen >= current_used + reclen) {
			exofs_pr_debug(
				"exofs_add_link: splitting entry at offset %u, "
				"current_reclen=%u, current_used=%u, reclen=%u\n",
				offset, current_reclen, current_used, reclen);

			de->rec_len = cpu_to_le16(current_used);
			de = (void *)de + current_used;

			exofs_insert_entry(de, current_reclen - current_used,
					   name, namelen, inode);
			exofs_commit_chunk(dir, folio);

			err = 0;
			break;
		}
	}

	folio_unlock(folio);
	folio_release_kmap(folio, kaddr);

	return err;
}

static int exofs_add_link_new_page(struct inode *dir, const char *name,
				   unsigned namelen, struct inode *inode)
{
	struct folio *folio;
	struct exofs_dir_entry *de;
	void *kaddr;
	int err;

	err = exofs_extend_dir(dir, &folio, &kaddr);
	if (err)
		return err;

	de = kaddr;

	exofs_insert_entry(de, PAGE_SIZE, name, namelen, inode);

	folio_mark_uptodate(folio);

	exofs_commit_chunk(dir, folio);

	folio_unlock(folio);
	folio_release_kmap(folio, kaddr);

	return 0;
}

int exofs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	unsigned namelen = dentry->d_name.len;
	unsigned reclen;
	unsigned long pages;
	unsigned long n;
	int err;

	if (namelen > EXOFS_NAME_LEN)
		return -ENAMETOOLONG;

	reclen = EXOFS_DIR_REC_LEN(namelen);
	pages = dir_pages(dir);

	for (n = 0; n < pages; n++) {
		err = exofs_add_link_folio(dir, n, name, namelen, reclen,
					   inode);

		if (err == -ENOSPC)
			continue;

		if (err)
			return err;

		goto success;
	}

	err = exofs_add_link_new_page(dir, name, namelen, inode);
	if (err)
		return err;

success:
	inode_inc_iversion(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

	return 0;
}

int exofs_find_entry(struct inode *dir, const char *name, unsigned namelen,
		     struct exofs_dir_lookup *res)
{
	unsigned long n;
	unsigned long npages;

	npages = dir_pages(dir);

	for (n = 0; n < npages; n++) {
		struct folio *folio;
		void *kaddr;
		unsigned offset;
		unsigned limit;

		kaddr = exofs_get_folio(dir, n, &folio);

		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);

		limit = exofs_last_byte(dir, n);

		for (offset = 0; offset < limit;) {
			struct exofs_dir_entry *de;
			unsigned rec_len;

			de = kaddr + offset;

			rec_len = le16_to_cpu(de->rec_len);

			if (rec_len < EXOFS_DIR_REC_LEN(1)) {
				folio_release_kmap(folio, kaddr);

				return -EFSCORRUPTED;
			}

			if (exofs_match(namelen, name, de)) {
				res->de = de;
				res->folio = folio;
				res->kaddr = kaddr;

				return 0;
			}

			offset += rec_len;
		}

		folio_release_kmap(folio, kaddr);
	}

	return -ENOENT;
}

int exofs_delete_entry(struct inode *dir, struct dentry *dentry)
{
	struct exofs_dir_entry *prev = NULL;
	struct exofs_dir_entry *tmp;
	unsigned entry_offset;
	unsigned rec_len;
	int err = 0;

	struct exofs_dir_lookup lookup;
	err = exofs_find_entry(dir, dentry->d_name.name, dentry->d_name.len,
			       &lookup);

	if (err)
		return err;

	folio_lock(lookup.folio);

	entry_offset = (char *)lookup.de - (char *)lookup.kaddr;
	rec_len = le16_to_cpu(lookup.de->rec_len);

	if (rec_len < EXOFS_DIR_REC_LEN(1)) {
		err = -EFSCORRUPTED;
		goto out;
	}

	/*
	 * Find previous entry in this directory chunk.
	 *
	 * Directory entries cannot cross pages, so scanning the
	 * current folio is sufficient.
	 */
	if (entry_offset) {
		unsigned off = 0;
		unsigned limit = exofs_last_byte(dir, lookup.folio->index);

		while (off < entry_offset) {
			unsigned prev_len;

			tmp = (void *)lookup.kaddr + off;
			prev_len = le16_to_cpu(tmp->rec_len);

			if (prev_len < EXOFS_DIR_REC_LEN(1) ||
			    prev_len > limit - off) {
				err = -EFSCORRUPTED;
				goto out;
			}

			if (off + prev_len == entry_offset) {
				prev = tmp;
				break;
			}

			off += prev_len;
		}

		if (!prev) {
			err = -EFSCORRUPTED;
			goto out;
		}
	}

	if (prev) {
		/*
		 * Merge deleted entry into previous free space.
		 */

		unsigned prev_len = le16_to_cpu(prev->rec_len);
		unsigned new_len = prev_len + rec_len;

		exofs_pr_debug(
			"exofs_delete_entry: merging entry at offset %u, rec_len=%u into previous entry at offset %u, rec_len=%u\n",
			entry_offset, rec_len,
			(unsigned)((char *)prev - (char *)lookup.kaddr),
			le16_to_cpu(prev->rec_len));

		prev->rec_len = cpu_to_le16(new_len);
		memset(lookup.de, 0, rec_len);
	} else {
		exofs_clear_entry(lookup.de);
	}

	exofs_commit_chunk(dir, lookup.folio);

out:
	folio_unlock(lookup.folio);
	folio_release_kmap(lookup.folio, lookup.kaddr);

	if (!err) {
		inode_inc_iversion(dir);

		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

		mark_inode_dirty(dir);
	}

	return err;
}

int exofs_set_link(struct inode *dir, struct exofs_dir_entry *de,
		   struct folio *folio, struct inode *inode)
{
	void *kaddr;
	unsigned len;
	loff_t pos;
	int err;

	if (!folio_test_locked(folio))
		return -EINVAL;

	kaddr = kmap_local_folio(folio, 0);

	/*
	 * Caller normally passes the pointer obtained from the
	 * same mapped folio. Convert it into a folio offset.
	 */
	pos = folio_pos(folio) +
	      offset_in_folio(folio, (char *)de - (char *)kaddr);

	len = le16_to_cpu(de->rec_len);

	if (len < EXOFS_DIR_REC_LEN(1)) {
		err = -EFSCORRUPTED;
		goto out;
	}

	/*
	 * Preserve:
	 *   rec_len
	 *   name
	 *
	 * Only update inode number/type.
	 */
	de->inode_no = cpu_to_le64(inode->i_ino);

	de->file_type = fs_umode_to_ftype(inode->i_mode);

	exofs_commit_chunk(dir, folio);

out:
	kunmap_local(kaddr);

	if (!err) {
		inode_inc_iversion(dir);

		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

		mark_inode_dirty(dir);
	}

	return err;
}

/*
 * Check whether a directory is empty.
 *
 * Returns:
 *   0          - directory is empty
 *   -ENOTEMPTY - directory contains entries other than "." and ".."
 *   <0         - error
 */
int exofs_empty_dir(struct inode *dir)
{
	unsigned long n;
	unsigned long npages;

	npages = dir_pages(dir);

	for (n = 0; n < npages; n++) {
		struct folio *folio;
		void *kaddr;
		unsigned limit;
		unsigned offset;

		kaddr = exofs_get_folio(dir, n, &folio);

		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);

		limit = exofs_last_byte(dir, n);

		for (offset = 0; offset < limit;) {
			struct exofs_dir_entry *de;
			unsigned rec_len;

			de = (struct exofs_dir_entry *)((char *)kaddr + offset);

			rec_len = le16_to_cpu(de->rec_len);

			if (rec_len < EXOFS_DIR_REC_LEN(1) ||
			    offset + rec_len > limit) {
				folio_release_kmap(folio, kaddr);

				return -EFSCORRUPTED;
			}

			if (de->inode_no) {
				bool is_dot;
				bool is_dotdot;

				is_dot = de->name_len == 1 &&
					 de->name[0] == '.';

				is_dotdot = de->name_len == 2 &&
					    de->name[0] == '.' &&
					    de->name[1] == '.';

				if (!is_dot && !is_dotdot) {
					folio_release_kmap(folio, kaddr);

					return -ENOTEMPTY;
				}
			}

			offset += rec_len;
		}

		folio_release_kmap(folio, kaddr);
	}

	return 0;
}

/*
 * Initialize a newly created directory.
 *
 * The new directory contains:
 *
 *   [ "." ][ ".." ][ free space ... ]
 *
 * The directory is represented by one page initially.
 *
 * Parent directory inode must be locked by the caller.
 */
int exofs_make_empty(struct inode *inode, struct inode *parent)
{
	struct folio *folio;
	struct exofs_dir_entry *de;
	void *kaddr;
	unsigned dot_len;
	unsigned dotdot_len;
	unsigned free_len;
	int err;

	dot_len = EXOFS_DIR_REC_LEN(1);
	dotdot_len = EXOFS_DIR_REC_LEN(2);

	/*
	 * Allocate the first directory page.
	 */
	err = exofs_extend_dir(inode, &folio, &kaddr);
	if (err)
		return err;

	/*
	 * The first entry is ".".
	 */
	de = kaddr;

	exofs_insert_entry(de, dot_len, ".", 1, inode);

	/*
	 * The second entry is "..".
	 *
	 * It occupies all remaining space in the directory page.
	 * This is the standard directory representation: the last
	 * entry in a directory chunk absorbs the remaining space.
	 */
	de = exofs_next_entry(de);

	free_len = PAGE_SIZE - dot_len;

	exofs_insert_entry(de, free_len, "..", 2, parent);

	folio_mark_uptodate(folio);

	exofs_commit_chunk(inode, folio);

	folio_unlock(folio);
	folio_release_kmap(folio, kaddr);

	if (err)
		return err;

	inode_inc_iversion(inode);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

	return 0;
}
