/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 *
 * Architecture independence:
 *   Copyright (c) 2005, Bull S.A.
 *   Written by Pierre Peiffer <pierre.peiffer@bull.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */

/*
 * Extents support for EXT4
 *
 * TODO:
 *   - ext4*_error() should be used in some situations
 *   - analyze all BUG()/BUG_ON(), use -EIO where appropriate
 *   - smart tree reduction
 */

#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd2.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/falloc.h>
#include <asm/uaccess.h>
#include <linux/fiemap.h>
#include "ext4_jbd2.h"
#include "ext4_extents.h"
#include "xattr.h"

#include <trace/events/ext4.h>

/*
 * used by extent splitting.
 */
//zeroout����һ���ò��������������ext4_extent���ڴ治��ָ�ʧ�ܺ�ָ�֮��
#define EXT4_EXT_MAY_ZEROOUT	0x1  /* safe to zeroout if split fails \
					due to ENOSPC */
#define EXT4_EXT_MARK_UNINIT1	0x2  /* mark first half uninitialized *///��Ƿָ��ĵ�1��δ��ʼ��״̬
#define EXT4_EXT_MARK_UNINIT2	0x4  /* mark second half uninitialized *///��Ƿָ��ĵ�2��δ��ʼ��״̬

#define EXT4_EXT_DATA_VALID1	0x8  /* first half contains valid data */
#define EXT4_EXT_DATA_VALID2	0x10 /* second half contains valid data */

static __le32 ext4_extent_block_csum(struct inode *inode,
				     struct ext4_extent_header *eh)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	__u32 csum;

	csum = ext4_chksum(sbi, ei->i_csum_seed, (__u8 *)eh,
			   EXT4_EXTENT_TAIL_OFFSET(eh));
	return cpu_to_le32(csum);
}

static int ext4_extent_block_csum_verify(struct inode *inode,
					 struct ext4_extent_header *eh)
{
	struct ext4_extent_tail *et;

	if (!EXT4_HAS_RO_COMPAT_FEATURE(inode->i_sb,
		EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 1;

	et = find_ext4_extent_tail(eh);
	if (et->et_checksum != ext4_extent_block_csum(inode, eh))
		return 0;
	return 1;
}

static void ext4_extent_block_csum_set(struct inode *inode,
				       struct ext4_extent_header *eh)
{
	struct ext4_extent_tail *et;

	if (!EXT4_HAS_RO_COMPAT_FEATURE(inode->i_sb,
		EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
		return;

	et = find_ext4_extent_tail(eh);
	et->et_checksum = ext4_extent_block_csum(inode, eh);
}

static int ext4_split_extent(handle_t *handle,
				struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_map_blocks *map,
				int split_flag,
				int flags);

static int ext4_split_extent_at(handle_t *handle,
			     struct inode *inode,
			     struct ext4_ext_path *path,
			     ext4_lblk_t split,
			     int split_flag,
			     int flags);

static int ext4_find_delayed_extent(struct inode *inode,
				    struct extent_status *newes);

static int ext4_ext_truncate_extend_restart(handle_t *handle,
					    struct inode *inode,
					    int needed)
{
	int err;

	if (!ext4_handle_valid(handle))
		return 0;
	if (handle->h_buffer_credits > needed)
		return 0;
	err = ext4_journal_extend(handle, needed);
	if (err <= 0)
		return err;
	err = ext4_truncate_restart_trans(handle, inode, needed);
	if (err == 0)
		err = -EAGAIN;

	return err;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 */
static int ext4_ext_get_access(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	if (path->p_bh) {
		/* path points to block */
		return ext4_journal_get_write_access(handle, path->p_bh);
	}
	/* path points to leaf/index in inode body */
	/* we use in-core data, no need to protect them */
	return 0;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 *  - EIO
 */
//ext4_extentӳ����߼��鷶Χ���ܷ����仯�ˣ���Ƕ�Ӧ�������ӳ���bh�����ļ�inode��.
//Ϊʲôext4_extent�仯��Ӱ�쵽�����ӳ���bh�����ļ�inode����?
int __ext4_ext_dirty(const char *where, unsigned int line, handle_t *handle,
		     struct inode *inode, struct ext4_ext_path *path)
{
	int err;
	if (path->p_bh) {
		ext4_extent_block_csum_set(inode, ext_block_hdr(path->p_bh));
		/* path points to block */
		err = __ext4_handle_dirty_metadata(where, line, handle,
						   inode, path->p_bh);
	} else {
		/* path points to leaf/index in inode body */
		err = ext4_mark_inode_dirty(handle, inode);
	}
	return err;
}
//�ҵ�map->m_lblk����ex->ee_blockӳ���������ַ������
static ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
			      struct ext4_ext_path *path,
			      ext4_lblk_t block)//block��map->m_lblk����ex->ee_block
{
	if (path) {
		int depth = path->p_depth;
		struct ext4_extent *ex;

		/*
		 * Try to predict block placement assuming that we are
		 * filling in a file which will eventually be
		 * non-sparse --- i.e., in the case of libbfd writing
		 * an ELF object sections out-of-order but in a way
		 * the eventually results in a contiguous object or
		 * executable file, or some database extending a table
		 * space file.  However, this is actually somewhat
		 * non-ideal if we are writing a sparse file such as
		 * qemu or KVM writing a raw image file that is going
		 * to stay fairly sparse, since it will end up
		 * fragmenting the file system's free space.  Maybe we
		 * should have some hueristics or some way to allow
		 * userspace to pass a hint to file system,
		 * especially if the latter case turns out to be
		 * common.
		 */
		ex = path[depth].p_ext;
		if (ex) {
            //ex����ʼ������ַ
			ext4_fsblk_t ext_pblk = ext4_ext_pblock(ex);
            //ex����ʼ�߼����ַ
			ext4_lblk_t ext_block = le32_to_cpu(ex->ee_block);

            //blockӳ���������ַ=ex����ʼ������ַ + (ex����ʼ�߼����ַ��block�Ĳ�ֵ)
			if (block > ext_block)
				return ext_pblk + (block - ext_block);
			else
				return ext_pblk - (ext_block - block);
		}

		/* it looks like index is empty;
		 * try to find starting block from index itself */
		//???????????????????
		if (path[depth].p_bh)
			return path[depth].p_bh->b_blocknr;
	}

	/* OK. use inode's group */
    //ҪΪ�ļ�inode���䱣�����ݵ�������ˣ��ú����Ǵ�inode������������һ������Ŀ�������飬�������������鿪ʼ���������ղ��ұ���Ҫ����������
	return ext4_inode_to_goal_block(inode);
}

/*
 * Allocation for a meta data block
 */
//��ext4�ļ�ϵͳԪ����������һ������飬�������������š�Ӧ����4K��С������ext4 extent B+�������ڵ����Ҷ�ӽ���N��ext4_extent_idx��ext4_extent�ṹ
static ext4_fsblk_t
ext4_ext_new_meta_block(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path,
			struct ext4_extent *ex, int *err, unsigned int flags)
{
	ext4_fsblk_t goal, newblock;
    //�ҵ�ex->ee_blockӳ���������ַ�����ظ�goal����ֻ�Ǹ��ο���Ŀ��������
	goal = ext4_ext_find_goal(inode, path, le32_to_cpu(ex->ee_block));
    //��goal��ΪĿ��������ַ�������Ĵ�ext4 �ļ�ϵͳ����һ������飬�����ĵ�ַ��newblock��newblock��goal���Ǵ��������ţ�
    //������ʱ��ȣ���ʱ����ȡ�
	newblock = ext4_new_meta_blocks(handle, inode, goal, flags,
					NULL, err);
	return newblock;
}

static inline int ext4_ext_space_block(struct inode *inode, int check)
{
	int size;
    //���Ǽ���һ��4K��С�������������ɶ��ٸ�ext4_extent�ṹ����Ȼ��Ҫ����ext4 extent B+��Ҷ�ӽڵ�ͷext4_extent_header
	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 6)
		size = 6;
#endif
	return size;
}

static inline int ext4_ext_space_block_idx(struct inode *inode, int check)
{
	int size;
    //���Ǽ���һ��4K��С�������������ɶ��ٸ�ext4_extent_idx�ṹ����Ȼ��Ҫ����ext4 extent B+�������ڵ�ͷext4_extent_header
	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 5)
		size = 5;
#endif
	return size;
}

static inline int ext4_ext_space_root(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 3)
		size = 3;
#endif
	return size;
}

static inline int ext4_ext_space_root_idx(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 4)
		size = 4;
#endif
	return size;
}

/*
 * Calculate the number of metadata blocks needed
 * to allocate @blocks
 * Worse case is one block per extent
 */
int ext4_ext_calc_metadata_amount(struct inode *inode, ext4_lblk_t lblock)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	int idxs;

	idxs = ((inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
		/ sizeof(struct ext4_extent_idx));

	/*
	 * If the new delayed allocation block is contiguous with the
	 * previous da block, it can share index blocks with the
	 * previous block, so we only need to allocate a new index
	 * block every idxs leaf blocks.  At ldxs**2 blocks, we need
	 * an additional index block, and at ldxs**3 blocks, yet
	 * another index blocks.
	 */
	if (ei->i_da_metadata_calc_len &&
	    ei->i_da_metadata_calc_last_lblock+1 == lblock) {
		int num = 0;

		if ((ei->i_da_metadata_calc_len % idxs) == 0)
			num++;
		if ((ei->i_da_metadata_calc_len % (idxs*idxs)) == 0)
			num++;
		if ((ei->i_da_metadata_calc_len % (idxs*idxs*idxs)) == 0) {
			num++;
			ei->i_da_metadata_calc_len = 0;
		} else
			ei->i_da_metadata_calc_len++;
		ei->i_da_metadata_calc_last_lblock++;
		return num;
	}

	/*
	 * In the worst case we need a new set of index blocks at
	 * every level of the inode's extent tree.
	 */
	ei->i_da_metadata_calc_len = 1;
	ei->i_da_metadata_calc_last_lblock = lblock;
	return ext_depth(inode) + 1;
}

static int
ext4_ext_max_entries(struct inode *inode, int depth)
{
	int max;

	if (depth == ext_depth(inode)) {
		if (depth == 0)
			max = ext4_ext_space_root(inode, 1);
		else
			max = ext4_ext_space_root_idx(inode, 1);
	} else {
		if (depth == 0)
			max = ext4_ext_space_block(inode, 1);
		else
			max = ext4_ext_space_block_idx(inode, 1);
	}

	return max;
}

static int ext4_valid_extent(struct inode *inode, struct ext4_extent *ext)
{
	ext4_fsblk_t block = ext4_ext_pblock(ext);
	int len = ext4_ext_get_actual_len(ext);
	ext4_lblk_t lblock = le32_to_cpu(ext->ee_block);
	ext4_lblk_t last = lblock + len - 1;

	if (len == 0 || lblock > last)
		return 0;
	return ext4_data_block_valid(EXT4_SB(inode->i_sb), block, len);
}

static int ext4_valid_extent_idx(struct inode *inode,
				struct ext4_extent_idx *ext_idx)
{
	ext4_fsblk_t block = ext4_idx_pblock(ext_idx);

	return ext4_data_block_valid(EXT4_SB(inode->i_sb), block, 1);
}

static int ext4_valid_extent_entries(struct inode *inode,
				struct ext4_extent_header *eh,
				int depth)
{
	unsigned short entries;
	if (eh->eh_entries == 0)
		return 1;

	entries = le16_to_cpu(eh->eh_entries);

	if (depth == 0) {
		/* leaf entries */
		struct ext4_extent *ext = EXT_FIRST_EXTENT(eh);
		struct ext4_super_block *es = EXT4_SB(inode->i_sb)->s_es;
		ext4_fsblk_t pblock = 0;
		ext4_lblk_t lblock = 0;
		ext4_lblk_t prev = 0;
		int len = 0;
		while (entries) {
			if (!ext4_valid_extent(inode, ext))
				return 0;

			/* Check for overlapping extents */
			lblock = le32_to_cpu(ext->ee_block);
			len = ext4_ext_get_actual_len(ext);
			if ((lblock <= prev) && prev) {
				pblock = ext4_ext_pblock(ext);
				es->s_last_error_block = cpu_to_le64(pblock);
				return 0;
			}
			ext++;
			entries--;
			prev = lblock + len - 1;
		}
	} else {
		struct ext4_extent_idx *ext_idx = EXT_FIRST_INDEX(eh);
		while (entries) {
			if (!ext4_valid_extent_idx(inode, ext_idx))
				return 0;
			ext_idx++;
			entries--;
		}
	}
	return 1;
}

static int __ext4_ext_check(const char *function, unsigned int line,
			    struct inode *inode, struct ext4_extent_header *eh,
			    int depth)
{
	const char *error_msg;
	int max = 0;

	if (unlikely(eh->eh_magic != EXT4_EXT_MAGIC)) {
		error_msg = "invalid magic";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_depth) != depth)) {
		error_msg = "unexpected eh_depth";
		goto corrupted;
	}
	if (unlikely(eh->eh_max == 0)) {
		error_msg = "invalid eh_max";
		goto corrupted;
	}
	max = ext4_ext_max_entries(inode, depth);
	if (unlikely(le16_to_cpu(eh->eh_max) > max)) {
		error_msg = "too large eh_max";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_entries) > le16_to_cpu(eh->eh_max))) {
		error_msg = "invalid eh_entries";
		goto corrupted;
	}
	if (!ext4_valid_extent_entries(inode, eh, depth)) {
		error_msg = "invalid extent entries";
		goto corrupted;
	}
	/* Verify checksum on non-root extent tree nodes */
	if (ext_depth(inode) != depth &&
	    !ext4_extent_block_csum_verify(inode, eh)) {
		error_msg = "extent tree corrupted";
		goto corrupted;
	}
	return 0;

corrupted:
	ext4_error_inode(inode, function, line, 0,
			"bad header/extent: %s - magic %x, "
			"entries %u, max %u(%u), depth %u(%u)",
			error_msg, le16_to_cpu(eh->eh_magic),
			le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max),
			max, le16_to_cpu(eh->eh_depth), depth);

	return -EIO;
}

#define ext4_ext_check(inode, eh, depth)	\
	__ext4_ext_check(__func__, __LINE__, inode, eh, depth)

int ext4_ext_check_inode(struct inode *inode)
{
	return ext4_ext_check(inode, ext_inode_hdr(inode), ext_depth(inode));
}

static int __ext4_ext_check_block(const char *function, unsigned int line,
				  struct inode *inode,
				  struct ext4_extent_header *eh,
				  int depth,
				  struct buffer_head *bh)
{
	int ret;

	if (buffer_verified(bh))
		return 0;
	ret = ext4_ext_check(inode, eh, depth);
	if (ret)
		return ret;
	set_buffer_verified(bh);
	return ret;
}

#define ext4_ext_check_block(inode, eh, depth, bh)	\
	__ext4_ext_check_block(__func__, __LINE__, inode, eh, depth, bh)

#ifdef EXT_DEBUG
static void ext4_ext_show_path(struct inode *inode, struct ext4_ext_path *path)
{
	int k, l = path->p_depth;

	ext_debug("path:");
	for (k = 0; k <= l; k++, path++) {
		if (path->p_idx) {
		  ext_debug("  %d->%llu", le32_to_cpu(path->p_idx->ei_block),
			    ext4_idx_pblock(path->p_idx));
		} else if (path->p_ext) {
			ext_debug("  %d:[%d]%d:%llu ",
				  le32_to_cpu(path->p_ext->ee_block),
				  ext4_ext_is_uninitialized(path->p_ext),
				  ext4_ext_get_actual_len(path->p_ext),
				  ext4_ext_pblock(path->p_ext));
		} else
			ext_debug("  []");
	}
	ext_debug("\n");
}

static void ext4_ext_show_leaf(struct inode *inode, struct ext4_ext_path *path)
{
	int depth = ext_depth(inode);
	struct ext4_extent_header *eh;
	struct ext4_extent *ex;
	int i;

	if (!path)
		return;

	eh = path[depth].p_hdr;
	ex = EXT_FIRST_EXTENT(eh);

	ext_debug("Displaying leaf extents for inode %lu\n", inode->i_ino);

	for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ex++) {
		ext_debug("%d:[%d]%d:%llu ", le32_to_cpu(ex->ee_block),
			  ext4_ext_is_uninitialized(ex),
			  ext4_ext_get_actual_len(ex), ext4_ext_pblock(ex));
	}
	ext_debug("\n");
}

static void ext4_ext_show_move(struct inode *inode, struct ext4_ext_path *path,
			ext4_fsblk_t newblock, int level)
{
	int depth = ext_depth(inode);
	struct ext4_extent *ex;

	if (depth != level) {
		struct ext4_extent_idx *idx;
		idx = path[level].p_idx;
		while (idx <= EXT_MAX_INDEX(path[level].p_hdr)) {
			ext_debug("%d: move %d:%llu in new index %llu\n", level,
					le32_to_cpu(idx->ei_block),
					ext4_idx_pblock(idx),
					newblock);
			idx++;
		}

		return;
	}

	ex = path[depth].p_ext;
	while (ex <= EXT_MAX_EXTENT(path[depth].p_hdr)) {
		ext_debug("move %d:%llu:[%d]%d in new leaf %llu\n",
				le32_to_cpu(ex->ee_block),
				ext4_ext_pblock(ex),
				ext4_ext_is_uninitialized(ex),
				ext4_ext_get_actual_len(ex),
				newblock);
		ex++;
	}
}

#else
#define ext4_ext_show_path(inode, path)
#define ext4_ext_show_leaf(inode, path)
#define ext4_ext_show_move(inode, path, newblock, level)
#endif

void ext4_ext_drop_refs(struct ext4_ext_path *path)
{
	int depth = path->p_depth;
	int i;

	for (i = 0; i <= depth; i++, path++)
		if (path->p_bh) {
			brelse(path->p_bh);
			path->p_bh = NULL;
		}
}

/*
 * ext4_ext_binsearch_idx:
 * binary search for the closest index of the given block
 * the header must be checked before calling this
 */
//���ö��ַ���ext4 extent B+��path->p_hdr[]��ߵ�ext4_extent_idx[]�����У��ҵ���ʼ�߼���
//��ַ��ӽ��������ʼ�߼����ַblock��ext4_extent_idx��path->p_idxָ�����ext4_extent_idx
static void
ext4_ext_binsearch_idx(struct inode *inode,
			struct ext4_ext_path *path, ext4_lblk_t block)
{//block�Ǵ������ʼ�߼����ַ
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent_idx *r, *l, *m;


	ext_debug("binsearch for %u(idx):  ", block);
    /*��������ڵ�ֻ��һ��ext4_extent_idx�ṹ�������±�while��������path->p_idx��ָ���һ�������ڵ㡣�ر�ע�⣬ext4_ext_binsearch_idx()
    �ҵ���ext4_extent_idx����ʼ�߼����ַ<=block*/
    
	l = EXT_FIRST_INDEX(eh) + 1;//lָ��ext4_extent_header��ߵ�ext4_extent_idx����ĵ�2��ext4_extent_idx��Ա
	r = EXT_LAST_INDEX(eh);//rָ��ext4_extent_header��ߵ�ext4_extent_idx��������һ��ext4_extent_idx��Ա

    //��l��rָ���ext4_extent_idx����֮�䣬�ҵ�һ��ext4_extent_idx->ei_block��ӽ�
    //�������ʼ�߼����ַblock�ġ�ext4_extent_idx��extent B+�������ڵ㣬���Աei_block��
    //��������ڵ����ʼ�߼����ַ��ע�⣬���ext4_extent_idx->ei_block <= �����
    //��ʼ�߼����ַblock������ext4_extent_idx->ei_block��ӽ��������ʼ�߼����ַblock
    while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ei_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ei_block),
				m, le32_to_cpu(m->ei_block),
				r, le32_to_cpu(r->ei_block));
	}
    //path->p_idxָ����ʼ�߼����ַ��ӽ��������ʼ�߼����ַblock��ext4_extent_idx
	path->p_idx = l - 1;
	ext_debug("  -> %u->%lld ", le32_to_cpu(path->p_idx->ei_block),
		  ext4_idx_pblock(path->p_idx));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent_idx *chix, *ix;
		int k;

		chix = ix = EXT_FIRST_INDEX(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ix++) {
		  if (k != 0 &&
		      le32_to_cpu(ix->ei_block) <= le32_to_cpu(ix[-1].ei_block)) {
				printk(KERN_DEBUG "k=%d, ix=0x%p, "
				       "first=0x%p\n", k,
				       ix, EXT_FIRST_INDEX(eh));
				printk(KERN_DEBUG "%u <= %u\n",
				       le32_to_cpu(ix->ei_block),
				       le32_to_cpu(ix[-1].ei_block));
			}
			BUG_ON(k && le32_to_cpu(ix->ei_block)
					   <= le32_to_cpu(ix[-1].ei_block));
			if (block < le32_to_cpu(ix->ei_block))
				break;
			chix = ix;
		}
		BUG_ON(chix != path->p_idx);
	}
#endif

}

/*
 * ext4_ext_binsearch:
 * binary search for closest extent of the given block
 * the header must be checked before calling this
 */
//���ö��ַ���ext4 extent B+��path->p_hdr[]��ߵ�ext4_extent[]�����У��ҵ���ʼ�߼����ַ
//��ӽ��������ʼ�߼����ַblock��ext4_extent��path->p_extָ�����ext4_extent
static void
ext4_ext_binsearch(struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t block)
{//block�Ǵ������ʼ�߼����ַ
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent *r, *l, *m;

    /*���Ҷ�ӽ��û��һ����Ч��ext4_extent�ṹ��ֱ��return��path[ppos]->p_ext����NULL*/
	if (eh->eh_entries == 0) {
		/*
		 * this leaf is empty:
		 * we get such a leaf in split/add case
		 */
		return;
	}
    /*���Ҷ�ӽڵ�ֻ��һ��ext4_extent�ṹ�������±�while��������path->p_idx��ָ���һ�������ڵ㡣�ر�ע�⣬ext4_ext_binsearch()
    �ҵ���ext4_extent����ʼ�߼����ַ<=block*/

	ext_debug("binsearch for %u:  ", block);

    //lָ��ext4_extent_header��ߵ�ext4_extent����ĵ�2��ext4_extent��Ա
	l = EXT_FIRST_EXTENT(eh) + 1;
    //rָ��ext4_extent_header��ߵ�ext4_extent��������һ��ext4_extent��Ա
	r = EXT_LAST_EXTENT(eh);

    //��l��rָ���ext4_extent����֮�䣬�ҵ�һ��ext4_extent->ee_block��ӽ�
    //�������ʼ�߼����ַblock�ġ�ext4_extent��extent B+��Ҷ�ӽڵ㣬���Աee_block��
    //���Ҷ�ӽڵ����ʼ�߼����ַ��ע�⣬���ext4_extent->ee_block <= �����
    //��ʼ�߼����ַblock������ext4_extent->ei_block��ӽ��������ʼ�߼����ַblock
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ee_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ee_block),
				m, le32_to_cpu(m->ee_block),
				r, le32_to_cpu(r->ee_block));
	}
    //path->p_extָ����ʼ�߼����ַ��ӽ��������ʼ�߼����ַblock��ext4_extent
	path->p_ext = l - 1;
	ext_debug("  -> %d:%llu:[%d]%d ",
			le32_to_cpu(path->p_ext->ee_block),
			ext4_ext_pblock(path->p_ext),
			ext4_ext_is_uninitialized(path->p_ext),
			ext4_ext_get_actual_len(path->p_ext));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent *chex, *ex;
		int k;

		chex = ex = EXT_FIRST_EXTENT(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ex++) {
			BUG_ON(k && le32_to_cpu(ex->ee_block)
					  <= le32_to_cpu(ex[-1].ee_block));
			if (block < le32_to_cpu(ex->ee_block))
				break;
			chex = ex;
		}
		BUG_ON(chex != path->p_ext);
	}
#endif

}

int ext4_ext_tree_init(handle_t *handle, struct inode *inode)
{
	struct ext4_extent_header *eh;

	eh = ext_inode_hdr(inode);
	eh->eh_depth = 0;
	eh->eh_entries = 0;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = cpu_to_le16(ext4_ext_space_root(inode, 0));
	ext4_mark_inode_dirty(handle, inode);
	return 0;
}
/*����ext4 extent B+���ĸ��ڵ��ext4_extent_header�����ҵ�ÿһ�������ڵ�����ʼ�߼����ַ��ӽ�������߼����ַblock��ext4_extent_idx
���浽path[ppos]->p_idx.Ȼ���ҵ����һ���Ҷ�ӽڵ�����ʼ�߼����ַ��ӽ�������߼����ַblock��ext4_extent�����浽path[ppos]->p_ext��
���ext4_extent�Ű������߼����ַ��������ַ��ӳ���ϵ��ע�⣬�ҵ���Щ��ʼ�߼����ַ�ӽ�block��ext4_extent_idx��ext4_extent��
��ʼ�߼����ַ<=block����block����ߣ�����������������block��Ӧ��ext4_extent����ext4 extent B+��ʱ��Ҳ�ǲ��뵽��Щext4_extent_idx
��ext4_extent�ṹ���ұߡ�ext4 extent B+�������ڵ��Ҷ�ӽڵ��е�ext4_extent_idx��ext4_extent���߼����ַ��������������˳���Ų���

˵�����ܳ���һ���������������Ҷ�ӽڵ���û��һ��ext4_extent�ṹ����path[ppos].p_ext��NULL����path[ppos].p_hdrָ�����Ҷ�ӽڵ��ͷ���
*/
struct ext4_ext_path *
ext4_ext_find_extent(struct inode *inode, ext4_lblk_t block,
					struct ext4_ext_path *path)//block�Ǵ������ʼ�߼����ַ
{
	struct ext4_extent_header *eh;
	struct buffer_head *bh;
	short int depth, i, ppos = 0, alloc = 0;
	int ret;
    //��ext4_inode_info->i_data����õ�ext4 extent B+���ĸ��ڵ�
	eh = ext_inode_hdr(inode);
    //xt4 extent B+�����
	depth = ext_depth(inode);

	/* account possible depth increase */
	if (!path) {
        //����B+������ȷ���ext4_ext_path�ṹ
		path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 2),
				GFP_NOFS);
		if (!path)
			return ERR_PTR(-ENOMEM);
		alloc = 1;
	}
	path[0].p_hdr = eh;
	path[0].p_bh = NULL;

	i = depth;

    /*ext4 extent B+���������ڵ��Ҷ�ӽڵ����
�����ڵ�    ext4_extent_header + ext4_extent_idx +  ext4_extent_idx + ........
                                     |
�����ڵ�               ext4_extent_header + ext4_extent_idx + ext4_extent_idx+ ........
                                               |
Ҷ�ӽڵ�                                    ext4_extent_header + ext4_extent + ext4_extent+ ........

    path[0].p_hdrָ��B+���ĸ��ڵ��ext4_extent_header��
    �±����whileѭ���Ǹ������B+���ĸ��ڵ��ext4_extent_header�����ҵ�ÿһ��
    �����ڵ�����ӽ��������ʼ�߼����ַblock��ext4_extent_idx���浽path[ppos]->p_idx��
    Ȼ���ҵ����һ���Ҷ�ӽڵ�����ӽ��������ʼ�߼����ַblock��ext4_extent�����浽
    path[ppos]->p_ext�����ext4_extent�Ű������߼����ַ��������ַ��ӳ���ϵ��
    */
	/* walk through the tree */
	while (i) {
		ext_debug("depth %d: num %d, max %d\n",
			  ppos, le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));
        
        //���ö��ַ���ext4 extent B+��path[ppos]->p_hdr[]��ߵ�ext4_extent_idx[]�����У�
        //�ҵ���ʼ�߼����ַ��ӽ��������ʼ�߼����ַblock��ext4_extent_idx��path[ppos]->p_idxָ�����ext4_extent_idx
		ext4_ext_binsearch_idx(inode, path + ppos, block);
        //ͨ�������ڵ�ext4_extent_idx�ṹ��ei_leaf_lo��ei_leaf_hi��Ա������������ţ��������鱣�����²�Ҷ�ӽڵ���������ڵ�4K����
		path[ppos].p_block = ext4_idx_pblock(path[ppos].p_idx);//
		path[ppos].p_depth = i;//�߼����ַ�ӽ�map->m_lblk�������ڵ��Ҷ�ӽڵ�����ext4 extent B+���еĲ���
		path[ppos].p_ext = NULL;

        /*����������ַpath[ppos].p_block�õ������Ĵ��������ӳ���bh�������и������ص㣬ext4 extent B+��ÿһ�������ڵ�
        ��Ҷ�ӽڵ��ext4_extent_header��ext4_extent_idx��ext4_extent���ݱ��ʶ��Ǳ����ڴ�����ģ�ռһ������飬4K��С��
        path[ppos].p_block����ext4_idx_pblock(path[ppos].p_idx)��ext4_idx_pblock(path[ppos].p_idx)����ɶ?path[ppos].p_idx���ҵ���
        �߼����ַ��ӽ��������ʼ�߼����ַblock��ext4_extent_idx�ṹ��ext4_idx_pblock(path[ppos].p_idx)������ṹ����������ţ�
        �������鱣��ĸ������ڵ���һ�������ڵ�4K����(ext4_extent_header+N��ext4_extent_idx�ṹ)����Ҷ�ӽڵ��4K����
        (ext4_extent_header+N��ext4_extent�ṹ)��bh = sb_getblk(inode->i_sb, path[ppos].p_block)��ӳ�����������4K���ݵ�bh��
        
        ext4 extent B+����������ڵ��Ҷ�ӽڵ��4K���ݶ��Ǳ�����ĳ�������(root�ڵ����)����������ڵ��Ҷ�ӽڵ�����ô������ϵ����?
        �����ϲ������ڵ�ext4_extent_idx�ṹ��������Ա��¼�����²������ڵ����Ҷ�ӽڵ�4K�������ţ���ǰ��Щ��������ڵ��Ҷ�ӽڵ�
        �϶�Ҫ�й�ϵ�ģ���ʼ�߼����ַһһ��Ӧ���ŻὨ��������ϵ��������ص㡣
        */
        //path[ppos].p_block�Ǳ������²�Ҷ�ӽڵ���������ڵ�4K���ݣ�bhӳ��ָ����������
		bh = sb_getblk(inode->i_sb, path[ppos].p_block);
		if (unlikely(!bh)) {
			ret = -ENOMEM;
			goto err;
		}
		if (!bh_uptodate_or_lock(bh)) {
			trace_ext4_ext_load_extent(inode, block,
						path[ppos].p_block);
			ret = bh_submit_read(bh);
			if (ret < 0) {
				put_bh(bh);
				goto err;
			}
		}
        //ehָ��ǰ�����ڵ��Ӧ�� �²�������ڵ����Ҷ�ӽڵ��ͷ��㣬ע�⣬�ǵ�ǰppos�����ڵ��²�������ڵ����Ҷ�ӽڵ�
		eh = ext_block_hdr(bh);
        //�����ڵ������1
		ppos++;
		if (unlikely(ppos > depth)) {
			put_bh(bh);
			EXT4_ERROR_INODE(inode,
					 "ppos %d > depth %d", ppos, depth);
			ret = -EIO;
			goto err;
		}
        //�ϱ�ppos++�ˣ�ppos������һ�������ڵ����Ҷ�ӽڵ��ˡ�path[ppos].p_bhָ���µ�ppos��һ�� �����ڵ����Ҷ�ӽڵ� 4K���ݵ������ 
        //ӳ���bh
		path[ppos].p_bh = bh;
        //path[ppos].p_bhָ��ppos��һ�������ڵ����Ҷ�ӽڵ��ͷ�ṹext4_extent_header
		path[ppos].p_hdr = eh;
		i--;

		ret = ext4_ext_check_block(inode, eh, i, bh);
		if (ret < 0)
			goto err;
	}

	path[ppos].p_depth = i;
	path[ppos].p_ext = NULL;
	path[ppos].p_idx = NULL;

	/* find extent */
  //���ö��ַ���ext4 extent B+��path[ppos]->p_hdr[]��ߵ�ext4_extent[]�����У��ҵ���ʼ�߼����ַ��ӽ��������ʼ�߼����ַblock
  //��ext4_extent����path[ppos]->p_extָ�����ext4_extent�����Ҷ�ӽ��û��һ��ext4_extent�ṹ����path[ppos]->p_ext����NULL
	ext4_ext_binsearch(inode, path + ppos, block);
	/* if not an empty leaf */
	if (path[ppos].p_ext)
        //��ext4_extent�ṹ��ee_start_hi��ee_start_lo��Ա������������ţ������������ext4_extent���߼����ַӳ��ĵ���ʼ������
		path[ppos].p_block = ext4_ext_pblock(path[ppos].p_ext);

	ext4_ext_show_path(inode, path);

	return path;

