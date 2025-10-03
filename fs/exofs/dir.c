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
		if (limit & (chunk_size - 1) && false)
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
	if (offs != limit && false)
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
	obj.id = inode->i_ino;
	EXOFS_ERR("exofs_readdir: dir ino=%lx, obj.id=0x%llx, partition=0x%llx\n",
		inode->i_ino, obj.id, obj.partition);

	/* Read directory attributes for current size */
	err = exofs_get_obj_atribiute(&obj, &fcb);
	if (err) {
		EXOFS_ERR("exofs_readdir: exofs_get_obj_atribiute failed err=%d\n", err);
		return err;
	}

	buf_len = (size_t)le64_to_cpu(fcb.i_size);
	EXOFS_ERR("exofs_readdir: current dir size=%zu\n", buf_len);

	if (buf_len) {
		err = exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)buf_len);
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
		unsigned int offset = (unsigned int)pos;

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
			unsigned cur_offset = (char *)de - kaddr;

			// Bounds check
			if ((char *)de + sizeof(*de) > kaddr + buf_len) {
				EXOFS_ERR("ERROR: directory entry extends beyond buffer\n");
				break;
			}

			if (de->rec_len == 0) {
				EXOFS_ERR("ERROR: zero-length entry in directory(0x%lx)\n", inode->i_ino);
				err = -EIO;
				goto cleanup;
			}

			// Log EVERY entry
			EXOFS_ERR("readdir: off=%u rec_len=%u name_len=%u inode=%llx name='%.*s'\n",
				cur_offset, le16_to_cpu(de->rec_len), de->name_len,
				le64_to_cpu(de->inode_no),
				de->name_len, de->name);

			// Another bounds check for full entry
			if ((char *)de + le16_to_cpu(de->rec_len) > kaddr + buf_len) {
				EXOFS_ERR("ERROR: directory entry record extends beyond buffer\n");
				break;
			}

			if (de->inode_no) {
				unsigned char t;

				EXOFS_ERR("readdir: EMITTING entry '%.*s' inode=%llx\n",
					de->name_len, de->name, le64_to_cpu(de->inode_no));

				if (de->file_type < EXOFS_FT_MAX)
					t = exofs_filetype_table[de->file_type];
				else
					t = DT_UNKNOWN;

				if (!dir_emit(ctx, de->name, de->name_len,
					le64_to_cpu(de->inode_no), t)) {
					goto cleanup;
				}
			} else {
				EXOFS_ERR("readdir: SKIPPING deleted entry (inode_no=0) name_len=%u\n",
					de->name_len);
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
                                          struct dentry *dentry,
                                          struct exofs_dir_search_result *result)
{
    const unsigned char *name = dentry->d_name.name;
    int namelen = dentry->d_name.len;
    unsigned reclen = EXOFS_DIR_REC_LEN(namelen);
    struct exofs_i_info *oi_dir = exofs_i(dir);
    struct exofs_dir_entry *de;
    struct osd_obj_id obj;
    struct exofs_fcb fcb;
    char *buf = NULL;
    size_t buf_len;
    char *kaddr;
    char *limit;
    int err;

    // Initialize result
    memset(result, 0, sizeof(*result));

    // Setup object ID for directory
    obj.partition = oi_dir->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi_dir);

    EXOFS_ERR("exofs_find_entry: searching for '%.*s' in dir ino=%lx\n",
              namelen, name, dir->i_ino);

    // Read directory attributes for current size
    err = exofs_get_obj_atribiute(&obj, &fcb);
    if (err) {
        EXOFS_ERR("exofs_find_entry: exofs_get_obj_atribiute failed err=%d\n", err);
        return NULL;
    }

    buf_len = (size_t)le64_to_cpu(fcb.i_size);
    if (!buf_len) {
        EXOFS_ERR("exofs_find_entry: directory is empty\n");
        return NULL;
    }

    // Read entire directory data
    err = exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)buf_len);
    if (err) {
        EXOFS_ERR("exofs_find_entry: exofs_get_obj_data failed err=%d\n", err);
        return NULL;
    }

    kaddr = buf;
    limit = kaddr + buf_len - reclen;
    de = (struct exofs_dir_entry *)kaddr;

    // Search through all directory entries
    while ((char *)de <= limit) {
        // Bounds check
        if ((char *)de + sizeof(*de) > kaddr + buf_len) {
            EXOFS_ERR("exofs_find_entry: entry extends beyond buffer\n");
            break;
        }

        if (de->rec_len == 0) {
            EXOFS_ERR("ERROR: zero-length entry in directory(0x%lx)\n",
                     dir->i_ino);
            goto out_free;
        }

        // Check for buffer overflow
        if ((char *)de + le16_to_cpu(de->rec_len) > kaddr + buf_len) {
            EXOFS_ERR("exofs_find_entry: rec_len extends beyond buffer\n");
            break;
        }

        // Check if this is the entry we're looking for
        if (exofs_match(namelen, name, de)) {
            EXOFS_ERR("exofs_find_entry: found '%.*s' at offset %ld\n",
                     namelen, name, (char *)de - kaddr);

            // Fill in result structure
            result->de = de;
            result->buf = buf;
            result->buf_len = buf_len;
            result->offset = (char *)de - kaddr;

            return de;  // Found it!
        }

        de = exofs_next_entry(de);
    }

    EXOFS_ERR("exofs_find_entry: entry '%.*s' not found\n", namelen, name);

