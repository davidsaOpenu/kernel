/*
 * Copyright (C) 2005, 2006
 * Avishay Traeger (avishay@gmail.com)
 * Copyright (C) 2008, 2009
 * Boaz Harrosh <ooo@electrozaur.com>
 *
 * Copyrights for code taken from ext2:
 *     Copyright (C) 1992, 1993, 1994, 1995
 *     Remy Card (card@masi.ibp.fr)
 *     Laboratoire MASI - Institut Blaise Pascal
 *     Universite Pierre et Marie Curie (Paris VI)
 *     from
 *     linux/fs/minix/inode.c
 *     Copyright (C) 1991, 1992  Linus Torvalds
 *
 * This file is part of exofs.
 *
 * exofs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.  Since it is based on ext2, and the only
 * valid version of GPL for the Linux kernel is version 2, the only valid
 * version of GPL for exofs is version 2.
 *
 * exofs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with exofs; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/iversion.h>
#include <linux/pagemap.h>
#include "exofs.h"

static inline unsigned exofs_chunk_size(struct inode *inode)
{
	return inode->i_sb->s_blocksize;
}

static inline void exofs_put_page(struct page *page)
{
	kunmap(page);
	put_page(page);
}

static unsigned exofs_last_byte(struct inode *inode, unsigned long page_nr)
{
	loff_t last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static int exofs_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;

	inode_inc_iversion(dir);

	if (!PageUptodate(page))
		SetPageUptodate(page);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	set_page_dirty(page);

	if (IS_DIRSYNC(dir))
		err = write_one_page(page);
	else
		unlock_page(page);

	return err;
}

static bool exofs_check_page(struct page *page)
{
	struct inode *dir = page->mapping->host;
	unsigned chunk_size = exofs_chunk_size(dir);
	char *kaddr = page_address(page);
	unsigned offs, rec_len;
	unsigned limit = PAGE_SIZE;
	struct exofs_dir_entry *p;
	char *error;

	/* if the page is the last one in the directory */
	if ((dir->i_size >> PAGE_SHIFT) == page->index) {
		limit = dir->i_size & ~PAGE_MASK;
		if (limit & (chunk_size - 1))
			goto Ebadsize;
		if (!limit)
			goto out;
	}
	for (offs = 0; offs <= limit - EXOFS_DIR_REC_LEN(1); offs += rec_len) {
		p = (struct exofs_dir_entry *)(kaddr + offs);
		rec_len = le16_to_cpu(p->rec_len);

		if (rec_len < EXOFS_DIR_REC_LEN(1))
			goto Eshort;
		if (rec_len & 3)
			goto Ealign;
		if (rec_len < EXOFS_DIR_REC_LEN(p->name_len))
			goto Enamelen;
		if (((offs + rec_len - 1) ^ offs) & ~(chunk_size-1))
			goto Espan;
	}
	if (offs != limit)
		goto Eend;
out:
	SetPageChecked(page);
	return true;

Ebadsize:
	EXOFS_ERR("ERROR [exofs_check_page]: "
		"size of directory(0x%lx) is not a multiple of chunk size\n",
		dir->i_ino
	);
	goto fail;
Eshort:
	error = "rec_len is smaller than minimal";
	goto bad_entry;
Ealign:
	error = "unaligned directory entry";
	goto bad_entry;
Enamelen:
	error = "rec_len is too small for name_len";
	goto bad_entry;
Espan:
	error = "directory entry across blocks";
	goto bad_entry;
bad_entry:
	EXOFS_ERR(
		"ERROR [exofs_check_page]: bad entry in directory(0x%lx): %s - "
		"offset=%lu, inode=0x%llx, rec_len=%d, name_len=%d\n",
		dir->i_ino, error, (page->index<<PAGE_SHIFT)+offs,
		_LLU(le64_to_cpu(p->inode_no)),
		rec_len, p->name_len);
	goto fail;
Eend:
	p = (struct exofs_dir_entry *)(kaddr + offs);
	EXOFS_ERR("ERROR [exofs_check_page]: "
		"entry in directory(0x%lx) spans the page boundary"
		"offset=%lu, inode=0x%llx\n",
		dir->i_ino, (page->index<<PAGE_SHIFT)+offs,
		_LLU(le64_to_cpu(p->inode_no)));
