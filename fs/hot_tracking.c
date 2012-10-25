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
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/types.h>
#include <linux/list_sort.h>
#include <linux/debugfs.h>
#include <linux/limits.h>
#include "hot_tracking.h"

static struct dentry *hot_debugfs_root;

/* kmem_cache pointers for slab caches */
static struct kmem_cache *hot_inode_item_cachep __read_mostly;
static struct kmem_cache *hot_range_item_cachep __read_mostly;

static u64 hot_raw_shift(u64 counter, u32 bits, bool dir)
{
	if (dir)
		return counter << bits;
	else
		return counter >> bits;
}

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
	struct hot_info *root = container_of(he->hot_inode_tree,
				struct hot_info, hot_inode_tree);

	hr->start = start;
	hr->len = hot_raw_shift(1, root->hot_type->range_bits, true);
	hr->hot_inode = he;
	kref_init(&hr->hot_range.refs);
	spin_lock_init(&hr->hot_range.lock);
	INIT_LIST_HEAD(&hr->hot_range.n_list);
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
	struct hot_info *root = container_of(
			hr->hot_inode->hot_inode_tree,
		struct hot_info, hot_inode_tree);

	spin_lock(&hr->hot_range.lock);
	if (!list_empty(&hr->hot_range.n_list)) {
		list_del_init(&hr->hot_range.n_list);
		root->hot_map_nr--;
	}

	rb_erase(&hr->hot_range.rb_node,
		&hr->hot_inode->hot_range_tree.map);
	spin_unlock(&hr->hot_range.lock);

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
	struct hot_info *root = container_of(he->hot_inode_tree,
		struct hot_info, hot_inode_tree);

	spin_lock(&he->hot_inode.lock);
	if (!list_empty(&he->hot_inode.n_list)) {
		list_del_init(&he->hot_inode.n_list);
		root->hot_map_nr--;
	}
	spin_unlock(&he->hot_inode.lock);

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

struct hot_inode_item
*hot_inode_item_lookup(struct hot_info *root, u64 ino)
{
	struct rb_node **p = &root->hot_inode_tree.map.rb_node;
	struct rb_node *parent = NULL;
	struct hot_comm_item *ci;
	struct hot_inode_item *entry;

	/* walk tree to find insertion point */
	spin_lock(&root->lock);
	while (*p) {
		parent = *p;
		ci = rb_entry(parent, struct hot_comm_item, rb_node);
		entry = container_of(ci, struct hot_inode_item, hot_inode);
		if (ino < entry->i_ino)
			p = &(*p)->rb_left;
		else if (ino > entry->i_ino)
			p = &(*p)->rb_right;
		else {
			kref_get(&entry->hot_inode.refs);
			spin_unlock(&root->lock);
			return entry;
		}
	}
	spin_unlock(&root->lock);

	entry = kmem_cache_zalloc(hot_inode_item_cachep, GFP_NOFS);
	if (!entry)
		return ERR_PTR(-ENOMEM);

	spin_lock(&root->lock);
	hot_inode_item_init(entry, ino, &root->hot_inode_tree);
	rb_link_node(&entry->hot_inode.rb_node, parent, p);
	rb_insert_color(&entry->hot_inode.rb_node,
			&root->hot_inode_tree.map);
	spin_unlock(&root->lock);

	kref_get(&entry->hot_inode.refs);
	return entry;
}
EXPORT_SYMBOL_GPL(hot_inode_item_lookup);

static loff_t hot_range_end(struct hot_range_item *hr)
{
	if (hr->start + hr->len < hr->start)
		return (loff_t)-1;

	return hr->start + hr->len - 1;
}

static struct hot_range_item
*hot_range_item_lookup(struct hot_inode_item *he,
			loff_t start)
{
	struct rb_node **p = &he->hot_range_tree.map.rb_node;
	struct rb_node *parent = NULL;
	struct hot_comm_item *ci;
	struct hot_range_item *entry;

	/* walk tree to find insertion point */
	spin_lock(&he->lock);
	while (*p) {
		parent = *p;
		ci = rb_entry(parent, struct hot_comm_item, rb_node);
		entry = container_of(ci, struct hot_range_item, hot_range);
		if (start < entry->start)
			p = &(*p)->rb_left;
		else if (start > hot_range_end(entry))
			p = &(*p)->rb_right;
		else {
			kref_get(&entry->hot_range.refs);
			spin_unlock(&he->lock);
			return entry;
		}
	}
	spin_unlock(&he->lock);

	entry = kmem_cache_zalloc(hot_range_item_cachep, GFP_NOFS);
	if (!entry)
		return ERR_PTR(-ENOMEM);

	spin_lock(&he->lock);
	hot_range_item_init(entry, start, he);
	rb_link_node(&entry->hot_range.rb_node, parent, p);
	rb_insert_color(&entry->hot_range.rb_node,
			&he->hot_range_tree.map);
	spin_unlock(&he->lock);

	kref_get(&entry->hot_range.refs);
	return entry;
}

