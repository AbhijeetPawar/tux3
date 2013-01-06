/*
 * Copyright (c) 2008, Daniel Phillips
 * Copyright (c) 2008, OGAWA Hirofumi
 */

#include "tux3.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * Need frontend modification of backend buffers. (modification
 * after latest delta commit and before rollup).
 *
 * E.g. frontend modified backend buffers, stage_delta() of when
 * rollup is called.
 */
#define ALLOW_FRONTEND_MODIFY

static void setup_roots(struct sb *sb, struct disksuper *super)
{
	u64 iroot_val = be64_to_cpu(super->iroot);
	u64 oroot_val = be64_to_cpu(sb->super.oroot);
	init_btree(itable_btree(sb), sb, unpack_root(iroot_val), &itable_ops);
	init_btree(otable_btree(sb), sb, unpack_root(oroot_val), &otable_ops);
}

void setup_sb(struct sb *sb, struct disksuper *super)
{
	init_rwsem(&sb->delta_lock);
	mutex_init(&sb->loglock);
	INIT_LIST_HEAD(&sb->alloc_inodes);
	INIT_LIST_HEAD(&sb->orphan_add);
	INIT_LIST_HEAD(&sb->orphan_del);
#ifndef __KERNEL__
	INIT_LIST_HEAD(&sb->dirty_inodes);
#endif
	INIT_LIST_HEAD(&sb->pinned);
	stash_init(&sb->defree);
	stash_init(&sb->derollup);

	sb->blockbits = be16_to_cpu(super->blockbits);
	sb->volblocks = be64_to_cpu(super->volblocks);
	sb->version = 0;	/* FIXME: not yet implemented */

	sb->blocksize = 1 << sb->blockbits;
	sb->blockmask = (1 << sb->blockbits) - 1;
	sb->entries_per_node = calc_entries_per_node(sb->blocksize);
	/* Initialize base indexes for atable */
	atable_init_base(sb);

	/* Probably does not belong here (maybe metablock) */
#ifdef ATOMIC
	sb->freeblocks = sb->volblocks;
#else
	sb->freeblocks = be64_to_cpu(super->freeblocks);
#endif
	sb->nextalloc = be64_to_cpu(super->nextalloc);
	sb->atomdictsize = be64_to_cpu(super->atomdictsize);
	sb->atomgen = be32_to_cpu(super->atomgen);
	sb->freeatom = be32_to_cpu(super->freeatom);
	/* logchain and logcount are read from super directly */
	trace("blocksize %u, blockbits %u, blockmask %08x",
	      sb->blocksize, sb->blockbits, sb->blockmask);
	trace("volblocks %Lu, freeblocks %Lu, nextalloc %Lu",
	      sb->volblocks, sb->freeblocks, sb->nextalloc);
	trace("atom_dictsize %Lu, freeatom %u, atomgen %u",
	      (s64)sb->atomdictsize, sb->freeatom, sb->atomgen);

	setup_roots(sb, super);
}

int load_sb(struct sb *sb)
{
	struct disksuper *super = &sb->super;
	int err;

	err = devio(READ, sb_dev(sb), SB_LOC, super, SB_LEN);
	if (err)
		return err;
	if (memcmp(super->magic, TUX3_MAGIC, sizeof(super->magic)))
		return -EINVAL;

	setup_sb(sb, super);
	return 0;
}

int save_sb(struct sb *sb)
{
	struct disksuper *super = &sb->super;

	super->blockbits = cpu_to_be16(sb->blockbits);
	super->volblocks = cpu_to_be64(sb->volblocks);

	/* Probably does not belong here (maybe metablock) */
	super->iroot = cpu_to_be64(pack_root(&itable_btree(sb)->root));
	super->oroot = cpu_to_be64(pack_root(&otable_btree(sb)->root));
#ifndef ATOMIC
	super->freeblocks = cpu_to_be64(sb->freeblocks);
#endif
	super->nextalloc = cpu_to_be64(sb->nextalloc);
	super->atomdictsize = cpu_to_be64(sb->atomdictsize);
	super->freeatom = cpu_to_be32(sb->freeatom);
	super->atomgen = cpu_to_be32(sb->atomgen);
	/* logchain and logcount are written to super directly */

	return devio(WRITE, sb_dev(sb), SB_LOC, super, SB_LEN);
}

/* Delta transition */

static int relog_frontend_defer_as_bfree(struct sb *sb, u64 val)
{
	log_bfree_relog(sb, val & ~(-1ULL << 48), val >> 48);
	return 0;
}

