#include "bcachefs.h"
#include "alloc_background.h"
#include "alloc_foreground.h"
#include "btree_cache.h"
#include "btree_io.h"
#include "btree_update.h"
#include "btree_update_interior.h"
#include "btree_gc.h"
#include "buckets.h"
#include "clock.h"
#include "debug.h"
#include "ec.h"
#include "error.h"
#include "journal_io.h"

#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/sort.h>
#include <trace/events/bcachefs.h>

static const char * const bch2_alloc_field_names[] = {
#define x(name, bytes) #name,
	BCH_ALLOC_FIELDS()
#undef x
	NULL
};

static void bch2_recalc_oldest_io(struct bch_fs *, struct bch_dev *, int);

/* Ratelimiting/PD controllers */

static void pd_controllers_update(struct work_struct *work)
{
	struct bch_fs *c = container_of(to_delayed_work(work),
					   struct bch_fs,
					   pd_controllers_update);
	struct bch_dev *ca;
	unsigned i;

	for_each_member_device(ca, c, i) {
		struct bch_dev_usage stats = bch2_dev_usage_read(c, ca);

		u64 free = bucket_to_sector(ca,
				__dev_buckets_free(ca, stats)) << 9;
		/*
		 * Bytes of internal fragmentation, which can be
		 * reclaimed by copy GC
		 */
		s64 fragmented = (bucket_to_sector(ca,
					stats.buckets[BCH_DATA_USER] +
					stats.buckets[BCH_DATA_CACHED]) -
				  (stats.sectors[BCH_DATA_USER] +
				   stats.sectors[BCH_DATA_CACHED])) << 9;

		fragmented = max(0LL, fragmented);

		bch2_pd_controller_update(&ca->copygc_pd,
					 free, fragmented, -1);
	}

	schedule_delayed_work(&c->pd_controllers_update,
			      c->pd_controllers_update_seconds * HZ);
}

/* Persistent alloc info: */

static inline u64 get_alloc_field(const struct bch_alloc *a,
				  const void **p, unsigned field)
{
	unsigned bytes = BCH_ALLOC_FIELD_BYTES[field];
	u64 v;

	if (!(a->fields & (1 << field)))
		return 0;

	switch (bytes) {
	case 1:
		v = *((const u8 *) *p);
		break;
	case 2:
		v = le16_to_cpup(*p);
		break;
	case 4:
		v = le32_to_cpup(*p);
		break;
	case 8:
		v = le64_to_cpup(*p);
		break;
	default:
		BUG();
	}

	*p += bytes;
	return v;
}

static inline void put_alloc_field(struct bkey_i_alloc *a, void **p,
				   unsigned field, u64 v)
{
	unsigned bytes = BCH_ALLOC_FIELD_BYTES[field];

	if (!v)
		return;

	a->v.fields |= 1 << field;

	switch (bytes) {
	case 1:
		*((u8 *) *p) = v;
		break;
	case 2:
		*((__le16 *) *p) = cpu_to_le16(v);
		break;
	case 4:
		*((__le32 *) *p) = cpu_to_le32(v);
		break;
	case 8:
		*((__le64 *) *p) = cpu_to_le64(v);
		break;
	default:
		BUG();
	}

	*p += bytes;
}

struct bkey_alloc_unpacked bch2_alloc_unpack(const struct bch_alloc *a)
{
	struct bkey_alloc_unpacked ret = { .gen = a->gen };
	const void *d = a->data;
	unsigned idx = 0;

#define x(_name, _bits)	ret._name = get_alloc_field(a, &d, idx++);
	BCH_ALLOC_FIELDS()
#undef  x
	return ret;
}

static void bch2_alloc_pack(struct bkey_i_alloc *dst,
			    const struct bkey_alloc_unpacked src)
{
	unsigned idx = 0;
	void *d = dst->v.data;

	dst->v.fields	= 0;
	dst->v.gen	= src.gen;

#define x(_name, _bits)	put_alloc_field(dst, &d, idx++, src._name);
	BCH_ALLOC_FIELDS()
#undef  x

	set_bkey_val_bytes(&dst->k, (void *) d - (void *) &dst->v);
}

static unsigned bch_alloc_val_u64s(const struct bch_alloc *a)
{
	unsigned i, bytes = offsetof(struct bch_alloc, data);

	for (i = 0; i < ARRAY_SIZE(BCH_ALLOC_FIELD_BYTES); i++)
		if (a->fields & (1 << i))
			bytes += BCH_ALLOC_FIELD_BYTES[i];

	return DIV_ROUND_UP(bytes, sizeof(u64));
}

const char *bch2_alloc_invalid(const struct bch_fs *c, struct bkey_s_c k)
{
	struct bkey_s_c_alloc a = bkey_s_c_to_alloc(k);

	if (k.k->p.inode >= c->sb.nr_devices ||
	    !c->devs[k.k->p.inode])
		return "invalid device";

	/* allow for unknown fields */
	if (bkey_val_u64s(a.k) < bch_alloc_val_u64s(a.v))
		return "incorrect value size";

	return NULL;
}

void bch2_alloc_to_text(struct printbuf *out, struct bch_fs *c,
			struct bkey_s_c k)
{
	struct bkey_s_c_alloc a = bkey_s_c_to_alloc(k);
	const void *d = a.v->data;
	unsigned i;

	pr_buf(out, "gen %u", a.v->gen);

	for (i = 0; i < BCH_ALLOC_FIELD_NR; i++)
		if (a.v->fields & (1 << i))
			pr_buf(out, " %s %llu",
			       bch2_alloc_field_names[i],
			       get_alloc_field(a.v, &d, i));
}

static void __alloc_read_key(struct bucket *g, const struct bch_alloc *a)
{
	const void *d = a->data;
	unsigned idx = 0, data_type, dirty_sectors, cached_sectors;
	struct bucket_mark m;

	g->io_time[READ]	= get_alloc_field(a, &d, idx++);
	g->io_time[WRITE]	= get_alloc_field(a, &d, idx++);
	data_type		= get_alloc_field(a, &d, idx++);
	dirty_sectors		= get_alloc_field(a, &d, idx++);
	cached_sectors		= get_alloc_field(a, &d, idx++);
	g->oldest_gen		= get_alloc_field(a, &d, idx++);

	bucket_cmpxchg(g, m, ({
		m.gen			= a->gen;
		m.data_type		= data_type;
		m.dirty_sectors		= dirty_sectors;
		m.cached_sectors	= cached_sectors;
	}));

	g->gen_valid		= 1;
}