/*
 * This function does the actual work of updating
 * the frequency numbers, whatever they turn out to be.
 */
static void hot_rw_freq_calc(struct timespec old_atime,
		struct timespec cur_time, u64 *avg)
{
	struct timespec delta_ts;
	u64 new_delta;

	delta_ts = timespec_sub(cur_time, old_atime);
	new_delta = timespec_to_ns(&delta_ts) >> FREQ_POWER;

	*avg = (*avg << FREQ_POWER) - *avg + new_delta;
	*avg = *avg >> FREQ_POWER;
}

static void hot_freq_data_update(struct hot_info *root,
		struct hot_freq_data *freq_data, bool write)
{
	struct timespec cur_time = current_kernel_time();

	if (write) {
		freq_data->nr_writes += 1;
		root->hot_type->ops.hot_rw_freq_calc_fn(
				freq_data->last_write_time,
				cur_time,
				&freq_data->avg_delta_writes);
		freq_data->last_write_time = cur_time;
	} else {
		freq_data->nr_reads += 1;
			root->hot_type->ops.hot_rw_freq_calc_fn(
				freq_data->last_read_time,
				cur_time,
				&freq_data->avg_delta_reads);
		freq_data->last_read_time = cur_time;
	}
}

/*
 * hot_temp_calc() is responsible for distilling the six heat
 * criteria down into a single temperature value for the data,
 * which is an integer between 0 and HEAT_MAX_VALUE.
 */
static u32 hot_temp_calc(struct hot_freq_data *freq_data)
{
	u32 result = 0;

	struct timespec ckt = current_kernel_time();
	u64 cur_time = timespec_to_ns(&ckt);

	u32 nrr_heat = (u32)hot_raw_shift((u64)freq_data->nr_reads,
					NRR_MULTIPLIER_POWER, true);
	u32 nrw_heat = (u32)hot_raw_shift((u64)freq_data->nr_writes,
					NRW_MULTIPLIER_POWER, true);

	u64 ltr_heat =
	hot_raw_shift((cur_time - timespec_to_ns(&freq_data->last_read_time)),
			LTR_DIVIDER_POWER, false);
	u64 ltw_heat =
	hot_raw_shift((cur_time - timespec_to_ns(&freq_data->last_write_time)),
			LTW_DIVIDER_POWER, false);

	u64 avr_heat =
	hot_raw_shift((((u64) -1) - freq_data->avg_delta_reads),
			AVR_DIVIDER_POWER, false);
	u64 avw_heat =
	hot_raw_shift((((u64) -1) - freq_data->avg_delta_writes),
			AVW_DIVIDER_POWER, false);

	/* ltr_heat is now guaranteed to be u32 safe */
	if (ltr_heat >= hot_raw_shift((u64) 1, 32, true))
		ltr_heat = 0;
	else
		ltr_heat = hot_raw_shift((u64) 1, 32, true) - ltr_heat;

	/* ltw_heat is now guaranteed to be u32 safe */
	if (ltw_heat >= hot_raw_shift((u64) 1, 32, true))
		ltw_heat = 0;
	else
		ltw_heat = hot_raw_shift((u64) 1, 32, true) - ltw_heat;

	/* avr_heat is now guaranteed to be u32 safe */
	if (avr_heat >= hot_raw_shift((u64) 1, 32, true))
		avr_heat = (u32) -1;

	/* avw_heat is now guaranteed to be u32 safe */
	if (avw_heat >= hot_raw_shift((u64) 1, 32, true))
		avw_heat = (u32) -1;

	nrr_heat = (u32)hot_raw_shift((u64)nrr_heat,
		(3 - NRR_COEFF_POWER), false);
	nrw_heat = (u32)hot_raw_shift((u64)nrw_heat,
		(3 - NRW_COEFF_POWER), false);
	ltr_heat = hot_raw_shift(ltr_heat, (3 - LTR_COEFF_POWER), false);
	ltw_heat = hot_raw_shift(ltw_heat, (3 - LTW_COEFF_POWER), false);
	avr_heat = hot_raw_shift(avr_heat, (3 - AVR_COEFF_POWER), false);
	avw_heat = hot_raw_shift(avw_heat, (3 - AVW_COEFF_POWER), false);

	result = nrr_heat + nrw_heat + (u32) ltr_heat +
		(u32) ltw_heat + (u32) avr_heat + (u32) avw_heat;

	return result;
}