fail:
	SetPageError(page);
	return false;
}

static struct page *exofs_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
	struct page *page = read_mapping_page(mapping, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
		if (unlikely(!PageChecked(page))) {
			if (PageError(page) || !exofs_check_page(page))
				goto fail;
		}
	}
	return page;

fail:
	exofs_put_page(page);
	return ERR_PTR(-EIO);
}

static inline int exofs_match(int len, const unsigned char *name,
					struct exofs_dir_entry *de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode_no)
		return 0;
	return !memcmp(name, de->name, len);
}

static inline
struct exofs_dir_entry *exofs_next_entry(struct exofs_dir_entry *p)
{
	return (struct exofs_dir_entry *)((char *)p + le16_to_cpu(p->rec_len));
}

static inline unsigned
exofs_validate_entry(char *base, unsigned offset, unsigned mask)
{
	struct exofs_dir_entry *de = (struct exofs_dir_entry *)(base + offset);
	struct exofs_dir_entry *p =
			(struct exofs_dir_entry *)(base + (offset&mask));
	while ((char *)p < (char *)de) {
		if (p->rec_len == 0)
			break;
		p = exofs_next_entry(p);
	}
	return (char *)p - base;
}

static unsigned char exofs_filetype_table[EXOFS_FT_MAX] = {
	[EXOFS_FT_UNKNOWN]	= DT_UNKNOWN,
	[EXOFS_FT_REG_FILE]	= DT_REG,
	[EXOFS_FT_DIR]		= DT_DIR,
	[EXOFS_FT_CHRDEV]	= DT_CHR,
	[EXOFS_FT_BLKDEV]	= DT_BLK,
	[EXOFS_FT_FIFO]		= DT_FIFO,
	[EXOFS_FT_SOCK]		= DT_SOCK,
	[EXOFS_FT_SYMLINK]	= DT_LNK,
};

#define S_SHIFT 12
static unsigned char exofs_type_by_mode[S_IFMT >> S_SHIFT] = {
	[S_IFREG >> S_SHIFT]	= EXOFS_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= EXOFS_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= EXOFS_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= EXOFS_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= EXOFS_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= EXOFS_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= EXOFS_FT_SYMLINK,
};

static inline
void exofs_set_de_type(struct exofs_dir_entry *de, struct inode *inode)
{
	umode_t mode = inode->i_mode;
	de->file_type = exofs_type_by_mode[(mode & S_IFMT) >> S_SHIFT];
}

static int exofs_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	unsigned chunk_mask = ~(exofs_chunk_size(inode)-1);
	bool need_revalidate = !inode_eq_iversion(inode, file->f_version);
    struct exofs_i_info* oi_dir = exofs_i(inode);
    struct osd_obj_id obj;
    struct exofs_fcb fcb;
    void *buf = NULL;
    size_t buf_len = 0;
    int err = 0;

	obj.partition = oi_dir->one_comp.obj.partition;
	obj.id = exofs_oi_objno(oi_dir);
	EXOFS_ERR("exofs_readdir: dir ino=%lx obj.id=0x%llx partition=0x%llx\n", inode->i_ino, obj.id, obj.partition);

	/* Read directory attributes for current size */
	err = exofs_get_obj_atribiute(&obj, &fcb);
	if (err) {
		EXOFS_ERR("exofs_readdir: exofs_get_obj_atribiute failed err=%d\n", err);
		return err;
	}

	buf_len = (size_t)le64_to_cpu(fcb.i_size);
	EXOFS_ERR("exofs_readdir: current dir size=%zu\n", buf_len);

    if (buf_len) {
		err = exofs_get_obj_data(&obj, &buf, (unsigned)buf_len);
		EXOFS_ERR("exofs_readdir: exofs_get_obj_data(len=%zu) -> %d buf=%p\n", buf_len, err, buf);
		if (err) {
			EXOFS_ERR("exofs_readdir: exofs_get_obj_data failed err=%d\n", err);
			return err;
		}
	}
	if (pos > inode->i_size - EXOFS_DIR_REC_LEN(1))
		return 0;


    if (buf && buf_len > 0) {
        char *kaddr = (char *)buf;
        char *limit = kaddr + buf_len - EXOFS_DIR_REC_LEN(1);
        struct exofs_dir_entry *de;
        unsigned int offset = (unsigned int)pos;  // Direct offset into buffer
        
        // Handle revalidation if needed
        if (unlikely(need_revalidate)) {
            if (offset) {
                offset = exofs_validate_entry(kaddr, offset, chunk_mask);
                ctx->pos = offset;
            }
            file->f_version = inode_query_iversion(inode);
            need_revalidate = false;
        }
        if (offset >= buf_len) {
            goto cleanup;
        }

		de = (struct exofs_dir_entry *)(kaddr + offset);
	
		for (; (char *)de <= limit; de = exofs_next_entry(de)) {
            if ((char *)de + sizeof(*de) > kaddr + buf_len) {
                EXOFS_ERR("ERROR: directory entry extends beyond buffer\n");
                break;
            }

        if (de->rec_len == 0) {
            EXOFS_ERR("ERROR: zero-length entry in directory(0x%lx)\n", inode->i_ino);
            err = -EIO;
            goto cleanup;
        }

        // Another bounds check for full entry
        if ((char *)de + le16_to_cpu(de->rec_len) > kaddr + buf_len) {
            EXOFS_ERR("ERROR: directory entry record extends beyond buffer\n");
            break;
        }
        
        if (de->inode_no) {
            unsigned char t;
            
            if (de->file_type < EXOFS_FT_MAX)
                t = exofs_filetype_table[de->file_type];
            else
                t = DT_UNKNOWN;
                
            if (!dir_emit(ctx, de->name, de->name_len,
                         le64_to_cpu(de->inode_no), t)) {
                goto cleanup;
            }
        }

			ctx->pos += le16_to_cpu(de->rec_len);
		}
    }

