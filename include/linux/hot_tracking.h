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

#define HEAT_MAP_BITS 8
#define HEAT_MAP_SIZE (1 << HEAT_MAP_BITS)

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

struct hot_heat_info {
	__u64 avg_delta_reads;
	__u64 avg_delta_writes;
	__u64 last_read_time;
	__u64 last_write_time;
	__u32 num_reads;
	__u32 num_writes;
	__u32 temp;
	__u8 live;
};

/* List heads in hot map array */
struct hot_map_head {
	struct list_head node_list;
	u8 temp;
	spinlock_t lock;
};

/* The common info for both following structures */
struct hot_comm_item {
	struct rb_node rb_node; /* rbtree index */
	struct hot_freq_data hot_freq_data;  /* frequency data */
	spinlock_t lock; /* protects object data */
	struct kref refs;  /* prevents kfree */
	struct list_head n_list; /* list node index */
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

typedef void (hot_rw_freq_calc_fn) (struct timespec old_atime,
			struct timespec cur_time, u64 *avg);
typedef u32 (hot_temp_calc_fn) (struct hot_freq_data *freq_data);
typedef bool (hot_is_obsolete_fn) (struct hot_freq_data *freq_data);

struct hot_func_ops {
	hot_rw_freq_calc_fn *hot_rw_freq_calc_fn;
	hot_temp_calc_fn *hot_temp_calc_fn;
	hot_is_obsolete_fn *hot_is_obsolete_fn;
};

/* identifies an hot type */
struct hot_type {
	u64 range_bits;
	/* fields provided by specific FS */
	struct hot_func_ops ops;
};

struct hot_info {
	struct hot_rb_tree hot_inode_tree;
	spinlock_t lock; /*protect inode tree */

	/* map of inode temperature */
	struct hot_map_head heat_inode_map[HEAT_MAP_SIZE];
	/* map of range temperature */
	struct hot_map_head heat_range_map[HEAT_MAP_SIZE];
	unsigned int hot_map_nr;

	struct workqueue_struct *update_wq;
	struct delayed_work update_work;
	struct hot_type *hot_type;
	struct shrinker hot_shrink;
	struct dentry *vol_dentry;
};

/*
 * Two variables have meanings as below:
 * 1. time to quit keeping track of tracking data (seconds)
 * 2. set how often to update temperatures (seconds)
 */
extern int sysctl_hot_kick_time, sysctl_hot_update_delay;

/*
 * Hot data tracking ioctls:
 *
 * HOT_INFO - retrieve info on frequency of access
 */
#define FS_IOC_GET_HEAT_INFO _IOR('f', 17, \
			struct hot_heat_info)

extern void __init hot_cache_init(void);
extern int hot_track_init(struct super_block *sb);
extern void hot_track_exit(struct super_block *sb);
extern void hot_inode_item_put(struct hot_inode_item *he);
extern void hot_update_freqs(struct inode *inode, loff_t start,
				size_t len, int rw);
extern struct hot_inode_item *hot_inode_item_lookup(struct hot_info *root,
						u64 ino);

#endif  /* _LINUX_HOTTRACK_H */
