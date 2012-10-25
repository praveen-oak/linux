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
	INIT_LIST_HEAD(&he->hot_inode.n_list);
	he->hot_inode.hot_freq_data.avg_delta_reads = (u64) -1;
	he->hot_inode.hot_freq_data.avg_delta_writes = (u64) -1;
	he->hot_inode.hot_freq_data.flags = FREQ_DATA_TYPE_INODE;
	hot_range_tree_init(he);
}

static void hot_range_item_free(struct kref *kref)
{
	struct hot_comm_item *comm_item = container_of(kref,
		struct hot_comm_item, refs);
	struct hot_range_item *hr = container_of(comm_item,
		struct hot_range_item, hot_range);

	rb_erase(&hr->hot_range.rb_node,
		&hr->hot_inode->hot_range_tree.map);
	kmem_cache_free(hot_range_item_cachep, hr);
}

/*
 * Drops the reference out on hot_range_item by one
 * and free the structure if the reference count hits zero
 */
static void hot_range_item_put(struct hot_range_item *hr)
{
	kref_put(&hr->hot_range.refs, hot_range_item_free);
}

/* Frees the entire hot_range_tree. */
static void hot_range_tree_free(struct hot_inode_item *he)
{
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_range_item *hr;

	/* Free hot inode and range trees on fs root */
	spin_lock(&he->lock);
	while ((node = rb_first(&he->hot_range_tree.map))) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		hr = container_of(ci,
			struct hot_range_item, hot_range);
		hot_range_item_put(hr);
	}
	spin_unlock(&he->lock);
}

static void hot_inode_item_free(struct kref *kref)
{
	struct hot_comm_item *comm_item = container_of(kref,
			struct hot_comm_item, refs);
	struct hot_inode_item *he = container_of(comm_item,
			struct hot_inode_item, hot_inode);

	hot_range_tree_free(he);
	spin_lock(&he->hot_inode.lock);
	rb_erase(&he->hot_inode.rb_node, &he->hot_inode_tree->map);
	spin_unlock(&he->hot_inode.lock);
	kmem_cache_free(hot_inode_item_cachep, he);
}

/*
 * Drops the reference out on hot_inode_item by one
 * and free the structure if the reference count hits zero
 */
void hot_inode_item_put(struct hot_inode_item *he)
{
	kref_put(&he->hot_inode.refs, hot_inode_item_free);
}
EXPORT_SYMBOL_GPL(hot_inode_item_put);

/* Frees the entire hot_inode_tree. */
static void hot_inode_tree_exit(struct hot_info *root)
{
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_inode_item *he;

	/* Free hot inode and range trees on fs root */
	spin_lock(&root->lock);
	while ((node = rb_first(&root->hot_inode_tree.map))) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he = container_of(ci,
			struct hot_inode_item, hot_inode);
		hot_inode_item_put(he);
	}
	spin_unlock(&root->lock);
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

/*
 * Initialize the data structures for hot data tracking.
 */
int hot_track_init(struct super_block *sb)
{
	struct hot_info *root;
	int ret = -ENOMEM;

	root = kzalloc(sizeof(struct hot_info), GFP_NOFS);
	if (!root) {
		printk(KERN_ERR "%s: Failed to malloc memory for "
				"hot_info\n", __func__);
		return ret;
	}

	hot_inode_tree_init(root);

	sb->s_hot_root = root;

	printk(KERN_INFO "VFS: Turning on hot data tracking\n");

	return 0;
}
EXPORT_SYMBOL_GPL(hot_track_init);

void hot_track_exit(struct super_block *sb)
{
	struct hot_info *root = sb->s_hot_root;

	hot_inode_tree_exit(root);
	sb->s_hot_root = NULL;
	kfree(root);
}
EXPORT_SYMBOL_GPL(hot_track_exit);