cleanup:
    if (buf) {
        kfree(buf);
    }
    return err;
}

struct exofs_dir_entry *exofs_find_entry(struct inode *dir,
			struct dentry *dentry, struct page **res_page)
{
	const unsigned char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned reclen = EXOFS_DIR_REC_LEN(namelen);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct exofs_i_info *oi = exofs_i(dir);
	struct exofs_dir_entry *de;

	if (npages == 0)
		goto out;

	*res_page = NULL;

	start = oi->i_dir_start_lookup;
	if (start >= npages)
		start = 0;
	n = start;
	do {
		char *kaddr;
		page = exofs_get_page(dir, n);
		if (!IS_ERR(page)) {
			kaddr = page_address(page);
			de = (struct exofs_dir_entry *) kaddr;
			kaddr += exofs_last_byte(dir, n) - reclen;
			while ((char *) de <= kaddr) {
				if (de->rec_len == 0) {
					EXOFS_ERR("ERROR: zero-length entry in "
						  "directory(0x%lx)\n",
						  dir->i_ino);
					exofs_put_page(page);
					goto out;
				}
				if (exofs_match(namelen, name, de))
					goto found;
				de = exofs_next_entry(de);
			}
			exofs_put_page(page);
		}
		if (++n >= npages)
			n = 0;
	} while (n != start);
out:
	return NULL;

found:
	*res_page = page;
	oi->i_dir_start_lookup = n;
	return de;
}

struct exofs_dir_entry *exofs_dotdot(struct inode *dir, struct page **p)
{
	struct page *page = exofs_get_page(dir, 0);
	struct exofs_dir_entry *de = NULL;

	if (!IS_ERR(page)) {
		de = exofs_next_entry(
				(struct exofs_dir_entry *)page_address(page));
		*p = page;
	}
	return de;
}

ino_t exofs_parent_ino(struct dentry *child)
{
	struct page *page;
	struct exofs_dir_entry *de;
	ino_t ino;

	de = exofs_dotdot(d_inode(child), &page);
	if (!de)
		return 0;

	ino = le64_to_cpu(de->inode_no);
	exofs_put_page(page);
	return ino;
}

ino_t exofs_inode_by_name(struct inode *dir, struct dentry *dentry)
{
	ino_t res = 0;
	struct exofs_dir_entry *de;
	struct page *page;

	de = exofs_find_entry(dir, dentry, &page);
	if (de) {
		res = le64_to_cpu(de->inode_no);
		exofs_put_page(page);
	}
	return res;
}