out_free:
    kfree(buf);
    return NULL;
}

struct exofs_dir_entry *exofs_dotdot(struct inode *dir,
                                      struct exofs_dir_search_result *result)
{
    struct exofs_i_info *oi_dir = exofs_i(dir);
    struct osd_obj_id obj;
    struct exofs_fcb fcb;
    char *buf = NULL;
    size_t buf_len;
    struct exofs_dir_entry *de = NULL;
    struct exofs_dir_entry *dotdot_de = NULL;
    int err;

    // Initialize result
    memset(result, 0, sizeof(*result));

    // Setup object ID for directory
    obj.partition = oi_dir->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi_dir);

    EXOFS_ERR("exofs_dotdot: getting '..' for dir ino=%lx\n", dir->i_ino);

    // Read directory attributes
    err = exofs_get_obj_atribiute(&obj, &fcb);
    if (err) {
        EXOFS_ERR("exofs_dotdot: exofs_get_obj_atribiute failed err=%d\n", err);
        return NULL;
    }

    buf_len = (size_t)le64_to_cpu(fcb.i_size);
    if (!buf_len) {
        EXOFS_ERR("exofs_dotdot: directory is empty\n");
        return NULL;
    }

    // Read directory data
    err = exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)buf_len);
    if (err) {
        EXOFS_ERR("exofs_dotdot: exofs_get_obj_data failed err=%d\n", err);
        return NULL;
    }

    // First entry should be "."
    de = (struct exofs_dir_entry *)buf;

    // Validate first entry
    if (de->rec_len == 0) {
        EXOFS_ERR("exofs_dotdot: invalid first entry (zero rec_len)\n");
        kfree(buf);
        return NULL;
    }

    // Second entry is ".."
    dotdot_de = exofs_next_entry(de);

    // Bounds check
    if ((char *)dotdot_de >= buf + buf_len) {
        EXOFS_ERR("exofs_dotdot: '..' entry beyond buffer bounds\n");
        kfree(buf);
        return NULL;
    }

    // Validate ".." entry
    if (dotdot_de->rec_len == 0) {
        EXOFS_ERR("exofs_dotdot: invalid '..' entry (zero rec_len)\n");
        kfree(buf);
        return NULL;
    }

    EXOFS_ERR("exofs_dotdot: found '..' at offset %ld, inode=%llu\n",
             (char *)dotdot_de - buf, le64_to_cpu(dotdot_de->inode_no));

    // Fill result structure
    result->de = dotdot_de;
    result->buf = buf;
    result->buf_len = buf_len;
    result->offset = (char *)dotdot_de - buf;

    return dotdot_de;
}

ino_t exofs_parent_ino(struct dentry *child)
{
    struct exofs_dir_search_result search_result;
    struct exofs_dir_entry *de;
    ino_t ino;

    EXOFS_ERR("exofs_parent_ino: getting parent ino for child ino=%lu\n",
              d_inode(child)->i_ino);

    de = exofs_dotdot(d_inode(child), &search_result);
    if (!de) {
        EXOFS_ERR("exofs_parent_ino: dotdot not found\n");
        return 0;
    }

    ino = le64_to_cpu(de->inode_no);

    EXOFS_ERR("exofs_parent_ino: parent ino=%lu\n", ino);

    /* Release the search result buffer */
    exofs_release_search_result(&search_result);

    return ino;
}

