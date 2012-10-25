/*
 *  include/linux/hot_tracking.h
 *
 * This file has definitions for VFS hot data tracking
 * structures etc.
 *
 * Copyright (C) 2012 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#ifndef _LINUX_HOTTRACK_H
#define _LINUX_HOTTRACK_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/fs.h>

struct hot_rb_tree {
	struct rb_root map;
};

/*
 * A frequency data struct holds values that are used to
 * determine temperature of files and file ranges. These structs
 * are members of hot_inode_item and hot_range_item
 */
struct hot_freq_data {
	struct timespec last_read_time;
	struct timespec last_write_time;
	u32 nr_reads;
	u32 nr_writes;
	u64 avg_delta_reads;
	u64 avg_delta_writes;
	u32 flags;
	u32 last_temp;
};

/* The common info for both following structures */
struct hot_comm_item {
	struct rb_node rb_node; /* rbtree index */
	struct hot_freq_data hot_freq_data;  /* frequency data */
	spinlock_t lock; /* protects object data */
	struct kref refs;  /* prevents kfree */
};

/* An item representing an inode and its access frequency */
struct hot_inode_item {
	struct hot_comm_item hot_inode; /* node in hot_inode_tree */
	struct hot_rb_tree hot_range_tree; /* tree of ranges */
	spinlock_t lock; /* protect range tree */
	struct hot_rb_tree *hot_inode_tree;
	u64 i_ino; /* inode number from inode */
};

/*
 * An item representing a range inside of
 * an inode whose frequency is being tracked
 */
struct hot_range_item {
	struct hot_comm_item hot_range;
	struct hot_inode_item *hot_inode; /* associated hot_inode_item */
	loff_t start; /* item offset in bytes in hot_range_tree */
	size_t len; /* length in bytes */
};

struct hot_info {
	struct hot_rb_tree hot_inode_tree;
	spinlock_t lock; /*protect inode tree */
};

extern void __init hot_cache_init(void);
extern int hot_track_init(struct super_block *sb);
extern void hot_track_exit(struct super_block *sb);
extern void hot_inode_item_put(struct hot_inode_item *he);

#endif  /* _LINUX_HOTTRACK_H */