err:
	ext4_ext_drop_refs(path);
	if (alloc)
		kfree(path);
	return ERR_PTR(ret);
}

/*
 * ext4_ext_insert_index:
 * insert new index [@logical;@ptr] into the block at @curp;
 * check where to insert: before @curp or after @curp
 */
//���µ������ڵ�ext4_extent_idx�ṹ(��ʼ�߼����ַlogical,������ptr)���뵽ext4 extent B+��curp->p_idxָ���ext4_extent_idx�ṹǰ��
//����ı��ʺܼ򵥣���curp->p_idx����(curp->p_idx+1)��ߵ�����ext4_extent_idx�ṹȫ����ƶ�һ��ext4_extent_idx�ṹ��С�����µ�
//ext4_extent_idx����curp->p_idx����(curp->p_idx+1)ԭ����λ�á�
static int ext4_ext_insert_index(handle_t *handle, struct inode *inode,
				 struct ext4_ext_path *curp,
				 int logical, ext4_fsblk_t ptr)
{
	struct ext4_extent_idx *ix;
	int len, err;

	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		return err;

	if (unlikely(logical == le32_to_cpu(curp->p_idx->ei_block))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d == ei_block %d!",
				 logical, le32_to_cpu(curp->p_idx->ei_block));
		return -EIO;
	}

	if (unlikely(le16_to_cpu(curp->p_hdr->eh_entries)
			     >= le16_to_cpu(curp->p_hdr->eh_max))) {
		EXT4_ERROR_INODE(inode,
				 "eh_entries %d >= eh_max %d!",
				 le16_to_cpu(curp->p_hdr->eh_entries),
				 le16_to_cpu(curp->p_hdr->eh_max));
		return -EIO;
	}
    //curp->p_idx��ext4 extent B+����ʼ�߼����ַ��ӽ��������ʼ�߼����ַmap->m_lblk��ext4_extent_idx�ṹ�������ǰ��µ�
    //ext4_extent_idx(��ʼ�߼����ַ��logical,��ʼ������ptr)���뵽curp->p_idxָ���ext4_extent_idx�ṹǰ��
	if (logical > le32_to_cpu(curp->p_idx->ei_block)) {
		/* insert after */
        //�������ext4_extent_idx�ṹ��ʼ�߼����ַlogical����curp->p_idx����ʼ�߼����ַ�� ��Ҫ����curp->p_idx���ext4_extent_idx
        //��ߣ�(curp->p_idx + 1)���ext4_extent_idx��ߡ�����ǰ���±�memmove�Ȱ�(curp->p_idx+1)��ߵ�����ext4_extent_idx�ṹȫ���
        //�ƶ�һ��ext4_extent_idx�ṹ��С��Ȼ����µ�ext4_extent_idx���뵽curp->p_idx + 1λ�ô�
		ext_debug("insert new index %d after: %llu\n", logical, ptr);
		ix = curp->p_idx + 1;
	} else {
		/* insert before */
        //�������ext4_extent_idx�ṹ��ʼ�߼����ַlogical��С���Ͳ��뵽curp->p_idx���ext4_extent_idxǰ�ߡ�����ǰ���±�memmove
        //�Ȱ�curp->p_idx��ߵ�����ext4_extent_idx�ṹȫ����ƶ�һ��ext4_extent_idx�ṹ��С��
        //Ȼ����µ�ext4_extent_idx���뵽curp->p_idxλ�ô�
		ext_debug("insert new index %d before: %llu\n", logical, ptr);
		ix = curp->p_idx;
	}
    //ix��curp->p_idx����(curp->p_idx+1)��len��ix��������ڵ��ext4_extent_idx�ṹ�������ڵ����һ��ext4_extent_idx�ṹ(��Ч��)֮��
    //���е�ext4_extent_idx�ṹ������ע�⣬EXT_LAST_INDEX(curp->p_hdr)�������ڵ����һ����Ч��ext4_extent_idx�ṹ����������ڵ�ֻ��
    //һ��ext4_extent_idx�ṹ����EXT_LAST_INDEX(curp->p_hdr)��ָ�����һ��ext4_extent_idx�ṹ
	len = EXT_LAST_INDEX(curp->p_hdr) - ix + 1;
	BUG_ON(len < 0);
	if (len > 0) {
		ext_debug("insert new index %d: "
				"move %d indices from 0x%p to 0x%p\n",
				logical, len, ix, ix + 1);
        //��ix��ߵ�len��ext4_extent_idx�ṹ����ƶ�һ��ext4_extent_idx�ṹ��С
		memmove(ix + 1, ix, len * sizeof(struct ext4_extent_idx));
	}

	if (unlikely(ix > EXT_MAX_INDEX(curp->p_hdr))) {
		EXT4_ERROR_INODE(inode, "ix > EXT_MAX_INDEX!");
		return -EIO;
	}
    //����ixָ��ext4_extent_idx�ṹ�ǿ��еģ���������Ҫ������߼����ַlogial�Ͷ�Ӧ�������š��൱�ڰѱ���Ҫ����ext4 extent b+����ext4_extent_idx���뵽ixָ���ext4_extent_idxλ�ô�
	ix->ei_block = cpu_to_le32(logical);
	ext4_idx_store_pblock(ix, ptr);
    //�����ڵ���Ч��ext4_extent_idx����һ������Ϊ�ղ��²�����һ��ext4_extent_idx
	le16_add_cpu(&curp->p_hdr->eh_entries, 1);
    /*���ص㣬��Ȼû�ж�ei_len��ֵ����ȥ��ext4_extent_idx�ṹû��ei_len��Ա*/
	if (unlikely(ix > EXT_LAST_INDEX(curp->p_hdr))) {
		EXT4_ERROR_INODE(inode, "ix > EXT_LAST_INDEX!");
		return -EIO;
	}

	err = ext4_ext_dirty(handle, inode, curp);
	ext4_std_error(inode->i_sb, err);

	return err;
}

/*
 * ext4_ext_split:
 * inserts new subtree into the path, using free index entry
 * at depth @at:
 * - allocates all needed blocks (new leaf and all intermediate index blocks)
 * - makes decision where to split
 * - moves remaining extents and index entries (right to the split point)
 *   into the newly allocated blocks
 * - initializes subtree
 */



/*
��������:
1:����ȷ��ext4 extent B+���ķָ���߼���ַborder�����path[depth].p_ext����ext4_extent B+��Ҷ�ӽڵ�ڵ�
���һ��ext4 extent�ṹ����ָ���߼���ַborder��path[depth].p_ext��ߵ�ext4_extent��ʼ�߼����ַ����
border=path[depth].p_ext[1].ee_block������border���²���ext4 extent B+����ext4_extent����ʼ�߼����ַ����newext->ee_block

2:��Ϊext4_extent B+��at��һ�������ڵ��п���entry�������at~depth(B+�����)֮��ĵ�ÿһ�������ڵ��Ҷ�ӽڵ�
�������µ������ڵ��Ҷ�ӽ�㣬ÿ�������ڵ��Ҷ�ӽ�㶼ռһ��block��С(4K)���ֱ𱣴�N��ext4_extent_idx�ṹ
��N��ext4_extent�ṹ������ext4_extent_header����while (k--)�Ǹ�ѭ������Щ�·���������ڵ��Ҷ�ӽڵ��У�B+��������2����Ǹ������ڵ��
��һ��ext4_extent_idx�������ų�Ա(ei_leaf_lo��ei_leaf_hi)��¼���·���ı���Ҷ�ӽ��4K���ݵ�������
(������ext4_idx_store_pblock(fidx, oldblock))����һ��ext4_extent_idx����ʼ�߼����ַ��border
(������fidx->ei_block = border)��B+��������3����Ǹ������ڵ�ĵ�һ��ext4_extent_idx�������ų�Ա��¼��
�Ǳ��浹����2��������ڵ�4K���ݵ������ţ���������ڵ��һ��ext4_extent_idx����ʼ�߼����ַ
��border(������fidx->ei_block = border)........�������ơ�

at��һ���·���������ڵ�(��������newblock����ʼ�߼����ַborder)��ִ��ext4_ext_insert_index()���뵽ext4_extent B+��at
��ԭ�е������ڵ�(path + at)->p_idxָ���ext4_extent_idx�ṹǰ���ext4_extent_idx�ṹλ�ô������������:��
(path + at)->p_idxָ��������ڵ��ext4_extent_idx�ṹ�������ext4_extent_idx�ṹ����ƶ�
һ��ext4_extent_idx�ṹ��С�������(path + at)->p_idxָ��������ڵ��ext4_extent_idx����
����һ�����е�ext4_extent_idx�ṹ��С�ռ䣬�·���������ڵ���ǲ��뵽���

3:Ҫ��ext4_extent B+��ԭ����at~depth��� path[i].p_idx~path[depth-1].p_idxָ���ext4_extent_idx�ṹ���
������ext4_extent_idx�ṹ �� path[depth].p_extָ���ext4_extent�������ext4_extent�ṹ���Խ��ƶ����ϱ�
���ext4_extent B+��at~denth�·��������ڵ��Ҷ�ӽڵ�������ӳ��bh�ڴ档���Ƕ�ԭ�е�ext4 extent B+���������ݵ��ص㡣
*/

//ext4_ext_map_blocks()->ext4_ext_handle_uninitialized_extents()/ext4_ext_handle_unwritten_extents()->ext4_ext_convert_to_initialized()
//->ext4_split_extent()->ext4_split_extent_at()->ext4_ext_insert_extent()->ext4_ext_create_new_leaf()->ext4_ext_split()
/*
�ϱߵĽ���û��ڹ�͵����ʡ�ֱ����꣬Ϊʲô��ִ�е�ext4_ext_insert_extent()->ext4_ext_create_new_leaf()->ext4_ext_split()?��ʲô����?
���ȣ�ext4_split_extent_at()�����У���path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χ�ָ�����Σ�����ext4_extent�ṹ��ǰ��
��ext4_extent�ṹ����ex��ֻ���߼��鷶Χ�����ˡ�������ext4_extent�ṹ��newext��Ҫ������뵽��ext4 extent B+������ext4_ext_insert_extent()
�����������ʱex����Ҷ�ӽڵ��ext4_extent�ṹ�����ˣ���if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))������������
if (le32_to_cpu(newext->ee_block) > le32_to_cpu(fex->ee_block))��������newext����ʼ�߼����ַС��ex����Ҷ�ӽڵ�����һ��ext4_extent
�ṹ����ʼ�߼����ַ����ִ��next = ext4_ext_next_leaf_block(path)�ȴ��룬�ص��ϲ������ڵ㣬�ҵ���ʼ�߼����ַ����������ڵ��Ҷ�ӽڵ㣬
����µ�Ҷ�ӽڵ��ext4_extent�ṹ���Ǳ������Ǿ�Ҫִ��ext4_ext_create_new_leaf()����ext4_extent B+�������ˡ�

����ext4_ext_create_new_leaf()����������ײ�������ڵ㿪ʼ�����������ҵ��п���entry�������ڵ㡣����ҵ���ִ��ext4_ext_split()��
����Ҳ�����ִ��ext4_ext_grow_indepth()��ext4_extent B+��root�ڵ�����һ�������ڵ�(��Ҷ�ӽڵ�)��Ȼ��Ҳִ��ext4_ext_split()��

��ִ�е�ext4_ext_split()��atһ���ext4_extent B+���п���entry�����Դ�at�㵽Ҷ�ӽڵ���һ�㣬�����µ������ڵ��Ҷ�ӽڵ㣬������Щ
�µ������ڵ��Ҷ�ӽڵ�˴˵������ŵ���ϵ�����Ǽ���ext4_ext_split()��if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr))
������������ִ��:

���·����Ҷ�ӽڵ㸴��m��ext4_extent�ṹʱ�����Ƶĵ�һ��ext4_extent�ṹ����path[depth].p_ext������
����ߵ� path[depth].p_ext[1]���ext4_extent�ṹ�����ң��±��´����������ڵ�ĵ�һ��ext4_extent_idx�ṹ����ʼ�߼������ַ
����border����path[depth].p_ext[1]���߼����ַ��Ҳ��path[depth].p_ext[1].ee_block��Ȼ�����´������������ڵ�ĵ�2��
ext4_extent_idx�ṹ����֮����m��ext4_extent_idx�ṹ���´������������ڵ�ĵ�һ��ext4_extent_idx����ʼ�߼����ַ��border��
����ʹ�ã���Ϊ�ָ���ext4_extent_idx�ṹ����ˣ�����ִ��ext4_ext_find_extent(newext->ee_block)���ϵ�ext4_extent B+���ҵ���
path[depth].p_extָ���ext4_extent�����ϵģ�����path[depth].p_ext��ߵ�m��ext4_extent�ṹ�ƶ������·����Ҷ�ӽڵ㣬
path[depth].p_ext����Ҷ�ӽڵ���пռ��ˣ�newext�Ͳ��뵽path[depth].p_extָ���ext4_extentҶ�ӽڵ��ߡ���δ�����
ext4_ext_insert_extent()��has_space ��if (!nearex)........} else{......}��else��֧

���ext4_ext_split()��if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr))��������������ִ��:
�������·����Ҷ�ӽڵ㸴��ext4_extent�ṹ��m��0����Ϊpath[depth].p_ext����Ҷ�ӽڵ����һ��ext4_extent
�ṹ���±ߵ�m = EXT_MAX_EXTENT(path[depth].p_hdr) - path[depth].p_ext++=0�����ң��±��´����������ڵ�ĵ�һ��ext4_extent_idx�ṹ
����ʼ�߼������ַ����newext->ee_block����������ִ��ext4_ext_find_extent()��ext4_extent B+�������ҵ���ʼ�߼����ַ��
newext->ee_block�Ĳ�������ڵ��ˣ�����ƥ�䡣��Ҷ�ӽڵ���?�����֧û�����µ�Ҷ�ӽڵ㸴��ext4_extent�ṹ���յģ�
ext4_ext_find_extent()ִ�к�path[ppos].depthָ���µ�Ҷ�ӽڵ��ͷ��㣬��ʱֱ�����Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ��
�߼����ַ��newext->ee_block������!��δ�����ext4_ext_insert_extent()��has_space ��if (!nearex)��֧��

ע�⣬����ext4_extent B+��Ҷ�ӽڵ��������ӵĵ�һ��ext4_extent�ṹ�����ҵ�һ��ext4_extent�ṹ����ʼ�߼����ַ�����ϱߵ������ڵ�
��ext4_extent_idx����ʼ�߼����ַ����newext->ee_block�����ϲ�������ڵ��ext4_extent_idx����ʼ�߼����ַҲ��newext->ee_block��
ֱ����at��

��ˣ����ǿ���ext4_ext_split()����ĵ�������:atһ���ext4_extent B+���п���entry�����at�㿪ʼ�����µ������ڵ��Ҷ�ӽڵ㣬
������Щ�µ������ڵ��Ҷ�ӽڵ�˴˵���������ϵ��Ȼ���path[depth].p_ext��ߵ�ext4_extent�ṹ�ƶ����µ�Ҷ�ӽڵ㣬��
path[at~depth-1].p_idx��Щ�����ڵ��ߵ�ext4_extent_idx�ṹ�����ƶ����´����������ڵ㡣����Ҫô�ϵ�path[depth].p_ext����Ҷ�ӽڵ�
���˿��е�ext4_extent entry����newex���뵽�ϵ�path[depth].p_ext����Ҷ�ӽڵ��߼��ɡ������´�����at~denth�������ڵ�
��Ҷ�ӽڵ㣬�д������е�entry����Щ�����ڵ����ʼ�߼����ַ����newext->ee_block����ֱ�Ӱ�newext���뵽�´�����Ҷ�ӽڵ��һ��
ext4_extent�ṹ���ɡ�
*/

//ext4_ext_split���ܽ�:
/*����ִ�е�ext4_ext_split()������˵��ext4 extent B+������newext->ee_block�йص�Ҷ�ӽڵ�ext4_extent�ṹ�����ˡ�
���Ǵ�ext4 extent B+��at��һ�������ڵ㵽Ҷ�ӽڵ㣬���ÿһ�㶼�����µ������ڵ㣬Ҳ����Ҷ�ӽڵ㡣���᳢�԰�
�����ڵ�path[at~depth].p_hdrָ���ext4_extent_idx�ṹ�ĺ�ߵ�ext4_extent_idx�ṹ��path[depth].p_extָ���
ext4_extent�ṹ��ߵ�ext4_extent�ṹ���ƶ����´�����Ҷ�ӽڵ�������ڵ㡣�������ܱ�֤ext4 extent B+���У�
��newext->ee_block�йص�Ҷ�ӽڵ��п���entry���ܴ��newext��*/
static int ext4_ext_split(handle_t *handle, struct inode *inode,
			  unsigned int flags,
			  struct ext4_ext_path *path,
//newext��Ҫ����ext4_extent B+����ext4_extent����ext4_extent B+���ĵ�at�����newext����at��������ڵ��п���entry
			  struct ext4_extent *newext, int at)
{
	struct buffer_head *bh = NULL;
	int depth = ext_depth(inode);
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	int i = at, k, m, a;
	ext4_fsblk_t newblock, oldblock;
	__le32 border;
	ext4_fsblk_t *ablocks = NULL; /* array of allocated blocks */
	int err = 0;

	/* make decision: where to split? */
	/* FIXME: now decision is simplest: at current extent */

	/* if current leaf will be split, then we should use
	 * border from split point */
	if (unlikely(path[depth].p_ext > EXT_MAX_EXTENT(path[depth].p_hdr))) {
		EXT4_ERROR_INODE(inode, "p_ext > EXT_MAX_EXTENT!");
		return -EIO;
	}

    //path[depth].p_ext��ext4 extent B+��Ҷ�ӽڵ��У��߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr)) {
        //path[depth].p_ext����Ҷ�ӽڵ����һ��ext4_extent�ṹ����������ߵ�ext4_extent�ṹpath[depth].p_ext[1]����ʼ�߼����ַ��Ϊ�ָ��border
        /*�ߵ������֧���±߰����·����Ҷ�ӽڵ㸴��m��ext4_extent�ṹʱ�����Ƶĵ�һ��ext4_extent�ṹ����path[depth].p_ext������
        ����ߵ� path[depth].p_ext[1]���ext4_extent�ṹ�����ң��±��´����������ڵ�ĵ�һ��ext4_extent_idx�ṹ����ʼ�߼������ַ
        ����border����path[depth].p_ext[1]���߼����ַ����path[depth].p_ext[1].ee_block��Ȼ�����´������������ڵ�ĵ�2��
        ext4_extent_idx�ṹλ�ô���֮����m��ext4_extent_idx�ṹ���´������������ڵ�ĵ�һ��ext4_extent_idx����ʼ�߼����ַ��border��
        ����ʹ�ã���Ϊ�ָ���ext4_extent_idx�ṹ����ˣ�����ִ��ext4_ext_find_extent(newext->ee_block)���ϵ�ext4_extent B+���ҵ���
        path[depth].p_extָ���ext4_extent�����ϵģ�����path[depth].p_ext��ߵ�m��ext4_extent�ṹ�ƶ������·����Ҷ�ӽڵ㣬
        path[depth].p_ext����Ҷ�ӽڵ���пռ��ˣ�newext�Ͳ��뵽path[depth].p_extָ���ext4_extentҶ�ӽڵ��ߡ���δ�����
        ext4_ext_insert_extent()��has_space ��if (!nearex)........} else{......}��else��֧*/
		border = path[depth].p_ext[1].ee_block;
		ext_debug("leaf will be split."
				" next leaf starts at %d\n",
				  le32_to_cpu(border));
	} else {
	    //����˵��path[depth].p_extָ�����Ҷ�ӽڵ����һ��ext4_extent�ṹ
	   /*�ߵ������֧���±߲������·����Ҷ�ӽڵ㸴��ext4_extent�ṹ��m��0����Ϊpath[depth].p_ext����Ҷ�ӽڵ����һ��ext4_extent
	   �ṹ���±ߵ�m = EXT_MAX_EXTENT(path[depth].p_hdr) - path[depth].p_ext++=0�����ң��±��´����������ڵ�ĵ�һ��ext4_extent_idx�ṹ
	   ����ʼ�߼������ַ����newext->ee_block����������ִ��ext4_ext_find_extent()��ext4_extent B+�������ҵ���ʼ�߼����ַ��
	   newext->ee_block�Ĳ�������ڵ��ˣ�����ƥ�䡣����һ�㣬���´������������ڵ�ĵ�2��ext4_extent_idx�ṹλ�ô���֮����
	   m��ext4_extent_idx�ṹ�����m�᲻��Ҳ��0�أ���һ������Ϊpath[i].p_idxָ���ext4_extent_idx�ṹ��һ���������ڵ����һ��
	   ext4_extent_idx�ṹѽ!�ǵĻ�m����0��
	   ��ע�⣬�����֧û�����µ�Ҷ�ӽڵ㸴��ext4_extent�ṹ���յģ�
	   ext4_ext_find_extent()ִ�к�path[ppos].depthָ���µ�Ҷ�ӽڵ��ͷ��㣬��ʱֱ�����Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ��
	   �߼����ַ��newext->ee_block���൱�ڰ�newexֱ�Ӳ��뵽��Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹλ�ô�!
	   ��δ�����ext4_ext_insert_extent()��has_space ��if (!nearex)��֧��*/
		border = newext->ee_block;
		ext_debug("leaf will be added."
				" next leaf starts at %d\n",
				le32_to_cpu(border));
	}

	/*
	 * If error occurs, then we break processing
	 * and mark filesystem read-only. index won't
	 * be inserted and tree will be in consistent
	 * state. Next mount will repair buffers too.
	 */

	/*
	 * Get array to track all allocated blocks.
	 * We need this to handle errors and free blocks
	 * upon them.
	 */
	//����ext4_extent B+����������depth��ext4_fsblk_t�����飬�±߱�������������
	ablocks = kzalloc(sizeof(ext4_fsblk_t) * depth, GFP_NOFS);
	if (!ablocks)
		return -ENOMEM;

	/* allocate all needed blocks */
	ext_debug("allocate %d blocks for indexes/leaf\n", depth - at);
    //����(depth - at)������飬newext����ext4 extent B+�ĵ�at����룬��at�㵽depth�㣬ÿ�����һ�������
	for (a = 0; a < depth - at; a++) {
        //��ext4�ļ�ϵͳԪ����������һ������飬�������������ţ�4K��С������ext4 extent B+�������ڵ����Ҷ�ӽ���
        //ͷ�ṹext4_extent_header+N��ext4_extent_idx����N��ext4_extent�ṹ
		newblock = ext4_ext_new_meta_block(handle, inode, path,
						   newext, &err, flags);
		if (newblock == 0)
			goto cleanup;
        //����������Ŀ�ű��浽ablocks
		ablocks[a] = newblock;
	}

	/* initialize new leaf */
	newblock = ablocks[--a];
	if (unlikely(newblock == 0)) {
		EXT4_ERROR_INODE(inode, "newblock == 0!");
		err = -EIO;
		goto cleanup;
	}
    //bhӳ��newblock�����ţ�������Ҷ�ӽڵ�������
	bh = sb_getblk(inode->i_sb, newblock);
	if (unlikely(!bh)) {
		err = -ENOMEM;
		goto cleanup;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err)
		goto cleanup;

    //nehָ���·����Ҷ�ӽڵ����ڴ��ͷ�ṹext4_extent_header���±߶��·����Ҷ�ӽڵ�ͷ�ṹext4_extent_header���г�ʼ��
	neh = ext_block_hdr(bh);
	neh->eh_entries = 0;
	neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	neh->eh_depth = 0;

	/* move remainder of path[depth] to the new leaf */
	if (unlikely(path[depth].p_hdr->eh_entries !=
		     path[depth].p_hdr->eh_max)) {
		EXT4_ERROR_INODE(inode, "eh_entries %d != eh_max %d!",
				 path[depth].p_hdr->eh_entries,
				 path[depth].p_hdr->eh_max);
		err = -EIO;
		goto cleanup;
	}
	/* start copy from next extent */
    //��path[depth].p_ext��ߵ�ext4_extent�ṹ��Ҷ�ӽڵ����һ��ext4_extent�ṹ֮�䣬һ����m��ext4_extent�ṹ
    /*����и����ص㣬������path[depth].p_ext++��ִ�к�path[depth].p_ext�Ѿ�ָ������һ��ext4_extent�ṹ�ˣ������±�memmove(ex, path[depth].p_ext,....)
     ��ex���Ƶ�m��ext4_extent�ṹ����������path[depth].p_ext���ָ���ext4_extent������һ�����ص㣬���path[depth].p_ext��������ϵ�
     Ҷ�ӽڵ�����һ��ext4_extent�ṹ���±߼������m��0���ǾͲ��������µ�Ҷ�ӽڵ㸳ֵext4_extent�ṹ��*/
	m = EXT_MAX_EXTENT(path[depth].p_hdr) - path[depth].p_ext++;
	ext4_ext_show_move(inode, path, newblock, depth);
	if (m) {
		struct ext4_extent *ex;
        //exָ���ϱ��·����Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ
		ex = EXT_FIRST_EXTENT(neh);
        //�ϵ�Ҷ�ӽڵ�path[depth].p_ext���m��ext4_extent�ṹ�ƶ����ϱ��·����Ҷ�ӽڵ�
		memmove(ex, path[depth].p_ext, sizeof(struct ext4_extent) * m);
        //�·����Ҷ�ӽڵ�������m��ext4_extent�ṹ
		le16_add_cpu(&neh->eh_entries, m);
	}

	ext4_extent_block_csum_set(inode, neh);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto cleanup;
	brelse(bh);
	bh = NULL;

	/* correct old leaf */
	if (m) {
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto cleanup;
        //path[depth].p_hdr���Ҷ�ӽڵ��ext4_extent�ṹ����m��
		le16_add_cpu(&path[depth].p_hdr->eh_entries, -m);
        //Ҷ�ӽڵ��ext4_extent�������٣����Ҷ�ӽڵ��Ӧ��bh��
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto cleanup;

	}

	/* create intermediate indexes */
    //ext4_extent B+��at��һ��������ڵ㵽���һ�������ڵ�֮��Ĳ��������Ǵ�at��ʼ�ж��ٲ������ڵ�
	k = depth - at - 1;
	if (unlikely(k < 0)) {
		EXT4_ERROR_INODE(inode, "k %d < 0!", k);
		err = -EIO;
		goto cleanup;
	}
	if (k)
		ext_debug("create %d intermediate indices\n", k);
	/* insert new index into current index block */
	/* current depth stored in i var */
    //i��ֵ��ext4_extent B+�����һ�������ڵ�Ĳ���������Ҷ�ӽڵ��ϱߵ��ǲ������ڵ�
	i = depth - 1;
	//ѭ��k�α�֤��at��һ���ext4_extent B+�������ڵ㵽���һ�������ڵ��У�ÿһ�������ڵ�path[i].p_idxָ���ext4_extent_idx�ṹ�����
	//һ��ext4_extent_idx�ṹ֮���ext4_extent_idx�ṹ�������Ƶ��ϱ��´����������ڵ��������У���������ablocks[--a]��newblock��
	//bhӳ���������顣nehָ����������ڵ�ͷext4_extent_header�ṹ��fidx����������ڵ��һ��ext4_extent_idx�ṹ��
	//ע�⣬�Ǵ�ext4_extent B+�����²�������ڵ����Ͽ�ʼ���ƣ���Ϊi�ĳ�ֵ��depth - 1��
	//����ext4_extent B+�����±�һ�������ڵ�Ĳ���
	while (k--) {
		oldblock = newblock;
        //��ȡ��һ��������Ӧ�Ŀ��newblock���������鱣�������ڵ�ext4_extent_headerͷ�ṹ+N��ext4_extent_idx�ṹ��4K����
		newblock = ablocks[--a];
        //newblock�����ӳ���bh
		bh = sb_getblk(inode->i_sb, newblock);
		if (unlikely(!bh)) {
			err = -ENOMEM;
			goto cleanup;
		}
		lock_buffer(bh);

		err = ext4_journal_get_create_access(handle, bh);
		if (err)
			goto cleanup;
        //nehָ��newblock��������ӳ���bh���ڴ��׵�ַ����Ƭ�ڴ��ͷ���������ڵ��ͷext4_extent_header�ṹ
		neh = ext_block_hdr(bh);
		neh->eh_entries = cpu_to_le16(1);
		neh->eh_magic = EXT4_EXT_MAGIC;
        //����������������ɵ�ext4_extent_idx�ṹ����
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
        //�����ڵ�����B+������
		neh->eh_depth = cpu_to_le16(depth - i);
        //fidxָ�������ڵ�ĵ�һ��ext4_extent_idx�ṹ
		fidx = EXT_FIRST_INDEX(neh);
        //fidx����ʼ�߼����ַ���ϱߵķָ���߼����ַborder
		fidx->ei_block = border;
        /*�ص㣬��һ��whileѭ�������oldblock(��newblock)�Ǳ����ϱ��·����Ҷ�ӽڵ�ext4_extent_headerͷ�ṹ+N��ext4_extent�ṹ
         ��4K���ݵ������ţ�fidxָ�����һ�������ڵ�(����Ҷ�ӽڵ��ϱߵ��ǲ������ڵ�)
         ��һ��ext4_extent_idx�ṹ�������ǰѱ����·���ı���Ҷ�ӽ���4K���ݵ�������newblock���浽���һ�������ڵ��һ�������ڵ��
         ext4_extent_idx�ṹ�С�������ѭ���������·�����ϲ������ڵ�ĵ�һ��ext4_extent_idx�ṹ��¼ �����²��·���������ڵ�4K����
         (ext4_extent_headerͷ�ṹ+N��ext4_extent_idx�ṹ)�������š�
         ˵���ˣ�ext4_ext_split()������ԭ�е�ext4_extent B+��at~depth��������ڵ��Ҷ�ӽ��ĺ��������ƶ����´�����
         �����ڵ��Ҷ�ӽ���С����������ϲ������ڵ��м�¼ �����²������ڵ����Ҷ�ӽ��4K���ݵ�������*/
        //����һ��Ҷ�ӽڵ������һ�������ڵ�������ű��浽��ǰ�����ڵ��һ��ext4_extent_idx�ṹ��ei_leaf_lo��ei_leaf_hi��Ա�С�
        //��������ͨ�����ext4_extent_idx�ṹ��ei_leaf_lo��ei_leaf_hi��Ա�ҵ���ָ�����һ��������ڵ����Ҷ�ӽڵ�
		ext4_idx_store_pblock(fidx, oldblock);

		ext_debug("int.index at %d (block %llu): %u -> %llu\n",
				i, newblock, le32_to_cpu(border), oldblock);

		/* move remainder of path[i] to the new index block */
		if (unlikely(EXT_MAX_INDEX(path[i].p_hdr) !=
					EXT_LAST_INDEX(path[i].p_hdr))) {
			EXT4_ERROR_INODE(inode,
					 "EXT_MAX_INDEX != EXT_LAST_INDEX ee_block %d!",
					 le32_to_cpu(path[i].p_ext->ee_block));
			err = -EIO;
			goto cleanup;
		}
		/* start copy indexes */
       //path[i].p_hdr��һ�������ڵ��У���path[i].p_idxָ���ext4_extent_idx�ṹ�����һ��ext4_extent_idx�ṹ֮��ext4_extent_idx����
       /*���ص㣬path[i].p_idx++��ִ�к�path[i].p_idxָ����һ��ext4_extent_idx�ṹ�������±�memmove(++fidx, path[i].p_idx...)��
        fidx���Ƶ�m��ext4_extent_idx�ṹ����������path[i].p_idx���ָ���ext4_extent_idx������һ�����ص㣬���path[i].p_idxָ��ľ���
        �ϵ������ڵ�����һ��ext4_extent_idx�����±߼��������m��0���ǾͲ������µ������ڵ㸴��ext4_extent_idx�ṹ�ˡ�*/
		m = EXT_MAX_INDEX(path[i].p_hdr) - path[i].p_idx++;
		ext_debug("cur 0x%p, last 0x%p\n", path[i].p_idx,
				EXT_MAX_INDEX(path[i].p_hdr));
		ext4_ext_show_move(inode, path, newblock, i);
		if (m) {
            //��path[i].p_idx��ߵ�m��ext4_extent_idx�ṹ��ֵ��newblock���������Ӧ�������ڵ㿪ͷ�ĵ�1��ext4_extent_idx��ߣ���fidxָ����ڴ�
            /*���ص㣬������++fid����fidxָ����µ������ڵ�ĵ�2��ext4_extent_idxλ�ô����������µ������ڵ��2��ext4_extent_idx�������
             ����m��ext4_extent_idx�ṹ*/
			memmove(++fidx, path[i].p_idx,
				sizeof(struct ext4_extent_idx) * m);
            //newblock���������Ӧ���µ������ڵ�������m��ext4_extent_idx�ṹ
			le16_add_cpu(&neh->eh_entries, m);
		}
		ext4_extent_block_csum_set(inode, neh);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		err = ext4_handle_dirty_metadata(handle, inode, bh);
		if (err)
			goto cleanup;
		brelse(bh);
		bh = NULL;

		/* correct old index */
		if (m) {
			err = ext4_ext_get_access(handle, inode, path + i);
			if (err)
				goto cleanup;
            //path[i].p_hdrָ����ϵ�ext4 extent B+����һ�������ڵ������m��ext4_extent_idx�ṹ
			le16_add_cpu(&path[i].p_hdr->eh_entries, -m);
            //path[i].p_hdrָ���ext4 extent B+����һ�������ڵ㣬�����ڵ������б仯��������һ�������ڵ����ݵ������Ҫ�����
			err = ext4_ext_dirty(handle, inode, path + i);
			if (err)
				goto cleanup;
		}
        //i--�����´�ѭ�����ͻ����һ��ext4_extent B+�������ڵ��path[i].p_idxָ���ext4_extent_idx�ṹ�����һ��ext4_extent_idx�ṹ֮��
        //���е�ext4_extent_idx�ṹ�����Ƶ�ablocks[--a]��newblock��������ӳ��bh
		i--;
	}

	/* insert new index */
