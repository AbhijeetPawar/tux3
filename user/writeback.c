#include "tux3.h"

/* dummy for not including commit.c */
struct buffer_head * __weak blockdirty(struct buffer_head *buffer, unsigned newdelta)
{
	return buffer;
}

void clear_inode(struct inode *inode)
{
	list_del_init(&inode->list);
	inode->state = 0;
}

void __mark_inode_dirty(struct inode *inode, unsigned flags)
{
	if ((inode->state & flags) != flags) {
		inode->state |= flags;
		if (list_empty(&inode->list))
			list_add_tail(&inode->list, &inode->i_sb->dirty_inodes);
	}
}

void mark_inode_dirty(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY);
}

void mark_buffer_dirty(struct buffer_head *buffer)
{
	if (!buffer_dirty(buffer)) {
		set_buffer_dirty(buffer);
		__mark_inode_dirty(buffer_inode(buffer), I_DIRTY_PAGES);
	}
}

/* dummy for not including inode.c */
int __weak write_inode(struct inode *inode)
{
	assert(0);
	return 0;
}

int sync_inode(struct inode *inode)
{
	unsigned dirty;

	/* To handle redirty, this clears before flushing */
	dirty = inode->state;
	inode->state &= ~I_DIRTY;
	list_del_init(&inode->list);

	if (dirty & I_DIRTY_PAGES) {
		int err = flush_buffers(mapping(inode));
		if (err)
			return err;
	}
	if (dirty & (I_DIRTY_SYNC | I_DIRTY_DATASYNC)) {
		int err = write_inode(inode);
		if (err)
			return err;
	}
	return 0;
}

static int sync_inodes(struct sb *sb)
{
	struct inode *inode, *safe;
	LIST_HEAD(dirty_inodes);
	int err;

	list_splice_init(&sb->dirty_inodes, &dirty_inodes);

	list_for_each_entry_safe(inode, safe, &dirty_inodes, list) {
		/*
		 * FIXME: this is hack. those inodes is dirtied by
		 * sync_inode() of other inodes, so it should be
		 * flushed after other inodes.
		 */
		switch (inode->inum) {
		case TUX_BITMAP_INO:
		case TUX_VOLMAP_INO:
			continue;
		}

		err = sync_inode(inode);
		if (err)
			goto error;
	}
	err = sync_inode(sb->bitmap);
	if (err)
		goto error;
	err = sync_inode(sb->volmap);
	if (err)
		goto error;
	assert(list_empty(&dirty_inodes));

	return 0;

error:
	list_splice_init(&dirty_inodes, &sb->dirty_inodes);
	return err;
}

/* dummy for not including super.c */
int __weak save_sb(struct sb *sb)
{
	assert(0);
	return 0;
}

int sync_super(struct sb *sb)
{
	int err;

	printf("sync inodes\n");
	if ((err = sync_inodes(sb)))
		return err;
	printf("sync super\n");
	if ((err = save_sb(sb)))
		return err;

	return 0;
}