static void __alloc_write_key(struct bkey_i_alloc *a, struct bucket *g,
			      struct bucket_mark m)
{
	unsigned idx = 0;
	void *d = a->v.data;

	a->v.fields	= 0;
	a->v.gen	= m.gen;

	d = a->v.data;
	put_alloc_field(a, &d, idx++, g->io_time[READ]);
	put_alloc_field(a, &d, idx++, g->io_time[WRITE]);
	put_alloc_field(a, &d, idx++, m.data_type);
	put_alloc_field(a, &d, idx++, m.dirty_sectors);
	put_alloc_field(a, &d, idx++, m.cached_sectors);
	put_alloc_field(a, &d, idx++, g->oldest_gen);

	set_bkey_val_bytes(&a->k, (void *) d - (void *) &a->v);
}

static void bch2_alloc_read_key(struct bch_fs *c, struct bkey_s_c k)
{
	struct bch_dev *ca;
	struct bkey_s_c_alloc a;

	if (k.k->type != KEY_TYPE_alloc)
		return;

	a = bkey_s_c_to_alloc(k);
	ca = bch_dev_bkey_exists(c, a.k->p.inode);

	if (a.k->p.offset >= ca->mi.nbuckets)
		return;

	percpu_down_read_preempt_disable(&c->mark_lock);
	__alloc_read_key(bucket(ca, a.k->p.offset), a.v);
	percpu_up_read_preempt_enable(&c->mark_lock);
}

int bch2_alloc_read(struct bch_fs *c, struct list_head *journal_replay_list)
{
	struct journal_replay *r;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_dev *ca;
	unsigned i;
	int ret;

	for_each_btree_key(&iter, c, BTREE_ID_ALLOC, POS_MIN, 0, k) {
		bch2_alloc_read_key(c, k);
		bch2_btree_iter_cond_resched(&iter);
	}

	ret = bch2_btree_iter_unlock(&iter);
	if (ret)
		return ret;

	list_for_each_entry(r, journal_replay_list, list) {
		struct bkey_i *k, *n;
		struct jset_entry *entry;

		for_each_jset_key(k, n, entry, &r->j)
			if (entry->btree_id == BTREE_ID_ALLOC)
				bch2_alloc_read_key(c, bkey_i_to_s_c(k));
	}

	for_each_member_device(ca, c, i)
		bch2_dev_usage_from_buckets(c, ca);

	mutex_lock(&c->bucket_clock[READ].lock);
	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		bch2_recalc_oldest_io(c, ca, READ);
		up_read(&ca->bucket_lock);
	}
	mutex_unlock(&c->bucket_clock[READ].lock);

	mutex_lock(&c->bucket_clock[WRITE].lock);
	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		bch2_recalc_oldest_io(c, ca, WRITE);
		up_read(&ca->bucket_lock);
	}
	mutex_unlock(&c->bucket_clock[WRITE].lock);

	return 0;
}

static int __bch2_alloc_write_key(struct bch_fs *c, struct bch_dev *ca,
				  size_t b, struct btree_iter *iter,
				  u64 *journal_seq, unsigned flags)
{
#if 0
	__BKEY_PADDED(k, BKEY_ALLOC_VAL_U64s_MAX) alloc_key;
#else
	/* hack: */
	__BKEY_PADDED(k, 8) alloc_key;
#endif
	struct bkey_i_alloc *a = bkey_alloc_init(&alloc_key.k);
	struct bucket *g;
	struct bucket_mark m, new;
	int ret;

	BUG_ON(BKEY_ALLOC_VAL_U64s_MAX > 8);

	a->k.p = POS(ca->dev_idx, b);

	bch2_btree_iter_set_pos(iter, a->k.p);

	ret = bch2_btree_iter_traverse(iter);
	if (ret)
		return ret;

	percpu_down_read_preempt_disable(&c->mark_lock);
	g = bucket(ca, b);
	m = READ_ONCE(g->mark);

	if (!m.dirty) {
		percpu_up_read_preempt_enable(&c->mark_lock);
		return 0;
	}

	__alloc_write_key(a, g, m);
	percpu_up_read_preempt_enable(&c->mark_lock);

	bch2_btree_iter_cond_resched(iter);

	ret = bch2_btree_insert_at(c, NULL, journal_seq,
				   BTREE_INSERT_NOCHECK_RW|
				   BTREE_INSERT_NOFAIL|
				   BTREE_INSERT_USE_RESERVE|
				   BTREE_INSERT_USE_ALLOC_RESERVE|
				   BTREE_INSERT_NOMARK|
				   flags,
				   BTREE_INSERT_ENTRY(iter, &a->k_i));
	if (ret)
		return ret;

	new = m;
	new.dirty = false;
	atomic64_cmpxchg(&g->_mark.v, m.v.counter, new.v.counter);

	if (ca->buckets_written)
		set_bit(b, ca->buckets_written);

	return 0;
}

int bch2_alloc_replay_key(struct bch_fs *c, struct bkey_i *k)
{
	struct bch_dev *ca;
	struct btree_iter iter;
	int ret;

	if (k->k.p.inode >= c->sb.nr_devices ||
	    !c->devs[k->k.p.inode])
		return 0;

	ca = bch_dev_bkey_exists(c, k->k.p.inode);

	if (k->k.p.offset >= ca->mi.nbuckets)
		return 0;

	bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, k->k.p,
			     BTREE_ITER_INTENT);

	ret = bch2_btree_iter_traverse(&iter);
	if (ret)
		goto err;

	/* check buckets_written with btree node locked: */

	ret = test_bit(k->k.p.offset, ca->buckets_written)
		? 0
		: bch2_btree_insert_at(c, NULL, NULL,
				       BTREE_INSERT_NOFAIL|
				       BTREE_INSERT_JOURNAL_REPLAY|
				       BTREE_INSERT_NOMARK,
				       BTREE_INSERT_ENTRY(&iter, k));
err:
	bch2_btree_iter_unlock(&iter);
	return ret;
}