static bool hot_is_obsolete(struct hot_freq_data *freq_data)
{
	int ret = 0;
	struct timespec ckt = current_kernel_time();

	u64 cur_time = timespec_to_ns(&ckt);
	u64 last_read_ns =
		(cur_time - timespec_to_ns(&freq_data->last_read_time));
	u64 last_write_ns =
		(cur_time - timespec_to_ns(&freq_data->last_write_time));
	u64 kick_ns =  TIME_TO_KICK * NSEC_PER_SEC;

	if ((last_read_ns > kick_ns) && (last_write_ns > kick_ns))
		ret = 1;

	return ret;
}

/*
 * Calculate a new temperature and, if necessary,
 * move the list_head corresponding to this inode or range
 * to the proper list with the new temperature
 */
static void hot_map_update(struct hot_freq_data *freq_data,
				struct hot_info *root)
{
	struct hot_map_head *buckets, *cur_bucket;
	struct hot_comm_item *comm_item;
	struct hot_inode_item *he;
	struct hot_range_item *hr;
	u32 temp = root->hot_type->ops.hot_temp_calc_fn(freq_data);
	u8 a_temp = (u8)hot_raw_shift((u64)temp, (32 - HEAT_MAP_BITS), false);
	u8 b_temp = (u8)hot_raw_shift((u64)freq_data->last_temp,
					(32 - HEAT_MAP_BITS), false);

	comm_item = container_of(freq_data,
			struct hot_comm_item, hot_freq_data);

	if (freq_data->flags & FREQ_DATA_TYPE_INODE) {
		he = container_of(comm_item,
			struct hot_inode_item, hot_inode);
		buckets = root->heat_inode_map;

		if (he == NULL)
			return;

		spin_lock(&he->hot_inode.lock);
		if (list_empty(&he->hot_inode.n_list) || (a_temp != b_temp)) {
			if (!list_empty(&he->hot_inode.n_list)) {
				list_del_init(&he->hot_inode.n_list);
				root->hot_map_nr--;
			}

			cur_bucket = buckets + a_temp;
			list_add_tail(&he->hot_inode.n_list,
					&cur_bucket->node_list);
			root->hot_map_nr++;
			freq_data->last_temp = temp;
		}
		spin_unlock(&he->hot_inode.lock);
	} else if (freq_data->flags & FREQ_DATA_TYPE_RANGE) {
		hr = container_of(comm_item,
			struct hot_range_item, hot_range);
		buckets = root->heat_range_map;

		if (hr == NULL)
			return;

		spin_lock(&hr->hot_range.lock);
		if (list_empty(&hr->hot_range.n_list) || (a_temp != b_temp)) {
			if (!list_empty(&hr->hot_range.n_list)) {
				list_del_init(&hr->hot_range.n_list);
				root->hot_map_nr--;
			}

			cur_bucket = buckets + a_temp;
			list_add_tail(&hr->hot_range.n_list,
					&cur_bucket->node_list);
			root->hot_map_nr++;
			freq_data->last_temp = temp;
		}
		spin_unlock(&hr->hot_range.lock);
	}
}

/* Update temperatures for each range item for aging purposes */
static void hot_range_update(struct hot_inode_item *he,
					struct hot_info *root)
{
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_range_item *hr;
	bool obsolete;

	spin_lock(&he->lock);
	node = rb_first(&he->hot_range_tree.map);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		hr = container_of(ci, struct hot_range_item, hot_range);
		kref_get(&hr->hot_range.refs);
		hot_map_update(&hr->hot_range.hot_freq_data, root);

		spin_lock(&hr->hot_range.lock);
		obsolete = root->hot_type->ops.hot_is_obsolete_fn(
				&hr->hot_range.hot_freq_data);
		spin_unlock(&hr->hot_range.lock);

		node = rb_next(node);

		hot_range_item_put(hr);
		if (obsolete)
			hot_range_item_put(hr);
	}
	spin_unlock(&he->lock);
}

/*
 * Initialize inode and range map info.
 */
static void hot_map_init(struct hot_info *root)
{
	int i;
	for (i = 0; i < HEAT_MAP_SIZE; i++) {
		INIT_LIST_HEAD(&root->heat_inode_map[i].node_list);
		INIT_LIST_HEAD(&root->heat_range_map[i].node_list);
		root->heat_inode_map[i].temp = i;
		root->heat_range_map[i].temp = i;
		spin_lock_init(&root->heat_inode_map[i].lock);
		spin_lock_init(&root->heat_range_map[i].lock);
	}
}

static void hot_map_list_free(struct list_head *node_list,
				struct hot_info *root)
{
	struct list_head *pos, *next;
	struct hot_comm_item *node;