int exofs_set_link(struct inode *dir, struct exofs_dir_entry *de,
			struct page *page, struct inode *inode)
{
	loff_t pos = page_offset(page) +
			(char *) de - (char *) page_address(page);
	unsigned len = le16_to_cpu(de->rec_len);
	int err;

	lock_page(page);
	err = exofs_write_begin(NULL, page->mapping, pos, len, 0, &page, NULL);
	if (err)
		EXOFS_ERR("exofs_set_link: exofs_write_begin FAILED => %d\n",
			  err);

	de->inode_no = cpu_to_le64(inode->i_ino);
	exofs_set_de_type(de, inode);
	if (likely(!err))
		err = exofs_commit_chunk(page, pos, len);
	exofs_put_page(page);
	dir->i_mtime = dir->i_ctime = current_time(dir);
	mark_inode_dirty(dir);
	return err;
}

int exofs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const unsigned char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned chunk_size = exofs_chunk_size(dir);
	unsigned reclen = EXOFS_DIR_REC_LEN(namelen);
	struct exofs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct exofs_dir_entry *de;
	struct exofs_i_info *oi_dir = exofs_i(dir);
	struct osd_obj_id obj;
	struct exofs_fcb fcb;
	void *buf = NULL;
	size_t buf_len = 0;
	size_t new_len;
	size_t off;
	int err = 0;

	EXOFS_ERR("exofs_add_link: name %s, namelen %u\n", name, namelen);

	/* Build directory object identifier */
	obj.partition = oi_dir->one_comp.obj.partition;
	obj.id = exofs_oi_objno(oi_dir);
	EXOFS_ERR("exofs_add_link: dir ino=%lx obj.id=0x%llx partition=0x%llx\n", dir->i_ino, obj.id, obj.partition);

	/* Read directory attributes for current size */
	err = exofs_get_obj_atribiute(&obj, &fcb);
	if (err) {
		EXOFS_ERR("exofs_add_link: exofs_get_obj_atribiute failed err=%d\n", err);
		return err;
	}

	buf_len = (size_t)le64_to_cpu(fcb.i_size);
	EXOFS_ERR("exofs_add_link: current dir size=%zu chunk_size=%u\n", buf_len, chunk_size);

	if (buf_len) {
		err = exofs_get_obj_data(&obj, &buf, (unsigned)buf_len);
		EXOFS_ERR("exofs_add_link: exofs_get_obj_data(len=%zu) -> %d buf=%p\n", buf_len, err, buf);
		if (err) {
			EXOFS_ERR("exofs_add_link: exofs_get_obj_data failed err=%d\n", err);
			return err;
		}
	} else {
		/* Initialize empty directory chunk if no data exists */
		buf = kzalloc(chunk_size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		buf_len = chunk_size;
		de = (struct exofs_dir_entry *)buf;
		de->inode_no = 0;
		de->name_len = 0;
		de->rec_len = cpu_to_le16(chunk_size);
		EXOFS_ERR("exofs_add_link: initialized new dir chunk len=%u\n", chunk_size);
	}

	/* Scan for space or extend and retry */
restart_scan:
	EXOFS_ERR("exofs_add_link: scanning for space buf_len=%zu\n", buf_len);
	for (off = 0; off < buf_len; ) {
		unsigned short rec_len;
		unsigned short name_min;

		de = (struct exofs_dir_entry *)((char *)buf + off);
		rec_len = le16_to_cpu(de->rec_len);
		if (rec_len == 0) {
			EXOFS_ERR("exofs_add_link: ERROR zero rec_len at off=%zu\n", off);
			err = -EIO;
			goto out_free;
		}
		EXOFS_ERR("exofs_add_link: entry at off=%zu rec_len=%u name_len=%u inode_no=0x%llx\n",
				 off, rec_len, de->name_len, _LLU(le64_to_cpu(de->inode_no)));

		/* Ensure entry does not cross chunk boundary */
		if (((off + rec_len - 1) ^ off) & ~(chunk_size - 1)) {
			EXOFS_ERR("exofs_add_link: ERROR entry spans chunk boundary off=%zu rec_len=%u\n", off, rec_len);
			err = -EIO;
			goto out_free;
		}

		/* Duplicate name check */
		if (exofs_match(namelen, name, de)) {
			EXOFS_ERR("exofs_add_link: EEXIST matched existing name at off=%zu\n", off);
			err = -EEXIST;
			goto out_free;
		}

		name_min = EXOFS_DIR_REC_LEN(de->name_len);
		EXOFS_ERR("exofs_add_link: name_min=%u reclen=%u\n", name_min, reclen);

		/* Case 1: free record big enough */
		if (!de->inode_no && rec_len >= reclen) {
			struct exofs_dir_entry *new_de = de;
			unsigned short remain = rec_len - reclen;
			EXOFS_ERR("exofs_add_link: using free slot off=%zu remain=%u\n", off, remain);

			if (remain) {
				struct exofs_dir_entry *tail =
					(struct exofs_dir_entry *)((char *)new_de + reclen);
				tail->inode_no = 0;
				tail->name_len = 0;
				tail->rec_len = cpu_to_le16(remain);
			}
			new_de->rec_len = cpu_to_le16(reclen);
			new_de->name_len = namelen;
			memcpy(new_de->name, name, namelen);
			new_de->inode_no = cpu_to_le64(inode->i_ino);
			exofs_set_de_type(new_de, inode);
			goto write_back;
		}

		/* Case 2: used record has tail space */
		if (rec_len >= name_min + reclen) {
			struct exofs_dir_entry *free_de =
				(struct exofs_dir_entry *)((char *)de + name_min);
			unsigned short free_len = rec_len - name_min;
			EXOFS_ERR("exofs_add_link: splitting used entry off=%zu free_len=%u\n", off, free_len);

			de->rec_len = cpu_to_le16(name_min);

			if (free_len > reclen) {
				struct exofs_dir_entry *tail =
					(struct exofs_dir_entry *)((char *)free_de + reclen);
				tail->inode_no = 0;
				tail->name_len = 0;
				tail->rec_len = cpu_to_le16(free_len - reclen);
			}

			free_de->rec_len = cpu_to_le16(reclen);
			free_de->name_len = namelen;
			memcpy(free_de->name, name, namelen);
			free_de->inode_no = cpu_to_le64(inode->i_ino);
			exofs_set_de_type(free_de, inode);
			goto write_back;
		}

		off += rec_len;
	}

	/* No space found: extend directory by one chunk */
	{
		void *new_buf;
		size_t old_len = buf_len;

		new_buf = kzalloc(buf_len + chunk_size, GFP_KERNEL);
		if (!new_buf) {
			EXOFS_ERR("exofs_add_link: ENOMEM extending dir from %zu by %u\n", buf_len, chunk_size);
			err = -ENOMEM;
			goto out_free;
		}
		memcpy(new_buf, buf, buf_len);
		kfree(buf);
		buf = new_buf;
		buf_len += chunk_size;

		de = (struct exofs_dir_entry *)((char *)buf + old_len);
		de->inode_no = 0;
		de->name_len = 0;
		de->rec_len = cpu_to_le16(chunk_size);
		EXOFS_ERR("exofs_add_link: extended dir new_len=%zu\n", buf_len);
		goto restart_scan;
	}

write_back:
	/* Persist directory data; helper updates attribute size */
	new_len = buf_len;
	EXOFS_ERR("exofs_add_link: writing back new_len=%zu\n", new_len);
	err = exofs_set_obj_data(&obj, buf, (unsigned)new_len);
	EXOFS_ERR("exofs_add_link: exofs_set_obj_data -> %d\n", err);
	if (err)
		goto out_free;

	/* Revalidate and update inode metadata */
	inode_inc_iversion(dir);
	invalidate_inode_pages2(dir->i_mapping);
	if (new_len > i_size_read(dir))
		i_size_write(dir, new_len);
	dir->i_mtime = dir->i_ctime = current_time(dir);
	mark_inode_dirty(dir);
	sbi->s_numfiles++;
	EXOFS_ERR("exofs_add_link: success s_numfiles=%u\n", sbi->s_numfiles);

out_free:
	if (err)
		EXOFS_ERR("exofs_add_link: returning err=%d\n", err);
	kfree(buf);
	return err;
}