int bch2_alloc_write(struct bch_fs *c, bool nowait, bool *wrote)
{
	struct bch_dev *ca;
	unsigned i;
	int ret = 0;

	*wrote = false;

	for_each_rw_member(ca, c, i) {
		struct btree_iter iter;
		struct bucket_array *buckets;
		size_t b;

		bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, POS_MIN,
				     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for (b = buckets->first_bucket;
		     b < buckets->nbuckets;
		     b++) {
			if (!buckets->b[b].mark.dirty)
				continue;

			ret = __bch2_alloc_write_key(c, ca, b, &iter, NULL,
						     nowait
						     ? BTREE_INSERT_NOWAIT
						     : 0);
			if (ret)
				break;

			*wrote = true;
		}
		up_read(&ca->bucket_lock);
		bch2_btree_iter_unlock(&iter);

		if (ret) {
			percpu_ref_put(&ca->io_ref);
			break;
		}
	}

	return ret;
}

/* Bucket IO clocks: */

static void bch2_recalc_oldest_io(struct bch_fs *c, struct bch_dev *ca, int rw)
{
	struct bucket_clock *clock = &c->bucket_clock[rw];
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket *g;
	u16 max_last_io = 0;
	unsigned i;

	lockdep_assert_held(&c->bucket_clock[rw].lock);

	/* Recalculate max_last_io for this device: */
	for_each_bucket(g, buckets)
		max_last_io = max(max_last_io, bucket_last_io(c, g, rw));

	ca->max_last_bucket_io[rw] = max_last_io;

	/* Recalculate global max_last_io: */
	max_last_io = 0;

	for_each_member_device(ca, c, i)
		max_last_io = max(max_last_io, ca->max_last_bucket_io[rw]);

	clock->max_last_io = max_last_io;
}

static void bch2_rescale_bucket_io_times(struct bch_fs *c, int rw)
{
	struct bucket_clock *clock = &c->bucket_clock[rw];
	struct bucket_array *buckets;
	struct bch_dev *ca;
	struct bucket *g;
	unsigned i;

	trace_rescale_prios(c);

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets)
			g->io_time[rw] = clock->hand -
			bucket_last_io(c, g, rw) / 2;

		bch2_recalc_oldest_io(c, ca, rw);

		up_read(&ca->bucket_lock);
	}
}

static inline u64 bucket_clock_freq(u64 capacity)
{
	return max(capacity >> 10, 2028ULL);
}

static void bch2_inc_clock_hand(struct io_timer *timer)
{
	struct bucket_clock *clock = container_of(timer,
						struct bucket_clock, rescale);
	struct bch_fs *c = container_of(clock,
					struct bch_fs, bucket_clock[clock->rw]);
	struct bch_dev *ca;
	u64 capacity;
	unsigned i;

	mutex_lock(&clock->lock);

	/* if clock cannot be advanced more, rescale prio */
	if (clock->max_last_io >= U16_MAX - 2)
		bch2_rescale_bucket_io_times(c, clock->rw);

	BUG_ON(clock->max_last_io >= U16_MAX - 2);

	for_each_member_device(ca, c, i)
		ca->max_last_bucket_io[clock->rw]++;
	clock->max_last_io++;
	clock->hand++;

	mutex_unlock(&clock->lock);

	capacity = READ_ONCE(c->capacity);

	if (!capacity)
		return;

	/*
	 * we only increment when 0.1% of the filesystem capacity has been read
	 * or written too, this determines if it's time
	 *
	 * XXX: we shouldn't really be going off of the capacity of devices in
	 * RW mode (that will be 0 when we're RO, yet we can still service
	 * reads)
	 */
	timer->expire += bucket_clock_freq(capacity);

	bch2_io_timer_add(&c->io_clock[clock->rw], timer);
}

static void bch2_bucket_clock_init(struct bch_fs *c, int rw)
{
	struct bucket_clock *clock = &c->bucket_clock[rw];

	clock->hand		= 1;
	clock->rw		= rw;
	clock->rescale.fn	= bch2_inc_clock_hand;
	clock->rescale.expire	= bucket_clock_freq(c->capacity);
	mutex_init(&clock->lock);
}

/* Background allocator thread: */

/*
 * Scans for buckets to be invalidated, invalidates them, rewrites prios/gens
 * (marking them as invalidated on disk), then optionally issues discard
 * commands to the newly free buckets, then puts them on the various freelists.
 */

#define BUCKET_GC_GEN_MAX	96U

/**
 * wait_buckets_available - wait on reclaimable buckets
 *
 * If there aren't enough available buckets to fill up free_inc, wait until
 * there are.
 */
static int wait_buckets_available(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned long gc_count = c->gc_count;
	int ret = 0;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			ret = 1;
			break;
		}

		if (gc_count != c->gc_count)
			ca->inc_gen_really_needs_gc = 0;

		if ((ssize_t) (dev_buckets_available(c, ca) -
			       ca->inc_gen_really_needs_gc) >=
		    (ssize_t) fifo_free(&ca->free_inc))
			break;

		up_read(&c->gc_lock);
		schedule();
		try_to_freeze();
		down_read(&c->gc_lock);
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

static bool bch2_can_invalidate_bucket(struct bch_dev *ca,
				       size_t bucket,
				       struct bucket_mark mark)
{
	u8 gc_gen;

	if (!is_available_bucket(mark))
		return false;

	if (ca->buckets_nouse &&
	    test_bit(bucket, ca->buckets_nouse))
		return false;

	gc_gen = bucket_gc_gen(ca, bucket);

	if (gc_gen >= BUCKET_GC_GEN_MAX / 2)
		ca->inc_gen_needs_gc++;

	if (gc_gen >= BUCKET_GC_GEN_MAX)
		ca->inc_gen_really_needs_gc++;

	return gc_gen < BUCKET_GC_GEN_MAX;
}

/*
 * Determines what order we're going to reuse buckets, smallest bucket_key()
 * first.
 *
 *
 * - We take into account the read prio of the bucket, which gives us an
 *   indication of how hot the data is -- we scale the prio so that the prio
 *   farthest from the clock is worth 1/8th of the closest.
 *
 * - The number of sectors of cached data in the bucket, which gives us an
 *   indication of the cost in cache misses this eviction will cause.
 *
 * - If hotness * sectors used compares equal, we pick the bucket with the
 *   smallest bucket_gc_gen() - since incrementing the same bucket's generation
 *   number repeatedly forces us to run mark and sweep gc to avoid generation
 *   number wraparound.
 */