	list_for_each_safe(pos, next, node_list) {
		node = list_entry(pos, struct hot_comm_item, n_list);
		list_del_init(&node->n_list);
		root->hot_map_nr--;
	}

}

/* Free inode and range map info */
static void hot_map_exit(struct hot_info *root)
{
	int i;
	for (i = 0; i < HEAT_MAP_SIZE; i++) {
		spin_lock(&root->heat_inode_map[i].lock);
		hot_map_list_free(&root->heat_inode_map[i].node_list, root);
		spin_unlock(&root->heat_inode_map[i].lock);
		spin_lock(&root->heat_range_map[i].lock);
		hot_map_list_free(&root->heat_range_map[i].node_list, root);
		spin_unlock(&root->heat_range_map[i].lock);
	}
}

/* Temperature compare function*/
static int hot_temp_cmp(void *priv, struct list_head *a,
				struct list_head *b)
{
	struct hot_comm_item *ap =
			container_of(a, struct hot_comm_item, n_list);
	struct hot_comm_item *bp =
			container_of(b, struct hot_comm_item, n_list);

	int diff = ap->hot_freq_data.last_temp
				- bp->hot_freq_data.last_temp;
	if (diff > 0)
		return -1;
	if (diff < 0)
		return 1;
	return 0;
}

/*
 * Every sync period we update temperatures for
 * each hot inode item and hot range item for aging
 * purposes.
 */
static void hot_update_worker(struct work_struct *work)
{
	struct hot_info *root = container_of(to_delayed_work(work),
					struct hot_info, update_work);
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_inode_item *he;
	int i;

	node = rb_first(&root->hot_inode_tree.map);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he = container_of(ci, struct hot_inode_item, hot_inode);
		kref_get(&he->hot_inode.refs);
		hot_map_update(
			&he->hot_inode.hot_freq_data, root);
		hot_range_update(he, root);
		node = rb_next(node);
		hot_inode_item_put(he);
	}

	/* Sort temperature map info */
	for (i = 0; i < HEAT_MAP_SIZE; i++) {
		spin_lock(&root->heat_inode_map[i].lock);
		list_sort(NULL, &root->heat_inode_map[i].node_list,
			hot_temp_cmp);
		spin_unlock(&root->heat_inode_map[i].lock);
		spin_lock(&root->heat_range_map[i].lock);
		list_sort(NULL, &root->heat_range_map[i].node_list,
			hot_temp_cmp);
		spin_unlock(&root->heat_range_map[i].lock);
	}

	/* Instert next delayed work */
	queue_delayed_work(root->update_wq, &root->update_work,
		msecs_to_jiffies(HEAT_UPDATE_DELAY * MSEC_PER_SEC));
}

static void *hot_range_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct rb_node *node, *node2;
	struct hot_comm_item *ci;
	struct hot_inode_item *he;
	struct hot_range_item *hr;
	loff_t l = *pos;

	spin_lock(&root->lock);
	node = rb_first(&root->hot_inode_tree.map);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he = container_of(ci, struct hot_inode_item, hot_inode);
		spin_lock(&he->lock);
		node2 = rb_first(&he->hot_range_tree.map);
		while (node2) {
			if (!l--) {
				ci = rb_entry(node2,
					struct hot_comm_item, rb_node);
				hr = container_of(ci,
					struct hot_range_item, hot_range);
				kref_get(&hr->hot_range.refs);
				spin_unlock(&he->lock);
				spin_unlock(&root->lock);
				return hr;
			}
			node2 = rb_next(node2);
		}
		node = rb_next(node);
		spin_unlock(&he->lock);
	}
	spin_unlock(&root->lock);
	return NULL;
}

static void *hot_range_seq_next(struct seq_file *seq,
				void *v, loff_t *pos)
{
	struct rb_node *node, *node2;
	struct hot_comm_item *ci;
	struct hot_inode_item *he;
	struct hot_range_item *hr_next = NULL, *hr = v;

	spin_lock(&hr->hot_range.lock);
	(*pos)++;
	node2 = rb_next(&hr->hot_range.rb_node);
	if (node2)
		goto next;

	node = rb_next(&hr->hot_inode->hot_inode.rb_node);
	if (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he = container_of(ci, struct hot_inode_item, hot_inode);
		node2 = rb_first(&he->hot_range_tree.map);
		if (node2) {
next:
			ci = rb_entry(node2,
				struct hot_comm_item, rb_node);
			hr_next = container_of(ci,
				struct hot_range_item, hot_range);
			kref_get(&hr_next->hot_range.refs);
		}
	}
	spin_unlock(&hr->hot_range.lock);

	hot_range_item_put(hr);
	return hr_next;
}