static int relog_as_bfree(struct sb *sb, u64 val)
{
	log_bfree_relog(sb, val & ~(-1ULL << 48), val >> 48);
	return stash_value(&sb->defree, val);
}

/* Obsolete the old rollup, then start the log of new rollup */
static void new_cycle_log(struct sb *sb)
{
#if 0 /* ALLOW_FRONTEND_MODIFY */
	/*
	 * FIXME: we don't need to write the logs generated by
	 * frontend at all.  However, for now, we are writing those
	 * logs for debugging.
	 */

	/* Discard the logs generated by frontend. */
	log_finish(sb);
	log_finish_cycle(sb);
#endif
	/* Initialize logcount to count log blocks on new rollup cycle. */
	sb->super.logcount = 0;
}

/*
 * Flush a snapshot of the allocation map to disk.  Physical blocks for
 * the bitmaps and new or redirected bitmap btree nodes may be allocated
 * during the rollup.  Any bitmap blocks that are (re)dirtied by these
 * allocations will be written out in the next rollup cycle.
 */
static int rollup_log(struct sb *sb)
{
	/* further block allocations belong to the next cycle */
	unsigned rollup = sb->rollup++;

	trace(">>>>>>>>> commit rollup %u", rollup);
#ifndef __KERNEL__
	LIST_HEAD(orphan_add);
	LIST_HEAD(orphan_del);

	/*
	 * Orphan inodes are still living, or orphan inodes in
	 * sb->otable are dead. And logs will be obsoleted, so, we
	 * apply those to sb->otable.
	 * [If we may want to have two orphan_{add,del} lists for
	 * frontend and backend.]
	 */
	list_splice_init(&sb->orphan_add, &orphan_add);
	list_splice_init(&sb->orphan_del, &orphan_del);

	/* This is starting the new rollup cycle of the log */
	new_cycle_log(sb);
	/* Add rollup log as mark of new rollup cycle. */
	log_rollup(sb);
	/* Log to store freeblocks for flushing bitmap data */
	log_freeblocks(sb, sb->freeblocks);
#ifdef ALLOW_FRONTEND_MODIFY
	/*
	 * If frontend made defered bfree (i.e. it is not applied to
	 * bitmap yet), we have to re-log it on this cycle. Because we
	 * obsolete all logs in past.
	 */
	stash_walk(sb, &sb->defree, relog_frontend_defer_as_bfree);
#endif
	/*
	 * Re-logging defered bfree blocks after rollup as defered
	 * bfree (LOG_BFREE_RELOG) after delta.  With this, we can
	 * obsolete log records on previous rollup.
	 */
	unstash(sb, &sb->derollup, relog_as_bfree);

	/* bnode blocks */
	trace("> flush pinned buffers %u", rollup);
	flush_list(&sb->pinned);
	trace("< done pinned buffers %u", rollup);

	/* Flush bitmap */
	trace("> flush bitmap %u", rollup);
	sync_inode(sb->bitmap, rollup);
	trace("< done bitmap %u", rollup);

	trace("> apply orphan inodes %u", rollup);
	{
		int err;

		/*
		 * This defered deletion of orphan from sb->otable.
		 * It should be done before adding new orphan, because
		 * orphan_add may have same inum in orphan_del.
		 */
		err = tux3_rollup_orphan_del(sb, &orphan_del);
		if (err)
			return err;

		/*
		 * This apply orphan inodes to sb->otable after flushed bitmap.
		 */
		err = tux3_rollup_orphan_add(sb, &orphan_add);
		if (err)
			return err;
	}
	trace("< apply orphan inodes %u", rollup);
	assert(list_empty(&orphan_add));
	assert(list_empty(&orphan_del));
#endif
	trace("<<<<<<<<< commit rollup done %u", rollup);

	return 0;
}

/* Apply frontend modifications to backend buffers, and flush data buffers. */
static int stage_delta(struct sb *sb, unsigned delta)
{
	/* flush inodes */
	return sync_inodes(sb, delta);
}

static int write_leaves(struct sb *sb, unsigned delta)
{
	/*
	 * Flush leaves blocks.  FIXME: Now we are using DEFAULT_DIRTY_WHEN
	 * for leaves. Do we need to per delta dirty buffers?
	 */
	return sync_inode(sb->volmap, DEFAULT_DIRTY_WHEN);
}