static unsigned long bucket_sort_key(struct bch_fs *c, struct bch_dev *ca,
				     size_t b, struct bucket_mark m)
{
	unsigned last_io = bucket_last_io(c, bucket(ca, b), READ);
	unsigned max_last_io = ca->max_last_bucket_io[READ];

	/*
	 * Time since last read, scaled to [0, 8) where larger value indicates
	 * more recently read data:
	 */
	unsigned long hotness = (max_last_io - last_io) * 7 / max_last_io;

	/* How much we want to keep the data in this bucket: */
	unsigned long data_wantness =
		(hotness + 1) * bucket_sectors_used(m);

	unsigned long needs_journal_commit =
		bucket_needs_journal_commit(m, c->journal.last_seq_ondisk);

	return  (data_wantness << 9) |
		(needs_journal_commit << 8) |
		(bucket_gc_gen(ca, b) / 16);
}

static inline int bucket_alloc_cmp(alloc_heap *h,
				   struct alloc_heap_entry l,
				   struct alloc_heap_entry r)
{
	return (l.key > r.key) - (l.key < r.key) ?:
		(l.nr < r.nr)  - (l.nr  > r.nr) ?:
		(l.bucket > r.bucket) - (l.bucket < r.bucket);
}

static inline int bucket_idx_cmp(const void *_l, const void *_r)
{
	const struct alloc_heap_entry *l = _l, *r = _r;

	return (l->bucket > r->bucket) - (l->bucket < r->bucket);
}

static void find_reclaimable_buckets_lru(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets;
	struct alloc_heap_entry e = { 0 };
	size_t b, i, nr = 0;

	ca->alloc_heap.used = 0;

	mutex_lock(&c->bucket_clock[READ].lock);
	down_read(&ca->bucket_lock);

	buckets = bucket_array(ca);

	bch2_recalc_oldest_io(c, ca, READ);

	/*
	 * Find buckets with lowest read priority, by building a maxheap sorted
	 * by read priority and repeatedly replacing the maximum element until
	 * all buckets have been visited.
	 */
	for (b = ca->mi.first_bucket; b < ca->mi.nbuckets; b++) {
		struct bucket_mark m = READ_ONCE(buckets->b[b].mark);
		unsigned long key = bucket_sort_key(c, ca, b, m);

		if (!bch2_can_invalidate_bucket(ca, b, m))
			continue;

		if (e.nr && e.bucket + e.nr == b && e.key == key) {
			e.nr++;
		} else {
			if (e.nr)
				heap_add_or_replace(&ca->alloc_heap, e,
					-bucket_alloc_cmp, NULL);

			e = (struct alloc_heap_entry) {
				.bucket = b,
				.nr	= 1,
				.key	= key,
			};
		}

		cond_resched();
	}

	if (e.nr)
		heap_add_or_replace(&ca->alloc_heap, e,
				-bucket_alloc_cmp, NULL);

	for (i = 0; i < ca->alloc_heap.used; i++)
		nr += ca->alloc_heap.data[i].nr;

	while (nr - ca->alloc_heap.data[0].nr >= ALLOC_SCAN_BATCH(ca)) {
		nr -= ca->alloc_heap.data[0].nr;
		heap_pop(&ca->alloc_heap, e, -bucket_alloc_cmp, NULL);
	}

	up_read(&ca->bucket_lock);
	mutex_unlock(&c->bucket_clock[READ].lock);
}

static void find_reclaimable_buckets_fifo(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket_mark m;
	size_t b, start;

	if (ca->fifo_last_bucket <  ca->mi.first_bucket ||
	    ca->fifo_last_bucket >= ca->mi.nbuckets)
		ca->fifo_last_bucket = ca->mi.first_bucket;

	start = ca->fifo_last_bucket;

	do {
		ca->fifo_last_bucket++;
		if (ca->fifo_last_bucket == ca->mi.nbuckets)
			ca->fifo_last_bucket = ca->mi.first_bucket;

		b = ca->fifo_last_bucket;
		m = READ_ONCE(buckets->b[b].mark);

		if (bch2_can_invalidate_bucket(ca, b, m)) {
			struct alloc_heap_entry e = { .bucket = b, .nr = 1, };

			heap_add(&ca->alloc_heap, e, bucket_alloc_cmp, NULL);
			if (heap_full(&ca->alloc_heap))
				break;
		}

		cond_resched();
	} while (ca->fifo_last_bucket != start);
}

static void find_reclaimable_buckets_random(struct bch_fs *c, struct bch_dev *ca)
{
	struct bucket_array *buckets = bucket_array(ca);
	struct bucket_mark m;
	size_t checked, i;

	for (checked = 0;
	     checked < ca->mi.nbuckets / 2;
	     checked++) {
		size_t b = bch2_rand_range(ca->mi.nbuckets -
					   ca->mi.first_bucket) +
			ca->mi.first_bucket;

		m = READ_ONCE(buckets->b[b].mark);

		if (bch2_can_invalidate_bucket(ca, b, m)) {
			struct alloc_heap_entry e = { .bucket = b, .nr = 1, };

			heap_add(&ca->alloc_heap, e, bucket_alloc_cmp, NULL);
			if (heap_full(&ca->alloc_heap))
				break;
		}

		cond_resched();
	}

	sort(ca->alloc_heap.data,
	     ca->alloc_heap.used,
	     sizeof(ca->alloc_heap.data[0]),
	     bucket_idx_cmp, NULL);

	/* remove duplicates: */
	for (i = 0; i + 1 < ca->alloc_heap.used; i++)
		if (ca->alloc_heap.data[i].bucket ==
		    ca->alloc_heap.data[i + 1].bucket)
			ca->alloc_heap.data[i].nr = 0;
}

static size_t find_reclaimable_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	size_t i, nr = 0;

	ca->inc_gen_needs_gc			= 0;

	switch (ca->mi.replacement) {
	case CACHE_REPLACEMENT_LRU:
		find_reclaimable_buckets_lru(c, ca);
		break;
	case CACHE_REPLACEMENT_FIFO:
		find_reclaimable_buckets_fifo(c, ca);
		break;
	case CACHE_REPLACEMENT_RANDOM:
		find_reclaimable_buckets_random(c, ca);
		break;
	}

	heap_resort(&ca->alloc_heap, bucket_alloc_cmp, NULL);

	for (i = 0; i < ca->alloc_heap.used; i++)
		nr += ca->alloc_heap.data[i].nr;

	return nr;
}