static ino_t exofs_inode_by_name_nvme(struct inode *dir, const unsigned char *name, int namelen)
{
	struct exofs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct osd_obj_id obj;
	struct exofs_fcb fcb;
	void *buf = NULL;
	size_t size;
	size_t off = 0;
	ino_t res = 0;

	obj.partition = sbi->one_comp.obj.partition;
	obj.id = dir->i_ino;
	/* on-disk object id is inode->i_ino */
	if (!obj.id)
		return 0;

	if (exofs_get_obj_atribiute(&obj, &fcb))
		return 0;

	size = (size_t)le64_to_cpu(fcb.i_size);
	if (!size)
		return 0;
	if (exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)size))
		return 0;

	while (off + EXOFS_DIR_REC_LEN(1) <= size) {
		struct exofs_dir_entry *de = (struct exofs_dir_entry *)((char *)buf + off);
		unsigned short rec_len = le16_to_cpu(de->rec_len);
		if (!rec_len)
			break;
		if (rec_len & 3)
			break;
		if (rec_len < EXOFS_DIR_REC_LEN(1))
			break;
		if (rec_len < EXOFS_DIR_REC_LEN(de->name_len))
			break;

		if (de->inode_no && de->name_len == namelen &&
		    !memcmp(de->name, name, namelen)) {
			res = (ino_t)le64_to_cpu(de->inode_no);
			break;
		}
		off += rec_len;
	}
	kfree(buf);
	return res;
}

ino_t exofs_inode_by_name(struct inode *dir, struct dentry *dentry)
{
    ino_t res = 0;

    EXOFS_ERR("exofs_inode_by_name: name '%s', namelen %u\n",
              dentry->d_name.name, dentry->d_name.len);

    /* Primary path: Use NVMe direct scan if available */
    res = exofs_inode_by_name_nvme(dir, dentry->d_name.name, dentry->d_name.len);
    if (res) {
        EXOFS_ERR("exofs_inode_by_name: found via NVMe scan, ino=%lu\n", res);
        return res;
    }

    /* Fallback: Use NVMe KV-based find_entry */
    {
        struct exofs_dir_entry *de;
        struct exofs_dir_search_result search_result;

        de = exofs_find_entry(dir, dentry, &search_result);
        if (de) {
            res = le64_to_cpu(de->inode_no);
            EXOFS_ERR("exofs_inode_by_name: found via find_entry, ino=%lu\n", res);

            /* Release the search result buffer */
            exofs_release_search_result(&search_result);
        } else {
            EXOFS_ERR("exofs_inode_by_name: entry not found\n");
        }
    }

    return res;
}

int exofs_set_link(struct inode *dir,
                   struct exofs_dir_entry *de,
                   struct exofs_dir_search_result *search_result,
                   struct inode *inode)
{
    struct exofs_i_info *oi_dir = exofs_i(dir);
    struct osd_obj_id obj;
    char *buf = search_result->buf;
    size_t buf_len = search_result->buf_len;
    unsigned len = le16_to_cpu(de->rec_len);
    int err;