int exofs_delete_entry(struct exofs_dir_entry *dir, struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	struct exofs_sb_info *sbi = inode->i_sb->s_fs_info;
	char *kaddr = page_address(page);
	unsigned from = ((char *)dir - kaddr) & ~(exofs_chunk_size(inode)-1);
	unsigned to = ((char *)dir - kaddr) + le16_to_cpu(dir->rec_len);
	loff_t pos;
	struct exofs_dir_entry *pde = NULL;
	struct exofs_dir_entry *de = (struct exofs_dir_entry *) (kaddr + from);
	int err;

	while (de < dir) {
		if (de->rec_len == 0) {
			EXOFS_ERR("ERROR: exofs_delete_entry:"
				  "zero-length entry in directory(0x%lx)\n",
				  inode->i_ino);
			err = -EIO;
			goto out;
		}
		pde = de;
		de = exofs_next_entry(de);
	}
	if (pde)
		from = (char *)pde - (char *)page_address(page);
	pos = page_offset(page) + from;
	lock_page(page);
	err = exofs_write_begin(NULL, page->mapping, pos, to - from, 0,
							&page, NULL);
	if (err)
		EXOFS_ERR("exofs_delete_entry: exofs_write_begin FAILED => %d\n",
			  err);
	if (pde)
		pde->rec_len = cpu_to_le16(to - from);
	dir->inode_no = 0;
	if (likely(!err))
		err = exofs_commit_chunk(page, pos, to - from);
	inode->i_ctime = inode->i_mtime = current_time(inode);
	mark_inode_dirty(inode);
	sbi->s_numfiles--;
out:
	exofs_put_page(page);
	return err;
}