static inline long next_alloc_bucket(struct bch_dev *ca)
{
	struct alloc_heap_entry e, *top = ca->alloc_heap.data;

	while (ca->alloc_heap.used) {
		if (top->nr) {
			size_t b = top->bucket;

			top->bucket++;
			top->nr--;
			return b;
		}

		heap_pop(&ca->alloc_heap, e, bucket_alloc_cmp, NULL);
	}

	return -1;
}

/*
 * returns sequence number of most recent journal entry that updated this
 * bucket:
 */
static u64 bucket_journal_seq(struct bch_fs *c, struct bucket_mark m)
{
	if (m.journal_seq_valid) {
		u64 journal_seq = atomic64_read(&c->journal.seq);
		u64 bucket_seq	= journal_seq;

		bucket_seq &= ~((u64) U16_MAX);
		bucket_seq |= m.journal_seq;

		if (bucket_seq > journal_seq)
			bucket_seq -= 1 << 16;

		return bucket_seq;
	} else {
		return 0;
	}
}

static int bch2_invalidate_one_bucket2(struct bch_fs *c, struct bch_dev *ca,
				       struct btree_iter *iter,
				       u64 *journal_seq, unsigned flags)
{
#if 0
	__BKEY_PADDED(k, BKEY_ALLOC_VAL_U64s_MAX) alloc_key;
#else
	/* hack: */
	__BKEY_PADDED(k, 8) alloc_key;
#endif
	struct bkey_i_alloc *a;
	struct bkey_alloc_unpacked u;
	struct bucket_mark m;
	struct bkey_s_c k;
	bool invalidating_cached_data;
	size_t b;
	int ret;

	BUG_ON(!ca->alloc_heap.used ||
	       !ca->alloc_heap.data[0].nr);
	b = ca->alloc_heap.data[0].bucket;

	/* first, put on free_inc and mark as owned by allocator: */
	percpu_down_read_preempt_disable(&c->mark_lock);
	spin_lock(&c->freelist_lock);

	verify_not_on_freelist(c, ca, b);

	BUG_ON(!fifo_push(&ca->free_inc, b));

	bch2_mark_alloc_bucket(c, ca, b, true, gc_pos_alloc(c, NULL), 0);
	m = bucket(ca, b)->mark;

	spin_unlock(&c->freelist_lock);
	percpu_up_read_preempt_enable(&c->mark_lock);

	bch2_btree_iter_cond_resched(iter);

	BUG_ON(BKEY_ALLOC_VAL_U64s_MAX > 8);

	bch2_btree_iter_set_pos(iter, POS(ca->dev_idx, b));
retry:
	k = bch2_btree_iter_peek_slot(iter);
	ret = btree_iter_err(k);
	if (ret)
		return ret;

	if (k.k && k.k->type == KEY_TYPE_alloc)
		u = bch2_alloc_unpack(bkey_s_c_to_alloc(k).v);
	else
		memset(&u, 0, sizeof(u));

	invalidating_cached_data = u.cached_sectors != 0;

	//BUG_ON(u.dirty_sectors);
	u.data_type	= 0;
	u.dirty_sectors	= 0;
	u.cached_sectors = 0;
	u.read_time	= c->bucket_clock[READ].hand;
	u.write_time	= c->bucket_clock[WRITE].hand;
	u.gen++;

	a = bkey_alloc_init(&alloc_key.k);
	a->k.p = iter->pos;
	bch2_alloc_pack(a, u);

	ret = bch2_btree_insert_at(c, NULL,
			invalidating_cached_data ? journal_seq : NULL,
			BTREE_INSERT_ATOMIC|
			BTREE_INSERT_NOCHECK_RW|
			BTREE_INSERT_NOFAIL|
			BTREE_INSERT_USE_RESERVE|
			BTREE_INSERT_USE_ALLOC_RESERVE|
			flags,
			BTREE_INSERT_ENTRY(iter, &a->k_i));
	if (ret == -EINTR)
		goto retry;

	if (!ret) {
		/* remove from alloc_heap: */
		struct alloc_heap_entry e, *top = ca->alloc_heap.data;

		top->bucket++;
		top->nr--;

		if (!top->nr)
			heap_pop(&ca->alloc_heap, e, bucket_alloc_cmp, NULL);

		/*
		 * Make sure we flush the last journal entry that updated this
		 * bucket (i.e. deleting the last reference) before writing to
		 * this bucket again:
		 */
		*journal_seq = max(*journal_seq, bucket_journal_seq(c, m));
	} else {
		size_t b2;

		/* remove from free_inc: */
		percpu_down_read_preempt_disable(&c->mark_lock);
		spin_lock(&c->freelist_lock);

		bch2_mark_alloc_bucket(c, ca, b, false,
				       gc_pos_alloc(c, NULL), 0);

		BUG_ON(!fifo_pop_back(&ca->free_inc, b2));
		BUG_ON(b != b2);

		spin_unlock(&c->freelist_lock);
		percpu_up_read_preempt_enable(&c->mark_lock);
	}

	return ret;
}

static bool bch2_invalidate_one_bucket(struct bch_fs *c, struct bch_dev *ca,
				       size_t bucket, u64 *flush_seq)
{
	struct bucket_mark m;

	percpu_down_read_preempt_disable(&c->mark_lock);
	spin_lock(&c->freelist_lock);

	bch2_invalidate_bucket(c, ca, bucket, &m);

	verify_not_on_freelist(c, ca, bucket);
	BUG_ON(!fifo_push(&ca->free_inc, bucket));

	spin_unlock(&c->freelist_lock);

	bucket_io_clock_reset(c, ca, bucket, READ);
	bucket_io_clock_reset(c, ca, bucket, WRITE);

	percpu_up_read_preempt_enable(&c->mark_lock);

	*flush_seq = max(*flush_seq, bucket_journal_seq(c, m));

	return m.cached_sectors != 0;
}

/*
 * Pull buckets off ca->alloc_heap, invalidate them, move them to ca->free_inc:
 */