/*���µ������ڵ�ext4_extent_idx�ṹ(��ʼ�߼����ַborder,������newblock)���뵽ext4 extent B+��at��һ�������ڵ�(path + at)->p_idxָ��
��ext4_extent_idx�ṹǰ��������ʱ��newblock���ϱ��·����at~depth��������ڵ��Ҷ�ӽ���У���ϣ�Ҳ����at��һ�������ڵ�������š�
�ϱ��Ѿ�������Щ�·���������ڵ��Ҷ�ӽ�㣬�ϲ��¼�²�������š������ٰ�at��һ���·���������ڵ��������newlbock��¼��ext4 extent
B+��ԭ����at��һ���(path + at)->p_idxָ���ext4_extent_idx�ṹǰ��ʵ���������λ�ò���һ���µ�ext4_extent_idx�ṹ����
����ʼ�߼���ų�Ա��border��������ʼ�����ų�Ա��newblock��ext4 extentB+��ԭ����at��һ���п��е�entry�������п��е�λ�ô��
�µ������ڵ㡣*/
	err = ext4_ext_insert_index(handle, inode, path + at,
				    le32_to_cpu(border), newblock);

cleanup:
	if (bh) {
		if (buffer_locked(bh))
			unlock_buffer(bh);
		brelse(bh);
	}

	if (err) {
		/* free all allocated blocks in error case */
		for (i = 0; i < depth; i++) {
			if (!ablocks[i])
				continue;
			ext4_free_blocks(handle, inode, NULL, ablocks[i], 1,
					 EXT4_FREE_BLOCKS_METADATA);
		}
	}
	kfree(ablocks);

	return err;
}

/*
 * ext4_ext_grow_indepth:
 * implements tree growing procedure:
 * - allocates new block
 * - moves top-level data (index block or leaf) into the new block
 * - initializes new top-level, creating index that points to the
 *   just created block
 */
//���ex->ee_block����һ���µ�����飬��Ϊ�µ������ڵ����Ҷ�ӽڵ���ӵ�ext4 extent B+�����ڵ��·��������൱�ڸ�ext4 extent B+��������
//һ���µĽڵ�
static int ext4_ext_grow_indepth(handle_t *handle, struct inode *inode,
				 unsigned int flags,
				 struct ext4_extent *newext)
{
	struct ext4_extent_header *neh;
	struct buffer_head *bh;
	ext4_fsblk_t newblock;
	int err = 0;
    //����һ���µ�����飬����������newblock
	newblock = ext4_ext_new_meta_block(handle, inode, NULL,
		newext, &err, flags);
	if (newblock == 0)
		return err;
    //newblock�����ӳ���bh
	bh = sb_getblk(inode->i_sb, newblock);
	if (unlikely(!bh))
		return -ENOMEM;
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err) {
		unlock_buffer(bh);
		goto out;
	}
    /*
    �ϱ����ex->ee_block����һ���µ�����飬��������newblock��4K��С�������newblock��������±�������Ϊ�µ�Ҷ�ӽ���
    �����ڵ���ӵ�ext4 extent B+���·��������ĵ�2�㡣
     
    �±���������Ҫ��������ext4 extent B+�����ڵ�ĸ��Ƶ�newblock��Ӧ��bh�ڴ棬newblock������Ϊ�µ�Ҷ�ӽ����������ڵ㣬
     �ͷ��ڸ��ڵ��·���neh = ext_block_hdr(bh)��neh->eh_magic = EXT4_EXT_MAGIC���Ƕ�����µĽڵ㸳ֵ���±ߵ�neh = ext_inode_hdr(inode)
     ��le16_add_cpu(&neh->eh_depth, 1)�Ǹ���ext4 extent B+���ڵ�����ݡ���Ϊ��ʱnewblock��Ϊ�µ�Ҷ�ӽ����������ڵ���ӵ���
     ���ڵ��±ߣ�ext4_idx_store_pblock(EXT_FIRST_INDEX(neh), newblock)���Ǹ��ڵ�ĵ�һ��ext4_extent_idx�ڵ��¼newblock���
     �µ�Ҷ�ӽ����������ڵ�������š�

     ��һ�����ص��ǣ���ʱext4 extent B+�����ڵ�ֻ�е�һ��ext4_extent_idx�ṹ����Ч�ģ��ýṹ�������ų�Ա���������newblock������
     ���*/
    
	/* move top-level index/leaf into new block */
    //��ext4 extent B+���ĸ��ڵ������(ͷ�ṹext4_extent_header+4��ext4_extent_idx����Ҷ�ӽ��ext4_extent_header+4��ext4_extent�ṹ)
    //���Ƶ�bh->b_data���൱�ڰѸ��ڵ�����ݸ��Ƶ��ϱ��´���������飬�ڿո��ڵ�
	memmove(bh->b_data, EXT4_I(inode)->i_data,
		sizeof(EXT4_I(inode)->i_data));

	/* set size of new block */
    //nehָ��bh�׵�ַ����Щ�ڴ��������ǰ����bh->b_data���Ƶĸ��ڵ��ͷ�ṹext4_extent_header
	neh = ext_block_hdr(bh);
	/* old root could have indexes or leaves
	 * so calculate e_max right way */
	if (ext_depth(inode))//���ext4 extent B+���������ڵ㣬nehָ����ڴ���Ϊ�����ڵ�
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
	else//���ext4 extent B+��û�������ڵ㣬ֻ�и��ڵ㣬nehָ����ڴ���ΪҶ�ӽ��
		neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	ext4_extent_block_csum_set(inode, neh);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto out;

	/* Update top-level index: num,max,pointer */
    //����neh��ָ��ext4 extent B+���ڵ�
	neh = ext_inode_hdr(inode);
    //���ڵ�����ֻ��һ��Ҷ�ӽڵ��ext4_extent�ṹ��ʹ�û���ֻ��һ�������ڵ��ext4_extent_idx�ṹ��ʹ��
	neh->eh_entries = cpu_to_le16(1);
    //���ǰ�ǰ���´����������ڵ����Ҷ�ӽڵ��������newblock��¼�����ڵ��һ��ext4_extent_idx�ṹ��ei_leaf_lo��ei_leaf_hi��Ա��
    //�����ͽ����˸��ڵ����´�������������newblock��Ҷ�ӽ��������ڵ����ϵ����Ϊͨ�����ڵ��һ��ext4_extent_idx�ṹ��
    //ei_leaf_lo��ei_leaf_hi��Ա���Ϳ����ҵ�����´�����Ҷ�ӽڵ���������ڵ��������newblock
	ext4_idx_store_pblock(EXT_FIRST_INDEX(neh), newblock);
    //���neh->eh_depth��0��˵��֮ǰext4 extent B+�������0����ֻ�и��ڵ�
	if (neh->eh_depth == 0) {
		/* Root extent block becomes index block */
        //��ǰB+��ֻ�и��ڵ㣬û�������ڵ㡣���ڸ��ڵ���Ϊ�����ڵ㣬���Ǽ�����ڵ��������ɵ�ext4_extent_idx�ṹ������4
		neh->eh_max = cpu_to_le16(ext4_ext_space_root_idx(inode, 0));
        //��ǰB+��ֻ�и��ڵ㣬û�������ڵ㣬���ڵ㶼��ext4_extent�ṹ������B+�����ڵ��������newblock���Ҷ�ӽڵ㡣���ڵ���������ڵ㣬
        //���ԭ����һ��ext4_extent�ṹҪ����ext4_extent_idx�ṹ���±߸�ֵ���ǰ�ԭ���ĸ��ڵ��һ��ext4_extent����ʼ�߼����ַ
        //��ֵ�����ڸ��ڵ�ĵ�һ��ext4_extent_idx����ʼ�߼����ַ
		EXT_FIRST_INDEX(neh)->ei_block =
			EXT_FIRST_EXTENT(neh)->ee_block;
	}
	ext_debug("new root: num %d(%d), lblock %d, ptr %llu\n",
		  le16_to_cpu(neh->eh_entries), le16_to_cpu(neh->eh_max),
		  le32_to_cpu(EXT_FIRST_INDEX(neh)->ei_block),
		  ext4_idx_pblock(EXT_FIRST_INDEX(neh)));
    //ext4 extent B+��������һ�������ڵ��Ҷ�ӽ�㣬����������newblock���Ǹ�������ȼ�1
	le16_add_cpu(&neh->eh_depth, 1);
	ext4_mark_inode_dirty(handle, inode);
out:
	brelse(bh);

	return err;
}

/*
 * ext4_ext_create_new_leaf:
 * finds empty index and adds new leaf.
 * if no free index is found, then it requests in-depth growing.
 */
/*ִ�е��ú�����˵��ext4 extent B+������newext->ee_block�йص�Ҷ�ӽڵ�ext4_extent�ṹ�����ˣ���Ҫ���ݡ����ȳ�������Ҷ�ӽڵ��ϵ�ÿһ��
�����ڵ���û�п���entry�ģ��еĻ���¼��һ�������ڵ�������at������ִ��ext4_ext_split():��ext4 extent B+��at��һ�������ڵ㵽Ҷ��
�ڵ㣬���ÿһ�㶼�����µ������ڵ㣬Ҳ����Ҷ�ӽڵ㡣���᳢�԰������ڵ�path[at~depth-1].p_hdrָ���ext4_extent_idx�ṹ�ĺ�ߵ�
ext4_extent_idx�ṹ��path[depth].p_extָ���ext4_extent�ṹ��ߵ�ext4_extent�ṹ���ƶ����´�����Ҷ�ӽڵ�������ڵ㡣�����Ϳ��ܱ�֤
ext4 extent B+���У���newext->ee_block�йص�Ҷ�ӽڵ��п���entry���ܴ��newext�����ext4 extent B+�������ڵ��ext4_extent_idx�ṹ
Ҳ�����ˣ���ִ��ext4_ext_grow_indepth()��ext4 extent B+��root�ڵ��µĴ���һ���µ������ڵ�(����Ҷ�ӽڵ�)����ʱext4 extent B+����2���
�����ڵ�(����Ҷ�ӽڵ�)�ǿյģ����Դ�Ŷ��ext4_extent_idx�ṹ�����п���entry�ˡ�Ȼ������goto repeat����ִ��ext4_ext_split()�ָ�
���������ڵ��Ҷ�ӽڵ㡣��֮����ext4_ext_create_new_leaf()��������ǰ�����ִ�е�ext4_ext_find_extent()�Ҵ��path[depth].p_extָ��
��Ҷ�ӽڵ��п���entry�����Դ��newext*/
static int ext4_ext_create_new_leaf(handle_t *handle, struct inode *inode,
				    unsigned int flags,
				    struct ext4_ext_path *path,
				    struct ext4_extent *newext)
{
	struct ext4_ext_path *curp;
	int depth, i, err = 0;

repeat:
	i = depth = ext_depth(inode);

	/* walk up to the tree and look for free index entry */
	curp = path + depth;//curp����ָ��ext4 extent B+��Ҷ�ӽڵ�
	
	//��while�Ǵ�ext4 extent B+��Ҷ�ӽڵ㿪ʼ������һֱ�������ڵ㣬�������ڵ����Ҷ�ӽڵ��ext4_extent_idx��ext4_extent�����Ƿ����
	//�������eh_max����������EXT_HAS_FREE_INDEX(curp)����0�����򷵻�1.�Ӹ�whileѭ���˳�ʱ�������ֿ��ܣ�1:curp��NULL��curpָ�������
	//�ڵ��Ҷ�ӽڵ��п���ext4_extent_idx��ext4_extent��ʹ�ã�2:i��0��ext4 extent B+�������ڵ��Ҷ�ӽڵ�ext4_extent_idx��ext4_extent����������û�п���ext4_extent_idx��ext4_extent��ʹ��
	while (i > 0 && !EXT_HAS_FREE_INDEX(curp)) {
		i--;
		curp--;
	}

	/* we use already allocated block for index block,
	 * so subsequent data blocks should be contiguous */
    //ext4 extent B+�������ڵ����Ҷ�ӽڵ��п���ext4_extent_idx��ext4_extent��ʹ�á���ʱ��i��ʾext4 extent B+����һ���п���
    //ext4_extent_idx��ext4_extent��ʹ�á�newext��Ҫ����ext4_extent B+����ext4_extent������ext4_extent B+���ĵ�i���Ҷ�ӽڵ����
    //��i�������ڵ��±ߵ�Ҷ�ӽڵ�*/
	if (EXT_HAS_FREE_INDEX(curp)) {
        /***/
		/* if we found index with free entry, then use that
		 * entry: create all needed subtree and add new leaf */
		/*����ִ�е�ext4_ext_split()������˵��ext4 extent B+������newext->ee_block�йص�Ҷ�ӽڵ�ext4_extent�ṹ�����ˡ�
           ���Ǵ�ext4 extent B+��at��һ�������ڵ㵽Ҷ�ӽڵ㣬���ÿһ�㶼�����µ������ڵ㣬Ҳ����Ҷ�ӽڵ㡣���᳢�԰�
           �����ڵ�path[at~depth].p_hdrָ���ext4_extent_idx�ṹ�ĺ�ߵ�ext4_extent_idx�ṹ��path[depth].p_extָ���
           ext4_extent�ṹ��ߵ�ext4_extent�ṹ���ƶ����´�����Ҷ�ӽڵ�������ڵ㡣�������ܱ�֤ext4 extent B+���У�
           ��newext->ee_block�йص�Ҷ�ӽڵ��п���entry�����ܴ��newext���ext4_extent�ṹ�ˡ�*/
		err = ext4_ext_split(handle, inode, flags, path, newext, i);
		if (err)
			goto out;

		/* refill path */
		ext4_ext_drop_refs(path);
        //ext4_ext_split()��ext4_extent B+�������ؽ��ͷָ�����ٴ���ext4_extent B+��������ʼ�߼����ַ�ӽ�newext->ee_block
        //�������ڵ��Ҷ�ӽ��
		path = ext4_ext_find_extent(inode,
				    (ext4_lblk_t)le32_to_cpu(newext->ee_block),
				    path);
		if (IS_ERR(path))
			err = PTR_ERR(path);
	} else {
	/*�������֧��ext4 extent B+�������ڵ��ext4_extent_idx��Ҷ�ӽڵ��ext4_extent����ȫ������ȫ��û�п�����Ŀentry��
	  ����˵ext4 extent B+��ȫ�����ˣ�ֻ������ִ��ext4_ext_grow_indepth()����ext4 extent B+��Ҷ�ӽڵ���������ڵ��ˡ�������ǳ��ؼ�*/

        //���newext->ee_block����һ���µ�����飬��Ϊ�µ������ڵ����Ҷ�ӽڵ���ӵ�ext4 extent B+�����ڵ��·��������൱��
        //��ext4 extent B+��������һ���µĽڵ�
		/* tree is full, time to grow in depth */
		err = ext4_ext_grow_indepth(handle, inode, flags, newext);
		if (err)
			goto out;

		/* refill path */
		ext4_ext_drop_refs(path);
        //�����ext4 extent B+�����ڵ��·�������һ���µ���������Ҷ�ӽڵ㣬��������ext4 extent B+��find_extent��ע�⣬ext4 extent B+��
        //��ʱ������������һ�������ڵ����Ҷ�ӽڵ㣬���������������һ�㣬������û�б仯������ext4_ext_find_extent()�ǿ϶����ҵ�
        //��ʼ�߼����ַ�ӽ�newext->ee_block�Ĳ�������ڵ����Ҷ�ӽڵ��ext4_extent_idx��ext4_extent�ṹ�����ң���ʱ����B+�����ڵ�
        //�·������ӵ������ڵ��п���entry�������ʱext4 extent B+��Ҷ�ӽڵ��п���entry�����ext4_ext_create_new_leaf()���غ��ֱ�Ӱ�
        //newext����Ҷ�ӽڵ㡣���Ҷ�ӽڵ�entry���������±ߵ�if (path[depth].p_hdr->eh_entries == path[depth].p_hdr->eh_max)������
        //�Ǿ�goto repeat��֧��ִ��ext4_ext_split()�ָ�ext4_extent B+���������µ�Ҷ�ӽ����������ڵ㣬ʹ��newext�ܲ����ȥ��
		path = ext4_ext_find_extent(inode,
				   (ext4_lblk_t)le32_to_cpu(newext->ee_block),
				    path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}

		/*
		 * only first (depth 0 -> 1) produces free space;
		 * in all other cases we have to split the grown tree
		 */
		depth = ext_depth(inode);
        //path[depth].p_hdrָ���Ҷ�ӽ�㱣��ext4_extent�ṹ�ﵽeh_max����ext4_extent�ṹ��������goto repeatѰ���п���ext4_extent_idr�������ڵ㣬
        //Ȼ��ָ�ext4 extent B+������if������ǳ����ģ���Ϊ֮����ִ�е�ext4_ext_create_new_leaf()��������Ϊ�ܶ�Ҷ�ӽ���ext4_extent
        //�ṹ�����ˣ����ϱ�ext4_ext_find_extent()ֻ����ext4 extent B+��root�ڵ��±�������һ�������ڵ�(����Ҷ�ӽڵ�)���ѡ�
		if (path[depth].p_hdr->eh_entries == path[depth].p_hdr->eh_max) {
			/* now we need to split */
			goto repeat;
		}
	}

out:
	return err;
}

/*
 * search the closest allocated block to the left for *logical
 * and returns it at @logical + it's physical address at @phys
 * if *logical is the smallest allocated block, the function
 * returns 0 at @phys
 * return value contains 0 (success) or error code
 */
//logical = le32_to_cpu(ex->ee_block) + ee_len - 1
static int ext4_ext_search_left(struct inode *inode,
				struct ext4_ext_path *path,
				ext4_lblk_t *logical, ext4_fsblk_t *phys)//logical����map->m_lblk
{
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	int depth, ee_len;

	if (unlikely(path == NULL)) {
		EXT4_ERROR_INODE(inode, "path == NULL *logical %d!", *logical);
		return -EIO;
	}
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;

	/* usually extent in the path covers blocks smaller
	 * then *logical, but it can be that extent is the
	 * first one in the file */

	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
    //logical��map->m_lblkС��path[depth].p_ext����ʼ�߼����ַ
	if (*logical < le32_to_cpu(ex->ee_block)) {
        //�����ж�ext4 extent B+��Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ�ǲ���path[depth].p_extָ���ext4_extent��Ϊʲô���߻������?????????
		if (unlikely(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex)) {
			EXT4_ERROR_INODE(inode,
					 "EXT_FIRST_EXTENT != ex *logical %d ee_block %d!",
					 *logical, le32_to_cpu(ex->ee_block));
			return -EIO;
		}
		while (--depth >= 0) {
            //ext4 extent B+���������ڵ��ͷ���
			ix = path[depth].p_idx;
            //ext4 extent B+���������ڵ�ĵ�һ��ext4_extent_idx�ṹ�ǲ���path[depth].p_idxָ����Ǹ�ext4_extent_idx��Ϊʲô���߻������?????????
			if (unlikely(ix != EXT_FIRST_INDEX(path[depth].p_hdr))) {
				EXT4_ERROR_INODE(inode,
				  "ix (%d) != EXT_FIRST_INDEX (%d) (depth %d)!",
				  ix != NULL ? le32_to_cpu(ix->ei_block) : 0,
				  EXT_FIRST_INDEX(path[depth].p_hdr) != NULL ?
		le32_to_cpu(EXT_FIRST_INDEX(path[depth].p_hdr)->ei_block) : 0,
				  depth);
				return -EIO;
			}
		}
		return 0;
	}

	if (unlikely(*logical < (le32_to_cpu(ex->ee_block) + ee_len))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d < ee_block %d + ee_len %d!",
				 *logical, le32_to_cpu(ex->ee_block), ee_len);
		return -EIO;
	}
    //logical����Ϊex->ee_block+ee_len
	*logical = le32_to_cpu(ex->ee_block) + ee_len - 1;
	*phys = ext4_ext_pblock(ex) + ee_len - 1;
	return 0;
}

/*
 * search the closest allocated block to the right for *logical
 * and returns it at @logical + it's physical address at @phys
 * if *logical is the largest allocated block, the function
 * returns 0 at @phys
 * return value contains 0 (success) or error code
 */
//path[depth].p_ext����Ҷ�ӽڵ����һ��ext4_extent�ṹ�����ҵ�path[depth].p_ext��ߵ�ext4_extent�ṹ��ret_ex��ret_ex����ʼ�߼����ַ����
//logical ������ѡ��ext4 extent B+����ߵ������ڵ��µ�Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ��ret_ex��ret_ex����ʼ�߼����ַ����logical
static int ext4_ext_search_right(struct inode *inode,
				 struct ext4_ext_path *path,
				 ext4_lblk_t *logical, ext4_fsblk_t *phys,//logical��map->m_lblk
				 struct ext4_extent **ret_ex)
{
	struct buffer_head *bh = NULL;
	struct ext4_extent_header *eh;
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	ext4_fsblk_t block;
	int depth;	/* Note, NOT eh_depth; depth from top of tree */
	int ee_len;

	if (unlikely(path == NULL)) {
		EXT4_ERROR_INODE(inode, "path == NULL *logical %d!", *logical);
		return -EIO;
	}
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;

	/* usually extent in the path covers blocks smaller
	 * then *logical, but it can be that extent is the
	 * first one in the file */

	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
	if (*logical < le32_to_cpu(ex->ee_block)) {
		if (unlikely(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex)) {
			EXT4_ERROR_INODE(inode,
					 "first_extent(path[%d].p_hdr) != ex",
					 depth);
			return -EIO;
		}
		while (--depth >= 0) {
			ix = path[depth].p_idx;
			if (unlikely(ix != EXT_FIRST_INDEX(path[depth].p_hdr))) {
				EXT4_ERROR_INODE(inode,
						 "ix != EXT_FIRST_INDEX *logical %d!",
						 *logical);
				return -EIO;
			}
		}
		goto found_extent;
	}

	if (unlikely(*logical < (le32_to_cpu(ex->ee_block) + ee_len))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d < ee_block %d + ee_len %d!",
				 *logical, le32_to_cpu(ex->ee_block), ee_len);
		return -EIO;
	}
    //ex����Ҷ�ӽڵ����һ��ext4_extent�ṹ
	if (ex != EXT_LAST_EXTENT(path[depth].p_hdr)) {
		/* next allocated block in this leaf */
        //ext4_extent B+��Ҷ�ӽڵ�ѡ��ex��ߵ�ext4_extent�ṹ�������Ҫѡ���ext4_extent
		ex++;
		goto found_extent;
	}

    /*������˵��ex��Ҷ�ӽ�����һ��ext4_extent�ṹ���Ǿʹ�B+����ײ������ڵ�--depth����������ֱ��path[depth].p_idx����
     �����ڵ����һ��ext4_extent_idx�ṹ��goto got_index��֧���ٴ�B+��depth�����ڵ���������������ҵ�ÿһ��Ҷ�ӽ������
     ext4_extent_idx�ṹ�����ҵ���ײ������ڵ����ext4_extent_idx�ṹ�ĵ�һ��ext4_extent�ṹ����Ϊ�ҵ����߼����ַ*/
    
	/* go up and search for index to the right */
    //�޷���ext4_extent B+��Ҷ�ӽ���ҵ����ʵ�ext4_extent�ṹ��ȥ�����ڵ���
	while (--depth >= 0) {//depth--ָ��ext4_extent B+���������ڵ�
	    //ix��ext4_extent B+�������ڵ��ext4_extent_idx
		ix = path[depth].p_idx;
        //path[depth].p_idxָ���ext4_extent_idx���������ڵ����һ��
		if (ix != EXT_LAST_INDEX(path[depth].p_hdr))
			goto got_index;
	}

	/* we've gone up to the root and found no index to the right */
	return 0;

got_index:
	/* we've found index to the right, let's
	 * follow it and find the closest allocated
	 * block to the right */
	//ixָ��ext4 extent B+�����������һ��ext4_extent_idx�ṹ
	ix++;
    /*���whileѭ����֤��ext4 extent B+�ϲ������ڵ����������������ҵ������ڵ��һ��ext4_extent_idx�ṹ�����ҵ�ext4_extent_idx�ṹ
    ��Ӧ��������ַblock���Ӹ�block��ȡ4K���ݣ�����ext4 extent B+����һ�������ڵ㡣��ͨ����������ڵ��һ��ext4_extent_idx�ṹ�ҵ�
    ��һ��*/
    //ix���ext4 extent B+�����ڵ��߼����ַӳ���������ַ��������̵�������ַ������ix���ext4 extent B+�����ڵ��4K���ݣ�
    //ix������ַblock��4K����=ext4 extent B+�����ڵ�ext4_extent_headerͷ���+N��ext4_extent_idx�ṹ
	block = ext4_idx_pblock(ix);
	while (++depth < path->p_depth) {//���whileѭ����֤�˳�ʱ��ixָ��ext4 extent B+�����²���������
        //ix���ext4 extent B+�����ڵ�ӳ���������ַ��Ӧ��bh
		bh = sb_bread(inode->i_sb, block);
		if (bh == NULL)
			return -EIO;
        //ehָ��bh�ڴ��׵�ַ����bh��4K��ix���ext4 extent B+�����ڵ��4K���ݣ�ext4_extent_headerͷ���+N��ext4_extent_idx�ṹ���ܴ�С��4K
		eh = ext_block_hdr(bh);
		/* subtract from p_depth to get proper eh_depth */
		if (ext4_ext_check_block(inode, eh,
					 path->p_depth - depth, bh)) {
			put_bh(bh);
			return -EIO;
		}
        //ix���ext4_extent B+�������ڵ��һ��ext4_extent_idx�ṹ
		ix = EXT_FIRST_INDEX(eh);
        //ix���ext4_extent B+�������ڵ��һ��ext4_extent_idx�ṹӳ���������ַ
		block = ext4_idx_pblock(ix);
		put_bh(bh);
	}
    //���е����block��ext4_extent B+�����²������ڵ��һ��ext4_extent_idx�ṹ��Ӧ��������ַ����4K��С������鱣����
    //ext4_extent B+����Ҷ�ӽڵ㣬��ϸ���� block�����4K����=ext4_extent B+��Ҷ�ӽڵ�ͷ���ext4_extent_header + N��ext4_extent�ṹ
	bh = sb_bread(inode->i_sb, block);
	if (bh == NULL)
		return -EIO;
    //eh�ڴ���ext4_extent B+����ײ��Ҷ�ӽ��
	eh = ext_block_hdr(bh);
	if (ext4_ext_check_block(inode, eh, path->p_depth - depth, bh)) {
		put_bh(bh);
		return -EIO;
	}
    //ext4_extent B+��Ҷ�ӽڵ��һ��ext4_extent�ṹ
	ex = EXT_FIRST_EXTENT(eh);
found_extent:
    //���logical��¼����ext4_extent B+�����������ڵ��µ�Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ���߼����ַ
	*logical = le32_to_cpu(ex->ee_block);
    //�߼����ַ��Ӧ��������ַ
	*phys = ext4_ext_pblock(ex);
	*ret_ex = ex;
	if (bh)
		put_bh(bh);
	return 0;
}

/*
 * ext4_ext_next_allocated_block:
 * returns allocated block in subsequent extent or EXT_MAX_BLOCKS.
 * NOTE: it considers block number from index entry as
 * allocated block. Thus, index entries have to be consistent
 * with leaves.
 */
