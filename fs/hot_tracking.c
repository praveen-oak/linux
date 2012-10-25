/*
 * fs/hot_tracking.c
 *
 * Copyright (C) 2012 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#include <linux/list.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/hardirq.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/types.h>
#include <linux/limits.h>
#include "hot_tracking.h"

/* kmem_cache pointers for slab caches */
static struct kmem_cache *hot_inode_item_cachep __read_mostly;
static struct kmem_cache *hot_range_item_cachep __read_mostly;

/*
 * Initialize the inode tree. Should be called for each new inode
 * access or other user of the hot_inode interface.
 */
static void hot_inode_tree_init(struct hot_info *root)
{
	root->hot_inode_tree.map = RB_ROOT;
	spin_lock_init(&root->lock);
}

/*
 * Initialize the hot range tree. Should be called for each new inode
 * access or other user of the hot_range interface.
 */
void hot_range_tree_init(struct hot_inode_item *he)
{
	he->hot_range_tree.map = RB_ROOT;
	spin_lock_init(&he->lock);
}

/*
 * Initialize a new hot_range_item structure. The new structure is
 * returned with a reference count of one and needs to be
 * freed using free_range_item()
 */
static void hot_range_item_init(struct hot_range_item *hr, loff_t start,
				struct hot_inode_item *he)
{
	hr->start = start;
	hr->len = RANGE_SIZE;
	hr->hot_inode = he;
	kref_init(&hr->hot_range.refs);
	spin_lock_init(&hr->hot_range.lock);
	hr->hot_range.hot_freq_data.avg_delta_reads = (u64) -1;
	hr->hot_range.hot_freq_data.avg_delta_writes = (u64) -1;
	hr->hot_range.hot_freq_data.flags = FREQ_DATA_TYPE_RANGE;
}

/*
 * Initialize a new hot_inode_item structure. The new structure is
 * returned with a reference count of one and needs to be
 * freed using hot_free_inode_item()
 */
static void hot_inode_item_init(struct hot_inode_item *he,
				u64 ino,
				struct hot_rb_tree *hot_inode_tree)
{
	he->i_ino = ino;
	he->hot_inode_tree = hot_inode_tree;
	kref_init(&he->hot_inode.refs);
	spin_lock_init(&he->hot_inode.lock);
	he->hot_inode.hot_freq_data.avg_delta_reads = (u64) -1;
	he->hot_inode.hot_freq_data.avg_delta_writes = (u64) -1;
	he->hot_inode.hot_freq_data.flags = FREQ_DATA_TYPE_INODE;
	hot_range_tree_init(he);
}

/*
 * Initialize kmem cache for hot_inode_item and hot_range_item.
 */
void __init hot_cache_init(void)
{
	hot_inode_item_cachep = kmem_cache_create("hot_inode_item",
			sizeof(struct hot_inode_item), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD,
			NULL);
	if (!hot_inode_item_cachep)
		return;

	hot_range_item_cachep = kmem_cache_create("hot_range_item",
			sizeof(struct hot_range_item), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD,
			NULL);
	if (!hot_range_item_cachep)
		goto err;

	return;

err:
	kmem_cache_destroy(hot_inode_item_cachep);
}
EXPORT_SYMBOL_GPL(hot_cache_init);