static int bch2_invalidate_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	struct btree_iter iter;
	u64 journal_seq = 0;
	int ret = 0;

	bch2_btree_iter_init(&iter, c, BTREE_ID_ALLOC, POS(ca->dev_idx, 0),
			     BTREE_ITER_SLOTS|BTREE_ITER_INTENT);

	/* Only use nowait if we've already invalidated at least one bucket: */
	while (!ret &&
	       !fifo_full(&ca->free_inc) &&
	       ca->alloc_heap.used)
		ret = bch2_invalidate_one_bucket2(c, ca, &iter, &journal_seq,
				BTREE_INSERT_GC_LOCK_HELD|
				(!fifo_empty(&ca->free_inc)
				 ? BTREE_INSERT_NOWAIT : 0));

	bch2_btree_iter_unlock(&iter);

	/* If we used NOWAIT, don't return the error: */
	if (!fifo_empty(&ca->free_inc))
		ret = 0;
	if (ret) {
		bch_err(ca, "error invalidating buckets: %i", ret);
		return ret;
	}

	if (journal_seq)
		ret = bch2_journal_flush_seq(&c->journal, journal_seq);
	if (ret) {
		bch_err(ca, "journal error: %i", ret);
		return ret;
	}

	return 0;
}

static int push_invalidated_bucket(struct bch_fs *c, struct bch_dev *ca, size_t bucket)
{
	unsigned i;
	int ret = 0;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);

		spin_lock(&c->freelist_lock);
		for (i = 0; i < RESERVE_NR; i++)
			if (fifo_push(&ca->free[i], bucket)) {
				fifo_pop(&ca->free_inc, bucket);

				closure_wake_up(&c->freelist_wait);
				ca->allocator_blocked_full = false;

				spin_unlock(&c->freelist_lock);
				goto out;
			}

		if (!ca->allocator_blocked_full) {
			ca->allocator_blocked_full = true;
			closure_wake_up(&c->freelist_wait);
		}

		spin_unlock(&c->freelist_lock);

		if ((current->flags & PF_KTHREAD) &&
		    kthread_should_stop()) {
			ret = 1;
			break;
		}

		schedule();
		try_to_freeze();
	}
out:
	__set_current_state(TASK_RUNNING);
	return ret;
}

/*
 * Pulls buckets off free_inc, discards them (if enabled), then adds them to
 * freelists, waiting until there's room if necessary:
 */
static int discard_invalidated_buckets(struct bch_fs *c, struct bch_dev *ca)
{
	while (!fifo_empty(&ca->free_inc)) {
		size_t bucket = fifo_peek(&ca->free_inc);

		if (ca->mi.discard &&
		    blk_queue_discard(bdev_get_queue(ca->disk_sb.bdev)))
			blkdev_issue_discard(ca->disk_sb.bdev,
					     bucket_to_sector(ca, bucket),
					     ca->mi.bucket_size, GFP_NOIO, 0);

		if (push_invalidated_bucket(c, ca, bucket))
			return 1;
	}

	return 0;
}

/**
 * bch_allocator_thread - move buckets from free_inc to reserves
 *
 * The free_inc FIFO is populated by find_reclaimable_buckets(), and
 * the reserves are depleted by bucket allocation. When we run out
 * of free_inc, try to invalidate some buckets and write out
 * prios and gens.
 */
static int bch2_allocator_thread(void *arg)
{
	struct bch_dev *ca = arg;
	struct bch_fs *c = ca->fs;
	size_t nr;
	int ret;

	set_freezable();

	while (1) {
		cond_resched();

		pr_debug("discarding %zu invalidated buckets",
			 fifo_used(&ca->free_inc));

		ret = discard_invalidated_buckets(c, ca);
		if (ret)
			goto stop;

		down_read(&c->gc_lock);

		ret = bch2_invalidate_buckets(c, ca);
		if (ret) {
			up_read(&c->gc_lock);
			goto stop;
		}

		if (!fifo_empty(&ca->free_inc)) {
			up_read(&c->gc_lock);
			continue;
		}

		pr_debug("free_inc now empty");

		do {
			/*
			 * Find some buckets that we can invalidate, either
			 * they're completely unused, or only contain clean data
			 * that's been written back to the backing device or
			 * another cache tier
			 */

			pr_debug("scanning for reclaimable buckets");

			nr = find_reclaimable_buckets(c, ca);

			pr_debug("found %zu buckets", nr);

			trace_alloc_batch(ca, nr, ca->alloc_heap.size);

			if ((ca->inc_gen_needs_gc >= ALLOC_SCAN_BATCH(ca) ||
			     ca->inc_gen_really_needs_gc) &&
			    c->gc_thread) {
				atomic_inc(&c->kick_gc);
				wake_up_process(c->gc_thread);
			}

			/*
			 * If we found any buckets, we have to invalidate them
			 * before we scan for more - but if we didn't find very
			 * many we may want to wait on more buckets being
			 * available so we don't spin:
			 */
			if (!nr ||
			    (nr < ALLOC_SCAN_BATCH(ca) &&
			     !fifo_full(&ca->free[RESERVE_MOVINGGC]))) {
				ca->allocator_blocked = true;
				closure_wake_up(&c->freelist_wait);

				ret = wait_buckets_available(c, ca);
				if (ret) {
					up_read(&c->gc_lock);
					goto stop;
				}
			}
		} while (!nr);

		ca->allocator_blocked = false;
		up_read(&c->gc_lock);

		pr_debug("%zu buckets to invalidate", nr);

		/*
		 * alloc_heap is now full of newly-invalidated buckets: next,
		 * write out the new bucket gens:
		 */
	}

stop:
	pr_debug("alloc thread stopping (ret %i)", ret);
	return 0;
}

/* Startup/shutdown (ro/rw): */