static void hot_range_seq_stop(struct seq_file *seq, void *v)
{
	struct hot_range_item *hr = v;

	if (hr)
		hot_range_item_put(hr);
}

static int hot_range_seq_show(struct seq_file *seq, void *v)
{
	struct hot_range_item *hr = v;
	struct hot_inode_item *he = hr->hot_inode;
	struct hot_freq_data *freq_data = &hr->hot_range.hot_freq_data;
	struct hot_info *root = container_of(he->hot_inode_tree,
		struct hot_info, hot_inode_tree);
	loff_t start = hr->start * hot_raw_shift(1,
			root->hot_type->range_bits, true);

	/* Always lock hot_inode_item first */
	spin_lock(&he->hot_inode.lock);
	spin_lock(&hr->hot_range.lock);
	seq_printf(seq, "inode %llu, range " \
			"%llu+%llu, reads %u, writes %u, temp %u\n",
			he->i_ino, (unsigned long long)start,
			(unsigned long long)hr->len,
			freq_data->nr_reads,
			freq_data->nr_writes,
			(u8)hot_raw_shift((u64)freq_data->last_temp,
					(32 - HEAT_MAP_BITS), false));
	spin_unlock(&hr->hot_range.lock);
	spin_unlock(&he->hot_inode.lock);

	return 0;
}

static void *hot_inode_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_inode_item *he = NULL;
	loff_t l = *pos;

	spin_lock(&root->lock);
	node = rb_first(&root->hot_inode_tree.map);
	while (node) {
		if (!l--) {
			ci = rb_entry(node, struct hot_comm_item, rb_node);
			he = container_of(ci,
				struct hot_inode_item, hot_inode);
			kref_get(&he->hot_inode.refs);
			break;
		}
		node = rb_next(node);
	}
	spin_unlock(&root->lock);

	return he;
}

static void *hot_inode_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct hot_inode_item *he_next = NULL, *he = v;
	struct rb_node *node;
	struct hot_comm_item *ci;

	spin_lock(&he->hot_inode.lock);
	(*pos)++;
	node = rb_next(&he->hot_inode.rb_node);
	if (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he_next = container_of(ci,
			struct hot_inode_item, hot_inode);
		kref_get(&he_next->hot_inode.refs);
	}
	spin_unlock(&he->hot_inode.lock);

	hot_inode_item_put(he);

	return he_next;
}

static void hot_inode_seq_stop(struct seq_file *seq, void *v)
{
	struct hot_inode_item *he = v;

	if (he)
		hot_inode_item_put(he);
}

static int hot_inode_seq_show(struct seq_file *seq, void *v)
{
	struct hot_inode_item *he = v;
	struct hot_freq_data *freq_data = &he->hot_inode.hot_freq_data;

	spin_lock(&he->hot_inode.lock);
	seq_printf(seq, "inode %llu, reads %u, writes %u, temp %d\n",
		he->i_ino,
		freq_data->nr_reads,
		freq_data->nr_writes,
		(u8)hot_raw_shift((u64)freq_data->last_temp,
				(32 - HEAT_MAP_BITS), false));
	spin_unlock(&he->hot_inode.lock);

	return 0;
}

static void *hot_spot_range_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct hot_range_item *hr;
	struct hot_comm_item *comm_item;
	struct list_head *n_list;
	int i;

	for (i = HEAT_MAP_SIZE - 1; i >= 0; i--) {
		spin_lock(&root->heat_range_map[i].lock);
		n_list = seq_list_start(
			&root->heat_range_map[i].node_list, *pos);
		if (n_list) {
			comm_item = container_of(n_list,
				struct hot_comm_item, n_list);
			hr = container_of(comm_item,
				struct hot_range_item, hot_range);
			kref_get(&hr->hot_range.refs);
			spin_unlock(&root->heat_range_map[i].lock);
			return hr;
		}
		spin_unlock(&root->heat_range_map[i].lock);
	}

	return NULL;
}

static void *hot_spot_range_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct hot_range_item *hr_next, *hr = v;
	struct hot_comm_item *comm_item;
	struct list_head *n_list;
	int i, j;
	spin_lock(&hr->hot_range.lock);
	i = (int)hot_raw_shift(hr->hot_range.hot_freq_data.last_temp,
				(32 - HEAT_MAP_BITS), false);
	spin_unlock(&hr->hot_range.lock);

	spin_lock(&root->heat_range_map[i].lock);
	n_list = seq_list_next(&hr->hot_range.n_list,
		&root->heat_range_map[i].node_list, pos);
	hot_range_item_put(hr);
