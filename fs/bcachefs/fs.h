#ifndef _BCACHEFS_FS_H
#define _BCACHEFS_FS_H

#include "inode.h"
#include "opts.h"
#include "str_hash.h"
#include "quota_types.h"

#include <linux/seqlock.h>
#include <linux/stat.h>

struct bch_inode_info {
	struct inode		v;

	struct mutex		ei_update_lock;
	u64			ei_journal_seq;
	u64			ei_quota_reserved;
	unsigned long		ei_last_dirtied;

	struct mutex		ei_quota_lock;
	struct bch_qid		ei_qid;

	struct bch_hash_info	ei_str_hash;

	/* copy of inode in btree: */
	struct bch_inode_unpacked ei_inode;
};

#define to_bch_ei(_inode)					\
	container_of_or_null(_inode, struct bch_inode_info, v)

static inline int ptrcmp(void *l, void *r)
{
	return (l > r) - (l < r);
}

#define __bch2_lock_inodes(_lock, ...)					\
do {									\
	struct bch_inode_info *a[] = { NULL, __VA_ARGS__ };		\
	unsigned i;							\
									\
	bubble_sort(&a[1], ARRAY_SIZE(a) - 1 , ptrcmp);			\
									\
	for (i = ARRAY_SIZE(a) - 1; a[i]; --i)				\
		if (a[i] != a[i - 1]) {					\
			if (_lock)					\
				mutex_lock_nested(&a[i]->ei_update_lock, i);\
			else						\
				mutex_unlock(&a[i]->ei_update_lock);	\
		}							\
} while (0)

#define bch2_lock_inodes(...)	__bch2_lock_inodes(true, __VA_ARGS__)
#define bch2_unlock_inodes(...)	__bch2_lock_inodes(false, __VA_ARGS__)

static inline struct bch_inode_info *file_bch_inode(struct file *file)
{
	return to_bch_ei(file_inode(file));
}

static inline u8 mode_to_type(umode_t mode)
{
	return (mode >> 12) & 15;
}

static inline unsigned nlink_bias(umode_t mode)
{
	return S_ISDIR(mode) ? 2 : 1;
}

static inline u64 bch2_current_time(struct bch_fs *c)
{
	struct timespec64 ts;
	ktime_get_coarse_real_ts64(&ts);
	return timespec_to_bch2_time(c, ts);
}

static inline bool inode_attr_changing(struct bch_inode_info *dir,
				struct bch_inode_info *inode,
				enum inode_opt_id id)
{
	return !(inode->ei_inode.bi_fields_set & (1 << id)) &&
		bch2_inode_opt_get(&dir->ei_inode, id) !=
		bch2_inode_opt_get(&inode->ei_inode, id);
}

static inline bool inode_attrs_changing(struct bch_inode_info *dir,
				 struct bch_inode_info *inode)
{
	unsigned id;

	for (id = 0; id < Inode_opt_nr; id++)
		if (inode_attr_changing(dir, inode, id))
			return true;

	return false;
}

struct bch_inode_unpacked;

#ifndef NO_BCACHEFS_FS

int bch2_fs_quota_transfer(struct bch_fs *,
			   struct bch_inode_info *,
			   struct bch_qid,
			   unsigned,
			   enum quota_acct_mode);

static inline int bch2_set_projid(struct bch_fs *c,
				  struct bch_inode_info *inode,
				  u32 projid)
{
	struct bch_qid qid = inode->ei_qid;

	qid.q[QTYP_PRJ] = projid;

	return bch2_fs_quota_transfer(c, inode, qid,
				      1 << QTYP_PRJ,
				      KEY_TYPE_QUOTA_PREALLOC);
}

struct inode *bch2_vfs_inode_get(struct bch_fs *, u64);

/* returns 0 if we want to do the update, or error is passed up */
typedef int (*inode_set_fn)(struct bch_inode_info *,
			    struct bch_inode_unpacked *, void *);

void bch2_inode_update_after_write(struct bch_fs *,
				   struct bch_inode_info *,
				   struct bch_inode_unpacked *,
				   unsigned);
int __must_check bch2_write_inode_trans(struct btree_trans *,
				struct bch_inode_info *,
				struct bch_inode_unpacked *,
				inode_set_fn, void *);
int __must_check bch2_write_inode(struct bch_fs *, struct bch_inode_info *,
				  inode_set_fn, void *, unsigned);

int bch2_reinherit_attrs_fn(struct bch_inode_info *,
			    struct bch_inode_unpacked *,
			    void *);

void bch2_vfs_exit(void);
int bch2_vfs_init(void);

#else

static inline void bch2_vfs_exit(void) {}
static inline int bch2_vfs_init(void) { return 0; }

#endif /* NO_BCACHEFS_FS */

#endif /* _BCACHEFS_FS_H */