void bch2_recalc_capacity(struct bch_fs *c)
{
	struct bch_dev *ca;
	u64 capacity = 0, reserved_sectors = 0, gc_reserve;
	unsigned bucket_size_max = 0;
	unsigned long ra_pages = 0;
	unsigned i, j;

	lockdep_assert_held(&c->state_lock);

	for_each_online_member(ca, c, i) {
		struct backing_dev_info *bdi = ca->disk_sb.bdev->bd_bdi;

		ra_pages += bdi->ra_pages;
	}

	bch2_set_ra_pages(c, ra_pages);

	for_each_rw_member(ca, c, i) {
		u64 dev_reserve = 0;

		/*
		 * We need to reserve buckets (from the number
		 * of currently available buckets) against
		 * foreground writes so that mainly copygc can
		 * make forward progress.
		 *
		 * We need enough to refill the various reserves
		 * from scratch - copygc will use its entire
		 * reserve all at once, then run against when
		 * its reserve is refilled (from the formerly
		 * available buckets).
		 *
		 * This reserve is just used when considering if
		 * allocations for foreground writes must wait -
		 * not -ENOSPC calculations.
		 */
		for (j = 0; j < RESERVE_NONE; j++)
			dev_reserve += ca->free[j].size;

		dev_reserve += 1;	/* btree write point */
		dev_reserve += 1;	/* copygc write point */
		dev_reserve += 1;	/* rebalance write point */

		dev_reserve *= ca->mi.bucket_size;

		ca->copygc_threshold = dev_reserve;

		capacity += bucket_to_sector(ca, ca->mi.nbuckets -
					     ca->mi.first_bucket);

		reserved_sectors += dev_reserve * 2;

		bucket_size_max = max_t(unsigned, bucket_size_max,
					ca->mi.bucket_size);
	}

	gc_reserve = c->opts.gc_reserve_bytes
		? c->opts.gc_reserve_bytes >> 9
		: div64_u64(capacity * c->opts.gc_reserve_percent, 100);

	reserved_sectors = max(gc_reserve, reserved_sectors);

	reserved_sectors = min(reserved_sectors, capacity);

	c->capacity = capacity - reserved_sectors;

	c->bucket_size_max = bucket_size_max;

	if (c->capacity) {
		bch2_io_timer_add(&c->io_clock[READ],
				 &c->bucket_clock[READ].rescale);
		bch2_io_timer_add(&c->io_clock[WRITE],
				 &c->bucket_clock[WRITE].rescale);
	} else {
		bch2_io_timer_del(&c->io_clock[READ],
				 &c->bucket_clock[READ].rescale);
		bch2_io_timer_del(&c->io_clock[WRITE],
				 &c->bucket_clock[WRITE].rescale);
	}

	/* Wake up case someone was waiting for buckets */
	closure_wake_up(&c->freelist_wait);
}

static bool bch2_dev_has_open_write_point(struct bch_fs *c, struct bch_dev *ca)
{
	struct open_bucket *ob;
	bool ret = false;

	for (ob = c->open_buckets;
	     ob < c->open_buckets + ARRAY_SIZE(c->open_buckets);
	     ob++) {
		spin_lock(&ob->lock);
		if (ob->valid && !ob->on_partial_list &&
		    ob->ptr.dev == ca->dev_idx)
			ret = true;
		spin_unlock(&ob->lock);
	}

	return ret;
}

/* device goes ro: */
void bch2_dev_allocator_remove(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	BUG_ON(ca->alloc_thread);

	/* First, remove device from allocation groups: */

	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		clear_bit(ca->dev_idx, c->rw_devs[i].d);

	/*
	 * Capacity is calculated based off of devices in allocation groups:
	 */
	bch2_recalc_capacity(c);

	/* Next, close write points that point to this device... */
	for (i = 0; i < ARRAY_SIZE(c->write_points); i++)
		bch2_writepoint_stop(c, ca, &c->write_points[i]);

	bch2_writepoint_stop(c, ca, &ca->copygc_write_point);
	bch2_writepoint_stop(c, ca, &c->rebalance_write_point);
	bch2_writepoint_stop(c, ca, &c->btree_write_point);

	mutex_lock(&c->btree_reserve_cache_lock);
	while (c->btree_reserve_cache_nr) {
		struct btree_alloc *a =
			&c->btree_reserve_cache[--c->btree_reserve_cache_nr];

		bch2_open_buckets_put(c, &a->ob);
	}
	mutex_unlock(&c->btree_reserve_cache_lock);

	while (1) {
		struct open_bucket *ob;

		spin_lock(&c->freelist_lock);
		if (!ca->open_buckets_partial_nr) {
			spin_unlock(&c->freelist_lock);
			break;
		}
		ob = c->open_buckets +
			ca->open_buckets_partial[--ca->open_buckets_partial_nr];
		ob->on_partial_list = false;
		spin_unlock(&c->freelist_lock);

		bch2_open_bucket_put(c, ob);
	}

	bch2_ec_stop_dev(c, ca);

	/*
	 * Wake up threads that were blocked on allocation, so they can notice
	 * the device can no longer be removed and the capacity has changed:
	 */
	closure_wake_up(&c->freelist_wait);

	/*
	 * journal_res_get() can block waiting for free space in the journal -
	 * it needs to notice there may not be devices to allocate from anymore:
	 */
	wake_up(&c->journal.wait);

	/* Now wait for any in flight writes: */

	closure_wait_event(&c->open_buckets_wait,
			   !bch2_dev_has_open_write_point(c, ca));
}

/* device goes rw: */
void bch2_dev_allocator_add(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(c->rw_devs); i++)
		if (ca->mi.data_allowed & (1 << i))
			set_bit(ca->dev_idx, c->rw_devs[i].d);
}

void bch2_dev_allocator_quiesce(struct bch_fs *c, struct bch_dev *ca)
{
	if (ca->alloc_thread)
		closure_wait_event(&c->freelist_wait, ca->allocator_blocked_full);
}

/* stop allocator thread: */
void bch2_dev_allocator_stop(struct bch_dev *ca)
{
	struct task_struct *p;

	p = rcu_dereference_protected(ca->alloc_thread, 1);
	ca->alloc_thread = NULL;

	/*
	 * We need an rcu barrier between setting ca->alloc_thread = NULL and
	 * the thread shutting down to avoid bch2_wake_allocator() racing:
	 *
	 * XXX: it would be better to have the rcu barrier be asynchronous
	 * instead of blocking us here
	 */
	synchronize_rcu();

	if (p) {
		kthread_stop(p);
		put_task_struct(p);
	}
}

/* start allocator thread: */
int bch2_dev_allocator_start(struct bch_dev *ca)
{
	struct task_struct *p;

	/*
	 * allocator thread already started?
	 */
	if (ca->alloc_thread)
		return 0;

	p = kthread_create(bch2_allocator_thread, ca,
			   "bch_alloc[%s]", ca->name);
	if (IS_ERR(p))
		return PTR_ERR(p);

	get_task_struct(p);
	rcu_assign_pointer(ca->alloc_thread, p);
	wake_up_process(p);
	return 0;
}