static ext4_lblk_t
ext4_ext_next_allocated_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	if (depth == 0 && path->p_ext == NULL)
		return EXT_MAX_BLOCKS;

	while (depth >= 0) {
		if (depth == path->p_depth) {
			/* leaf */
			if (path[depth].p_ext &&
				path[depth].p_ext !=
					EXT_LAST_EXTENT(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_ext[1].ee_block);
		} else {
			/* index */
			if (path[depth].p_idx !=
					EXT_LAST_INDEX(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_idx[1].ei_block);
		}
		depth--;
	}

	return EXT_MAX_BLOCKS;
}

/*
 * ext4_ext_next_leaf_block:
 * returns first allocated block from next leaf or EXT_MAX_BLOCKS
 */
/**/
/*�ص�eh���Ҷ�ӽڵ��ϲ�������ڵ㣬�ҵ�path[depth-1].p_idxָ���ext4_extent_idx����������ڵ�ṹext4_extent_idx����ʼ�߼����ַ
��ӽ�������߼����ַmap->m_lblk�������ҵ����������ext4_extent_idx�ṹ��ߵ�ext4_extent_idx�����ext4_extent_idx����ʼ
�߼����ַ�Ϳ��ܴ���newext->ee_block��������Ҫ����ext4 extent B+����ext4_extent(��newext)����ʼ�߼����ַ�������Ļ�newex�Ͳ���
���ҵ��������ڵ�ext4_extent_idx���±ߵ�Ҷ�ӽڵ㣬��󷵻�����µ�ext4_extent_idx����ʼ�߼����ַ��
����Ҳ�����������ڵ�ext4_extent_idx������EXT_MAX_BLOCKS*/
//ext4_ext_insert_extent->ext4_ext_next_leaf_block
static ext4_lblk_t ext4_ext_next_leaf_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

  /*ִ�е����˵��ԭ��path[depth].p_ext����Ҷ�ӽڵ������ext4_extent�߼����ַ��Χ̫С��Ҫ����ext4 extent B+��
  ����ext4_extent��newext��ʼ�߼����ַ̫�������ҵ�path[depth-1].p_ext����Ҷ�ӽڵ��ϲ�������ڵ��ext4_extent_idx�ṹ��
  ���������Ϊext4_extent_idx1.���ext4_extent_idx��ext4_ext_find_extent->ext4_ext_binsearch_idx()���ҵ�����ֵ��
  �����Ǵ�ext4 extent B+�����ϲ㵽�²㣬������ÿһ�������ڵ��в����ĸ�ext4_extent_idx��ʼ�߼����ַ��ӽ�
  �����Ҫ�����߼����ַblock��Ȼ��path[depth-1].p_idx=ext4_extent_idx1��

  ����Ҫ����ext4_extent_idx1��ߵ�ext4_extent_idx�ṹ������newex����Ҫ�������ext4_extent_idx�±ߵ�Ҷ�ӽڵ㡣

  ΪʲôҪ������������Ϊnewext����Ҫ����path[depth-1].p_idxָ��������ڵ�ext4_extent_idx1�µ�Ҷ�ӽڵ�(��path[depth].p_ext����Ҷ�ӽڵ�)
  ������newext����ʼ�߼����ַ����path[depth].p_ext����Ҷ�ӽڵ�����ext4_extent�ṹ����ʼ�߼����ַ���ǿ϶�Ҫ�Ҹ���ʼ�߼����ַ
  ����������ڵ�ext4_extent_idx�����Ǿ��ҵ�path[depth-1].p_idxָ��������ڵ�ext4_extent_idx���
  ext4_extent_idx1��newex����Ҫ���뵽ext4_extent_idx1�±ߵ������ڵ㡣

  ext4 extent B+�������ڵ����һ����ext4_extent_idx��������ɵģ������ң�ÿ��ext4_extent_idx���߼���
  ��ʼ��ַ��������Ҷ�ӽڵ��ext4_extent�ṹҲ�Ǵ�������ʼ�߼����ַ��������

  �����и����⣬��һ���ҵ���ext4_extent_idx��ʼ�߼����ַ����̫Сզ��?

  ���ң��ϱߵĽ����������������depth-1��һ�������ڵ���ҵ���path[depth-1].p_idxָ��������ڵ�ext4_extent_idx���
  ext4_extent_idx1����һ�±�if (path[depth].p_idx != EXT_LAST_INDEX(path[depth].p_hdr))��������
  ��ֻ����depth---ȥ������2�������ڵ�����,��Ȼû�ҵ���depth---ȥ������3�������ڵ���

  ���Ͻ��ͺ������Լ�������̫������
  */
	if (depth == 0)//B+��ֻ�и��ڵ�ֱ�ӷ���EXT_MAX_BLOCKS
        /* zero-tree has no leaf blocks at all */
		return EXT_MAX_BLOCKS;

	/* go to index block */
	depth--;//depth--�͵�ext4_extent B+�������²�������ڵ���

	while (depth >= 0) {
        //path[depth].p_idxָ����ʼ�߼����ַ��ӽ��������ʼ�߼����ַmap->m_lblk��ext4_extent_idx��
        //EXT_LAST_INDEX(path[depth].p_hdr)��ext4 extent B+�������ڵ����һ��ext4_extent_idx�����߲�����ȣ�
        //��Ϊ������return path[depth].p_idx��ߵ�ext4_extent_idx��path[depth].p_idx[1].ei_block��
		if (path[depth].p_idx != EXT_LAST_INDEX(path[depth].p_hdr))
            //path[depth].p_idx[1],��path[depth].p_idxָ���ext4_extent_idx��ߵ��Ǹ�ext4_extent_idx�ṹ
            /*���ص㣬���path[depth].p_idx[1]ָ��������ڵ�ext4_extent_idx�ṹ��û��ʹ�ù�����path[depth].p_idx[1].ei_block
              ��ȫ0xFFFFFFF����EXT_MAX_BLOCKS���൱��û���ҵ���Ч��ext4_extent_idx*/
			return (ext4_lblk_t) le32_to_cpu(path[depth].p_idx[1].ei_block);
        
		depth--;//��1����һ���ext4 extent B+�������ڵ�
	}

	return EXT_MAX_BLOCKS;
}

/*
 * ext4_ext_correct_indexes:
 * if leaf gets modified and modified extent is first in the leaf,
 * then we have to correct all indexes above.
 * TODO: do we need to correct tree in all cases?
 */
//�������޸�ext4 extent B+�������ڵ�����ݣ���ΪҶ�ӽڵ��и�����
static int ext4_ext_correct_indexes(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	int depth = ext_depth(inode);
	struct ext4_extent *ex;
	__le32 border;
	int k, err = 0;

	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;

	if (unlikely(ex == NULL || eh == NULL)) {
		EXT4_ERROR_INODE(inode,
				 "ex %p == NULL or eh %p == NULL", ex, eh);
		return -EIO;
	}

	if (depth == 0) {
		/* there is no tree at all */
		return 0;
	}

	if (ex != EXT_FIRST_EXTENT(eh)) {
		/* we correct tree if first leaf got modified only */
		return 0;
	}

	/*
	 * TODO: we need correction if border is smaller than current one
	 */
	k = depth - 1;
	border = path[depth].p_ext->ee_block;
	err = ext4_ext_get_access(handle, inode, path + k);
	if (err)
		return err;
	path[k].p_idx->ei_block = border;
	err = ext4_ext_dirty(handle, inode, path + k);
	if (err)
		return err;

	while (k--) {
		/* change all left-side indexes */
		if (path[k+1].p_idx != EXT_FIRST_INDEX(path[k+1].p_hdr))
			break;
		err = ext4_ext_get_access(handle, inode, path + k);
		if (err)
			break;
		path[k].p_idx->ei_block = border;
		err = ext4_ext_dirty(handle, inode, path + k);
		if (err)
			break;
	}

	return err;
}
//����ex1������ߵ�ex2������ext4_extent���߼����������ַ�Ƿ�����ţ�����ex1���Ժϲ���ex2������1�����ܺϲ�����0
int
ext4_can_extents_be_merged(struct inode *inode, struct ext4_extent *ex1,
				struct ext4_extent *ex2)
{
	unsigned short ext1_ee_len, ext2_ee_len, max_len;

	/*
	 * Make sure that both extents are initialized. We don't merge
	 * uninitialized extents so that we can be sure that end_io code has
	 * the extent that was written properly split out and conversion to
	 * initialized is trivial.
	 */
	//����ϲ�������ext4_extent������initialized״̬�������޷��ϲ�
	if (ext4_ext_is_uninitialized(ex1) || ext4_ext_is_uninitialized(ex2))
		return 0;

	if (ext4_ext_is_uninitialized(ex1))
		max_len = EXT_UNINIT_MAX_LEN;
	else
		max_len = EXT_INIT_MAX_LEN;//ext4_extent����߼������max_len��0x8000

	ext1_ee_len = ext4_ext_get_actual_len(ex1);
	ext2_ee_len = ext4_ext_get_actual_len(ex2);

    //ex1���߼��������ַ���������ex2�߼�����ʼ��ַ
	if (le32_to_cpu(ex1->ee_block) + ext1_ee_len !=
			le32_to_cpu(ex2->ee_block))
		return 0;

	/*
	 * To allow future support for preallocated extents to be added
	 * as an RO_COMPAT feature, refuse to merge to extents if
	 * this can result in the top bit of ee_len being set.
	 */
	//ex1��ex2���߼������֮�Ͳ��ܳ���max_len����Ϊext4_extent����߼������max_len��0x8000
	if (ext1_ee_len + ext2_ee_len > max_len)
		return 0;
#ifdef AGGRESSIVE_TEST
	if (ext1_ee_len >= 4)
		return 0;
#endif
    //ex1������������ַ���������ex2�������ʼ��ַ
	if (ext4_ext_pblock(ex1) + ext1_ee_len == ext4_ext_pblock(ex2))
		return 1;
	return 0;
}

/*
 * This function tries to merge the "ex" extent to the next extent in the tree.
 * It always tries to merge towards right. If you want to merge towards
 * left, pass "ex - 1" as argument instead of "ex".
 * Returns 0 if the extents (ex and ex+1) were _not_ merged and returns
 * 1 if they got merged.
 */
//���԰�ex��ߵ�ex+1��ex+2 ....��Щext4_extent���߼����������ַѭ���ϲ���ex����Ȼ�ϲ�
//��ǰ��������ext4_extent���߼����ַ��������ַǰ�������
static int ext4_ext_try_to_merge_right(struct inode *inode,
				 struct ext4_ext_path *path,
				 struct ext4_extent *ex)
{
	struct ext4_extent_header *eh;
	unsigned int depth, len;
	int merge_done = 0;
	int uninitialized = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

    //ex������ext4_extent B+��Ҷ�ӽڵ������һ��ext4_extent�ṹ������զ�ϲ�
	while (ex < EXT_LAST_EXTENT(eh)) {
        //����ex1������ߵ�ex + 1������ext4_extent���߼����������ַ�Ƿ�����ţ���ex1����Ժϲ���ex2������1�����ܺϲ�����0
		if (!ext4_can_extents_be_merged(inode, ex, ex + 1))
			break;
		/* merge with next extent! */
		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
        //ex->ee_len���¸�ֵΪex��ex+1������ext4_extent���߼�����֮��
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
				+ ext4_ext_get_actual_len(ex + 1));
		if (uninitialized)
			ext4_ext_mark_uninitialized(ex);

        //ex+1�������һ��ext4_extent�ṹ
		if (ex + 1 < EXT_LAST_EXTENT(eh)) {
            //len��ex+1���ext4_extent�ṹ��ĳ���
			len = (EXT_LAST_EXTENT(eh) - ex - 1)
				* sizeof(struct ext4_extent);
            //��ex+1���ext4_extent�ṹ������ݸ��Ƶ�ex+2���������ǻ��ex+2�����ݸ�����
            //��ΪʲôҪ��������?????
			memmove(ex + 1, ex + 2, len);
		}
        //ext4 extent B+��extent����1
		le16_add_cpu(&eh->eh_entries, -1);
		merge_done = 1;//��1��ʾ�ϲ��ɹ�
		WARN_ON(eh->eh_entries == 0);
		if (!eh->eh_entries)
			EXT4_ERROR_INODE(inode, "eh->eh_entries = 0!");
	}

	return merge_done;
}

/*
 * This function does a very simple check to see if we can collapse
 * an extent tree with a single extent tree leaf block into the inode.
 */
//���ext4_extent B+�������1������Ҷ�ӽ���к��ٵ�ext4_extent�ṹ�����Ҷ�ӽ���ext4_extent�ṹ���Ƶ�root�ڵ㣬
//����ԭ������Ҷ�ӽڵ�ext4_extent�ṹ�����ݵ�������ͷŻ�ext4�ļ�ϵͳ����ʡ�ռ�
static void ext4_ext_try_to_merge_up(handle_t *handle,
				     struct inode *inode,
				     struct ext4_ext_path *path)
{
	size_t s;
    //����ext4_extent B+��root�ڵ������ɶ��ٸ�ext4_extent�ṹ��max_root
	unsigned max_root = ext4_ext_space_root(inode, 0);
	ext4_fsblk_t blk;

    //ext4_extent B+����ȱ�����1����root�����ڵ�+Ҷ�ӽ�㡣���ң�root�ڵ��entry��������1����ֻ����һ��Ҷ�ӽ�㡣����Ҷ�ӽ��
    //��ext4_extent�����ܴ���max_root��������������һ����������ֱ��return��
	if ((path[0].p_depth != 1) ||
	    (le16_to_cpu(path[0].p_hdr->eh_entries) != 1) ||
	    (le16_to_cpu(path[1].p_hdr->eh_entries) > max_root))
		return;

	/*
	 * We need to modify the block allocation bitmap and the block
	 * group descriptor to release the extent tree block.  If we
	 * can't get the journal credits, give up.
	 */
	if (ext4_journal_extend(handle, 2))
		return;

	/*
	 * Copy the extent data up to the inode
	 */
	//root�ڵ������ڵ㱣��������ţ���4K��С����鱣����Ҷ�ӽ���ext4_extent������
	blk = ext4_idx_pblock(path[0].p_idx);
    //Ҷ�ӽڵ��ext4_extent�ṹ������Ӧ���ֽڿռ��s
	s = le16_to_cpu(path[1].p_hdr->eh_entries) *
		sizeof(struct ext4_extent_idx);
	s += sizeof(struct ext4_extent_header);//�ټ���Ҷ�ӽ���ͷ����ֽڿռ�

    //�±����ǰ�Ҷ�ӽڵ��ext4_extent�ṹ�����ݸ��Ƶ�root �ڵ��ڴ�
	memcpy(path[0].p_hdr, path[1].p_hdr, s);
	path[0].p_depth = 0;//ext4_extent B+��root�ڵ������0
	//EXT_FIRST_EXTENT(path[0].p_hdr)��root�ڵ��һ��ext4_extent�ṹ���ڴ��ַ��(path[1].p_ext - EXT_FIRST_EXTENT(path[1].p_hdr))
	//�Ǽ���ԭ��Ҷ�ӽڵ��У�path[1].p_extָ���ext4_extent�ṹ�ڴ��ַ���һ��ext4_extent�ṹ�ڴ��ַ֮ǰ�Ĳ�ֵ��
	//EXT_FIRST_EXTENT(path[0].p_hdr)���������ֵ������path[0].p_extָ���ext4_extent���ڴ��ַ
	path[0].p_ext = EXT_FIRST_EXTENT(path[0].p_hdr) +
		(path[1].p_ext - EXT_FIRST_EXTENT(path[1].p_hdr));
    //root�ڵ�����µ�Ҷ�ӽ�������ܱ����ext4_extent�ṹ����
	path[0].p_hdr->eh_max = cpu_to_le16(max_root);

    //��ԭ������Ҷ�ӽڵ��ext4_extent�����ݵ�������ͷŻ�ext4�ļ�ϵͳ
	brelse(path[1].p_bh);
	ext4_free_blocks(handle, inode, NULL, blk, 1,
			 EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET |
			 EXT4_FREE_BLOCKS_RESERVE);
}

/*
 * This function tries to merge the @ex extent to neighbours in the tree.
 * return 1 if merge left else 0.
 */
//���԰�ex���ext4_extent�ṹ���߼����������ַ�ϲ���ex�����������ext4_extent B+�������1������Ҷ�ӽ���к��ٵ�ext4_extent�ṹ��
//���԰�Ҷ�ӽ���ext4_extent�ṹ�ƶ���root�ڵ�
static void ext4_ext_try_to_merge(handle_t *handle,
				  struct inode *inode,
				  struct ext4_ext_path *path,
				  struct ext4_extent *ex) {
	struct ext4_extent_header *eh;
	unsigned int depth;
	int merge_done = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	if (ex > EXT_FIRST_EXTENT(eh))
        //���԰�(ex-1)��ߵ�ex��ex+1 ....��Щext4_extentѭ���ϲ���ex-1,��һ�κϲ��򷵻�1
		merge_done = ext4_ext_try_to_merge_right(inode, path, ex - 1);

	if (!merge_done)
        //�ϱ�û�з���ext4_extent�ϲ����������԰�ex��ߵ�ex+1��ex+2 ....��Щext4_extentѭ���ϲ���ex
		(void) ext4_ext_try_to_merge_right(inode, path, ex);
    
    //���ext4_extent B+�������1������Ҷ�ӽ���к��ٵ�ext4_extent�ṹ�����Ҷ�ӽ���ext4_extent�ṹ�ƶ���root�ڵ㣬
    //����ԭ������Ҷ�ӽڵ�ext4_extent�ṹ�����ݵ�������ͷŻ�ext4�ļ�ϵͳ����ʡ�ռ�
	ext4_ext_try_to_merge_up(handle, inode, path);
}

/*
 * check if a portion of the "newext" extent overlaps with an
 * existing extent.
 *
 * If there is an overlap discovered, it updates the length of the newext
 * such that there will be no overlap, and then returns 1.
 * If there is no overlap found, it returns 0.
 */
static unsigned int ext4_ext_check_overlap(struct ext4_sb_info *sbi,
					   struct inode *inode,
					   struct ext4_extent *newext,
					   struct ext4_ext_path *path)
{
	ext4_lblk_t b1, b2;
	unsigned int depth, len1;
	unsigned int ret = 0;

	b1 = le32_to_cpu(newext->ee_block);
	len1 = ext4_ext_get_actual_len(newext);
	depth = ext_depth(inode);
	if (!path[depth].p_ext)
		goto out;
	b2 = EXT4_LBLK_CMASK(sbi, le32_to_cpu(path[depth].p_ext->ee_block));

	/*
	 * get the next allocated block if the extent in the path
	 * is before the requested block(s)
	 */
	if (b2 < b1) {
		b2 = ext4_ext_next_allocated_block(path);
		if (b2 == EXT_MAX_BLOCKS)
			goto out;
		b2 = EXT4_LBLK_CMASK(sbi, b2);
	}

	/* check for wrap through zero on extent logical start block*/
	if (b1 + len1 < b1) {
		len1 = EXT_MAX_BLOCKS - b1;
		newext->ee_len = cpu_to_le16(len1);
		ret = 1;
	}

	/* check for overlap */
	if (b1 + len1 > b2) {
		newext->ee_len = cpu_to_le16(b2 - b1);
		ret = 1;
	}
out:
	return ret;
}

/*
 * ext4_ext_insert_extent:
 * tries to merge requsted extent into the existing extent or
 * inserts requested extent as new one into the tree,
 * creating new leaf in the no-space case.
 */
//ext4_ext_map_blocks()->ext4_ext_handle_uninitialized_extents()/ext4_ext_handle_unwritten_extents()->
//ext4_ext_convert_to_initialized()->ext4_split_extent()->ext4_split_extent_at()->ext4_ext_insert_extent()

/*ʲôʵ�ʻ�ִ��ext4_ext_insert_extent()����?������������1:ext4_ext_map_blocks()Ϊmap��ext4 extent B+���Ҳ����߼����ַ�ӽ���
ext4_extent�ṹ����Ϊmap����һ���µ�ext4_extent�ṹ��Ȼ��ִ��ext4_ext_insert_extent()������µ�ext4_extent�ṹ����ext4 extent B+����
���2:��ext4_split_extent_at()�У���path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χ�ָ�����Σ��Ѻ����߼��鷶Χ��Ӧ��
ext4_extent�ṹִ��ext4_ext_insert_extent()����ext4 extent B+����
*/

/*���ȳ��԰�newext�ϲ���ex(��path[depth].p_ext)������(ex+1)������(ex-1)ָ���ext4_extent�ṹ���ϲ������ܿ��̣��ϲ��ɹ���ֱ�ӷ��ء�
���ſ�ext4 extent B+������newext->ee_block(���Ҫ����B+����ext4_extent�ṹ����ʼ�߼����ַ)�йص�Ҷ�ӽڵ��Ƿ�ext4_extent�ṹ������
���Ƿ��п���entry��û�п���entry�Ļ���ִ��ext4_ext_create_new_leaf()�����µ������ڵ��Ҷ�ӽڵ㣬�����Ϳ��Ա�֤
ext4_ext_create_new_leaf()->ext4_ext_find_extent()ִ�к�path[depth].p_extָ���ext4_extent�ṹ���ڵ�Ҷ�ӽڵ��п���entry�����Դ��
newext�������Ǹú���has_space��֧��ֻ�Ǽ򵥵İ�newex����path[depth].p_extǰ���ext4_extentλ�ô�����󻹻�ִ��ext4_ext_try_to_merge()
���԰�ex���ext4_extent�ṹ���߼����������ַ�ϲ���ex�����᳢�԰�Ҷ�ӽ���ext4_extent�ṹ�ƶ���root�ڵ㣬��ʡ�ռ�
*/
int ext4_ext_insert_extent(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext, int flag)//newext����Ҫ����extent B+����ext4_extent
{
	struct ext4_extent_header *eh;
	struct ext4_extent *ex, *fex;
	struct ext4_extent *nearex; /* nearest extent */
	struct ext4_ext_path *npath = NULL;
	int depth, len, err;
	ext4_lblk_t next;
	unsigned uninitialized = 0;
	int flags = 0;

	if (unlikely(ext4_ext_get_actual_len(newext) == 0)) {
		EXT4_ERROR_INODE(inode, "ext4_ext_get_actual_len(newext) == 0");
		return -EIO;
	}
    //ext4 extent B+�����
	depth = ext_depth(inode);
    //ext4 extent B+��Ҷ�ӽڵ�����ʼ�߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	ex = path[depth].p_ext;
	eh = path[depth].p_hdr;
	if (unlikely(path[depth].p_hdr == NULL)) {
		EXT4_ERROR_INODE(inode, "path[%d].p_hdr == NULL", depth);
		return -EIO;
	}

    /*�±����if (ex && !(flag & EXT4_GET_BLOCKS_PRE_IO))�жϣ����ж�newex��ex��exǰ�ߵ�ext4_extent�ṹ��ex��ߵ�ext4_extent�ṹ
     �߼����ַ��Χ�Ƿ�����ţ��ǵĻ����ܽ����ߺϲ�����!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
     �ܺϲ���Ҫ����һ����������:����ϲ�������ext4_extent������initialized״̬�������޷��ϲ�*/
    
	/* try to insert block into found extent and return */
	if (ex && !(flag & EXT4_GET_BLOCKS_PRE_IO)) {//if����

		/*
		 * Try to see whether we should rather test the extent on
		 * right from ex, or from the left of ex. This is because
		 * ext4_ext_find_extent() can return either extent on the
		 * left, or on the right from the searched position. This
		 * will make merging more effective.
		 */
		//newext��Ҫ�����ex�߼���ַ��Χ��ߣ�����newex�޷�����ex��ֻ����취���뵽ex��ߵ��Ǹ�ext4_extent�ṹ
		if (ex < EXT_LAST_EXTENT(eh) &&
		    (le32_to_cpu(ex->ee_block) +
		    ext4_ext_get_actual_len(ex) <
		    le32_to_cpu(newext->ee_block))) {
			ex += 1;//ex++  ָ���ߵ�ext4_extent�ṹ
			goto prepend;
        //newext��Ҫ�����ex�߼���ַ��Χǰ�ߣ�����newex�޷�����ex��ֻ����취���뵽exǰ�ߵ��Ǹ�ext4_extent�ṹ
		} else if ((ex > EXT_FIRST_EXTENT(eh)) &&
			   (le32_to_cpu(newext->ee_block) +
			   ext4_ext_get_actual_len(newext) <
			   le32_to_cpu(ex->ee_block)))
			ex -= 1;

        //������п����ϱߵ�����if����������ex += 1 �� ex -= 1��ûִ�У�ex����path[depth].p_ext�Ǹ�ext4_extent�ṹ
		/* Try to append newex to the ex */
        //����ex������ߵ�newext������ext4_extent���߼����������ַ�Ƿ�����ţ�����ϲ������߼����ַ������1
        /*����ϲ�������ext4_extent������initialized״̬�������޷��ϲ�*/
		if (ext4_can_extents_be_merged(inode, ex, newext)) {
			ext_debug("append [%d]%d block to %u:[%d]%d"
				  "(from %llu)\n",
				  ext4_ext_is_uninitialized(newext),
				  ext4_ext_get_actual_len(newext),
				  le32_to_cpu(ex->ee_block),
				  ext4_ext_is_uninitialized(ex),
				  ext4_ext_get_actual_len(ex),
				  ext4_ext_pblock(ex));
			err = ext4_ext_get_access(handle, inode,
						  path + depth);
			if (err)
				return err;

			/*
			 * ext4_can_extents_be_merged should have checked
			 * that either both extents are uninitialized, or
			 * both aren't. Thus we need to check only one of
			 * them here.
			 */
			//exû�г�ʼ������uninitialized = 1
			if (ext4_ext_is_uninitialized(ex))
				uninitialized = 1;
            //��newext���߼����ַ��Χ�ϲ���ex
			ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
			if (uninitialized)
				ext4_ext_mark_uninitialized(ex);//���exδ��ʼ��
			eh = path[depth].p_hdr;
            
			nearex = ex;//nearex��ex
			
			goto merge;//��ת��merge��֧
		}

prepend:
		/* Try to prepend newex to the ex */
        //����newext������ߵ�ex������ext4_extent���߼����������ַ�Ƿ�����ţ�����ϲ������߼����ַ������1����ʱex += 1�ˣ�
        //������path[depth].p_ext�Ǹ�ext4_extent�ṹ
        /*����ϲ�������ext4_extent������initialized״̬�������޷��ϲ�*/
		if (ext4_can_extents_be_merged(inode, newext, ex)) {
			ext_debug("prepend %u[%d]%d block to %u:[%d]%d"
				  "(from %llu)\n",
				  le32_to_cpu(newext->ee_block),
				  ext4_ext_is_uninitialized(newext),
				  ext4_ext_get_actual_len(newext),
				  le32_to_cpu(ex->ee_block),
				  ext4_ext_is_uninitialized(ex),
				  ext4_ext_get_actual_len(ex),
				  ext4_ext_pblock(ex));
			err = ext4_ext_get_access(handle, inode,
						  path + depth);
			if (err)
				return err;

			/*
			 * ext4_can_extents_be_merged should have checked
			 * that either both extents are uninitialized, or
			 * both aren't. Thus we need to check only one of
			 * them here.
			 */
			//exû�г�ʼ������uninitialized = 1
			if (ext4_ext_is_uninitialized(ex))
				uninitialized = 1;
            //��ex���߼����ַ��Χ�ϲ���newext��������exΪĸ��ѽ
			ex->ee_block = newext->ee_block;
            //����ex����ʼ������ַΪnewext��������ַ
			ext4_ext_store_pblock(ex, ext4_ext_pblock(newext));
            //ex->ee_len����newext���߼������
			ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
			if (uninitialized)
				ext4_ext_mark_uninitialized(ex);
			eh = path[depth].p_hdr;
            
			nearex = ex;//nearex��ex
			
			goto merge;//��ת��merge��֧
		}
	}

    /*�ߵ����˵��ex��newexû�з����ϲ�*/
    
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
    //eh->eh_max��ext4_extent B+��Ҷ�ӽڵ����ext4_extent���������ǲ���path[depth].p_hdr����Ҷ�ӽڵ��ext4_extent�ṹ�Ƿ�����
    //û�г����Ż�����has_space��֧
	if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))
		goto has_space;

    /*������˵��ext4_extent B+Ҷ�ӽڵ�ռ䲻����*/
    
	/* probably next leaf has space for us? */
    //ext4 extent B+��Ҷ�ӽڵ����һ��ext4_extent�ṹ
	fex = EXT_LAST_EXTENT(eh);
	next = EXT_MAX_BLOCKS;//0x8000-1

    //���Ҫ�����newext��ʼ�߼����ַ����ext4 extent B+��Ҷ�ӽڵ����һ��ext4_extent�ṹ�ģ�˵����ǰ��Ҷ�ӽڵ��߼����ַ��Χ̫С��
	if (le32_to_cpu(newext->ee_block) > le32_to_cpu(fex->ee_block))
	/*�ص�eh���Ҷ�ӽڵ��ϲ�������ڵ㣬�ҵ�path[depth].p_idxָ���ext4_extent_idx����������ڵ�ṹext4_extent_idx����ʼ�߼����ַ
	��ӽ�������߼����ַmap->m_lblk�������ҵ����������ext4_extent_idx�ṹ��ߵ�ext4_extent_idx�����ext4_extent_idx����ʼ
	�߼����ַ�Ϳ��ܴ���newext->ee_block��������Ҫ����ext4 extent B+����ext4_extent(��newext)����ʼ�߼����ַ�������Ļ�newex�Ͳ���
	���ҵ��������ڵ�ext4_extent_idx���±ߵ�Ҷ�ӽڵ㣬��󷵻�����µ�ext4_extent_idx����ʼ�߼����ַ��
	����Ҳ�����������ڵ�ext4_extent_idx������EXT_MAX_BLOCKS*/
		next = ext4_ext_next_leaf_block(path);
    
	if (next != EXT_MAX_BLOCKS) {//����˵���ҵ��˺��ʵ�ext4_extent_idx
		ext_debug("next leaf block - %u\n", next);
		BUG_ON(npath != NULL);
        //next��ext4 extent B+�����ҵ��������ڵ�ext4_extent_idx����ʼ�߼����ַ������߼����ַ���󣬱���Ҫ�����newext���߼����ַ
        //�����ext4_extent_idx���߼����ַ��Χ�ڡ��±��Ǹ���next����߼���ַ����ext4 extent B+�������ϲ㵽�²㣬һ����ҵ�
        //��ʼ�߼����ַ��ӽ�next�������ڵ�ext4_extent_idx�ṹ��Ҷ�ӽڵ�ext4_extent�ṹ�����浽npath[]
		npath = ext4_ext_find_extent(inode, next, NULL);
		if (IS_ERR(npath))
			return PTR_ERR(npath);
		BUG_ON(npath->p_depth != path->p_depth);
        //����next����߼����ַ�ҵ����µ�Ҷ�ӽڵ��ext4_extent_header�ṹ
		eh = npath[depth].p_hdr;
        //Ҷ�ӽڵ��ext4_extent��û�г���eh->eh_max��������Ҷ�ӽڵ�ext4_extent�ṹû�б���
		if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max)) {
			ext_debug("next leaf isn't full(%d)\n",
				  le16_to_cpu(eh->eh_entries));
            //pathָ����next����߼����ַ�ҵ���struct ext4_ext_path
			path = npath;
            //����has_space��֧����newext���뵽����next����߼����ַ�ҵ���Ҷ�ӽڵ�
			goto has_space;
		}
		ext_debug("next leaf has no free space(%d,%d)\n",
			  le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));
	}

/*�ߵ����1:˵��ǰ��ext4_ext_next_leaf_block()û���ҵ����ʵ�ext4 extent B+�������ڵ�
ext4_extent_idx����Ҫ�����newext��ʼ�߼���ַ̫���ˣ�ext4 extent B+�������ڵ����ʼ�߼�
�鷶Χ̫С��newext�޷����롣2:ext4 extent B+��Ҷ�ӽ�㱣���ext4_extent�ṹ������û�пռ��ˣ���Ҫ����������

����һ���������Ҫ���ǣ�ext4 extent B+���ǿյ�!�ڸն�д�ļ�ʱ��B+��������0����1����ʱҶ�ӽ��ext4_extent�ṹ�����׾ͱ����ˣ�
��Ҫ���������������һ��ext4_ext_insert_extent()������ִ������*/

	/*
	 * There is no free space in the found leaf.
	 * We're gonna add a new leaf in the tree.
	 */
	if (flag & EXT4_GET_BLOCKS_METADATA_NOFAIL)
		flags = EXT4_MB_USE_RESERVED;

/*ִ�е����˵��ext4 extent B+������newext->ee_block�йص�Ҷ�ӽڵ�ext4_extent�ṹ�����ˣ���û�п���entry�ˡ�

����˵��һ�㣬�ж�Ҷ�ӽڵ�ext4_extent�ṹ���������������룬�ϱߵ�if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))��
if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))��ǰ�����ж�path[depth].p_ext����Ҷ�ӽڵ�ext4_extent�ṹ�Ƿ�����
��������path[depth].p_ext����Ҷ�ӽڵ�ext4_extent�ṹ��������£�ִ��next = ext4_ext_next_leaf_block(path)ȥ�ϲ������ڵ�����ʼ
�߼����ַ�����ext4_extent_idx�� ������ext4_extent_idx�±ߵ�Ҷ�ӽڵ��ext4_extent�ṹ��ȻҲ������

�����ϱ��жϵĵط�:���ȳ�������Ҷ�ӽڵ��ϵ�ÿһ��
�����ڵ���û�п���entry�ģ��еĻ���¼��һ�������ڵ�������at������ִ��ext4_ext_split():��ext4 extent B+��at��һ�������ڵ㵽Ҷ��
�ڵ㣬���ÿһ�㶼�����µ������ڵ㣬Ҳ����Ҷ�ӽڵ㡣���᳢�԰������ڵ�path[at~depth-1].p_hdrָ���ext4_extent_idx�ṹ�ĺ�ߵ�
ext4_extent_idx�ṹ��path[depth].p_extָ���ext4_extent�ṹ��ߵ�ext4_extent�ṹ���ƶ����´�����Ҷ�ӽڵ�������ڵ㡣�����Ϳ��ܱ�֤
ext4 extent B+���У���newext->ee_block�йص�Ҷ�ӽڵ��п���entry���ܴ��newext�����ext4 extent B+�������ڵ��ext4_extent_idx�ṹ
Ҳ�����ˣ���ִ��ext4_ext_grow_indepth()��ext4 extent B+��root�ڵ��µĴ���һ���µ������ڵ�(����Ҷ�ӽڵ�)����ʱext4 extent B+����2���
�����ڵ�(����Ҷ�ӽڵ�)�ǿյģ����Դ�Ŷ��ext4_extent_idx�ṹ�����п���entry�ˡ�Ȼ������goto repeat����ִ��ext4_ext_split()�ָ�
���������ڵ��Ҷ�ӽڵ㡣��֮����ext4_ext_create_new_leaf()��������ǰ�����ִ�е�ext4_ext_find_extent()�Ҵ��path[depth].p_extָ��
��Ҷ�ӽڵ��п���entry�����Դ��newext*/
	err = ext4_ext_create_new_leaf(handle, inode, flags, path, newext);
	if (err)
		goto cleanup;
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;