next:
	j = i;
	if (n_list) {
		comm_item = container_of(n_list,
			struct hot_comm_item, n_list);
		hr_next = container_of(comm_item,
			struct hot_range_item, hot_range);
		kref_get(&hr_next->hot_range.refs);
		spin_unlock(&root->heat_range_map[i].lock);
		return hr_next;
	} else if (--i >= 0) {
		spin_unlock(&root->heat_range_map[j].lock);
		spin_lock(&root->heat_range_map[i].lock);
		n_list = seq_list_next(&root->heat_range_map[i].node_list,
				&root->heat_range_map[i].node_list, pos);
		goto next;
	}

	spin_unlock(&root->heat_range_map[j].lock);
	return NULL;
}

static void *hot_spot_inode_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct hot_inode_item *he;
	struct hot_comm_item *comm_item;
	struct list_head *n_list;
	int i;

	for (i = HEAT_MAP_SIZE - 1; i >= 0; i--) {
		spin_lock(&root->heat_inode_map[i].lock);
		n_list = seq_list_start(
			&root->heat_inode_map[i].node_list, *pos);
		if (n_list) {
			comm_item = container_of(n_list,
				struct hot_comm_item, n_list);
			he = container_of(comm_item,
				struct hot_inode_item, hot_inode);
			kref_get(&he->hot_inode.refs);
			spin_unlock(&root->heat_inode_map[i].lock);
			return he;
		}
		spin_unlock(&root->heat_inode_map[i].lock);
	}

	return NULL;
}

static void *hot_spot_inode_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct hot_info *root = seq->private;
	struct hot_inode_item *he_next, *he = v;
	struct hot_comm_item *comm_item;
	struct list_head *n_list;
	int i, j;
	spin_lock(&he->hot_inode.lock);
	i = (int)hot_raw_shift(he->hot_inode.hot_freq_data.last_temp,
				(32 - HEAT_MAP_BITS), false);
	spin_unlock(&he->hot_inode.lock);

	spin_lock(&root->heat_inode_map[i].lock);
	n_list = seq_list_next(&he->hot_inode.n_list,
			&root->heat_inode_map[i].node_list, pos);
	hot_inode_item_put(he);
next:
	j = i;
	if (n_list) {
		comm_item = container_of(n_list,
			struct hot_comm_item, n_list);
		he_next = container_of(comm_item,
			struct hot_inode_item, hot_inode);
		kref_get(&he_next->hot_inode.refs);
		spin_unlock(&root->heat_inode_map[i].lock);
		return he_next;
	} else if (--i >= 0) {
		spin_unlock(&root->heat_inode_map[j].lock);
		spin_lock(&root->heat_inode_map[i].lock);
		n_list = seq_list_next(&root->heat_inode_map[i].node_list,
				&root->heat_inode_map[i].node_list, pos);
		goto next;
	}

	spin_unlock(&root->heat_inode_map[j].lock);
	return NULL;
}

static const struct seq_operations hot_range_seq_ops = {
	.start = hot_range_seq_start,
	.next = hot_range_seq_next,
	.stop = hot_range_seq_stop,
	.show = hot_range_seq_show
};

static const struct seq_operations hot_inode_seq_ops = {
	.start = hot_inode_seq_start,
	.next = hot_inode_seq_next,
	.stop = hot_inode_seq_stop,
	.show = hot_inode_seq_show
};

static const struct seq_operations hot_spot_range_seq_ops = {
	.start = hot_spot_range_seq_start,
	.next = hot_spot_range_seq_next,
	.stop = hot_range_seq_stop,
	.show = hot_range_seq_show
};

static const struct seq_operations hot_spot_inode_seq_ops = {
	.start = hot_spot_inode_seq_start,
	.next = hot_spot_inode_seq_next,
	.stop = hot_inode_seq_stop,
	.show = hot_inode_seq_show
};

static int hot_range_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open_private(file, &hot_range_seq_ops, 0);
	if (ret == 0) {
		struct seq_file *seq = file->private_data;
		seq->private = inode->i_private;
	}
	return ret;
}

static int hot_inode_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open_private(file, &hot_inode_seq_ops, 0);
	if (ret == 0) {
		struct seq_file *seq = file->private_data;
		seq->private = inode->i_private;
	}
	return ret;
}

static int hot_spot_range_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open_private(file, &hot_spot_range_seq_ops, 0);
	if (ret == 0) {
		struct seq_file *seq = file->private_data;
		seq->private = inode->i_private;
	}
	return ret;
}

static int hot_spot_inode_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open_private(file, &hot_spot_inode_seq_ops, 0);
	if (ret == 0) {
		struct seq_file *seq = file->private_data;
		seq->private = inode->i_private;
	}
	return ret;
}