static void flush_held_btree_writes(struct bch_fs *c)
{
	struct bucket_table *tbl;
	struct rhash_head *pos;
	struct btree *b;
	bool nodes_blocked;
	size_t i;
	struct closure cl;

	closure_init_stack(&cl);

	clear_bit(BCH_FS_HOLD_BTREE_WRITES, &c->flags);
again:
	pr_debug("flushing dirty btree nodes");
	cond_resched();
	closure_wait(&c->btree_interior_update_wait, &cl);

	nodes_blocked = false;

	rcu_read_lock();
	for_each_cached_btree(b, c, tbl, i, pos)
		if (btree_node_need_write(b)) {
			if (btree_node_may_write(b)) {
				rcu_read_unlock();
				btree_node_lock_type(c, b, SIX_LOCK_read);
				bch2_btree_node_write(c, b, SIX_LOCK_read);
				six_unlock_read(&b->lock);
				goto again;
			} else {
				nodes_blocked = true;
			}
		}
	rcu_read_unlock();

	if (c->btree_roots_dirty)
		bch2_journal_meta(&c->journal);

	if (nodes_blocked) {
		closure_sync(&cl);
		goto again;
	}

	closure_wake_up(&c->btree_interior_update_wait);
	closure_sync(&cl);

	closure_wait_event(&c->btree_interior_update_wait,
			   !bch2_btree_interior_updates_nr_pending(c));
}

static void allocator_start_issue_discards(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned dev_iter;
	size_t bu;

	for_each_rw_member(ca, c, dev_iter)
		while (fifo_pop(&ca->free_inc, bu))
			blkdev_issue_discard(ca->disk_sb.bdev,
					     bucket_to_sector(ca, bu),
					     ca->mi.bucket_size, GFP_NOIO, 0);
}

static int resize_free_inc(struct bch_dev *ca)
{
	alloc_fifo free_inc;

	if (!fifo_full(&ca->free_inc))
		return 0;

	if (!init_fifo(&free_inc,
		       ca->free_inc.size * 2,
		       GFP_KERNEL))
		return -ENOMEM;

	fifo_move(&free_inc, &ca->free_inc);
	swap(free_inc, ca->free_inc);
	free_fifo(&free_inc);
	return 0;
}

static int __bch2_fs_allocator_start(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned dev_iter;
	u64 journal_seq = 0;
	long bu;
	int ret = 0;

	if (test_alloc_startup(c))
		goto not_enough;

	/* Scan for buckets that are already invalidated: */
	for_each_rw_member(ca, c, dev_iter) {
		struct bucket_array *buckets;
		struct bucket_mark m;

		down_read(&ca->bucket_lock);
		percpu_down_read_preempt_disable(&c->mark_lock);

		buckets = bucket_array(ca);

		for (bu = buckets->first_bucket;
		     bu < buckets->nbuckets; bu++) {
			m = READ_ONCE(buckets->b[bu].mark);

			if (!buckets->b[bu].gen_valid ||
			    !test_bit(bu, ca->buckets_nouse) ||
			    !is_available_bucket(m) ||
			    m.cached_sectors)
				continue;

			bch2_mark_alloc_bucket(c, ca, bu, true,
					gc_pos_alloc(c, NULL), 0);

			fifo_push(&ca->free_inc, bu);

			discard_invalidated_buckets(c, ca);

			if (fifo_full(&ca->free[RESERVE_BTREE]))
				break;
		}
		percpu_up_read_preempt_enable(&c->mark_lock);
		up_read(&ca->bucket_lock);
	}

	/* did we find enough buckets? */
	for_each_rw_member(ca, c, dev_iter)
		if (!fifo_full(&ca->free[RESERVE_BTREE])) {
			percpu_ref_put(&ca->io_ref);
			goto not_enough;
		}

	return 0;
not_enough:
	pr_debug("not enough empty buckets; scanning for reclaimable buckets");

	/*
	 * We're moving buckets to freelists _before_ they've been marked as
	 * invalidated on disk - we have to so that we can allocate new btree
	 * nodes to mark them as invalidated on disk.
	 *
	 * However, we can't _write_ to any of these buckets yet - they might
	 * have cached data in them, which is live until they're marked as
	 * invalidated on disk:
	 */
	set_bit(BCH_FS_HOLD_BTREE_WRITES, &c->flags);

	while (1) {
		bool wrote = false;

		for_each_rw_member(ca, c, dev_iter) {
			find_reclaimable_buckets(c, ca);

			while (!fifo_full(&ca->free[RESERVE_BTREE]) &&
			       (bu = next_alloc_bucket(ca)) >= 0) {
				ret = resize_free_inc(ca);
				if (ret) {
					percpu_ref_put(&ca->io_ref);
					return ret;
				}

				bch2_invalidate_one_bucket(c, ca, bu,
							   &journal_seq);

				fifo_push(&ca->free[RESERVE_BTREE], bu);
			}
		}

		pr_debug("done scanning for reclaimable buckets");

		/*
		 * XXX: it's possible for this to deadlock waiting on journal reclaim,
		 * since we're holding btree writes. What then?
		 */
		ret = bch2_alloc_write(c, true, &wrote);

		/*
		 * If bch2_alloc_write() did anything, it may have used some
		 * buckets, and we need the RESERVE_BTREE freelist full - so we
		 * need to loop and scan again.
		 * And if it errored, it may have been because there weren't
		 * enough buckets, so just scan and loop again as long as it
		 * made some progress:
		 */
		if (!wrote && ret)
			return ret;
		if (!wrote && !ret)
			break;
	}

	pr_debug("flushing journal");

	ret = bch2_journal_flush(&c->journal);
	if (ret)
		return ret;

	pr_debug("issuing discards");
	allocator_start_issue_discards(c);

	set_bit(BCH_FS_ALLOCATOR_STARTED, &c->flags);

	/* now flush dirty btree nodes: */
	flush_held_btree_writes(c);

	return 0;
}

int bch2_fs_allocator_start(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;
	int ret;

	down_read(&c->gc_lock);
	ret = __bch2_fs_allocator_start(c);
	up_read(&c->gc_lock);

	if (ret)
		return ret;

	for_each_rw_member(ca, c, i) {
		ret = bch2_dev_allocator_start(ca);
		if (ret) {
			percpu_ref_put(&ca->io_ref);
			return ret;
		}
	}

	set_bit(BCH_FS_ALLOCATOR_RUNNING, &c->flags);
	return 0;
}

void bch2_fs_allocator_background_init(struct bch_fs *c)
{
	spin_lock_init(&c->freelist_lock);
	bch2_bucket_clock_init(c, READ);
	bch2_bucket_clock_init(c, WRITE);

	c->pd_controllers_update_seconds = 5;
	INIT_DELAYED_WORK(&c->pd_controllers_update, pd_controllers_update);
}