/*��������µ�path[depth].p_ext����Ҷ�ӽڵ�϶��п���entry��������λ�ÿ��Դ��newext���ext4_extent�ṹ����ֱ�Ӱ�newext���뵽Ҷ�ӽڵ�
ĳ��ext4_extentλ�ô�*/
has_space:
    //nearexָ����ʼ�߼����ַ��ӽ� newext->ee_block�����ʼ�߼����ַ��ext4_extent��newext�Ǳ���Ҫext4 extent b+����ext4_extent
	nearex = path[depth].p_ext;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto cleanup;

	if (!nearex) {//path[depth].p_ext����Ҷ�ӽڵ㻹û��ʹ�ù�һ��ext4_extent�ṹ
		/* there is no extent in this leaf, create first one */
		ext_debug("first extent in the leaf: %u:%llu:[%d]%d\n",
				le32_to_cpu(newext->ee_block),
				ext4_ext_pblock(newext),
				ext4_ext_is_uninitialized(newext),
				ext4_ext_get_actual_len(newext));
        //nearexָ��Ҷ�ӽڵ��һ��ext4_extent�ṹ��newext�Ͳ��뵽����
		nearex = EXT_FIRST_EXTENT(eh);
	} else {
	    //newext����ʼ�߼����ַ����nearex����ʼ�߼����ַ
		if (le32_to_cpu(newext->ee_block)
			   > le32_to_cpu(nearex->ee_block)) {
			/* Insert after */
			ext_debug("insert %u:%llu:[%d]%d before: "
					"nearest %p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_uninitialized(newext),
					ext4_ext_get_actual_len(newext),
					nearex);
			nearex++;//nearex++ָ���ߵ�һ��ext4_extent�ṹ
		} else {
			/* Insert before */
			BUG_ON(newext->ee_block == nearex->ee_block);
			ext_debug("insert %u:%llu:[%d]%d after: "
					"nearest %p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_uninitialized(newext),
					ext4_ext_get_actual_len(newext),
					nearex);
		}
        //���Ǽ���nearex���ext4_extent�ṹ��Ҷ�ӽڵ����һ��ext4_extent�ṹ(��Ч��)֮���ext4_extent�ṹ������ע��"��Ч��"3���֣�
        //����Ҷ�ӽڵ�ֻʹ����һ��ext4_extent����EXT_LAST_EXTENT(eh)��Ҷ�ӽڵ��һ��ext4_extent�ṹ��
		len = EXT_LAST_EXTENT(eh) - nearex + 1;
		if (len > 0) {
			ext_debug("insert %u:%llu:[%d]%d: "
					"move %d extents from 0x%p to 0x%p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_uninitialized(newext),
					ext4_ext_get_actual_len(newext),
					len, nearex, nearex + 1);
            //���ǰ�nearex���ext4_extent�ṹ ~ ���һ��ext4_extent�ṹ֮�������
            //ext4_extent�ṹ��������������ƶ�һ��ext4_extent�ṹ��С���ڳ�ԭ��
            //nearex���ext4_extent�ṹ�Ŀռ䣬�±����ǰ�newext���뵽����������ڰ�newex����ext4_extent B+����
			memmove(nearex + 1, nearex,
				len * sizeof(struct ext4_extent));
		}
	}

    /*�±��ǰ�newext����ʼ�߼����ַ����ʼ�������ʼ��ַ���߼����ַӳ���������������Ϣ��ֵ��nearex���൱�ڰ�newext��ӵ�
    Ҷ�ӽڵ�ԭ��nearex��λ�á�Ȼ��Ҷ�ӽڵ�ext4_extent������1��path[depth].p_extָ��newext*/
	le16_add_cpu(&eh->eh_entries, 1);//ext4 extent B+��Ҷ�ӽڵ�ext4_extent������1
	path[depth].p_ext = nearex;//�൱��path[depth].p_extָ��newext
	
    //nearex->ee_block��ֵΪnewext��ʼ�߼����ַ
	nearex->ee_block = newext->ee_block;
    //��newext��ʼ������ַ��ֵ��nearex
	ext4_ext_store_pblock(nearex, ext4_ext_pblock(newext));
	nearex->ee_len = newext->ee_len;//nearex->ee_len��ֵΪnewext��

/*�������newextҪô�Ѿ��ϲ�����ex��Ҫô�Ѿ�����ext4 extent B+�����±ߵ�ûɶ��Ҫ����*/
merge:
	/* try to merge extents */
	if (!(flag & EXT4_GET_BLOCKS_PRE_IO))
      //���԰�ex���ext4_extent�ṹ���߼����������ַ�ϲ���ex�����ң����ext4_extent B+�������1������Ҷ�ӽ���к��ٵ�ext4_extent�ṹ��
      //���԰�Ҷ�ӽ���ext4_extent�ṹ�ƶ���root�ڵ㣬��ʡ�ռ���ѡ�
		ext4_ext_try_to_merge(handle, inode, path, nearex);


	/* time to correct all indexes above */
    //�������޸�ext4 extent B+�������ڵ�����ݣ���ΪҶ�ӽڵ��и�����
	err = ext4_ext_correct_indexes(handle, inode, path);
	if (err)
		goto cleanup;

	err = ext4_ext_dirty(handle, inode, path + path->p_depth);

cleanup:
	if (npath) {
		ext4_ext_drop_refs(npath);
		kfree(npath);
	}
	return err;
}

static int ext4_fill_fiemap_extents(struct inode *inode,
				    ext4_lblk_t block, ext4_lblk_t num,
				    struct fiemap_extent_info *fieinfo)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent *ex;
	struct extent_status es;
	ext4_lblk_t next, next_del, start = 0, end = 0;
	ext4_lblk_t last = block + num;
	int exists, depth = 0, err = 0;
	unsigned int flags = 0;
	unsigned char blksize_bits = inode->i_sb->s_blocksize_bits;

	while (block < last && block != EXT_MAX_BLOCKS) {
		num = last - block;
		/* find extent for this block */
		down_read(&EXT4_I(inode)->i_data_sem);

		if (path && ext_depth(inode) != depth) {
			/* depth was changed. we have to realloc path */
			kfree(path);
			path = NULL;
		}

		path = ext4_ext_find_extent(inode, block, path);
		if (IS_ERR(path)) {
			up_read(&EXT4_I(inode)->i_data_sem);
			err = PTR_ERR(path);
			path = NULL;
			break;
		}

		depth = ext_depth(inode);
		if (unlikely(path[depth].p_hdr == NULL)) {
			up_read(&EXT4_I(inode)->i_data_sem);
			EXT4_ERROR_INODE(inode, "path[%d].p_hdr == NULL", depth);
			err = -EIO;
			break;
		}
		ex = path[depth].p_ext;
		next = ext4_ext_next_allocated_block(path);
		ext4_ext_drop_refs(path);

		flags = 0;
		exists = 0;
		if (!ex) {
			/* there is no extent yet, so try to allocate
			 * all requested space */
			start = block;
			end = block + num;
		} else if (le32_to_cpu(ex->ee_block) > block) {
			/* need to allocate space before found extent */
			start = block;
			end = le32_to_cpu(ex->ee_block);
			if (block + num < end)
				end = block + num;
		} else if (block >= le32_to_cpu(ex->ee_block)
					+ ext4_ext_get_actual_len(ex)) {
			/* need to allocate space after found extent */
			start = block;
			end = block + num;
			if (end >= next)
				end = next;
		} else if (block >= le32_to_cpu(ex->ee_block)) {
			/*
			 * some part of requested space is covered
			 * by found extent
			 */
			start = block;
			end = le32_to_cpu(ex->ee_block)
				+ ext4_ext_get_actual_len(ex);
			if (block + num < end)
				end = block + num;
			exists = 1;
		} else {
			BUG();
		}
		BUG_ON(end <= start);

		if (!exists) {
			es.es_lblk = start;
			es.es_len = end - start;
			es.es_pblk = 0;
		} else {
			es.es_lblk = le32_to_cpu(ex->ee_block);
			es.es_len = ext4_ext_get_actual_len(ex);
			es.es_pblk = ext4_ext_pblock(ex);
			if (ext4_ext_is_uninitialized(ex))
				flags |= FIEMAP_EXTENT_UNWRITTEN;
		}

		/*
		 * Find delayed extent and update es accordingly. We call
		 * it even in !exists case to find out whether es is the
		 * last existing extent or not.
		 */
		next_del = ext4_find_delayed_extent(inode, &es);
		if (!exists && next_del) {
			exists = 1;
			flags |= FIEMAP_EXTENT_DELALLOC;
		}
		up_read(&EXT4_I(inode)->i_data_sem);

		if (unlikely(es.es_len == 0)) {
			EXT4_ERROR_INODE(inode, "es.es_len == 0");
			err = -EIO;
			break;
		}

		/*
		 * This is possible iff next == next_del == EXT_MAX_BLOCKS.
		 * we need to check next == EXT_MAX_BLOCKS because it is
		 * possible that an extent is with unwritten and delayed
		 * status due to when an extent is delayed allocated and
		 * is allocated by fallocate status tree will track both of
		 * them in a extent.
		 *
		 * So we could return a unwritten and delayed extent, and
		 * its block is equal to 'next'.
		 */
		if (next == next_del && next == EXT_MAX_BLOCKS) {
			flags |= FIEMAP_EXTENT_LAST;
			if (unlikely(next_del != EXT_MAX_BLOCKS ||
				     next != EXT_MAX_BLOCKS)) {
				EXT4_ERROR_INODE(inode,
						 "next extent == %u, next "
						 "delalloc extent = %u",
						 next, next_del);
				err = -EIO;
				break;
			}
		}

		if (exists) {
			err = fiemap_fill_next_extent(fieinfo,
				(__u64)es.es_lblk << blksize_bits,
				(__u64)es.es_pblk << blksize_bits,
				(__u64)es.es_len << blksize_bits,
				flags);
			if (err < 0)
				break;
			if (err == 1) {
				err = 0;
				break;
			}
		}

		block = es.es_lblk + es.es_len;
	}

	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}

	return err;
}

/*
 * ext4_ext_put_gap_in_cache:
 * calculate boundaries of the gap that the requested block fits into
 * and cache this gap
 */
static void
ext4_ext_put_gap_in_cache(struct inode *inode, struct ext4_ext_path *path,
				ext4_lblk_t block)
{
	int depth = ext_depth(inode);
	unsigned long len;
	ext4_lblk_t lblock;
	struct ext4_extent *ex;

	ex = path[depth].p_ext;
	if (ex == NULL) {
		/*
		 * there is no extent yet, so gap is [0;-] and we
		 * don't cache it
		 */
		ext_debug("cache gap(whole file):");
	} else if (block < le32_to_cpu(ex->ee_block)) {
		lblock = block;
		len = le32_to_cpu(ex->ee_block) - block;
		ext_debug("cache gap(before): %u [%u:%u]",
				block,
				le32_to_cpu(ex->ee_block),
				 ext4_ext_get_actual_len(ex));
		if (!ext4_find_delalloc_range(inode, lblock, lblock + len - 1))
			ext4_es_insert_extent(inode, lblock, len, ~0,
					      EXTENT_STATUS_HOLE);
	} else if (block >= le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex)) {
		ext4_lblk_t next;
		lblock = le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex);

		next = ext4_ext_next_allocated_block(path);
		ext_debug("cache gap(after): [%u:%u] %u",
				le32_to_cpu(ex->ee_block),
				ext4_ext_get_actual_len(ex),
				block);
		BUG_ON(next == lblock);
		len = next - lblock;
		if (!ext4_find_delalloc_range(inode, lblock, lblock + len - 1))
			ext4_es_insert_extent(inode, lblock, len, ~0,
					      EXTENT_STATUS_HOLE);
	} else {
		lblock = len = 0;
		BUG();
	}

	ext_debug(" -> %u:%lu\n", lblock, len);
}

/*
 * ext4_ext_rm_idx:
 * removes index from the index block.
 */
static int ext4_ext_rm_idx(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path, int depth)
{
	int err;
	ext4_fsblk_t leaf;

	/* free index block */
	depth--;
	path = path + depth;
	leaf = ext4_idx_pblock(path->p_idx);
	if (unlikely(path->p_hdr->eh_entries == 0)) {
		EXT4_ERROR_INODE(inode, "path->p_hdr->eh_entries == 0");
		return -EIO;
	}
	err = ext4_ext_get_access(handle, inode, path);
	if (err)
		return err;

	if (path->p_idx != EXT_LAST_INDEX(path->p_hdr)) {
		int len = EXT_LAST_INDEX(path->p_hdr) - path->p_idx;
		len *= sizeof(struct ext4_extent_idx);
		memmove(path->p_idx, path->p_idx + 1, len);
	}

	le16_add_cpu(&path->p_hdr->eh_entries, -1);
	err = ext4_ext_dirty(handle, inode, path);
	if (err)
		return err;
	ext_debug("index is empty, remove it, free block %llu\n", leaf);
	trace_ext4_ext_rm_idx(inode, leaf);

	ext4_free_blocks(handle, inode, NULL, leaf, 1,
			 EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);

	while (--depth >= 0) {
		if (path->p_idx != EXT_FIRST_INDEX(path->p_hdr))
			break;
		path--;
		err = ext4_ext_get_access(handle, inode, path);
		if (err)
			break;
		path->p_idx->ei_block = (path+1)->p_idx->ei_block;
		err = ext4_ext_dirty(handle, inode, path);
		if (err)
			break;
	}
	return err;
}

/*
 * ext4_ext_calc_credits_for_single_extent:
 * This routine returns max. credits that needed to insert an extent
 * to the extent tree.
 * When pass the actual path, the caller should calculate credits
 * under i_data_sem.
 */
int ext4_ext_calc_credits_for_single_extent(struct inode *inode, int nrblocks,
						struct ext4_ext_path *path)
{
	if (path) {
		int depth = ext_depth(inode);
		int ret = 0;

		/* probably there is space in leaf? */
		if (le16_to_cpu(path[depth].p_hdr->eh_entries)
				< le16_to_cpu(path[depth].p_hdr->eh_max)) {

			/*
			 *  There are some space in the leaf tree, no
			 *  need to account for leaf block credit
			 *
			 *  bitmaps and block group descriptor blocks
			 *  and other metadata blocks still need to be
			 *  accounted.
			 */
			/* 1 bitmap, 1 block group descriptor */
			ret = 2 + EXT4_META_TRANS_BLOCKS(inode->i_sb);
			return ret;
		}
	}

	return ext4_chunk_trans_blocks(inode, nrblocks);
}

/*
 * How many index/leaf blocks need to change/allocate to modify nrblocks?
 *
 * if nrblocks are fit in a single extent (chunk flag is 1), then
 * in the worse case, each tree level index/leaf need to be changed
 * if the tree split due to insert a new extent, then the old tree
 * index/leaf need to be updated too
 *
 * If the nrblocks are discontiguous, they could cause
 * the whole tree split more than once, but this is really rare.
 */
int ext4_ext_index_trans_blocks(struct inode *inode, int nrblocks, int chunk)
{
	int index;
	int depth;

	/* If we are converting the inline data, only one is needed here. */
	if (ext4_has_inline_data(inode))
		return 1;

	depth = ext_depth(inode);

	if (chunk)
		index = depth * 2;
	else
		index = depth * 3;

	return index;
}

static int ext4_remove_blocks(handle_t *handle, struct inode *inode,
			      struct ext4_extent *ex,
			      ext4_fsblk_t *partial_cluster,
			      ext4_lblk_t from, ext4_lblk_t to)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	unsigned short ee_len =  ext4_ext_get_actual_len(ex);
	ext4_fsblk_t pblk;
	int flags = 0;

	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
		flags |= EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET;
	else if (ext4_should_journal_data(inode))
		flags |= EXT4_FREE_BLOCKS_FORGET;

	/*
	 * For bigalloc file systems, we never free a partial cluster
	 * at the beginning of the extent.  Instead, we make a note
	 * that we tried freeing the cluster, and check to see if we
	 * need to free it on a subsequent call to ext4_remove_blocks,
	 * or at the end of the ext4_truncate() operation.
	 */
	flags |= EXT4_FREE_BLOCKS_NOFREE_FIRST_CLUSTER;

	trace_ext4_remove_blocks(inode, ex, from, to, *partial_cluster);
	/*
	 * If we have a partial cluster, and it's different from the
	 * cluster of the last block, we need to explicitly free the
	 * partial cluster here.
	 */
	pblk = ext4_ext_pblock(ex) + ee_len - 1;
	if (*partial_cluster && (EXT4_B2C(sbi, pblk) != *partial_cluster)) {
		ext4_free_blocks(handle, inode, NULL,
				 EXT4_C2B(sbi, *partial_cluster),
				 sbi->s_cluster_ratio, flags);
		*partial_cluster = 0;
	}

#ifdef EXTENTS_STATS
	{
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		spin_lock(&sbi->s_ext_stats_lock);
		sbi->s_ext_blocks += ee_len;
		sbi->s_ext_extents++;
		if (ee_len < sbi->s_ext_min)
			sbi->s_ext_min = ee_len;
		if (ee_len > sbi->s_ext_max)
			sbi->s_ext_max = ee_len;
		if (ext_depth(inode) > sbi->s_depth_max)
			sbi->s_depth_max = ext_depth(inode);
		spin_unlock(&sbi->s_ext_stats_lock);
	}
#endif
	if (from >= le32_to_cpu(ex->ee_block)
	    && to == le32_to_cpu(ex->ee_block) + ee_len - 1) {
		/* tail removal */
		ext4_lblk_t num;

		num = le32_to_cpu(ex->ee_block) + ee_len - from;
		pblk = ext4_ext_pblock(ex) + ee_len - num;
		ext_debug("free last %u blocks starting %llu\n", num, pblk);
		ext4_free_blocks(handle, inode, NULL, pblk, num, flags);
		/*
		 * If the block range to be freed didn't start at the
		 * beginning of a cluster, and we removed the entire
		 * extent, save the partial cluster here, since we
		 * might need to delete if we determine that the
		 * truncate operation has removed all of the blocks in
		 * the cluster.
		 */
		if (EXT4_PBLK_COFF(sbi, pblk) &&
		    (ee_len == num))
			*partial_cluster = EXT4_B2C(sbi, pblk);
		else
			*partial_cluster = 0;
	} else if (from == le32_to_cpu(ex->ee_block)
		   && to <= le32_to_cpu(ex->ee_block) + ee_len - 1) {
		/* head removal */
		ext4_lblk_t num;
		ext4_fsblk_t start;

		num = to - from;
		start = ext4_ext_pblock(ex);

		ext_debug("free first %u blocks starting %llu\n", num, start);
		ext4_free_blocks(handle, inode, NULL, start, num, flags);

	} else {
		printk(KERN_INFO "strange request: removal(2) "
				"%u-%u from %u:%u\n",
				from, to, le32_to_cpu(ex->ee_block), ee_len);
	}
	return 0;
}


/*
 * ext4_ext_rm_leaf() Removes the extents associated with the
 * blocks appearing between "start" and "end", and splits the extents
 * if "start" and "end" appear in the same extent
 *
 * @handle: The journal handle
 * @inode:  The files inode
 * @path:   The path to the leaf
 * @start:  The first block to remove
 * @end:   The last block to remove
 */
static int
ext4_ext_rm_leaf(handle_t *handle, struct inode *inode,
		 struct ext4_ext_path *path, ext4_fsblk_t *partial_cluster,
		 ext4_lblk_t start, ext4_lblk_t end)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int err = 0, correct_index = 0;
	int depth = ext_depth(inode), credits;
	struct ext4_extent_header *eh;
	ext4_lblk_t a, b;
	unsigned num;
	ext4_lblk_t ex_ee_block;
	unsigned short ex_ee_len;
	unsigned uninitialized = 0;
	struct ext4_extent *ex;

	/* the header must be checked already in ext4_ext_remove_space() */
	ext_debug("truncate since %u in leaf to %u\n", start, end);
	if (!path[depth].p_hdr)
		path[depth].p_hdr = ext_block_hdr(path[depth].p_bh);
	eh = path[depth].p_hdr;
	if (unlikely(path[depth].p_hdr == NULL)) {
		EXT4_ERROR_INODE(inode, "path[%d].p_hdr == NULL", depth);
		return -EIO;
	}
	/* find where to start removing */
	ex = EXT_LAST_EXTENT(eh);

	ex_ee_block = le32_to_cpu(ex->ee_block);
	ex_ee_len = ext4_ext_get_actual_len(ex);

	/*
	 * If we're starting with an extent other than the last one in the
	 * node, we need to see if it shares a cluster with the extent to
	 * the right (towards the end of the file). If its leftmost cluster
	 * is this extent's rightmost cluster and it is not cluster aligned,
	 * we'll mark it as a partial that is not to be deallocated.
	 */

	if (ex != EXT_LAST_EXTENT(eh)) {
		ext4_fsblk_t current_pblk, right_pblk;
		long long current_cluster, right_cluster;

		current_pblk = ext4_ext_pblock(ex) + ex_ee_len - 1;
		current_cluster = (long long)EXT4_B2C(sbi, current_pblk);
		right_pblk = ext4_ext_pblock(ex + 1);
		right_cluster = (long long)EXT4_B2C(sbi, right_pblk);
		if (current_cluster == right_cluster &&
			EXT4_PBLK_COFF(sbi, right_pblk))
			*partial_cluster = -right_cluster;
	}

	trace_ext4_ext_rm_leaf(inode, start, ex, *partial_cluster);

	while (ex >= EXT_FIRST_EXTENT(eh) &&
			ex_ee_block + ex_ee_len > start) {

		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		else
			uninitialized = 0;

		ext_debug("remove ext %u:[%d]%d\n", ex_ee_block,
			 uninitialized, ex_ee_len);
		path[depth].p_ext = ex;

		a = ex_ee_block > start ? ex_ee_block : start;
		b = ex_ee_block+ex_ee_len - 1 < end ?
			ex_ee_block+ex_ee_len - 1 : end;

		ext_debug("  border %u:%u\n", a, b);

		/* If this extent is beyond the end of the hole, skip it */
		if (end < ex_ee_block) {
			ex--;
			ex_ee_block = le32_to_cpu(ex->ee_block);
			ex_ee_len = ext4_ext_get_actual_len(ex);
			continue;
		} else if (b != ex_ee_block + ex_ee_len - 1) {
			EXT4_ERROR_INODE(inode,
					 "can not handle truncate %u:%u "
					 "on extent %u:%u",
					 start, end, ex_ee_block,
					 ex_ee_block + ex_ee_len - 1);
			err = -EIO;
			goto out;
		} else if (a != ex_ee_block) {
			/* remove tail of the extent */
			num = a - ex_ee_block;
		} else {
			/* remove whole extent: excellent! */
			num = 0;
		}
		/*
		 * 3 for leaf, sb, and inode plus 2 (bmap and group
		 * descriptor) for each block group; assume two block
		 * groups plus ex_ee_len/blocks_per_block_group for
		 * the worst case
		 */
		credits = 7 + 2*(ex_ee_len/EXT4_BLOCKS_PER_GROUP(inode->i_sb));
		if (ex == EXT_FIRST_EXTENT(eh)) {
			correct_index = 1;
			credits += (ext_depth(inode)) + 1;
		}
		credits += EXT4_MAXQUOTAS_TRANS_BLOCKS(inode->i_sb);

		err = ext4_ext_truncate_extend_restart(handle, inode, credits);
		if (err)
			goto out;

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		err = ext4_remove_blocks(handle, inode, ex, partial_cluster,
					 a, b);
		if (err)
			goto out;

		if (num == 0)
			/* this extent is removed; mark slot entirely unused */
			ext4_ext_store_pblock(ex, 0);

		ex->ee_len = cpu_to_le16(num);
		/*
		 * Do not mark uninitialized if all the blocks in the
		 * extent have been removed.
		 */
		if (uninitialized && num)
			ext4_ext_mark_uninitialized(ex);
		/*
		 * If the extent was completely released,
		 * we need to remove it from the leaf
		 */
		if (num == 0) {
			if (end != EXT_MAX_BLOCKS - 1) {
				/*
				 * For hole punching, we need to scoot all the
				 * extents up when an extent is removed so that
				 * we dont have blank extents in the middle
				 */
				memmove(ex, ex+1, (EXT_LAST_EXTENT(eh) - ex) *
					sizeof(struct ext4_extent));

				/* Now get rid of the one at the end */
				memset(EXT_LAST_EXTENT(eh), 0,
					sizeof(struct ext4_extent));
			}
			le16_add_cpu(&eh->eh_entries, -1);
		} else
			*partial_cluster = 0;

		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;

		ext_debug("new extent: %u:%u:%llu\n", ex_ee_block, num,
				ext4_ext_pblock(ex));
		ex--;
		ex_ee_block = le32_to_cpu(ex->ee_block);
		ex_ee_len = ext4_ext_get_actual_len(ex);
	}

	if (correct_index && eh->eh_entries)
		err = ext4_ext_correct_indexes(handle, inode, path);

	/*
	 * If there is still a entry in the leaf node, check to see if
	 * it references the partial cluster.  This is the only place
	 * where it could; if it doesn't, we can free the cluster.
	 */
	if (*partial_cluster && ex >= EXT_FIRST_EXTENT(eh) &&
	    (EXT4_B2C(sbi, ext4_ext_pblock(ex) + ex_ee_len - 1) !=
	     *partial_cluster)) {
		int flags = EXT4_FREE_BLOCKS_FORGET;

		if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
			flags |= EXT4_FREE_BLOCKS_METADATA;

		ext4_free_blocks(handle, inode, NULL,
				 EXT4_C2B(sbi, *partial_cluster),
				 sbi->s_cluster_ratio, flags);
		*partial_cluster = 0;
	}

	/* if this leaf is free, then we should
	 * remove it from index block above */
	if (err == 0 && eh->eh_entries == 0 && path[depth].p_bh != NULL)
		err = ext4_ext_rm_idx(handle, inode, path, depth);

out:
	return err;
}

/*
 * ext4_ext_more_to_rm:
 * returns 1 if current index has to be freed (even partial)
 */
static int
ext4_ext_more_to_rm(struct ext4_ext_path *path)
{
	BUG_ON(path->p_idx == NULL);

	if (path->p_idx < EXT_FIRST_INDEX(path->p_hdr))
		return 0;

	/*
	 * if truncate on deeper level happened, it wasn't partial,
	 * so we have to consider current index for truncation
	 */
	if (le16_to_cpu(path->p_hdr->eh_entries) == path->p_block)
		return 0;
	return 1;
}

int ext4_ext_remove_space(struct inode *inode, ext4_lblk_t start,
			  ext4_lblk_t end)
{
	struct super_block *sb = inode->i_sb;
	int depth = ext_depth(inode);
	struct ext4_ext_path *path = NULL;
	ext4_fsblk_t partial_cluster = 0;
	handle_t *handle;
	int i = 0, err = 0;

	ext_debug("truncate since %u to %u\n", start, end);

	/* probably first extent we're gonna free will be last in block */
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, depth + 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

again:
	trace_ext4_ext_remove_space(inode, start, depth);

	/*
	 * Check if we are removing extents inside the extent tree. If that
	 * is the case, we are going to punch a hole inside the extent tree
	 * so we have to check whether we need to split the extent covering
	 * the last block to remove so we can easily remove the part of it
	 * in ext4_ext_rm_leaf().
	 */
	if (end < EXT_MAX_BLOCKS - 1) {
		struct ext4_extent *ex;
		ext4_lblk_t ee_block;

		/* find extent for this block */
		path = ext4_ext_find_extent(inode, end, NULL);
		if (IS_ERR(path)) {
			ext4_journal_stop(handle);
			return PTR_ERR(path);
		}
		depth = ext_depth(inode);
		/* Leaf not may not exist only if inode has no blocks at all */
		ex = path[depth].p_ext;
		if (!ex) {
			if (depth) {
				EXT4_ERROR_INODE(inode,
						 "path[%d].p_hdr == NULL",
						 depth);
				err = -EIO;
			}
			goto out;
		}

		ee_block = le32_to_cpu(ex->ee_block);

		/*
		 * See if the last block is inside the extent, if so split
		 * the extent at 'end' block so we can easily remove the
		 * tail of the first part of the split extent in
		 * ext4_ext_rm_leaf().
		 */
		if (end >= ee_block &&
		    end < ee_block + ext4_ext_get_actual_len(ex) - 1) {
			int split_flag = 0;

			if (ext4_ext_is_uninitialized(ex))
				split_flag = EXT4_EXT_MARK_UNINIT1 |
					     EXT4_EXT_MARK_UNINIT2;

			/*
			 * Split the extent in two so that 'end' is the last
			 * block in the first new extent. Also we should not
			 * fail removing space due to ENOSPC so try to use
			 * reserved block if that happens.
			 */
			err = ext4_split_extent_at(handle, inode, path,
					end + 1, split_flag,
					EXT4_GET_BLOCKS_PRE_IO |
					EXT4_GET_BLOCKS_METADATA_NOFAIL);

			if (err < 0)
				goto out;
		}
	}
	/*
	 * We start scanning from right side, freeing all the blocks
	 * after i_size and walking into the tree depth-wise.
	 */
	depth = ext_depth(inode);
	if (path) {
		int k = i = depth;
		while (--k > 0)
			path[k].p_block =
				le16_to_cpu(path[k].p_hdr->eh_entries)+1;
	} else {
		path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 1),
			       GFP_NOFS);
		if (path == NULL) {
			ext4_journal_stop(handle);
			return -ENOMEM;
		}
		path[0].p_depth = depth;
		path[0].p_hdr = ext_inode_hdr(inode);
		i = 0;

		if (ext4_ext_check(inode, path[0].p_hdr, depth)) {
			err = -EIO;
			goto out;
		}
	}
	err = 0;

	while (i >= 0 && err == 0) {
		if (i == depth) {
			/* this is leaf block */
			err = ext4_ext_rm_leaf(handle, inode, path,
					       &partial_cluster, start,
					       end);
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			continue;
		}

		/* this is index block */
		if (!path[i].p_hdr) {
			ext_debug("initialize header\n");
			path[i].p_hdr = ext_block_hdr(path[i].p_bh);
		}

		if (!path[i].p_idx) {
			/* this level hasn't been touched yet */
			path[i].p_idx = EXT_LAST_INDEX(path[i].p_hdr);
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries)+1;
			ext_debug("init index ptr: hdr 0x%p, num %d\n",
				  path[i].p_hdr,
				  le16_to_cpu(path[i].p_hdr->eh_entries));
		} else {
			/* we were already here, see at next index */
			path[i].p_idx--;
		}

		ext_debug("level %d - index, first 0x%p, cur 0x%p\n",
				i, EXT_FIRST_INDEX(path[i].p_hdr),
				path[i].p_idx);
		if (ext4_ext_more_to_rm(path + i)) {
			struct buffer_head *bh;
			/* go to the next level */
			ext_debug("move to level %d (block %llu)\n",
				  i + 1, ext4_idx_pblock(path[i].p_idx));
			memset(path + i + 1, 0, sizeof(*path));
			bh = sb_bread(sb, ext4_idx_pblock(path[i].p_idx));
			if (!bh) {
				/* should we reset i_size? */
				err = -EIO;
				break;
			}
			if (WARN_ON(i + 1 > depth)) {
				err = -EIO;
				break;
			}
			if (ext4_ext_check_block(inode, ext_block_hdr(bh),
							depth - i - 1, bh)) {
				err = -EIO;
				break;
			}
			path[i + 1].p_bh = bh;

			/* save actual number of indexes since this
			 * number is changed at the next iteration */
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries);
			i++;
		} else {
			/* we finished processing this index, go up */
			if (path[i].p_hdr->eh_entries == 0 && i > 0) {
				/* index is empty, remove it;
				 * handle must be already prepared by the
				 * truncatei_leaf() */
				err = ext4_ext_rm_idx(handle, inode, path, i);
			}
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			ext_debug("return to level %d\n", i);
		}
	}

	trace_ext4_ext_remove_space_done(inode, start, depth, partial_cluster,
			path->p_hdr->eh_entries);

	/* If we still have something in the partial cluster and we have removed
	 * even the first extent, then we should free the blocks in the partial
	 * cluster as well. */
	if (partial_cluster && path->p_hdr->eh_entries == 0) {
		int flags = EXT4_FREE_BLOCKS_FORGET;

		if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
			flags |= EXT4_FREE_BLOCKS_METADATA;

		ext4_free_blocks(handle, inode, NULL,
				 EXT4_C2B(EXT4_SB(sb), partial_cluster),
				 EXT4_SB(sb)->s_cluster_ratio, flags);
		partial_cluster = 0;
	}

	/* TODO: flexible tree reduction should be here */
	if (path->p_hdr->eh_entries == 0) {
		/*
		 * truncate to zero freed all the tree,
		 * so we need to correct eh_depth
		 */
		err = ext4_ext_get_access(handle, inode, path);
		if (err == 0) {
			ext_inode_hdr(inode)->eh_depth = 0;
			ext_inode_hdr(inode)->eh_max =
				cpu_to_le16(ext4_ext_space_root(inode, 0));
			err = ext4_ext_dirty(handle, inode, path);
		}
	}