/* fops to override for printing range data */
static const struct file_operations hot_debugfs_range_fops = {
	.open = hot_range_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/* fops to override for printing inode data */
static const struct file_operations hot_debugfs_inode_fops = {
	.open = hot_inode_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/* fops to override for printing temperature data */
static const struct file_operations hot_debugfs_spot_range_fops = {
	.open = hot_spot_range_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct file_operations hot_debugfs_spot_inode_fops = {
	.open = hot_spot_inode_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct hot_debugfs hot_debugfs[] = {
	{
		.name = "rt_stats_range",
		.fops  = &hot_debugfs_range_fops,
	},
	{
		.name = "rt_stats_inode",
		.fops  = &hot_debugfs_inode_fops,
	},
	{
		.name = "hot_spots_range",
		.fops  = &hot_debugfs_spot_range_fops,
	},
	{
		.name = "hot_spots_inode",
		.fops  = &hot_debugfs_spot_inode_fops,
	},
};

/* initialize debugfs */
static int hot_debugfs_init(struct super_block *sb)
{
	static const char hot_name[] = "hot_track";
	struct dentry *dentry;
	int i, ret = 0;

	/* Determine if hot debufs root has existed */
	if (!hot_debugfs_root) {
		hot_debugfs_root = debugfs_create_dir(hot_name, NULL);
		if (IS_ERR(hot_debugfs_root)) {
			ret = PTR_ERR(hot_debugfs_root);
			return ret;
		}
	}

	if (!S_ISDIR(hot_debugfs_root->d_inode->i_mode))
		return -ENOTDIR;

	/* create debugfs folder for this volume by mounted dev name */
	sb->s_hot_root->vol_dentry =
			debugfs_create_dir(sb->s_id, hot_debugfs_root);
	if (IS_ERR(sb->s_hot_root->vol_dentry)) {
		ret = PTR_ERR(sb->s_hot_root->vol_dentry);
		goto err;
	}

	/* create debugfs hot data files */
	for (i = 0; i < ARRAY_SIZE(hot_debugfs); i++) {
		dentry = debugfs_create_file(hot_debugfs[i].name,
					S_IFREG | S_IRUSR | S_IWUSR,
					sb->s_hot_root->vol_dentry,
					sb->s_hot_root,
					hot_debugfs[i].fops);
		if (IS_ERR(dentry)) {
			ret = PTR_ERR(dentry);
			goto err;
		}
	}

	return 0;

err:
	debugfs_remove_recursive(sb->s_hot_root->vol_dentry);

	if (list_empty(&hot_debugfs_root->d_subdirs)) {
		debugfs_remove(hot_debugfs_root);
		hot_debugfs_root = NULL;
	}

	return ret;
}

/* remove dentries for debugsfs */
static void hot_debugfs_exit(struct super_block *sb)
{
	/* remove all debugfs entries recursively from the volume root */
	if (sb->s_hot_root->vol_dentry)
		debugfs_remove_recursive(sb->s_hot_root->vol_dentry);
	else
		BUG();

	if (list_empty(&hot_debugfs_root->d_subdirs)) {
		debugfs_remove(hot_debugfs_root);
		hot_debugfs_root = NULL;
	}
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

static int hot_track_prune_map(struct hot_map_head *map_head,
				bool type, int nr)
{
	struct hot_comm_item *node;
	int i;

	for (i = 0; i < HEAT_MAP_SIZE; i++) {
		spin_lock(&(map_head + i)->lock);
		while (!list_empty(&(map_head + i)->node_list)) {
			if (nr-- <= 0)
				break;

			node = list_first_entry(&(map_head + i)->node_list,
					struct hot_comm_item, n_list);
			if (type) {
				struct hot_inode_item *hot_inode =
					container_of(node,
					struct hot_inode_item, hot_inode);
				hot_inode_item_put(hot_inode);
			} else {
				struct hot_range_item *hot_range =
					container_of(node,
					struct hot_range_item, hot_range);
				hot_range_item_put(hot_range);
			}
		}
		spin_unlock(&(map_head + i)->lock);
	}

	return nr;
}

/* The shrinker callback function */
static int hot_track_prune(struct shrinker *shrink,
			struct shrink_control *sc)
{
	struct hot_info *root =
		container_of(shrink, struct hot_info, hot_shrink);
	int ret;

	if (sc->nr_to_scan == 0)
		return root->hot_map_nr;

	if (!(sc->gfp_mask & __GFP_FS))
		return -1;

	ret = hot_track_prune_map(root->heat_range_map,
				false, sc->nr_to_scan);
	if (ret > 0)
		ret = hot_track_prune_map(root->heat_inode_map,
					true, ret);
	if (ret > 0)
		root->hot_map_nr -= (sc->nr_to_scan - ret);

	return root->hot_map_nr;
}

/*
 * Main function to update access frequency from read/writepage(s) hooks
 */
void hot_update_freqs(struct inode *inode, loff_t start,
			size_t len, int rw)
{
	struct hot_info *root = inode->i_sb->s_hot_root;
	struct hot_inode_item *he;
	struct hot_range_item *hr;
	u64 range_size;
	loff_t cur, end;

	if (!root || (len == 0))
		return;

	he = hot_inode_item_lookup(root, inode->i_ino);
	if (IS_ERR(he)) {
		WARN_ON(1);
		return;
	}

	spin_lock(&he->hot_inode.lock);
	hot_freq_data_update(root, &he->hot_inode.hot_freq_data, rw);
	spin_unlock(&he->hot_inode.lock);

	/*
	 * Align ranges on range size boundary
	 * to prevent proliferation of range structs
	 */
	range_size  = hot_raw_shift(1,
			root->hot_type->range_bits, true);
	end = hot_raw_shift((start + len + range_size - 1),
			root->hot_type->range_bits, false);
	cur = hot_raw_shift(start, root->hot_type->range_bits, false);
	for (; cur < end; cur++) {
		hr = hot_range_item_lookup(he, cur);
		if (IS_ERR(hr)) {
			WARN(1, "hot_range_item_lookup returns %ld\n",
				PTR_ERR(hr));
			hot_inode_item_put(he);
			return;
		}

		spin_lock(&hr->hot_range.lock);
		hot_freq_data_update(root, &hr->hot_range.hot_freq_data, rw);
		spin_unlock(&hr->hot_range.lock);

		hot_range_item_put(hr);
	}

	hot_inode_item_put(he);
}
EXPORT_SYMBOL_GPL(hot_update_freqs);

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
	hot_map_init(root);

	/* Get hot type for specific FS */
	root->hot_type = &sb->s_type->hot_type;
	if (!root->hot_type->ops.hot_rw_freq_calc_fn)
		root->hot_type->ops.hot_rw_freq_calc_fn = hot_rw_freq_calc;
	if (!root->hot_type->ops.hot_temp_calc_fn)
		root->hot_type->ops.hot_temp_calc_fn = hot_temp_calc;
	if (!root->hot_type->ops.hot_is_obsolete_fn)
		root->hot_type->ops.hot_is_obsolete_fn = hot_is_obsolete;
	if (root->hot_type->range_bits == 0)
		root->hot_type->range_bits = RANGE_BITS;

	root->update_wq = alloc_workqueue(
		"hot_update_wq", WQ_NON_REENTRANT, 0);
	if (!root->update_wq) {
		printk(KERN_ERR "%s: Failed to create "
				"hot update workqueue\n", __func__);
		goto failed_wq;
	}

	/* Initialize hot tracking wq and arm one delayed work */
	INIT_DELAYED_WORK(&root->update_work, hot_update_worker);
	queue_delayed_work(root->update_wq, &root->update_work,
		msecs_to_jiffies(HEAT_UPDATE_DELAY * MSEC_PER_SEC));

	/* Register a shrinker callback */
	root->hot_shrink.shrink = hot_track_prune;
	root->hot_shrink.seeks = DEFAULT_SEEKS;
	register_shrinker(&root->hot_shrink);

	sb->s_hot_root = root;

	ret = hot_debugfs_init(sb);
	if (ret) {
		printk(KERN_ERR "%s: hot_debugfs_init error: %d\n",
				__func__, ret);
		goto failed_debugfs;
	}

	printk(KERN_INFO "VFS: Turning on hot data tracking\n");

	return 0;

failed_debugfs:
	unregister_shrinker(&root->hot_shrink);
	cancel_delayed_work_sync(&root->update_work);
	destroy_workqueue(root->update_wq);
	sb->s_hot_root = NULL;
failed_wq:
	hot_map_exit(root);
	hot_inode_tree_exit(root);
	kfree(root);
	return ret;
}
EXPORT_SYMBOL_GPL(hot_track_init);

void hot_track_exit(struct super_block *sb)
{
	struct hot_info *root = sb->s_hot_root;

	hot_debugfs_exit(sb);
	unregister_shrinker(&root->hot_shrink);
	cancel_delayed_work_sync(&root->update_work);
	destroy_workqueue(root->update_wq);
	hot_map_exit(root);
	hot_inode_tree_exit(root);
	sb->s_hot_root = NULL;
	kfree(root);
}
EXPORT_SYMBOL_GPL(hot_track_exit);