    // Setup object ID for directory
    obj.partition = oi_dir->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi_dir);

    EXOFS_ERR("exofs_set_link: updating entry in dir ino=%lx to point to ino=%lx\n",
             dir->i_ino, inode->i_ino);

    // Verify the entry is within bounds
    if ((char *)de < buf || (char *)de >= buf + buf_len) {
        EXOFS_ERR("exofs_set_link: entry pointer out of bounds\n");
        return -EINVAL;
    }

    if ((char *)de + len > buf + buf_len) {
        EXOFS_ERR("exofs_set_link: entry extends beyond buffer\n");
        return -EINVAL;
    }

    // Update the directory entry
    de->inode_no = cpu_to_le64(inode->i_ino);
    exofs_set_de_type(de, inode);

    EXOFS_ERR("exofs_set_link: updated entry '%.*s' to inode %llu (type=%u)\n",
             de->name_len, de->name, le64_to_cpu(de->inode_no), de->file_type);

    // Write modified directory back to storage
    err = exofs_set_obj_data(&obj, buf, (unsigned)buf_len);
    if (err) {
        EXOFS_ERR("exofs_set_link: exofs_set_obj_data failed err=%d\n", err);
        return err;
    }

    // Update directory timestamps
    dir->i_mtime = dir->i_ctime = current_time(dir);
    mark_inode_dirty(dir);

    EXOFS_ERR("exofs_set_link: successfully updated link\n");
    return 0;
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
	obj.id = dir->i_ino;
	EXOFS_ERR("exofs_add_link: dir ino=%lx, obj.id=0x%llx, partition=0x%llx\n", 
		dir->i_ino, obj.id, obj.partition);

	/* Read directory attributes for current size */
	err = exofs_get_obj_atribiute(&obj, &fcb);
	if (err) {
		EXOFS_ERR("exofs_add_link: exofs_get_obj_atribiute failed err=%d\n", err);
		return err;
	}

	buf_len = (size_t)le64_to_cpu(fcb.i_size);
	EXOFS_ERR("exofs_add_link: current dir size=%zu chunk_size=%u\n", buf_len, chunk_size);

	if (buf_len) {
		err = exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)buf_len);
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

            memset(new_de, 0, rec_len);
			EXOFS_ERR("exofs_add_link: using free slot off=%zu remain=%u\n", off, remain);

            if (remain >= EXOFS_DIR_REC_LEN(0)) {
                struct exofs_dir_entry *tail =
                    (struct exofs_dir_entry *)((char *)new_de + reclen);
                tail->inode_no = 0;
                tail->name_len = 0;
                tail->rec_len = cpu_to_le16(remain);
            } else {
                // Absorb tiny remaining space into this entry
                reclen = rec_len;
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

            memset(free_de, 0, free_len);

			if (free_len > reclen && (free_len - reclen) >= EXOFS_DIR_REC_LEN(0)) {
                struct exofs_dir_entry *tail =
                    (struct exofs_dir_entry *)((char *)free_de + reclen);
                tail->inode_no = 0;
                tail->name_len = 0;
                tail->rec_len = cpu_to_le16(free_len - reclen);
            } else {
                // Absorb remaining space
                reclen = free_len;
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

int exofs_delete_entry(struct exofs_dir_entry *dir, 
                        struct inode *inode,
                        struct exofs_dir_search_result *search_result)
{
    struct exofs_sb_info *sbi = inode->i_sb->s_fs_info;
    struct exofs_i_info *oi_dir = exofs_i(inode);
    struct osd_obj_id obj;
    char *buf = search_result->buf;
    size_t buf_len = search_result->buf_len;
    unsigned offset = search_result->offset;
    unsigned chunk_mask = ~(exofs_chunk_size(inode) - 1);
    unsigned from, to;
    struct exofs_dir_entry *pde = NULL;
    struct exofs_dir_entry *de;
    char *kaddr = buf;
    int err = 0;

    // Setup object ID for directory
    obj.partition = oi_dir->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi_dir);

    EXOFS_ERR("exofs_delete_entry: dir ino=%lx offset=%u\n", 
              inode->i_ino, offset);

    EXOFS_ERR("exofs_delete_entry: marking entry at offset=%u as deleted (rec_len=%u)\n",
             offset, le16_to_cpu(dir->rec_len));

    // Mark entry as deleted
    dir->inode_no = 0;

    // Write modified directory back to storage
    err = exofs_set_obj_data(&obj, buf, (unsigned)buf_len);
    if (err) {
        EXOFS_ERR("exofs_delete_entry: exofs_set_obj_data failed err=%d\n", err);
        return err;
    }

    // Update directory metadata
    inode->i_ctime = inode->i_mtime = current_time(inode);
    mark_inode_dirty(inode);
    sbi->s_numfiles--;

    EXOFS_ERR("exofs_delete_entry: successfully deleted entry\n");
    return 0;
}

/* kept aligned on 4 bytes */
#define THIS_DIR ".\0\0"
#define PARENT_DIR "..\0"


int exofs_make_empty(struct inode *inode, struct inode *parent)
{
    struct exofs_i_info *oi = exofs_i(inode);
    struct osd_obj_id obj;
    unsigned chunk_size = exofs_chunk_size(inode);
    struct exofs_dir_entry *de;
    char *buf;
    int err;

    EXOFS_ERR("exofs_make_empty: creating empty dir for ino=%lu, parent=%lu\n",
              inode->i_ino, parent->i_ino);

    // Allocate buffer for directory data (one chunk)
    buf = kzalloc(chunk_size, GFP_KERNEL);
    if (!buf) {
        EXOFS_ERR("exofs_make_empty: failed to allocate buffer\n");
        return -ENOMEM;
    }

    // Setup object ID for the new directory
    obj.partition = oi->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi);

    // Create "." entry (current directory)
    de = (struct exofs_dir_entry *)buf;
    de->name_len = 1;
    de->rec_len = cpu_to_le16(EXOFS_DIR_REC_LEN(1));
    memcpy(de->name, ".", 1);
    de->inode_no = cpu_to_le64(inode->i_ino);
    exofs_set_de_type(de, inode);

    EXOFS_ERR("exofs_make_empty: created '.' entry, rec_len=%u\n",
              le16_to_cpu(de->rec_len));

    // Create ".." entry (parent directory)
    de = (struct exofs_dir_entry *)(buf + EXOFS_DIR_REC_LEN(1));
    de->name_len = 2;
    de->rec_len = cpu_to_le16(chunk_size - EXOFS_DIR_REC_LEN(1));
    de->inode_no = cpu_to_le64(parent->i_ino);
    memcpy(de->name, "..", 2);
    exofs_set_de_type(de, inode);

    EXOFS_ERR("exofs_make_empty: created '..' entry, rec_len=%u\n",
              le16_to_cpu(de->rec_len));

    // Write the directory data to NVMe KV
    err = exofs_set_obj_data(&obj, buf, chunk_size);
    if (err) {
        EXOFS_ERR("exofs_make_empty: exofs_set_obj_data failed err=%d\n", err);
        goto fail;
    }

    // Update inode size to reflect the directory content
    inode->i_size = chunk_size;
    mark_inode_dirty(inode);

    EXOFS_ERR("exofs_make_empty: successfully created empty directory\n");

fail:
    kfree(buf);
    return err;
}