out:
	ext4_ext_drop_refs(path);
	kfree(path);
	if (err == -EAGAIN) {
		path = NULL;
		goto again;
	}
	ext4_journal_stop(handle);

	return err;
}

/*
 * called at mount time
 */
void ext4_ext_init(struct super_block *sb)
{
	/*
	 * possible initialization would be here
	 */

	if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
#if defined(AGGRESSIVE_TEST) || defined(CHECK_BINSEARCH) || defined(EXTENTS_STATS)
		printk(KERN_INFO "EXT4-fs: file extents enabled"
#ifdef AGGRESSIVE_TEST
		       ", aggressive tests"
#endif
#ifdef CHECK_BINSEARCH
		       ", check binsearch"
#endif
#ifdef EXTENTS_STATS
		       ", stats"
#endif
		       "\n");
#endif
#ifdef EXTENTS_STATS
		spin_lock_init(&EXT4_SB(sb)->s_ext_stats_lock);
		EXT4_SB(sb)->s_ext_min = 1 << 30;
		EXT4_SB(sb)->s_ext_max = 0;
#endif
	}
}

/*
 * called at umount time
 */
void ext4_ext_release(struct super_block *sb)
{
	if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_EXTENTS))
		return;

#ifdef EXTENTS_STATS
	if (EXT4_SB(sb)->s_ext_blocks && EXT4_SB(sb)->s_ext_extents) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		printk(KERN_ERR "EXT4-fs: %lu blocks in %lu extents (%lu ave)\n",
			sbi->s_ext_blocks, sbi->s_ext_extents,
			sbi->s_ext_blocks / sbi->s_ext_extents);
		printk(KERN_ERR "EXT4-fs: extents: %lu min, %lu max, max depth %lu\n",
			sbi->s_ext_min, sbi->s_ext_max, sbi->s_depth_max);
	}
#endif
}

/* FIXME!! we need to try to merge to left or right after zero-out  */
static int ext4_ext_zeroout(struct inode *inode, struct ext4_extent *ex)
{
	ext4_fsblk_t ee_pblock;
	unsigned int ee_len;
	int ret;

	ee_len    = ext4_ext_get_actual_len(ex);
	ee_pblock = ext4_ext_pblock(ex);

	ret = sb_issue_zeroout(inode->i_sb, ee_pblock, ee_len, GFP_NOFS);
	if (ret > 0)
		ret = 0;

	return ret;
}

/*
 * ext4_split_extent_at() splits an extent at given block.
 *
 * @handle: the journal handle
 * @inode: the file inode
 * @path: the path to the extent
 * @split: the logical block where the extent is splitted.
 * @split_flags: indicates if the extent could be zeroout if split fails, and
 *		 the states(init or uninit) of new extents.
 * @flags: flags used to insert new extent to extent tree.
 *
 *
 * Splits extent [a, b] into two extents [a, @split) and [@split, b], states
 * of which are deterimined by split_flag.
 *
 * There are two cases:
 *  a> the extent are splitted into two extent.
 *  b> split is not needed, and just mark the extent.
 *
 * return 0 on success.
 */
//ext4_ext_map_blocks()->ext4_ext_handle_uninitialized_extents()/ext4_ext_handle_unwritten_extents()->
//ext4_ext_convert_to_initialized()->ext4_split_extent()->ext4_split_extent_at()

/*��split����߼����ַΪ�ָ�㣬��path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χee_block~(ee_block+ee_len)�ָ��
ee_block~split��split~(ee_block+ee_len)��Ȼ��Ѻ���split~(ee_block+ee_len)��Ӧ��ext4_extent�ṹ��ӵ�ext4 extent B+��*/
static int ext4_split_extent_at(handle_t *handle,
			     struct inode *inode,
			     struct ext4_ext_path *path,
			     ext4_lblk_t split,
			     int split_flag,
			     int flags)
{
	ext4_fsblk_t newblock;
	ext4_lblk_t ee_block;
	struct ext4_extent *ex, newex, orig_ex, zero_ex;
	struct ext4_extent *ex2 = NULL;
	unsigned int ee_len, depth;
	int err = 0;

	BUG_ON((split_flag & (EXT4_EXT_DATA_VALID1 | EXT4_EXT_DATA_VALID2)) ==
	       (EXT4_EXT_DATA_VALID1 | EXT4_EXT_DATA_VALID2));

	ext_debug("ext4_split_extents_at: inode %lu, logical"
		"block %llu\n", inode->i_ino, (unsigned long long)split);

	ext4_ext_show_leaf(inode, path);
    //ext4 extent B+�����
	depth = ext_depth(inode);
    //ext4 extent B+��Ҷ�ӽڵ�����ʼ�߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	ex = path[depth].p_ext;
    //ex���ext4_extent�ṹ�������ʼ�߼����ַ
	ee_block = le32_to_cpu(ex->ee_block);
    //ex���ext4_extent�ṹ�����ӳ�����������
	ee_len = ext4_ext_get_actual_len(ex);
    //ee_block��ex��ʼ�߼����ַ��split�Ƿָ����߼����ַ��split����ee_block�����߶���ex���
    //ext4_extent���߼��鷶Χ�ڡ�newblock�Ƿָ����߼����ַ��Ӧ��������ַ
	newblock = split - ee_block + ext4_ext_pblock(ex);

	BUG_ON(split < ee_block || split >= (ee_block + ee_len));
	BUG_ON(!ext4_ext_is_uninitialized(ex) &&
	       split_flag & (EXT4_EXT_MAY_ZEROOUT |
			     EXT4_EXT_MARK_UNINIT1 |
			     EXT4_EXT_MARK_UNINIT2));

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;

    //�ָ����߼����ַ����ex��ʼ�߼����ַ�����÷ָ�
	if (split == ee_block) {
		/*
		 * case b: block @split is the block that the extent begins with
		 * then we just change the state of the extent, and splitting
		 * is not needed.
		 */
		if (split_flag & EXT4_EXT_MARK_UNINIT2)
			ext4_ext_mark_uninitialized(ex);//��"UNINIT2"��Ǿ�Ҫ���ex "uninitialized"
		else
			ext4_ext_mark_initialized(ex);//���ex��ʼ��

		if (!(flags & EXT4_GET_BLOCKS_PRE_IO))
            //���԰�exǰ���ext4_extent�ṹ���߼����������ַ�ϲ���ex
			ext4_ext_try_to_merge(handle, inode, path, ex);

        //ext4_extentӳ����߼��鷶Χ���ܷ����仯�ˣ���Ƕ�Ӧ�������ӳ���bh�����ļ�inode��.
		err = ext4_ext_dirty(handle, inode, path + path->p_depth);
		goto out;
	}

    /*�±����ǰ�ex���߼���ָ��������(ee_block~split)��(split~ee_block+ee_len)���ָ��ex�µ�
    �߼��鷶Χ��(ee_block~split)��ex2���߼��鷶Χ��(split~ee_block+ee_len)
    */
	/* case a */
    //orig_ex�ȱ���exԭ������
	memcpy(&orig_ex, ex, sizeof(orig_ex));
    /*�ص㣬���ex->ee_lenΪӳ���block��������ex���Ǳ���ǳ�ʼ��״̬�ˣ���Ϊex->ee_lenֻҪ����û�����EXT_INIT_MAX_LEN��
    ���ǳ�ʼ��״̬������һ���±�ִ��ext4_ext_mark_uninitialized(ex)��ex�ֳ�δ��ʼ��״̬��*/
	ex->ee_len = cpu_to_le16(split - ee_block);
	if (split_flag & EXT4_EXT_MARK_UNINIT1)
		ext4_ext_mark_uninitialized(ex);//��EXT4_EXT_MARK_UNINIT1����ٰ�ex���δ��ʼ��

	/*
	 * path may lead to new leaf, not to original leaf any more
	 * after ext4_ext_insert_extent() returns,
	 */
	err = ext4_ext_dirty(handle, inode, path + depth);
	if (err)
		goto fix_extent_len;

	ex2 = &newex;//ex2����ex�ָ��ĺ��ε��߼��鷶Χ��Ӧ��ext4_extent�ṹ
	ex2->ee_block = cpu_to_le32(split);//ex2���߼�����ʼ��ַ���ָ����߼����ַ
	ex2->ee_len   = cpu_to_le16(ee_len - (split - ee_block));//ex2�߼������
	ext4_ext_store_pblock(ex2, newblock);//ex2����ʼ������ַ
	if (split_flag & EXT4_EXT_MARK_UNINIT2)
		ext4_ext_mark_uninitialized(ex2);/*���ex2δ��ʼ��״̬*/

    //�ѷָ�ĺ���ext4_extent�ṹ��ex2��ӵ�ext4 extent B+�����ص㺯��������Դ������ϸע����
	err = ext4_ext_insert_extent(handle, inode, path, &newex, flags);

    //err��ENOSPCһ�㲻�������
	if (err == -ENOSPC && (EXT4_EXT_MAY_ZEROOUT & split_flag)) {
		if (split_flag & (EXT4_EXT_DATA_VALID1|EXT4_EXT_DATA_VALID2)) {
			if (split_flag & EXT4_EXT_DATA_VALID1) {
				err = ext4_ext_zeroout(inode, ex2);
				zero_ex.ee_block = ex2->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex2));
				ext4_ext_store_pblock(&zero_ex,
						      ext4_ext_pblock(ex2));
			} else {
				err = ext4_ext_zeroout(inode, ex);
				zero_ex.ee_block = ex->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex));
				ext4_ext_store_pblock(&zero_ex,
						      ext4_ext_pblock(ex));
			}
		} else {
			err = ext4_ext_zeroout(inode, &orig_ex);
			zero_ex.ee_block = orig_ex.ee_block;
			zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(&orig_ex));
			ext4_ext_store_pblock(&zero_ex,
					      ext4_ext_pblock(&orig_ex));
		}

		if (err)
			goto fix_extent_len;
		/* update the extent length and mark as initialized */
		ex->ee_len = cpu_to_le16(ee_len);
		ext4_ext_try_to_merge(handle, inode, path, ex);
		err = ext4_ext_dirty(handle, inode, path + path->p_depth);
		if (err)
			goto fix_extent_len;

		/* update extent status tree */
		err = ext4_es_zeroout(inode, &zero_ex);

		goto out;
	}
    else if (err)//����һ��Ҳ������
		goto fix_extent_len;

out:
	ext4_ext_show_leaf(inode, path);
	return err;//һ�����ﷵ��0

fix_extent_len:
    //��Ȼ����ex splitʧ�ܣ������ָ�exԭ�е�����
	ex->ee_len = orig_ex.ee_len;
	ext4_ext_dirty(handle, inode, path + depth);
	return err;
}

/*
 * ext4_split_extents() splits an extent and mark extent which is covered
 * by @map as split_flags indicates
 *
 * It may result in splitting the extent into multiple extents (upto three)
 * There are three possibilities:
 *   a> There is no split required
 *   b> Splits in two extents: Split is happening at either end of the extent
 *   c> Splits in three extents: Somone is splitting in middle of the extent
 *
 */
//ext4_ext_map_blocks()->ext4_ext_handle_uninitialized_extents()/ext4_ext_handle_unwritten_extents()->ext4_ext_convert_to_initialized()
//->ext4_split_extent()

/*ex = path[depth].p_ext
  �����map->m_lblk����ex����ʼ�߼����ַee_block�ǿ��Ա�֤�ġ���map����ʼ�߼����ַmap->m_lblk�϶���ex���߼��鷶Χ�ڡ�
  ����ִ��ext4_split_extent()��ex���߼��鷶Χee_block~(ee_block + ee_len)���зָ�ָ�����м���
1:��� map->m_lblk +map->m_len С��ee_block + ee_len����map�Ľ����߼����ַС��ex�Ľ����߼����ַ�����ex���߼��鷶Χ�ָ��3��
 ee_block~map->m_lblk �� map->m_lblk~(map->m_lblk +map->m_len) �� (map->m_lblk +map->m_len)~(ee_block + ee_len)���������������
 ��֤����Ҫ��ӳ���map->m_len���߼��鶼�����ӳ�䣬��allocated =map->m_len��

 ����ϸ����:
 1.1:if (map->m_lblk + map->m_len < ee_block + ee_len)������split_flag1 |= EXT4_EXT_MARK_UNINIT1|EXT4_EXT_MARK_UNINIT2,Ȼ��
 ִ��ext4_split_extent_at()��map->m_lblk + map->m_len����߼����ַΪ�ָ�㣬��path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χ
 ee_block~(ee_block+ee_len)�ָ��ee_block~(map->m_lblk + map->m_len)��(map->m_lblk + map->m_len)~(ee_block+ee_len)������ext4_extent��
 ǰ��ε�ext4_extent����ex��ֻ��ӳ����߼������������(ee_block+ee_len)-(map->m_lblk + map->m_len)�����ε��Ǹ��µ�ext4_extent��
 ��Ϊsplit_flag1 |= EXT4_EXT_MARK_UNINIT1|EXT4_EXT_MARK_UNINIT2����Ҫ���������ext4_extent�ṹ"����δ��ʼ��״̬"��Ȼ��Ѻ���
 (map->m_lblk + map->m_len)~(ee_block+ee_len)��Ӧ��ext4_extent�ṹ��ӵ�ext4 extent B+����

 �ص�ext4_split_extent()������ext4_ext_find_extent(inode, map->m_lblk, path)��path[depth].p_ext����ʻ����ϵ�ex��
 1.2 if (map->m_lblk >= ee_block)�϶�������
 ��ߵ�if (uninitialized)������if (uninitialized)��ߵ�split_flag1 |= EXT4_EXT_MARK_UNINIT1�����ܲ������EXT4_EXT_MARK_UNINIT2��ǡ�
 ��Ϊsplit_flag1 |= split_flag & (EXT4_EXT_MAY_ZEROOUT |EXT4_EXT_MARK_UNINIT2);
 ���ţ��ٴ�ִ��ext4_split_extent_at(),��map->m_lblk����߼����ַΪ�ָ�㣬��path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χ
 ee_block~(ee_block+ee_len)�ָ��ee_block~map->m_lblk��map->m_lblk~(ee_block+ee_len)����ext4_extent�ṹ��ǰ��ε�ext4_extent�ṹ����
 ex�������߼�����������(ee_block+ee_len)-map->m_lblk������Ϊ��ʱsplit_flag1��EXT4_EXT_MARK_UNINIT1��ǣ�����û��
 EXT4_EXT_MARK_UNINIT2��ǣ����ٶ�ex����"δ��ʼ��״̬"�����ε�ext4_extent���ܻᱻȥ��"δ��ʼ��״̬"����Ϊsplit_flag1����û��
 EXT4_EXT_MARK_UNINIT2��ǡ����ţ��Ѻ��ε�ext4_extent�ṹ��ӵ�ext4 extent B+����

 �����и����������� if (map->m_lblk >= ee_block)���map->m_lblk == ee_block����map��Ҫӳ�����ʼ�߼����ַ����ex����ʼ�߼����ַ��
 ��ִ��ext4_split_extent_at()����ʱ�������ٷָ�ex�����if (split == ee_block)��������ִ��ext4_ext_mark_initialized(ex)���ex��
 "��ʼ��״̬"��ex����ת���ˡ�
 
2:��� map->m_lblk +map->m_len ���ڵ���ee_block + ee_len����map�Ľ����߼����ַ����ex�Ľ����߼����ַ�����ex���߼��鷶Χ�ָ��2��
 ee_block~map->m_lblk �� map->m_lblk~(ee_block + ee_len)��������������ܱ�֤����Ҫ��ӳ���map->m_len���߼��鶼���ӳ�䡣ֻ��ӳ��
 (ee_block + ee_len) - map->m_lblk���߼��飬��allocated =(ee_block + ee_len) - map->m_lblk������ָ���̾���1.2�����̣���1.2�ھ��С�
*/
static int ext4_split_extent(handle_t *handle,
			      struct inode *inode,
			      struct ext4_ext_path *path,
			      struct ext4_map_blocks *map,
			      int split_flag,
			      int flags)
{
	ext4_lblk_t ee_block;
	struct ext4_extent *ex;
	unsigned int ee_len, depth;
	int err = 0;
	int uninitialized;
	int split_flag1, flags1;
	int allocated = map->m_len;

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
    //ex�Ƿ���δ��ʼ��״̬
	uninitialized = ext4_ext_is_uninitialized(ex);

    //���map�Ľ����߼����ַС��ex�Ľ����߼����ַ����ִ��ext4_split_extent_at()��ex���߼����ַ�ָ�Ϊ
    //ee_block~(map->m_lblk+map->m_len)��(map->m_lblk+map->m_len)~(ee_block + ee_len)���±ߵ�if (map->m_lblk >= ee_block)
    //Ҳ�������ٴ�ִ��ext4_split_extent_at()��ex���߼��鷶Χee_block~(map->m_lblk+map->m_len)�ָ��ee_block~map->m_lblk
    //��~map->m_lblk~(map->m_lblk+map->m_len)���Σ���ߵ�map->m_lblk~(map->m_lblk+map->m_len)��map->m_len���߼����Ӧ��ext4_extent
    //���������map���߼�����������ӳ�䡣
	if (map->m_lblk + map->m_len < ee_block + ee_len) {
		split_flag1 = split_flag & EXT4_EXT_MAY_ZEROOUT;
        
		flags1 = flags | EXT4_GET_BLOCKS_PRE_IO;//flag����EXT4_GET_BLOCKS_PRE_IO���
		//���ex��δ��ʼ����ǣ���split_flag1������EXT4_EXT_MARK_UNINIT1��EXT4_EXT_MARK_UNINIT2��ǡ�EXT4_EXT_MARK_UNINIT1�Ǳ��
		//�ָ��ǰ���ext4_extentδ��ʼ��״̬,EXT4_EXT_MARK_UNINIT2�Ǳ�Ƿָ�ĺ���ext4_extentδ��ʼ��״̬
		if (uninitialized)
			split_flag1 |= EXT4_EXT_MARK_UNINIT1 |
				       EXT4_EXT_MARK_UNINIT2;
        
		if (split_flag & EXT4_EXT_DATA_VALID2)
			split_flag1 |= EXT4_EXT_DATA_VALID1;
        /*��map->m_lblk + map->m_len����߼����ַΪ�ָ�㣬��path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χ
        ee_block~(ee_block+ee_len)�ָ��ee_block~(map->m_lblk + map->m_len)��(map->m_lblk + map->m_len)~(ee_block+ee_len)��
        Ȼ��Ѻ���map->m_lblk + map->m_len)~(ee_block+ee_len)��Ӧ��ext4_extent�ṹ��ӵ�ext4 extent B+��*/
		err = ext4_split_extent_at(handle, inode, path,
				map->m_lblk + map->m_len, split_flag1, flags1);
		if (err)
			goto out;
	} else {
	    //�����˵��map�Ľ����߼����ַ����ex�Ľ����߼����ַ����allocated=(ee_len+ee_block)-map->m_lblk����mapֻ���õ�ex�߼��鷶Χ
	    //���allocated���߼��飬�±�if (map->m_lblk >= ee_block)�϶�������ִ��ext4_split_extent_at()��ex���߼��鷶Χ�ָ��
	    //ee_block~map->m_lblk �� map->m_lblk~(ee_block + ee_len)��map->m_lblk~(ee_block + ee_len)��map����ӳ����߼��飬û�дﵽmap->len��
		allocated = ee_len - (map->m_lblk - ee_block);
	}
	/*
	 * Update path is required because previous ext4_split_extent_at() may
	 * result in split of original leaf or extent zeroout.
	 */
	ext4_ext_drop_refs(path);
    //�ϱ߿��ܰ�ex���߼��鷶Χ�ָ��ˣ�����������ext4 extent B+�������߼����ַ��Χ�ӽ�map->m_lblk�������ڵ��Ҷ�ӽ��
	path = ext4_ext_find_extent(inode, map->m_lblk, path);
	if (IS_ERR(path))
		return PTR_ERR(path);
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
    //ex�Ƿ���δ��ʼ��״̬
	uninitialized = ext4_ext_is_uninitialized(ex);
	split_flag1 = 0;
    
    //���map����ʼ�߼����ַ����ex����ʼ�߼����ַ����map->m_lblkΪ�ָ�㣬�ٴηָ��µ�ex�߼��鷶Χ
	if (map->m_lblk >= ee_block) {
		split_flag1 = split_flag & EXT4_EXT_DATA_VALID2;

        //���ex��δ��ʼ����ǣ���split_flag1������EXT4_EXT_MARK_UNINIT1��ǣ�EXT4_EXT_MARK_UNINIT1�Ǳ�Ƿָ��ǰ���ext4_extentδ��ʼ��״̬
		if (uninitialized) {
			split_flag1 |= EXT4_EXT_MARK_UNINIT1;
			split_flag1 |= split_flag & (EXT4_EXT_MAY_ZEROOUT |
						     EXT4_EXT_MARK_UNINIT2);
		}
        /*��map->m_lblk����߼����ַΪ�ָ�㣬��path[depth].p_extָ���ext4_extent�ṹ(��ex)���߼��鷶Χee_block~(ee_block+ee_len)�ָ�
        ��ee_block~map->m_lblk��map->m_lblk~(ee_block+ee_len)��Ȼ��Ѻ���map->m_lblk~(ee_block+ee_len)��Ӧ��ext4_extent
        �ṹ��ӵ�ext4 extent B+����ע�⣬�ϱߵ�ext4_split_extent_at()��ԭʼex�Ľ����˷ָȻ��ext4_ext_find_extent()
        ������ext4 extent B+�������߼����ַ��Χ�ӽ�map->m_lblk�������ڵ��Ҷ�ӽ�㣬�������ext4_split_extent_at()��path[depth].p_ext
        ָ���ext4_extent�ṹ�߼����ַ���ܱ��ˡ�*/
		err = ext4_split_extent_at(handle, inode, path,
				map->m_lblk, split_flag1, flags);
		if (err)
			goto out;
	}

	ext4_ext_show_leaf(inode, path);
out:
	return err ? err : allocated;
}

/*
 * This function is called by ext4_ext_map_blocks() if someone tries to write
 * to an uninitialized extent. It may result in splitting the uninitialized
 * extent into multiple extents (up to three - one initialized and two
 * uninitialized).
 * There are three possibilities:
 *   a> There is no split required: Entire extent should be initialized
 *   b> Splits in two extents: Write is happening at either end of the extent
 *   c> Splits in three extents: Somone is writing in middle of the extent
 *
 * Pre-conditions:
 *  - The extent pointed to by 'path' is uninitialized.
 *  - The extent pointed to by 'path' contains a superset
 *    of the logical span [map->m_lblk, map->m_lblk + map->m_len).
 *
 * Post-conditions on success:
 *  - the returned value is the number of blocks beyond map->l_lblk
 *    that are allocated and initialized.
 *    It is guaranteed to be >= map->m_len.
 */
//ext4_ext_map_blocks()->ext4_ext_handle_uninitialized_extents()/ext4_ext_handle_unwritten_extents()->ext4_ext_convert_to_initialized()
//ִ�е����path[depth].p_extָ���ext4_extent��δ��ʼ��״̬��ע�⣬ִ�е�������Ա�֤map->m_lblk��path[depth].p_ext��ex���߼���
//��Χ�ڵģ���ee_block <= map->m_lblk <ee_len��

/*�������Ҫӳ����������(�����߼�����)map->lenС��ex�Ѿ�ӳ����߼�����ee_len�����԰�ex��map->len���߼���ϲ�����ǰ�߻��ߺ�ߵ�
ext4_extent�ṹ(��abut_ex)���ϲ��������̣���Ҫ�����߼����ַ��������ַ�����ŵȵȡ�����ϲ��ɹ�ֱ�Ӵ�ext4_ext_convert_to_initialized()
�������ء�����ִ��ext4_split_extent()��ex���߼����ַ���̷ָ��2�λ���3�Σ��ָ������map->m_lblkΪ��ʼ��ַ�ҹ�allocated���߼�����߼���
��Χ����������Ҫ�ģ���allocated���߼�����Ա�֤ӳ��������顣��allocated<=map->len���������ܱ�֤mapҪ��ӳ���map->len���߼���ȫӳ����ɡ�
ע�⣬ext4_split_extent()��ex�ָ�󣬻�ʣ������1~2���߼��鷶Χ����Ҫ�����Ƕ�Ӧ��ext4_extent�ṹ�����ext4_extent B+����
*/
static int ext4_ext_convert_to_initialized(handle_t *handle,
					   struct inode *inode,
					   struct ext4_map_blocks *map,
					   struct ext4_ext_path *path,
					   int flags)
{
	struct ext4_sb_info *sbi;
	struct ext4_extent_header *eh;
	struct ext4_map_blocks split_map;
	struct ext4_extent zero_ex;
	struct ext4_extent *ex, *abut_ex;
	ext4_lblk_t ee_block, eof_block;
	unsigned int ee_len, depth, map_len = map->m_len;
	int allocated = 0, max_zeroout = 0;
	int err = 0;
	int split_flag = 0;

	ext_debug("ext4_ext_convert_to_initialized: inode %lu, logical"
		"block %llu, max_blocks %u\n", inode->i_ino,
		(unsigned long long)map->m_lblk, map_len);

	sbi = EXT4_SB(inode->i_sb);
	eof_block = (inode->i_size + inode->i_sb->s_blocksize - 1) >>
		inode->i_sb->s_blocksize_bits;
	if (eof_block < map->m_lblk + map_len)
		eof_block = map->m_lblk + map_len;

    //ext4 extent B+�����
	depth = ext_depth(inode);
    //ָ��ext4 extent B+��Ҷ�ӽڵ�ͷ�ṹext4_extent_header
	eh = path[depth].p_hdr;
    //ext4 extent B+��Ҷ�ӽڵ㣬ָ����ʼ�߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	ex = path[depth].p_ext;
    //ex���ext4_extent�ṹ����ʼ�߼����ַ
	ee_block = le32_to_cpu(ex->ee_block);
    //ex���ext4_extent�ṹӳ�����������
	ee_len = ext4_ext_get_actual_len(ex);
	zero_ex.ee_len = 0;

	trace_ext4_ext_convert_to_initialized_enter(inode, map, ex);

	/* Pre-conditions */
	BUG_ON(!ext4_ext_is_uninitialized(ex));
	BUG_ON(!in_range(map->m_lblk, ee_block, ee_len));

	/*
	 * Attempt to transfer newly initialized blocks from the currently
	 * uninitialized extent to its neighbor. This is much cheaper
	 * than an insertion followed by a merge as those involve costly
	 * memmove() calls. Transferring to the left is the common case in
	 * steady state for workloads doing fallocate(FALLOC_FL_KEEP_SIZE)
	 * followed by append writes.
	 *
	 * Limitations of the current logic:
	 *  - L1: we do not deal with writes covering the whole extent.
	 *    This would require removing the extent if the transfer
	 *    is possible.
	 *  - L2: we only attempt to merge with an extent stored in the
	 *    same extent tree node.
	 */

    /*�±����������if�жϣ���Ҫ��ӳ����������map_lenҪС��ex�Ѿ�ӳ����������ee_len������£����exǰ�߻��ߺ�ߵ�ext4_extent
    �ṹabut_ex����ex���߼����ַ�����ţ����԰�ex��map_len���߼���ϲ���abut_ex,�����������1:ex����ʼ�߼����ַ����ǰ�ߵ�
    ext4_extent��abut_ex�Ľ����߼����ַ;2 ex�Ľ����߼����ַ������ߵ�ext4_extent��abut_ex����ʼ�߼����ַ�����������������
    ��ex��ǰ���߿����map_len���߼���ϲ���abut_ex��ex���߼������ֻʣ��ee_len-map_len���ϲ���ex�������ó�δ��ʼ��״̬,
    ��abut_ex���ֳ�ʼ��״̬��allocated��abut_ex���ӵ��߼������map_len�����û�з����ϲ���allocated����0*/
    
	//Ҫӳ�����ʼ�߼����ַmap->m_lblk����ex����ʼ�߼����ַ
	if ((map->m_lblk == ee_block) &&
		/* See if we can merge left */
		(map_len < ee_len) &&		/*L1*///Ҫ��ӳ����������map_lenҪС��ex�Ѿ�ӳ����������ee_len
		//ex��ָ��Ҷ�ӽڵ�ext4_extent_header���2�����Ժ�ext4_extent�ṹ
		(ex > EXT_FIRST_EXTENT(eh))) {	/*L2*/
		ext4_lblk_t prev_lblk;
		ext4_fsblk_t prev_pblk, ee_pblk;
		unsigned int prev_len;

        //abut_exָ��ex��һ��struct ext4_extent�ṹ
		abut_ex = ex - 1;
		//��һ��struct ext4_extent�ṹ��abut_ex�������ʼ�߼����ַ
		prev_lblk = le32_to_cpu(abut_ex->ee_block);
        //��һ��struct ext4_extent�ṹ��abut_exӳ�����������
		prev_len = ext4_ext_get_actual_len(abut_ex);
        //��һ��struct ext4_extent�ṹ��abut_ex�������ʼ������ַ
		prev_pblk = ext4_ext_pblock(abut_ex);
        //ex���struct ext4_extent�ṹ�������ʼ������ַ
		ee_pblk = ext4_ext_pblock(ex);

		/*
		 * A transfer of blocks from 'ex' to 'abut_ex' is allowed
		 * upon those conditions:
		 * - C1: abut_ex is initialized,
		 * - C2: abut_ex is logically abutting ex,
		 * - C3: abut_ex is physically abutting ex,
		 * - C4: abut_ex can receive the additional blocks without
		 *   overflowing the (initialized) length limit.
		 */
		 
		/*exǰ�ߵ�ext4_extent��abut_ex��abut_ex�Ѿ���ʼ����������abut_ex��ex�����ţ�����
        mapҪ��ӳ����������map_lenҪС��ex�Ѿ�ӳ����������ee_len����ʱ��abut_ex�̲���ex��
        �߼��鷶Χ:��ex֮ǰ���߼��鷶Χee_block~ee_block+map_len���ָ�abut_ex���
        ext4_extent��ex�µ��߼����ַ��Χ��(ee_block + map_len)~(ee_block + ee_len)��һСƬ*/
		if ((!ext4_ext_is_uninitialized(abut_ex)) &&/*C1*///abut_ex�����ǳ�ʼ��״̬
             //abut_ex���߼����ַ��������ַ��ex�Ľ�����
            ((prev_lblk + prev_len) == ee_block) &&	/*C2*/
			((prev_pblk + prev_len) == ee_pblk) &&		/*C3*/
			//abut_ex��ex��ӳ������������ܺ�С��0x8000��һ��ext4_extent�ṹ���ӳ���
			//�����������ܳ���0x8000������Ҫ��abut_ex��ex������ext4_extent�ϲ�
			(prev_len < (EXT_INIT_MAX_LEN - map_len))) /*C4*/
		{
			err = ext4_ext_get_access(handle, inode, path + depth);
			if (err)
				goto out;

			trace_ext4_ext_convert_to_initialized_fastpath(inode,
				map, ex, abut_ex);

			/* Shift the start of ex by 'map_len' blocks */
            //�±������»���ex���ext4_extent�ṹ���߼����ַ��Χ����֮ǰee_block~ee_block+map_len
            //���ָ�abut_ex���ext4_extent��ex�µ��߼����ַ��Χ��(ee_block + map_len)~
            //(ee_block + ee_len)��exӳ����߼���(�����)����������map_len����abut_ex��������map_len��
			ex->ee_block = cpu_to_le32(ee_block + map_len);//�����µ��߼����׵�ַ
			ext4_ext_store_pblock(ex, ee_pblk + map_len);//�����µ�������׵�ַ
			ex->ee_len = cpu_to_le16(ee_len - map_len);//�����µ�ӳ�����������
            /*��ex���ext4_extent����"uninitialized"��ǣ������ص�*/
			ext4_ext_mark_uninitialized(ex); /* Restore the flag */

			/* Extend abut_ex by 'map_len' blocks */
			abut_ex->ee_len = cpu_to_le16(prev_len + map_len);//abut_exӳ���������������map_len��

			/* Result: number of initialized blocks past m_lblk */
            //allocated��abut_ex������߼������
			allocated = map_len;
		}
	} 
    //mapҪӳ��Ľ����߼����ַmap->m_lblk+map_len����ex�Ľ����߼����ַee_block + ee_len
    else if (((map->m_lblk + map_len) == (ee_block + ee_len)) &&
		   (map_len < ee_len) &&	/*L1*///Ҫ��ӳ����������map_lenҪС��ex�Ѿ�ӳ����������ee_len
		   ex < EXT_LAST_EXTENT(eh)) {	/*L2*///ex��ָ��Ҷ�ӽڵ����һ��ext4_extent�ṹ
		/* See if we can merge right */
		ext4_lblk_t next_lblk;
		ext4_fsblk_t next_pblk, ee_pblk;
		unsigned int next_len;

        //abut_exָ��ex��һ��struct ext4_extent�ṹ
		abut_ex = ex + 1;
        //��һ��struct ext4_extent�ṹ��abut_ex�������ʼ�߼����ַ
		next_lblk = le32_to_cpu(abut_ex->ee_block);
        //��һ��struct ext4_extent�ṹ��abut_exӳ�����������
		next_len = ext4_ext_get_actual_len(abut_ex);
        //��һ��struct ext4_extent�ṹ��abut_ex�������ʼ������ַ
		next_pblk = ext4_ext_pblock(abut_ex);
        //ex���struct ext4_extent�ṹ�������ʼ������ַ
		ee_pblk = ext4_ext_pblock(ex);

		/*
		 * A transfer of blocks from 'ex' to 'abut_ex' is allowed
		 * upon those conditions:
		 * - C1: abut_ex is initialized,
		 * - C2: abut_ex is logically abutting ex,
		 * - C3: abut_ex is physically abutting ex,
		 * - C4: abut_ex can receive the additional blocks without
		 *   overflowing the (initialized) length limit.
		 */
		 
		/*ex������ߵ�abut_ex�߼����ַ�����ţ���ex�����߼����ַ����abut����ʼ�߼����ַ��
         ����Ҫ��ӳ����������map_lenҪС��ex�Ѿ�ӳ����������ee_len�ȵȡ���ex��
         (ex->ee_block + ee_len - map_len)~(ex->ee_block + ee_len)��map_len���߼���ϲ���
         abut_ex���ϲ���abut_ex���߼��鷶Χ��(ex->ee_block + ee_len - map_len)~
         (next_lblk+next_len),ex���߼��鷶Χ��СΪex->ee_block~(ee_len - map_len)*/
		if ((!ext4_ext_is_uninitialized(abut_ex)) &&/*C1*///abut_ex�����ǳ�ʼ��״̬
             //abut_ex���߼����ַ��������ַ��ex�Ľ�����,abut_ex��ex���
            ((map->m_lblk + map_len) == next_lblk) &&/*C2*/
		    ((ee_pblk + ee_len) == next_pblk) &&		/*C3*/
		    //abut_ex��ex��ӳ������������ܺ�С��0x8000��һ��ext4_extent�ṹ���ӳ��������������ܳ���0x8000
		    (next_len < (EXT_INIT_MAX_LEN - map_len))) {	/*C4*/
			err = ext4_ext_get_access(handle, inode, path + depth);
			if (err)
				goto out;

			trace_ext4_ext_convert_to_initialized_fastpath(inode,
				map, ex, abut_ex);

			/* Shift the start of abut_ex by 'map_len' blocks */
            //�±����ǰ�ex���߼��鷶Χ(ex->ee_block + ee_len - map_len)~(ex->ee_block + ee_len)
            //��map_len���߼���ϲ�����ߵ�abut_ex���ϲ���abut_ex���߼��鷶Χ��
            //(ex->ee_block + ee_len - map_len)~(next_lblk+next_len),ex���߼��鷶Χ��СΪ
            //ex->ee_block~ee_len - map_len
			abut_ex->ee_block = cpu_to_le32(next_lblk - map_len);
			ext4_ext_store_pblock(abut_ex, next_pblk - map_len);//�����µ�������׵�ַ
            //exӳ����߼������������map_len��
            ex->ee_len = cpu_to_le16(ee_len - map_len);
            /*���exΪ"uninitialized"״̬,�����ص㣬ex����δ��ʼ��״̬*/
			ext4_ext_mark_uninitialized(ex); /* Restore the flag */

			/* Extend abut_ex by 'map_len' blocks */
            //abut_ex�߼������������map_len��
			abut_ex->ee_len = cpu_to_le16(next_len + map_len);

			/* Result: number of initialized blocks past m_lblk */
            //abut_ex�߼������������map+len��
			allocated = map_len;
		}
	}

    /*���ϱ�ex��abut_ex�ĺϲ��������ܽ�:
    1:�������Ҫӳ����߼��鷶Χmap->m_lblk ~ (map->m_lblk+map->m_len)��ex(��path[depth].p_extָ���ext4_extent)
    ���߼��鷶Χ�ڣ�����ee_block~(ee_block + ee_len)��Χ�ڡ�
    2:map->m_lblk==ee_block~����map->m_lblk+map->m_len==ee_block + ee_len����map���߼��鷶Χ��ex���߼��鷶Χ�ͷ�������β��
    3:ex����ǰ����ߺ��ext4_extent(��abut_ex)���߼����ַ�����š�

    ��������������ʱ�����ex�߼��鷶Χ�ڵı���Ҫӳ����߼���map->m_len���߼���ϲ���abut_ex����Ϊ����Ҫӳ����߼������ʼ�߼���
    ��ַmap->m_lblk���߽����߼����ַmap->m_lblk+map->m_len��abut_ex�Ľ����߼����ַ����ʼ�߼����ַ�����š���ˣ��൱�ڰѱ���
    Ҫӳ����߼���map->m_lblk ~ (map->m_lblk+map->m_len)��ex�������ȫ�ϲ���abut_ex���ϲ�map->m_len���߼��顣ΪʲôҪ��������?
    ��Ϊabut_ex�Ǳ���ѳ�ʼ��״̬�ģ�ex��δ��ʼ��״̬����ӳ����߼��鷶Χmap->m_lblk ~ (map->m_lblk+map->m_len)����ex���ͷ
    �������β�������ϲ���abut_ex���깤�ˣ������Ϳ��Ը�ext4 extent treeʹ���ˡ����ex��Ȼ�����δ��ʼ��״̬��
    ���map�ϲ���abut_ex map->m_len���߼��飬��allocated=map->m_len��ֱ��goto out�˳��ú�����
    
    ���û�з����ϲ�����allocated = ee_len - (map->m_lblk - ee_block)=(ee_len+ee_block) - map->m_lblk����map->m_lblk��ex�����߼���
    ��ַ(ee_block + ee_len)֮����߼�����������ִ��ext4_split_extent()��ex���߼��鷶Χ���̷ָ*/
    if (allocated) {//allocated��0˵��abut_ex�߼��鷶Χ�̲���ex map_len���߼���
		/* Mark the block containing both extents as dirty */
        //ext4_extentӳ����߼��鷶Χ���ܷ����仯�ˣ���Ƕ�Ӧ�������ӳ���bh�����ļ�inode��.
		ext4_ext_dirty(handle, inode, path + depth);

		/* Update path to point to the right extent */
        /*ext4 extentҶ�ӽڵ��Ϊabut_ex��ԭ����ex�����ˣ�����֪ʶ��*/
		path[depth].p_ext = abut_ex;
        
		goto out;//�˳��ú���
	} else
	    //���abut_exû���̲�ex���߼��飬allocated��map->m_lblk��ex�����߼����ַ֮����߼�����
		allocated = ee_len - (map->m_lblk - ee_block);//allocated=(ee_len+ee_block) - map->m_lblk

	WARN_ON(map->m_lblk < ee_block);
	/*
	 * It is safe to convert extent to initialized via explicit
	 * zeroout only if extent is fully insde i_size or new_size.
	 */
	split_flag |= ee_block + ee_len <= eof_block ? EXT4_EXT_MAY_ZEROOUT : 0;

	if (EXT4_EXT_MAY_ZEROOUT & split_flag)
		max_zeroout = sbi->s_extent_max_zeroout_kb >>
			(inode->i_sb->s_blocksize_bits - 10);

	/* If extent is less than s_max_zeroout_kb, zeroout directly */
	if (max_zeroout && (ee_len <= max_zeroout)) {//����һ�㲻����
		err = ext4_ext_zeroout(inode, ex);
		if (err)
			goto out;
        //��ex�߼�����Ϣ���Ƹ�zero_ex��zero_exɶ��?
		zero_ex.ee_block = ex->ee_block;
		zero_ex.ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex));
		ext4_ext_store_pblock(&zero_ex, ext4_ext_pblock(ex));

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;
        //ex���"initialized"״̬
		ext4_ext_mark_initialized(ex);
		ext4_ext_try_to_merge(handle, inode, path, ex);
		err = ext4_ext_dirty(handle, inode, path + path->p_depth);
		goto out;
	}

	/*
	 * four cases:
	 * 1. split the extent into three extents.
	 * 2. split the extent into two extents, zeroout the first half.
	 * 3. split the extent into two extents, zeroout the second half.
	 * 4. split the extent into two extents with out zeroout.
	 */
	split_map.m_lblk = map->m_lblk;
	split_map.m_len = map->m_len;

	if (max_zeroout && (allocated > map->m_len)) {//����һ�㲻����
		if (allocated <= max_zeroout) {
			/* case 3 */
			zero_ex.ee_block =
					 cpu_to_le32(map->m_lblk);
			zero_ex.ee_len = cpu_to_le16(allocated);
			ext4_ext_store_pblock(&zero_ex,
				ext4_ext_pblock(ex) + map->m_lblk - ee_block);
			err = ext4_ext_zeroout(inode, &zero_ex);
			if (err)
				goto out;
			split_map.m_lblk = map->m_lblk;
			split_map.m_len = allocated;
		} else if (map->m_lblk - ee_block + map->m_len < max_zeroout) {
			/* case 2 */
			if (map->m_lblk != ee_block) {
				zero_ex.ee_block = ex->ee_block;
				zero_ex.ee_len = cpu_to_le16(map->m_lblk -
							ee_block);
				ext4_ext_store_pblock(&zero_ex,
						      ext4_ext_pblock(ex));
				err = ext4_ext_zeroout(inode, &zero_ex);
				if (err)
					goto out;
			}

			split_map.m_lblk = ee_block;
			split_map.m_len = map->m_lblk - ee_block + map->m_len;
			allocated = map->m_len;
		}
	}
    /*�����map->m_lblk����ex����ʼ�߼����ַee_block�ǿ��Ա�֤�ġ���map����ʼ�߼����ַmap->m_lblk�϶���ex���߼��鷶Χ�ڡ�
      ����ִ��ext4_split_extent()��ex���߼��鷶Χee_block~(ee_block + ee_len)���зָ�ָ�����м���
    1:��� map->m_lblk +map->m_len С��ee_block + ee_len����map�Ľ����߼����ַС��ex�Ľ����߼����ַ�����ex���߼��鷶Χ�ָ��3��
     ee_block~map->m_lblk �� map->m_lblk~(map->m_lblk +map->m_len) �� (map->m_lblk +map->m_len)~(ee_block + ee_len)���������������
     ��֤����Ҫ��ӳ���map->m_len���߼��鶼�����ӳ�䣬��allocated =map->m_len��
    2:��� map->m_lblk +map->m_len ���ڵ���ee_block + ee_len����map�Ľ����߼����ַ����ex�Ľ����߼����ַ�����ex���߼��鷶Χ�ָ��2��
     ee_block~map->m_lblk �� map->m_lblk~(ee_block + ee_len)��������������ܱ�֤����Ҫ��ӳ���map->m_len���߼��鶼���ӳ�䡣ֻ��ӳ��
     (ee_block + ee_len) - map->m_lblk���߼��飬��allocated =(ee_block + ee_len) - map->m_lblk��
    */
	allocated = ext4_split_extent(handle, inode, path,
				      &split_map, split_flag, flags);
	if (allocated < 0)
		err = allocated;

out:
	/* If we have gotten a failure, don't zero out status tree */
	if (!err)
		err = ext4_es_zeroout(inode, &zero_ex);
	return err ? err : allocated;
}

/*
 * This function is called by ext4_ext_map_blocks() from
 * ext4_get_blocks_dio_write() when DIO to write
 * to an uninitialized extent.
 *
 * Writing to an uninitialized extent may result in splitting the uninitialized
 * extent into multiple initialized/uninitialized extents (up to three)
 * There are three possibilities:
 *   a> There is no split required: Entire extent should be uninitialized
 *   b> Splits in two extents: Write is happening at either end of the extent
 *   c> Splits in three extents: Somone is writing in middle of the extent
 *
 * One of more index blocks maybe needed if the extent tree grow after
 * the uninitialized extent split. To prevent ENOSPC occur at the IO
 * complete, we need to split the uninitialized extent before DIO submit
 * the IO. The uninitialized extent called at this time will be split
 * into three uninitialized extent(at most). After IO complete, the part
 * being filled will be convert to initialized by the end_io callback function
 * via ext4_convert_unwritten_extents().
 *
 * Returns the size of uninitialized extent to be written on success.
 */
static int ext4_split_unwritten_extents(handle_t *handle,
					struct inode *inode,
					struct ext4_map_blocks *map,
					struct ext4_ext_path *path,
					int flags)
{
	ext4_lblk_t eof_block;
	ext4_lblk_t ee_block;
	struct ext4_extent *ex;
	unsigned int ee_len;
	int split_flag = 0, depth;

	ext_debug("ext4_split_unwritten_extents: inode %lu, logical"
		"block %llu, max_blocks %u\n", inode->i_ino,
		(unsigned long long)map->m_lblk, map->m_len);

	eof_block = (inode->i_size + inode->i_sb->s_blocksize - 1) >>
		inode->i_sb->s_blocksize_bits;
	if (eof_block < map->m_lblk + map->m_len)
		eof_block = map->m_lblk + map->m_len;
	/*
	 * It is safe to convert extent to initialized via explicit
	 * zeroout only if extent is fully insde i_size or new_size.
	 */
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);

	split_flag |= ee_block + ee_len <= eof_block ? EXT4_EXT_MAY_ZEROOUT : 0;
	split_flag |= EXT4_EXT_MARK_UNINIT2;
	if (flags & EXT4_GET_BLOCKS_CONVERT)
		split_flag |= EXT4_EXT_DATA_VALID2;
	flags |= EXT4_GET_BLOCKS_PRE_IO;
	return ext4_split_extent(handle, inode, path, map, split_flag, flags);
}

static int ext4_convert_unwritten_extents_endio(handle_t *handle,
						struct inode *inode,
						struct ext4_map_blocks *map,
						struct ext4_ext_path *path)
{
	struct ext4_extent *ex;
	ext4_lblk_t ee_block;
	unsigned int ee_len;
	int depth;
	int err = 0;

	depth = ext_depth(inode);
    //exָ����ʼ�߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	ex = path[depth].p_ext;
    //ext4_extent�ṹ�������ʼ�߼����ַ
	ee_block = le32_to_cpu(ex->ee_block);
    //ext4_extent�ṹ�����ӳ�����������
	ee_len = ext4_ext_get_actual_len(ex);

	ext_debug("ext4_convert_unwritten_extents_endio: inode %lu, logical"
		"block %llu, max_blocks %u\n", inode->i_ino,
		  (unsigned long long)ee_block, ee_len);

	/* If extent is larger than requested it is a clear sign that we still
	 * have some extent state machine issues left. So extent_split is still
	 * required.
	 * TODO: Once all related issues will be fixed this situation should be
	 * illegal.
	 */
	if (ee_block != map->m_lblk || ee_len > map->m_len) {
#ifdef EXT4_DEBUG
		ext4_warning("Inode (%ld) finished: extent logical block %llu,"
			     " len %u; IO logical block %llu, len %u\n",
			     inode->i_ino, (unsigned long long)ee_block, ee_len,
			     (unsigned long long)map->m_lblk, map->m_len);
#endif
		err = ext4_split_unwritten_extents(handle, inode, map, path,
						   EXT4_GET_BLOCKS_CONVERT);
		if (err < 0)
			goto out;
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode, map->m_lblk, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}
		depth = ext_depth(inode);
		ex = path[depth].p_ext;
	}

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;

    //���ex��initialized״̬
	/* first mark the extent as initialized */
	ext4_ext_mark_initialized(ex);

	/* note: ext4_ext_correct_indexes() isn't needed here because
	 * borders are not changed
	 */
	ext4_ext_try_to_merge(handle, inode, path, ex);

	/* Mark modified extent as dirty */
	err = ext4_ext_dirty(handle, inode, path + path->p_depth);
out:
	ext4_ext_show_leaf(inode, path);
	return err;
}

static void unmap_underlying_metadata_blocks(struct block_device *bdev,
			sector_t block, int count)
{
	int i;
	for (i = 0; i < count; i++)
                unmap_underlying_metadata(bdev, block + i);
}

/*
 * Handle EOFBLOCKS_FL flag, clearing it if necessary
 */
static int check_eofblocks_fl(handle_t *handle, struct inode *inode,
			      ext4_lblk_t lblk,
			      struct ext4_ext_path *path,
			      unsigned int len)
{
	int i, depth;
	struct ext4_extent_header *eh;
	struct ext4_extent *last_ex;

	if (!ext4_test_inode_flag(inode, EXT4_INODE_EOFBLOCKS))
		return 0;

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;

	/*
	 * We're going to remove EOFBLOCKS_FL entirely in future so we
	 * do not care for this case anymore. Simply remove the flag
	 * if there are no extents.
	 */
	if (unlikely(!eh->eh_entries))
		goto out;
	last_ex = EXT_LAST_EXTENT(eh);
	/*
	 * We should clear the EOFBLOCKS_FL flag if we are writing the
	 * last block in the last extent in the file.  We test this by
	 * first checking to see if the caller to
	 * ext4_ext_get_blocks() was interested in the last block (or
	 * a block beyond the last block) in the current extent.  If
	 * this turns out to be false, we can bail out from this
	 * function immediately.
	 */
	if (lblk + len < le32_to_cpu(last_ex->ee_block) +
	    ext4_ext_get_actual_len(last_ex))
		return 0;
	/*
	 * If the caller does appear to be planning to write at or
	 * beyond the end of the current extent, we then test to see
	 * if the current extent is the last extent in the file, by
	 * checking to make sure it was reached via the rightmost node
	 * at each level of the tree.
	 */
	for (i = depth-1; i >= 0; i--)
		if (path[i].p_idx != EXT_LAST_INDEX(path[i].p_hdr))
			return 0;
out:
	ext4_clear_inode_flag(inode, EXT4_INODE_EOFBLOCKS);
	return ext4_mark_inode_dirty(handle, inode);
}

/**
 * ext4_find_delalloc_range: find delayed allocated block in the given range.
 *
 * Return 1 if there is a delalloc block in the range, otherwise 0.
 */
int ext4_find_delalloc_range(struct inode *inode,
			     ext4_lblk_t lblk_start,
			     ext4_lblk_t lblk_end)
{
	struct extent_status es;

	ext4_es_find_delayed_extent_range(inode, lblk_start, lblk_end, &es);
	if (es.es_len == 0)
		return 0; /* there is no delay extent in this tree */
	else if (es.es_lblk <= lblk_start &&
		 lblk_start < es.es_lblk + es.es_len)
		return 1;
	else if (lblk_start <= es.es_lblk && es.es_lblk <= lblk_end)
		return 1;
	else
		return 0;
}

int ext4_find_delalloc_cluster(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_lblk_t lblk_start, lblk_end;
	lblk_start = EXT4_LBLK_CMASK(sbi, lblk);
	lblk_end = lblk_start + sbi->s_cluster_ratio - 1;

	return ext4_find_delalloc_range(inode, lblk_start, lblk_end);
}

/**
 * Determines how many complete clusters (out of those specified by the 'map')
 * are under delalloc and were reserved quota for.
 * This function is called when we are writing out the blocks that were
 * originally written with their allocation delayed, but then the space was
 * allocated using fallocate() before the delayed allocation could be resolved.
 * The cases to look for are:
 * ('=' indicated delayed allocated blocks
 *  '-' indicates non-delayed allocated blocks)
 * (a) partial clusters towards beginning and/or end outside of allocated range
 *     are not delalloc'ed.
 *	Ex:
 *	|----c---=|====c====|====c====|===-c----|
 *	         |++++++ allocated ++++++|
 *	==> 4 complete clusters in above example
 *
 * (b) partial cluster (outside of allocated range) towards either end is
 *     marked for delayed allocation. In this case, we will exclude that
 *     cluster.
 *	Ex:
 *	|----====c========|========c========|
 *	     |++++++ allocated ++++++|
 *	==> 1 complete clusters in above example
 *
 *	Ex:
 *	|================c================|
 *            |++++++ allocated ++++++|
 *	==> 0 complete clusters in above example
 *
 * The ext4_da_update_reserve_space will be called only if we
 * determine here that there were some "entire" clusters that span
 * this 'allocated' range.
 * In the non-bigalloc case, this function will just end up returning num_blks
 * without ever calling ext4_find_delalloc_range.
 */
static unsigned int
get_reserved_cluster_alloc(struct inode *inode, ext4_lblk_t lblk_start,
			   unsigned int num_blks)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_lblk_t alloc_cluster_start, alloc_cluster_end;
	ext4_lblk_t lblk_from, lblk_to, c_offset;
	unsigned int allocated_clusters = 0;

	alloc_cluster_start = EXT4_B2C(sbi, lblk_start);
	alloc_cluster_end = EXT4_B2C(sbi, lblk_start + num_blks - 1);

	/* max possible clusters for this allocation */
	allocated_clusters = alloc_cluster_end - alloc_cluster_start + 1;

	trace_ext4_get_reserved_cluster_alloc(inode, lblk_start, num_blks);

	/* Check towards left side */
	c_offset = EXT4_LBLK_COFF(sbi, lblk_start);
	if (c_offset) {
		lblk_from = EXT4_LBLK_CMASK(sbi, lblk_start);
		lblk_to = lblk_from + c_offset - 1;

		if (ext4_find_delalloc_range(inode, lblk_from, lblk_to))
			allocated_clusters--;
	}

	/* Now check towards right. */
	c_offset = EXT4_LBLK_COFF(sbi, lblk_start + num_blks);
	if (allocated_clusters && c_offset) {
		lblk_from = lblk_start + num_blks;
		lblk_to = lblk_from + (sbi->s_cluster_ratio - c_offset) - 1;

		if (ext4_find_delalloc_range(inode, lblk_from, lblk_to))
			allocated_clusters--;
	}

	return allocated_clusters;
}

//�ص�����ߵ�ext4_ext_convert_to_initialized()����
static int
ext4_ext_handle_uninitialized_extents(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map,
			struct ext4_ext_path *path, int flags,
			unsigned int allocated, ext4_fsblk_t newblock)
{
	int ret = 0;
	int err = 0;
	ext4_io_end_t *io = ext4_inode_aio(inode);

	ext_debug("ext4_ext_handle_uninitialized_extents: inode %lu, logical "
		  "block %llu, max_blocks %u, flags %x, allocated %u\n",
		  inode->i_ino, (unsigned long long)map->m_lblk, map->m_len,
		  flags, allocated);
	ext4_ext_show_leaf(inode, path);

	/*
	 * When writing into uninitialized space, we should not fail to
	 * allocate metadata blocks for the new extent block if needed.
	 */
	flags |= EXT4_GET_BLOCKS_METADATA_NOFAIL;

	trace_ext4_ext_handle_uninitialized_extents(inode, map, flags,
						    allocated, newblock);

	/* get_block() before submit the IO, split the extent */
    //���if��direct IOģʽ�ų���
	if ((flags & EXT4_GET_BLOCKS_PRE_IO/*0x0008*/)) {
		ret = ext4_split_unwritten_extents(handle, inode, map,
						   path, flags);
		if (ret <= 0)
			goto out;
		/*
		 * Flag the inode(non aio case) or end_io struct (aio case)
		 * that this IO needs to conversion to written when IO is
		 * completed
		 */
		if (io)
			ext4_set_io_unwritten_flag(inode, io);
		else
			ext4_set_inode_state(inode, EXT4_STATE_DIO_UNWRITTEN);
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		if (ext4_should_dioread_nolock(inode))
			map->m_flags |= EXT4_MAP_UNINIT;
		goto out;
	}

    
	/* IO end_io complete, convert the filled extent to written */
    //���ò����DIOģʽ��IO������ɻص�����end_io()ʱִ�е���
	if ((flags & EXT4_GET_BLOCKS_CONVERT/*0x0010*/)) {
		ret = ext4_convert_unwritten_extents_endio(handle, inode, map,
							path);
		if (ret >= 0) {
			ext4_update_inode_fsync_trans(handle, inode, 1);
			err = check_eofblocks_fl(handle, inode, map->m_lblk,
						 path, map->m_len);
		} else
			err = ret;
		map->m_flags |= EXT4_MAP_MAPPED;
		if (allocated > map->m_len)
			allocated = map->m_len;
		map->m_len = allocated;
		goto out2;
	}
    
	/* buffered IO case һ����ļ���д������*/
	/*
	 * repeat fallocate creation request
	 * we already have an unwritten extent
	 */
	if (flags & EXT4_GET_BLOCKS_UNINIT_EXT/*0x0002*/) {
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		goto map_out;
	}

	/* buffered READ or buffered write_begin() lookup */
    //�����֧�������ǵ�һ�ζ�д�ļ���ext4
	if ((flags & EXT4_GET_BLOCKS_CREATE/*0x0001*/) == 0) {
		/*
		 * We have blocks reserved already.  We
		 * return allocated blocks so that delalloc
		 * won't do block reservation for us.  But
		 * the buffer head will be unmapped so that
		 * a read from the block returns 0s.
		 */
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		goto out1;
	}

	/* buffered write, writepage time, convert*/
    //����ִ�е�����
/*�������Ҫӳ����������(�����߼�����)map->lenС��ex�Ѿ�ӳ����߼�����ee_len�����԰�ex��map->len���߼���ϲ�����ǰ�߻��ߺ�ߵ�
ext4_extent�ṹ(��abut_ex)���ϲ��������̣���Ҫ�����߼����ַ��������ַ�����ŵȵȡ�����ϲ��ɹ�ֱ�Ӵ�ext4_ext_convert_to_initialized()
�������ء�����ִ��ext4_split_extent()��ex���߼����ַ���̷ָ��2�λ���3�Σ��ָ������map->m_lblkΪ��ʼ��ַ�ҹ�allocated���߼�����߼���
��Χ����������Ҫ�ģ���allocated���߼�����Ա�֤ӳ��������顣��allocated<=map->len���������ܱ�֤mapҪ��ӳ���map->len���߼���ȫӳ����ɡ�
ע�⣬ext4_split_extent()��ex�ָ�󣬻�ʣ������1~2���߼��鷶Χ����Ҫ�����Ƕ�Ӧ��ext4_extent�ṹ�����ext4_extent B+����
*/
	ret = ext4_ext_convert_to_initialized(handle, inode, map, path, flags);
	if (ret >= 0)
		ext4_update_inode_fsync_trans(handle, inode, 1);
out:
	if (ret <= 0) {
		err = ret;
		goto out2;
	} else
		allocated = ret;
	map->m_flags |= EXT4_MAP_NEW;
	/*
	 * if we allocated more blocks than requested
	 * we need to make sure we unmap the extra block
	 * allocated. The actual needed block will get
	 * unmapped later when we find the buffer_head marked
	 * new.
	 */
	if (allocated > map->m_len) {
		unmap_underlying_metadata_blocks(inode->i_sb->s_bdev,
					newblock + map->m_len,
					allocated - map->m_len);
		allocated = map->m_len;
	}
	map->m_len = allocated;

	/*
	 * If we have done fallocate with the offset that is already
	 * delayed allocated, we would have block reservation
	 * and quota reservation done in the delayed write path.
	 * But fallocate would have already updated quota and block
	 * count for this offset. So cancel these reservation
	 */
	if (flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE) {
		unsigned int reserved_clusters;
		reserved_clusters = get_reserved_cluster_alloc(inode,
				map->m_lblk, map->m_len);
		if (reserved_clusters)
			ext4_da_update_reserve_space(inode,
						     reserved_clusters,
						     0);
	}

map_out:
	map->m_flags |= EXT4_MAP_MAPPED;
	if ((flags & EXT4_GET_BLOCKS_KEEP_SIZE/*0x0080*/) == 0) {
		err = check_eofblocks_fl(handle, inode, map->m_lblk, path,
					 map->m_len);
		if (err < 0)
			goto out2;
	}
out1:
	if (allocated > map->m_len)
		allocated = map->m_len;
	ext4_ext_show_leaf(inode, path);
	map->m_pblk = newblock;
	map->m_len = allocated;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	return err ? err : allocated;
}

/*
 * get_implied_cluster_alloc - check to see if the requested
 * allocation (in the map structure) overlaps with a cluster already
 * allocated in an extent.
 *	@sb	The filesystem superblock structure
 *	@map	The requested lblk->pblk mapping
 *	@ex	The extent structure which might contain an implied
 *			cluster allocation
 *
 * This function is called by ext4_ext_map_blocks() after we failed to
 * find blocks that were already in the inode's extent tree.  Hence,
 * we know that the beginning of the requested region cannot overlap
 * the extent from the inode's extent tree.  There are three cases we
 * want to catch.  The first is this case:
 *
 *		 |--- cluster # N--|
 *    |--- extent ---|	|---- requested region ---|
 *			|==========|
 *
 * The second case that we need to test for is this one:
 *
 *   |--------- cluster # N ----------------|
 *	   |--- requested region --|   |------- extent ----|
 *	   |=======================|
 *
 * The third case is when the requested region lies between two extents
 * within the same cluster:
 *          |------------- cluster # N-------------|
 * |----- ex -----|                  |---- ex_right ----|
 *                  |------ requested region ------|
 *                  |================|
 *
 * In each of the above cases, we need to set the map->m_pblk and
 * map->m_len so it corresponds to the return the extent labelled as
 * "|====|" from cluster #N, since it is already in use for data in
 * cluster EXT4_B2C(sbi, map->m_lblk).	We will then return 1 to
 * signal to ext4_ext_map_blocks() that map->m_pblk should be treated
 * as a new "allocated" block region.  Otherwise, we will return 0 and
 * ext4_ext_map_blocks() will then allocate one or more new clusters
 * by calling ext4_mb_new_blocks().
 */