/* allocate and write log blocks */
static int write_log(struct sb *sb)
{
	unsigned index, logcount;

	/* Finish to logging in this delta */
	log_finish(sb);

	for (index = 0; index < sb->lognext; index++) {
		block_t block;
		int err = balloc(sb, 1, &block);
		if (err)
			return err;
		struct buffer_head *buffer = blockget(mapping(sb->logmap), index);
		assert(buffer);
		struct logblock *log = bufdata(buffer);
		assert(log->magic == cpu_to_be16(TUX3_MAGIC_LOG));
		log->logchain = sb->super.logchain;
		err = blockio(WRITE, buffer, block);
		if (err) {
			blockput(buffer);
			bfree(sb, block, 1);
			return err;
		}

		/*
		 * We can obsolete the log blocks after next rollup
		 * by LOG_BFREE_RELOG.
		 */
		defer_bfree(&sb->derollup, block, 1);

		blockput(buffer);
		trace("logchain %lld", block);
		sb->super.logchain = cpu_to_be64(block);
	}

	/* Add count of log on this delta to rollup logcount */
	logcount = be32_to_cpu(sb->super.logcount);
	logcount += log_finish_cycle(sb);

	sb->super.logcount = cpu_to_be32(logcount);

	return 0;
}

/* userland only */
int apply_defered_bfree(struct sb *sb, u64 val)
{
	return bfree(sb, val & ~(-1ULL << 48), val >> 48);
}

static int commit_delta(struct sb *sb)
{
	trace("commit %i logblocks", be32_to_cpu(sb->super.logcount));
	int err = save_sb(sb);
	if (err)
		return err;

	/* Commit was finished, apply defered bfree. */
	return unstash(sb, &sb->defree, apply_defered_bfree);
}

static int need_delta(struct sb *sb)
{
	static unsigned crudehack;
	return !(++crudehack % 10);
}

static int need_rollup(struct sb *sb)
{
	static unsigned crudehack;
	return !(++crudehack % 3);
}

enum rollup_flags { NO_ROLLUP, ALLOW_ROLLUP, FORCE_ROLLUP, };

/* must hold down_write(&sb->delta_lock) */
static int do_commit(struct sb *sb, enum rollup_flags rollup_flag)
{
	unsigned delta = sb->delta++;
	int err = 0;

	trace(">>>>>>>>> commit delta %u", delta);
	/* further changes of frontend belong to the next delta */

	/* Add delta log for debugging. */
	log_delta(sb);

	/*
	 * NOTE: This works like modification from frontend. (i.e. this
	 * may generate defree log which is not committed yet at rollup.)
	 *
	 * - this is before rollup to merge modifications to this
	 *   rollup, and flush at once for optimization.
	 *
	 * - this is required to prevent unexpected buffer state for
	 *   cursor_redirect(). If we applied modification after
	 *   rollup_log, it made unexpected dirty state (i.e. leaf is
	 *   still dirty, but parent was already cleaned.)
	 */
	err = stage_delta(sb, delta);
	if (err)
		return err;

	if ((rollup_flag == ALLOW_ROLLUP && need_rollup(sb)) ||
	    rollup_flag == FORCE_ROLLUP) {
		err = rollup_log(sb);
		if (err)
			return err;

		/* Add delta log for debugging. */
		log_delta(sb);
	}

	write_leaves(sb, delta);
	write_log(sb);
	commit_delta(sb);
	trace("<<<<<<<<< commit done %u", delta);

	return err; /* FIXME: error handling */
}

#ifdef ATOMIC
/* FIXME: quickly designed, rethink this. */
int force_rollup(struct sb *sb)
{
	int err;

	down_write(&sb->delta_lock);
	err = do_commit(sb, FORCE_ROLLUP);
	up_write(&sb->delta_lock);

	return err;
}

/* FIXME: quickly designed, rethink this. */
int force_delta(struct sb *sb)
{
	int err;

	down_write(&sb->delta_lock);
	err = do_commit(sb, NO_ROLLUP);
	up_write(&sb->delta_lock);

	return err;
}
#endif /* !ATOMIC */

int change_begin(struct sb *sb)
{
#ifndef __KERNEL__
	down_read(&sb->delta_lock);
#endif
	return 0;
}

int change_end(struct sb *sb)
{
	int err = 0;
#ifndef __KERNEL__
	if (!need_delta(sb)) {
		up_read(&sb->delta_lock);
		return 0;
	}
	unsigned delta = sb->delta;
	up_read(&sb->delta_lock);

	down_write(&sb->delta_lock);
	/* FIXME: error handling */
	if (sb->delta == delta)
		err = do_commit(sb, ALLOW_ROLLUP);
	up_write(&sb->delta_lock);
#endif
	return err;
}

#ifdef __KERNEL__
static void *useme[] = { new_cycle_log, need_delta, do_commit, useme };
#endif