int exofs_empty_dir(struct inode *inode)
{
    struct exofs_i_info *oi_dir = exofs_i(inode);
    struct osd_obj_id obj;
    struct exofs_fcb fcb;
    char *buf = NULL;
    size_t buf_len;
    char *kaddr;
    char *limit;
    struct exofs_dir_entry *de;
    int err;
    int result = 1;  // Assume empty until proven otherwise

    // Setup object ID for directory
    obj.partition = oi_dir->one_comp.obj.partition;
    obj.id = exofs_oi_objno(oi_dir);

    EXOFS_ERR("exofs_empty_dir: checking if dir ino=%lx is empty\n", inode->i_ino);

    // Read directory attributes
    err = exofs_get_obj_atribiute(&obj, &fcb);
    if (err) {
        EXOFS_ERR("exofs_empty_dir: exofs_get_obj_atribiute failed err=%d\n", err);
        return 1;  // Treat error as empty (safer for unlink operations)
    }

    buf_len = (size_t)le64_to_cpu(fcb.i_size);
    if (!buf_len) {
        EXOFS_ERR("exofs_empty_dir: directory size is 0, empty\n");
        return 1;  // Empty directory
    }

    // Read directory data
    err = exofs_get_obj_data(&obj, (void**)(&buf), (unsigned)buf_len);
    if (err) {
        EXOFS_ERR("exofs_empty_dir: exofs_get_obj_data failed err=%d\n", err);
        return 1;  // Treat error as empty
    }

    kaddr = buf;
    limit = kaddr + buf_len - EXOFS_DIR_REC_LEN(1);
    de = (struct exofs_dir_entry *)kaddr;

    // Iterate through all directory entries
    while ((char *)de <= limit) {
        // Bounds check
        if ((char *)de + sizeof(*de) > kaddr + buf_len) {
            EXOFS_ERR("exofs_empty_dir: entry extends beyond buffer\n");
            break;
        }

        if (de->rec_len == 0) {
            EXOFS_ERR("ERROR: exofs_empty_dir: "
                     "zero-length directory entry kaddr=%p, de=%p\n", 
                     kaddr, de);
            result = 0;  // Not empty (or corrupted)
            goto out;
        }

        // Check for entry overflow
        if ((char *)de + le16_to_cpu(de->rec_len) > kaddr + buf_len) {
            EXOFS_ERR("exofs_empty_dir: rec_len extends beyond buffer\n");
            break;
        }

        // Check if entry is in use
        if (de->inode_no != 0) {
            // Check for "." and ".." entries (these are allowed)
            if (de->name[0] != '.') {
                EXOFS_ERR("exofs_empty_dir: found non-dot entry '%.*s'\n",
                         de->name_len, de->name);
                result = 0;  // Not empty
                goto out;
            }

            if (de->name_len > 2) {
                EXOFS_ERR("exofs_empty_dir: found long dot-entry '%.*s'\n",
                         de->name_len, de->name);
                result = 0;  // Not empty
                goto out;
            }

            if (de->name_len < 2) {
                // This is "." - verify it points to this inode
                if (le64_to_cpu(de->inode_no) != inode->i_ino) {
                    EXOFS_ERR("exofs_empty_dir: '.' points to wrong inode\n");
                    result = 0;  // Not empty (or corrupted)
                    goto out;
                }
            } else if (de->name[1] != '.') {
                // Name starts with '.' but second char is not '.'
                EXOFS_ERR("exofs_empty_dir: found entry '.%c...'\n", de->name[1]);
                result = 0;  // Not empty
                goto out;
            }
            // If we get here, it's either "." or ".." which are OK
        }

        de = exofs_next_entry(de);
    }

    EXOFS_ERR("exofs_empty_dir: directory is empty (only . and .. found)\n");

out:
    kfree(buf);
    return result;
}

const struct file_operations exofs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= exofs_readdir,
};