/* kept aligned on 4 bytes */
#define THIS_DIR ".\0\0"
#define PARENT_DIR "..\0"

int exofs_make_empty(struct inode *inode, struct inode *parent)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	unsigned chunk_size = exofs_chunk_size(inode);
	struct exofs_dir_entry *de;
	int err;
	void *kaddr;

	if (!page)
		return -ENOMEM;

	err = exofs_write_begin(NULL, page->mapping, 0, chunk_size, 0,
							&page, NULL);
	if (err) {
		unlock_page(page);
		goto fail;
	}

	kaddr = kmap_atomic(page);
	de = (struct exofs_dir_entry *)kaddr;
	de->name_len = 1;
	de->rec_len = cpu_to_le16(EXOFS_DIR_REC_LEN(1));
	memcpy(de->name, THIS_DIR, sizeof(THIS_DIR));
	de->inode_no = cpu_to_le64(inode->i_ino);
	exofs_set_de_type(de, inode);

	de = (struct exofs_dir_entry *)(kaddr + EXOFS_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = cpu_to_le16(chunk_size - EXOFS_DIR_REC_LEN(1));
	de->inode_no = cpu_to_le64(parent->i_ino);
	memcpy(de->name, PARENT_DIR, sizeof(PARENT_DIR));
	exofs_set_de_type(de, inode);
	kunmap_atomic(kaddr);
	err = exofs_commit_chunk(page, 0, chunk_size);
fail:
	put_page(page);
	return err;
}

int exofs_empty_dir(struct inode *inode)
{
	struct page *page = NULL;
	unsigned long i, npages = dir_pages(inode);

	for (i = 0; i < npages; i++) {
		char *kaddr;
		struct exofs_dir_entry *de;
		page = exofs_get_page(inode, i);

		if (IS_ERR(page))
			continue;

		kaddr = page_address(page);
		de = (struct exofs_dir_entry *)kaddr;
		kaddr += exofs_last_byte(inode, i) - EXOFS_DIR_REC_LEN(1);

		while ((char *)de <= kaddr) {
			if (de->rec_len == 0) {
				EXOFS_ERR("ERROR: exofs_empty_dir: "
					  "zero-length directory entry"
					  "kaddr=%p, de=%p\n", kaddr, de);
				goto not_empty;
			}
			if (de->inode_no != 0) {
				/* check for . and .. */
				if (de->name[0] != '.')
					goto not_empty;
				if (de->name_len > 2)
					goto not_empty;
				if (de->name_len < 2) {
					if (le64_to_cpu(de->inode_no) !=
					    inode->i_ino)
						goto not_empty;
				} else if (de->name[1] != '.')
					goto not_empty;
			}
			de = exofs_next_entry(de);
		}
		exofs_put_page(page);
	}
	return 1;

not_empty:
	exofs_put_page(page);
	return 0;
}

const struct file_operations exofs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= exofs_readdir,
};