static int get_implied_cluster_alloc(struct super_block *sb,
				     struct ext4_map_blocks *map,
				     struct ext4_extent *ex,
				     struct ext4_ext_path *path)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_lblk_t c_offset = EXT4_LBLK_COFF(sbi, map->m_lblk);
	ext4_lblk_t ex_cluster_start, ex_cluster_end;
	ext4_lblk_t rr_cluster_start;
	ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
	ext4_fsblk_t ee_start = ext4_ext_pblock(ex);
	unsigned short ee_len = ext4_ext_get_actual_len(ex);

	/* The extent passed in that we are trying to match */
	ex_cluster_start = EXT4_B2C(sbi, ee_block);
	ex_cluster_end = EXT4_B2C(sbi, ee_block + ee_len - 1);

	/* The requested region passed into ext4_map_blocks() */
	rr_cluster_start = EXT4_B2C(sbi, map->m_lblk);

	if ((rr_cluster_start == ex_cluster_end) ||
	    (rr_cluster_start == ex_cluster_start)) {
		if (rr_cluster_start == ex_cluster_end)
			ee_start += ee_len - 1;
		map->m_pblk = EXT4_PBLK_CMASK(sbi, ee_start) + c_offset;
		map->m_len = min(map->m_len,
				 (unsigned) sbi->s_cluster_ratio - c_offset);
		/*
		 * Check for and handle this case:
		 *
		 *   |--------- cluster # N-------------|
		 *		       |------- extent ----|
		 *	   |--- requested region ---|
		 *	   |===========|
		 */

		if (map->m_lblk < ee_block)
			map->m_len = min(map->m_len, ee_block - map->m_lblk);

		/*
		 * Check for the case where there is already another allocated
		 * block to the right of 'ex' but before the end of the cluster.
		 *
		 *          |------------- cluster # N-------------|
		 * |----- ex -----|                  |---- ex_right ----|
		 *                  |------ requested region ------|
		 *                  |================|
		 */
		if (map->m_lblk > ee_block) {
			ext4_lblk_t next = ext4_ext_next_allocated_block(path);
			map->m_len = min(map->m_len, next - map->m_lblk);
		}

		trace_ext4_get_implied_cluster_alloc_exit(sb, map, 1);
		return 1;
	}

	trace_ext4_get_implied_cluster_alloc_exit(sb, map, 0);
	return 0;
}


/*
 * Block allocation/map/preallocation routine for extents based files
 *
 *
 * Need to be called with
 * down_read(&EXT4_I(inode)->i_data_sem) if not allocating file system block
 * (ie, create is zero). Otherwise down_write(&EXT4_I(inode)->i_data_sem)
 *
 * return > 0, number of of blocks already mapped/allocated
 *          if create == 0 and these are pre-allocated blocks
 *          	buffer head is unmapped
 *          otherwise blocks are mapped
 *
 * return = 0, if plain look up failed (blocks have not been allocated)
 *          buffer head is unmapped
 *
 * return < 0, error case.
 */
//���ݴ�����ļ���Ŀ¼inode���߼���ַmap->m_lblk��ext4�ļ�ϵͳ��data block������map->m_len������飬�����߼���ַmap->m_lblk����ӳ�䣬����ӳ���ϵ���浽ext4 extent�ṹ
int ext4_ext_map_blocks(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map, int flags)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent newex, *ex, *ex2;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_fsblk_t newblock = 0;
	int free_on_err = 0, err = 0, depth, ret;
	unsigned int allocated = 0, offset = 0;
	unsigned int allocated_clusters = 0;
	struct ext4_allocation_request ar;
	ext4_io_end_t *io = ext4_inode_aio(inode);
	ext4_lblk_t cluster_offset;
	int set_unwritten = 0;

	ext_debug("blocks %u/%u requested for inode %lu\n",
		  map->m_lblk, map->m_len, inode->i_ino);
	trace_ext4_ext_map_blocks_enter(inode, map->m_lblk, map->m_len, flags);

	/* find extent for this block */

	/*��ext4 extent B+��ÿһ�������ڵ�(�������ڵ�)���ҵ���ʼ�߼����ַ��ӽ��������ʼ�߼����ַmap->m_lblk��ext4_extent_idx
	�ṹ���浽path[ppos]->p_idx.Ȼ���ҵ����һ���Ҷ�ӽڵ�����ӽ��������ʼ�߼����ַmap->m_lblk��ext4_extent�ṹ��
	���浽path[ppos]->p_ext�����ext4_extent�Ű������߼����ַ��������ַ��ӳ���ϵ��*/
	path = ext4_ext_find_extent(inode, map->m_lblk, NULL);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out2;
	}
    //ext4 extent B+�����
	depth = ext_depth(inode);

	/*
	 * consistent leaf must not be empty;
	 * this situation is possible, though, _during_ tree modification;
	 * this is why assert can't be put in ext4_ext_find_extent()
	 */
	if (unlikely(path[depth].p_ext == NULL && depth != 0)) {
		EXT4_ERROR_INODE(inode, "bad extent address "
				 "lblock: %lu, depth: %d pblock %lld",
				 (unsigned long) map->m_lblk, depth,
				 path[depth].p_block);
		err = -EIO;
		goto out2;
	}
    //ָ����ʼ�߼����ַ��ӽ�map->m_lblk�����ʼ�߼����ַ��ext4_extent
	ex = path[depth].p_ext;
	if (ex) {
        //ext4_extent�ṹ�������ʼ�߼����ַ
		ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
        //ext4_extent�ṹ�������ʼ������ַ
		ext4_fsblk_t ee_start = ext4_ext_pblock(ex);
		unsigned short ee_len;

		/*
		 * Uninitialized extents are treated as holes, except that
		 * we split out initialized portions during a write.
		 */
		//ex���߼����ַӳ�����������
		ee_len = ext4_ext_get_actual_len(ex);

		trace_ext4_ext_show_extent(inode, ee_block, ee_start, ee_len);

		/* if found extent covers block, simply return it */
        ////���map->m_lblk��ex���߼����ַ��Χ�ڣ���֣�Ϊʲôֻ��map->m_lblk��û��map->m_lblk+m_len��?map->m_lblk~map->m_lblk+
        //m_len���Ǳ���Ҫӳ����߼����ַ��Χѽ������ں����ĺ������жϡ�ע�⣬�����ܱ�֤map->m_lblk����ex�������ʼ�߼����ַ��Χ�ڣ�
        //��Ϊmap->m_lblk��ǳ���map->m_lblk > ee_block + ee_len����ʱֻ�����±߻���map->m_lblk����һ���µ�ext4_extent�ṹ������ext4_extent B+��
		if (in_range(map->m_lblk, ee_block, ee_len)) {
            //newblock : map->m_lblk�����ʼ�߼����ַ��Ӧ��������ַ
			newblock = map->m_lblk - ee_block + ee_start;
			/* number of remaining blocks in the extent */
            //allocated : map->m_lblk��(ee_block+ee_len)�����Χ��block����
            //ext4_extent->ee_block+ext4_extent->ee_len��ext4_extent�ṹ�Ľ����߼����ַ
            allocated = ee_len - (map->m_lblk - ee_block);
			ext_debug("%u fit into %u:%d -> %llu\n", map->m_lblk,
				  ee_block, ee_len, newblock);
            
            //ex�Ѿ���ʼ����ֱ��goto out���أ�����ִ���±ߵ�ext4_ext_handle_uninitialized_extents()
			if (!ext4_ext_is_uninitialized(ex))
				goto out;
            
            /*ע�⣬�����и����ص㣬ex��δ��ʼ��״̬�Ż�������ִ��ext4_ext_handle_uninitialized_extents()*/
			ret = ext4_ext_handle_uninitialized_extents(//�߰汾�ں� �������Ƹ�Ϊ ext4_ext_handle_unwritten_extents()
				handle, inode, map, path, flags,
				allocated, newblock);
			if (ret < 0)
				err = ret;
			else
				allocated = ret;
			goto out3;
		}
	}

	if ((sbi->s_cluster_ratio > 1) &&
	    ext4_find_delalloc_cluster(inode, map->m_lblk))
		map->m_flags |= EXT4_MAP_FROM_CLUSTER;

	/*
	 * requested block isn't allocated yet;
	 * we couldn't try to create block if create flag is zero
	 */
	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {
		/*
		 * put just found gap into cache to speed up
		 * subsequent requests
		 */
		if ((flags & EXT4_GET_BLOCKS_NO_PUT_HOLE) == 0)
			ext4_ext_put_gap_in_cache(inode, path, map->m_lblk);
		goto out2;
	}

	/*
	 * Okay, we need to do block allocation.
	 */
	map->m_flags &= ~EXT4_MAP_FROM_CLUSTER;
    //����newex����ʼ�߼���ţ�newex����Ա���ӳ������ext4_extent�ṹ
	newex.ee_block = cpu_to_le32(map->m_lblk);
	cluster_offset = EXT4_LBLK_COFF(sbi, map->m_lblk);

	/*
	 * If we are doing bigalloc, check to see if the extent returned
	 * by ext4_ext_find_extent() implies a cluster we can use.
	 */
	if (cluster_offset && ex &&
	    get_implied_cluster_alloc(inode->i_sb, map, ex, path)) {
		ar.len = allocated = map->m_len;
		newblock = map->m_pblk;
		map->m_flags |= EXT4_MAP_FROM_CLUSTER;
		goto got_allocated_blocks;
	}

	/* find neighbour allocated blocks */
	ar.lleft = map->m_lblk;
    //ar.lleft = le32_to_cpu(ex->ee_block) + ee_len - 1
	err = ext4_ext_search_left(inode, path, &ar.lleft, &ar.pleft);//ar.lleftӰ�쵽�±�ext4_mb_new_blocks()����ӳ��������
	if (err)
		goto out2;
	ar.lright = map->m_lblk;
	ex2 = NULL;
//path[depth].p_ext����Ҷ�ӽڵ����һ��ext4_extent�ṹ�����ҵ�path[depth].p_ext��ߵ�ext4_extent�ṹ��ex2��ex2����ʼ�߼����ַ����
//ar.lright ������ѡ��ext4 extent B+����ߵ������ڵ��µ�Ҷ�ӽڵ�ĵ�һ��ext4_extent�ṹ��ex2��ex2����ʼ�߼����ַ����ar.lright
	err = ext4_ext_search_right(inode, path, &ar.lright, &ar.pright, &ex2);//ar.lrightӰ�쵽�±�ext4_mb_new_blocks()����ӳ��������
	if (err)
		goto out2;

	/* Check if the extent after searching to the right implies a
	 * cluster we can use. */
	//sbi->s_cluster_ratio=1
	if ((sbi->s_cluster_ratio > 1) && ex2 &&
	    get_implied_cluster_alloc(inode->i_sb, map, ex2, path)) {//������
		ar.len = allocated = map->m_len;
		newblock = map->m_pblk;
		map->m_flags |= EXT4_MAP_FROM_CLUSTER;
		goto got_allocated_blocks;
	}

	/*
	 * See if request is beyond maximum number of blocks we can have in
	 * a single extent. For an initialized extent this limit is
	 * EXT_INIT_MAX_LEN and for an uninitialized extent this limit is
	 * EXT_UNINIT_MAX_LEN.
	 */
	if (map->m_len > EXT_INIT_MAX_LEN &&
	    !(flags & EXT4_GET_BLOCKS_UNINIT_EXT))//������
		map->m_len = EXT_INIT_MAX_LEN;
	else if (map->m_len > EXT_UNINIT_MAX_LEN &&
		 (flags & EXT4_GET_BLOCKS_UNINIT_EXT))//������
		map->m_len = EXT_UNINIT_MAX_LEN;

	/* Check if we can really insert (m_lblk)::(m_lblk + m_len) extent */
	newex.ee_len = cpu_to_le16(map->m_len);
	err = ext4_ext_check_overlap(sbi, inode, &newex, path);
	if (err)
		allocated = ext4_ext_get_actual_len(&newex);
	else
		allocated = map->m_len;

    /*ע�⣬ִ�е�����˵��û�д�ext4 extent�ҵ������߼���ַmap->m_lblkӳ�������飬���Ǿ�Ҫ��ext4�ļ�ϵͳ����map->m_len������飬
    Ȼ�����߼���ַmap->m_lblk����ӳ�䡣ext4_ext_find_goal()������һ��Ŀ��������ar.goal��Ȼ��ִ��ext4_mb_new_blocks():
    ��ar.goalΪ��׼����������map->m_len������顣����ٹ������߼���ַmap->m_lblk��ӳ��*/
    
	/* allocate new block */
	ar.inode = inode;
    //ҪΪ�ļ�inode���䱣�����ݵ�������ˣ��ú����Ǵ�inode������������һ������Ŀ�������飬�������������鿪ʼ����
    //�����ղ��ұ���Ҫ���������顣��˵���ҵ�map->m_lblk�߼����ַӳ���Ŀ�� ��ʼ������ַ�����ظ�ar.goal
	ar.goal = ext4_ext_find_goal(inode, path, map->m_lblk);
    //ar.logical���߼����ַ
	ar.logical = map->m_lblk;
	/*
	 * We calculate the offset from the beginning of the cluster
	 * for the logical block number, since when we allocate a
	 * physical cluster, the physical block should start at the
	 * same offset from the beginning of the cluster.  This is
	 * needed so that future calls to get_implied_cluster_alloc()
	 * work correctly.
	 */
	offset = EXT4_LBLK_COFF(sbi, map->m_lblk);//offset����ʱ0
	//�������������
	ar.len = EXT4_NUM_B2C(sbi, offset+allocated);
    //������������ʼ��ַ
	ar.goal -= offset;
	ar.logical -= offset;
	if (S_ISREG(inode->i_mode))
		ar.flags = EXT4_MB_HINT_DATA;
	else
		/* disable in-core preallocation for non-regular files */
		ar.flags = 0;
	if (flags & EXT4_GET_BLOCKS_NO_NORMALIZE)
		ar.flags |= EXT4_MB_HINT_NOPREALLOC;
    /*����map->m_len������飬�����map->m_lblk�߼����ַӳ���map->m_len������飬������map->m_len����������ʼ������newblock��*/
    //���Խ�� newblock �� ar.goal��ʱ��ȣ���ʱ����ȡ�����ӳ�����ʼ�߼����ַ��map->m_lblk��ӳ����������map->m_len��ext4_mb_new_blocks()
    //����Ҫ�ҵ�newblock�����ʼ�߼����ַ�����ñ�֤�ҵ�newblock��ͷ������map->m_len������飬�����������ģ�����Ǹ���Ҫ�ġ�
	newblock = ext4_mb_new_blocks(handle, &ar, &err);
	if (!newblock)
		goto out2;
	ext_debug("allocate new block: goal %llu, found %llu/%u\n",
		  ar.goal, newblock, allocated);
	free_on_err = 1;
	allocated_clusters = ar.len;
	ar.len = EXT4_C2B(sbi, ar.len) - offset;
	if (ar.len > allocated)
		ar.len = allocated;

got_allocated_blocks:
	/* try to insert new extent into found leaf and return */
    //���ñ���ӳ���map->m_len����������ʼ������(newblock)��newex��newex����Ա���ӳ������ext4_extent�ṹ
	ext4_ext_store_pblock(&newex, newblock + offset);//offset��0
	//����newexӳ���������������ִ��ext4_ext_mark_initialized()���ex�ѳ�ʼ��һ��Ч��
	newex.ee_len = cpu_to_le16(ar.len);
	/* Mark uninitialized */
	if (flags & EXT4_GET_BLOCKS_UNINIT_EXT){
		ext4_ext_mark_uninitialized(&newex);
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		/*
		 * io_end structure was created for every IO write to an
		 * uninitialized extent. To avoid unnecessary conversion,
		 * here we flag the IO that really needs the conversion.
		 * For non asycn direct IO case, flag the inode state
		 * that we need to perform conversion when IO is done.
		 */
		if ((flags & EXT4_GET_BLOCKS_PRE_IO))
			set_unwritten = 1;
		if (ext4_should_dioread_nolock(inode))
			map->m_flags |= EXT4_MAP_UNINIT;
	}

	err = 0;
	if ((flags & EXT4_GET_BLOCKS_KEEP_SIZE) == 0)//����
		err = check_eofblocks_fl(handle, inode, map->m_lblk,
					 path, ar.len);
    
	if (!err)//��newex�������ext4 extent B+��
		err = ext4_ext_insert_extent(handle, inode, path,
					     &newex, flags);

	if (!err && set_unwritten) {
		if (io)
			ext4_set_io_unwritten_flag(inode, io);
		else
			ext4_set_inode_state(inode,
					     EXT4_STATE_DIO_UNWRITTEN);
	}

	if (err && free_on_err) {
		int fb_flags = flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE ?
			EXT4_FREE_BLOCKS_NO_QUOT_UPDATE : 0;
		/* free data blocks we just allocated */
		/* not a good idea to call discard here directly,
		 * but otherwise we'd need to call it every free() */
		ext4_discard_preallocations(inode);
		ext4_free_blocks(handle, inode, NULL, ext4_ext_pblock(&newex),
				 ext4_ext_get_actual_len(&newex), fb_flags);
		goto out2;
	}

	/* previous routine could use block we allocated */
	newblock = ext4_ext_pblock(&newex);
	allocated = ext4_ext_get_actual_len(&newex);
	if (allocated > map->m_len)
		allocated = map->m_len;
	map->m_flags |= EXT4_MAP_NEW;

	/*
	 * Update reserved blocks/metadata blocks after successful
	 * block allocation which had been deferred till now.
	 */
	if (flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE) {
		unsigned int reserved_clusters;
		/*
		 * Check how many clusters we had reserved this allocated range
		 */
		reserved_clusters = get_reserved_cluster_alloc(inode,
						map->m_lblk, allocated);
		if (map->m_flags & EXT4_MAP_FROM_CLUSTER) {
			if (reserved_clusters) {
				/*
				 * We have clusters reserved for this range.
				 * But since we are not doing actual allocation
				 * and are simply using blocks from previously
				 * allocated cluster, we should release the
				 * reservation and not claim quota.
				 */
				ext4_da_update_reserve_space(inode,
						reserved_clusters, 0);
			}
		} else {
			BUG_ON(allocated_clusters < reserved_clusters);
			if (reserved_clusters < allocated_clusters) {
				struct ext4_inode_info *ei = EXT4_I(inode);
				int reservation = allocated_clusters -
						  reserved_clusters;
				/*
				 * It seems we claimed few clusters outside of
				 * the range of this allocation. We should give
				 * it back to the reservation pool. This can
				 * happen in the following case:
				 *
				 * * Suppose s_cluster_ratio is 4 (i.e., each
				 *   cluster has 4 blocks. Thus, the clusters
				 *   are [0-3],[4-7],[8-11]...
				 * * First comes delayed allocation write for
				 *   logical blocks 10 & 11. Since there were no
				 *   previous delayed allocated blocks in the
				 *   range [8-11], we would reserve 1 cluster
				 *   for this write.
				 * * Next comes write for logical blocks 3 to 8.
				 *   In this case, we will reserve 2 clusters
				 *   (for [0-3] and [4-7]; and not for [8-11] as
				 *   that range has a delayed allocated blocks.
				 *   Thus total reserved clusters now becomes 3.
				 * * Now, during the delayed allocation writeout
				 *   time, we will first write blocks [3-8] and
				 *   allocate 3 clusters for writing these
				 *   blocks. Also, we would claim all these
				 *   three clusters above.
				 * * Now when we come here to writeout the
				 *   blocks [10-11], we would expect to claim
				 *   the reservation of 1 cluster we had made
				 *   (and we would claim it since there are no
				 *   more delayed allocated blocks in the range
				 *   [8-11]. But our reserved cluster count had
				 *   already gone to 0.
				 *
				 *   Thus, at the step 4 above when we determine
				 *   that there are still some unwritten delayed
				 *   allocated blocks outside of our current
				 *   block range, we should increment the
				 *   reserved clusters count so that when the
				 *   remaining blocks finally gets written, we
				 *   could claim them.
				 */
				dquot_reserve_block(inode,
						EXT4_C2B(sbi, reservation));
				spin_lock(&ei->i_block_reservation_lock);
				ei->i_reserved_data_blocks += reservation;
				spin_unlock(&ei->i_block_reservation_lock);
			}
			/*
			 * We will claim quota for all newly allocated blocks.
			 * We're updating the reserved space *after* the
			 * correction above so we do not accidentally free
			 * all the metadata reservation because we might
			 * actually need it later on.
			 */
			ext4_da_update_reserve_space(inode, allocated_clusters,
							1);
		}
	}

	/*
	 * Cache the extent and update transaction to commit on fdatasync only
	 * when it is _not_ an uninitialized extent.
	 */
	if ((flags & EXT4_GET_BLOCKS_UNINIT_EXT) == 0)
		ext4_update_inode_fsync_trans(handle, inode, 1);
	else
		ext4_update_inode_fsync_trans(handle, inode, 0);
out:
	if (allocated > map->m_len)
		allocated = map->m_len;
	ext4_ext_show_leaf(inode, path);
	map->m_flags |= EXT4_MAP_MAPPED;
    //������ʼ�߼����ַmap->m_lblkӳ�����ʼ������
	map->m_pblk = newblock;
    //�����߼����ַ���ӳ�����������������ܱ�֤allocated���ڴ����map->m_len�����п���С��
	map->m_len = allocated;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}

out3:
	trace_ext4_ext_map_blocks_exit(inode, map, err ? err : allocated);

	return err ? err : allocated;
}

void ext4_ext_truncate(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	ext4_lblk_t last_block;
	int err = 0;

	/*
	 * TODO: optimization is possible here.
	 * Probably we need not scan at all,
	 * because page truncation is enough.
	 */

	/* we have to know where to truncate from in crash case */
	EXT4_I(inode)->i_disksize = inode->i_size;
	ext4_mark_inode_dirty(handle, inode);

	last_block = (inode->i_size + sb->s_blocksize - 1)
			>> EXT4_BLOCK_SIZE_BITS(sb);
retry:
	err = ext4_es_remove_extent(inode, last_block,
				    EXT_MAX_BLOCKS - last_block);
	if (err == -ENOMEM) {
		cond_resched();
		congestion_wait(BLK_RW_ASYNC, HZ/50);
		goto retry;
	}
	if (err) {
		ext4_std_error(inode->i_sb, err);
		return;
	}
	err = ext4_ext_remove_space(inode, last_block, EXT_MAX_BLOCKS - 1);
	ext4_std_error(inode->i_sb, err);
}

static void ext4_falloc_update_inode(struct inode *inode,
				int mode, loff_t new_size, int update_ctime)
{
	struct timespec now;

	if (update_ctime) {
		now = current_fs_time(inode->i_sb);
		if (!timespec_equal(&inode->i_ctime, &now))
			inode->i_ctime = now;
	}
	/*
	 * Update only when preallocation was requested beyond
	 * the file size.
	 */
	if (!(mode & FALLOC_FL_KEEP_SIZE)) {
		if (new_size > i_size_read(inode))
			i_size_write(inode, new_size);
		if (new_size > EXT4_I(inode)->i_disksize)
			ext4_update_i_disksize(inode, new_size);
	} else {
		/*
		 * Mark that we allocate beyond EOF so the subsequent truncate
		 * can proceed even if the new size is the same as i_size.
		 */
		if (new_size > i_size_read(inode))
			ext4_set_inode_flag(inode, EXT4_INODE_EOFBLOCKS);
	}

}

/*
 * preallocate space for a file. This implements ext4's fallocate file
 * operation, which gets called from sys_fallocate system call.
 * For block-mapped files, posix_fallocate should fall back to the method
 * of writing zeroes to the required new blocks (the same behavior which is
 * expected for file systems which do not support fallocate() system call).
 */
long ext4_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	handle_t *handle;
	loff_t new_size;
	unsigned int max_blocks;
	int ret = 0;
	int ret2 = 0;
	int retries = 0;
	int flags;
	struct ext4_map_blocks map;
	unsigned int credits, blkbits = inode->i_blkbits;

	/* Return error if mode is not supported */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		return ext4_punch_hole(file, offset, len);

	ret = ext4_convert_inline_data(inode);
	if (ret)
		return ret;

	/*
	 * currently supporting (pre)allocate mode for extent-based
	 * files _only_
	 */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return -EOPNOTSUPP;

	trace_ext4_fallocate_enter(inode, offset, len, mode);
	map.m_lblk = offset >> blkbits;
	/*
	 * We can't just convert len to max_blocks because
	 * If blocksize = 4096 offset = 3072 and len = 2048
	 */
	max_blocks = (EXT4_BLOCK_ALIGN(len + offset, blkbits) >> blkbits)
		- map.m_lblk;
	/*
	 * credits to insert 1 extent into extent tree
	 */
	credits = ext4_chunk_trans_blocks(inode, max_blocks);
	mutex_lock(&inode->i_mutex);
	ret = inode_newsize_ok(inode, (len + offset));
	if (ret) {
		mutex_unlock(&inode->i_mutex);
		trace_ext4_fallocate_exit(inode, offset, max_blocks, ret);
		return ret;
	}
	flags = EXT4_GET_BLOCKS_CREATE_UNINIT_EXT;
	if (mode & FALLOC_FL_KEEP_SIZE)
		flags |= EXT4_GET_BLOCKS_KEEP_SIZE;
	/*
	 * Don't normalize the request if it can fit in one extent so
	 * that it doesn't get unnecessarily split into multiple
	 * extents.
	 */
	if (len <= EXT_UNINIT_MAX_LEN << blkbits)
		flags |= EXT4_GET_BLOCKS_NO_NORMALIZE;

retry:
	while (ret >= 0 && ret < max_blocks) {
		map.m_lblk = map.m_lblk + ret;
		map.m_len = max_blocks = max_blocks - ret;
		handle = ext4_journal_start(inode, EXT4_HT_MAP_BLOCKS,
					    credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}
		ret = ext4_map_blocks(handle, inode, &map, flags);
		if (ret <= 0) {
#ifdef EXT4FS_DEBUG
			ext4_warning(inode->i_sb,
				     "inode #%lu: block %u: len %u: "
				     "ext4_ext_map_blocks returned %d",
				     inode->i_ino, map.m_lblk,
				     map.m_len, ret);
#endif
			ext4_mark_inode_dirty(handle, inode);
			ret2 = ext4_journal_stop(handle);
			break;
		}
		if ((map.m_lblk + ret) >= (EXT4_BLOCK_ALIGN(offset + len,
						blkbits) >> blkbits))
			new_size = offset + len;
		else
			new_size = ((loff_t) map.m_lblk + ret) << blkbits;

		ext4_falloc_update_inode(inode, mode, new_size,
					 (map.m_flags & EXT4_MAP_NEW));
		ext4_mark_inode_dirty(handle, inode);
		if ((file->f_flags & O_SYNC) && ret >= max_blocks)
			ext4_handle_sync(handle);
		ret2 = ext4_journal_stop(handle);
		if (ret2)
			break;
	}
	if (ret == -ENOSPC &&
			ext4_should_retry_alloc(inode->i_sb, &retries)) {
		ret = 0;
		goto retry;
	}
	mutex_unlock(&inode->i_mutex);
	trace_ext4_fallocate_exit(inode, offset, max_blocks,
				ret > 0 ? ret2 : ret);
	return ret > 0 ? ret2 : ret;
}

/*
 * This function convert a range of blocks to written extents
 * The caller of this function will pass the start offset and the size.
 * all unwritten extents within this range will be converted to
 * written extents.
 *
 * This function is called from the direct IO end io call back
 * function, to convert the fallocated extents after IO is completed.
 * Returns 0 on success.
 */
int ext4_convert_unwritten_extents(struct inode *inode, loff_t offset,
				    ssize_t len)
{
	handle_t *handle;
	unsigned int max_blocks;
	int ret = 0;
	int ret2 = 0;
	struct ext4_map_blocks map;
	unsigned int credits, blkbits = inode->i_blkbits;

	map.m_lblk = offset >> blkbits;
	/*
	 * We can't just convert len to max_blocks because
	 * If blocksize = 4096 offset = 3072 and len = 2048
	 */
	max_blocks = ((EXT4_BLOCK_ALIGN(len + offset, blkbits) >> blkbits) -
		      map.m_lblk);
	/*
	 * credits to insert 1 extent into extent tree
	 */
	credits = ext4_chunk_trans_blocks(inode, max_blocks);
	while (ret >= 0 && ret < max_blocks) {
		map.m_lblk += ret;
		map.m_len = (max_blocks -= ret);
		handle = ext4_journal_start(inode, EXT4_HT_MAP_BLOCKS, credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}
		ret = ext4_map_blocks(handle, inode, &map,
				      EXT4_GET_BLOCKS_IO_CONVERT_EXT);
		if (ret <= 0)
			ext4_warning(inode->i_sb,
				     "inode #%lu: block %u: len %u: "
				     "ext4_ext_map_blocks returned %d",
				     inode->i_ino, map.m_lblk,
				     map.m_len, ret);
		ext4_mark_inode_dirty(handle, inode);
		ret2 = ext4_journal_stop(handle);
		if (ret <= 0 || ret2 )
			break;
	}
	return ret > 0 ? ret2 : ret;
}

/*
 * If newes is not existing extent (newes->ec_pblk equals zero) find
 * delayed extent at start of newes and update newes accordingly and
 * return start of the next delayed extent.
 *
 * If newes is existing extent (newes->ec_pblk is not equal zero)
 * return start of next delayed extent or EXT_MAX_BLOCKS if no delayed
 * extent found. Leave newes unmodified.
 */
static int ext4_find_delayed_extent(struct inode *inode,
				    struct extent_status *newes)
{
	struct extent_status es;
	ext4_lblk_t block, next_del;

	if (newes->es_pblk == 0) {
		ext4_es_find_delayed_extent_range(inode, newes->es_lblk,
				newes->es_lblk + newes->es_len - 1, &es);

		/*
		 * No extent in extent-tree contains block @newes->es_pblk,
		 * then the block may stay in 1)a hole or 2)delayed-extent.
		 */
		if (es.es_len == 0)
			/* A hole found. */
			return 0;

		if (es.es_lblk > newes->es_lblk) {
			/* A hole found. */
			newes->es_len = min(es.es_lblk - newes->es_lblk,
					    newes->es_len);
			return 0;
		}

		newes->es_len = es.es_lblk + es.es_len - newes->es_lblk;
	}

	block = newes->es_lblk + newes->es_len;
	ext4_es_find_delayed_extent_range(inode, block, EXT_MAX_BLOCKS, &es);
	if (es.es_len == 0)
		next_del = EXT_MAX_BLOCKS;
	else
		next_del = es.es_lblk;

	return next_del;
}
/* fiemap flags we can handle specified here */
#define EXT4_FIEMAP_FLAGS	(FIEMAP_FLAG_SYNC|FIEMAP_FLAG_XATTR)

static int ext4_xattr_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo)
{
	__u64 physical = 0;
	__u64 length;
	__u32 flags = FIEMAP_EXTENT_LAST;
	int blockbits = inode->i_sb->s_blocksize_bits;
	int error = 0;

	/* in-inode? */
	if (ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {
		struct ext4_iloc iloc;
		int offset;	/* offset of xattr in inode */

		error = ext4_get_inode_loc(inode, &iloc);
		if (error)
			return error;
		physical = (__u64)iloc.bh->b_blocknr << blockbits;
		offset = EXT4_GOOD_OLD_INODE_SIZE +
				EXT4_I(inode)->i_extra_isize;
		physical += offset;
		length = EXT4_SB(inode->i_sb)->s_inode_size - offset;
		flags |= FIEMAP_EXTENT_DATA_INLINE;
		brelse(iloc.bh);
	} else { /* external block */
		physical = (__u64)EXT4_I(inode)->i_file_acl << blockbits;
		length = inode->i_sb->s_blocksize;
	}

	if (physical)
		error = fiemap_fill_next_extent(fieinfo, 0, physical,
						length, flags);
	return (error < 0 ? error : 0);
}

int ext4_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		__u64 start, __u64 len)
{
	ext4_lblk_t start_blk;
	int error = 0;

	if (ext4_has_inline_data(inode)) {
		int has_inline = 1;

		error = ext4_inline_data_fiemap(inode, fieinfo, &has_inline);

		if (has_inline)
			return error;
	}

	/* fallback to generic here if not in extents fmt */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return generic_block_fiemap(inode, fieinfo, start, len,
			ext4_get_block);

	if (fiemap_check_flags(fieinfo, EXT4_FIEMAP_FLAGS))
		return -EBADR;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR) {
		error = ext4_xattr_fiemap(inode, fieinfo);
	} else {
		ext4_lblk_t len_blks;
		__u64 last_blk;

		start_blk = start >> inode->i_sb->s_blocksize_bits;
		last_blk = (start + len - 1) >> inode->i_sb->s_blocksize_bits;
		if (last_blk >= EXT_MAX_BLOCKS)
			last_blk = EXT_MAX_BLOCKS-1;
		len_blks = ((ext4_lblk_t) last_blk) - start_blk + 1;

		/*
		 * Walk the extent tree gathering extent information
		 * and pushing extents back to the user.
		 */
		error = ext4_fill_fiemap_extents(inode, start_blk,
						 len_blks, fieinfo);
	}

	return error;
}
