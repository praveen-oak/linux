/*
 * fs/hot_tracking.h
 *
 * Copyright (C) 2012 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#ifndef __HOT_TRACKING__
#define __HOT_TRACKING__

#include <linux/workqueue.h>
#include <linux/hot_tracking.h>

/* values for hot_freq_data flags */
#define FREQ_DATA_TYPE_INODE (1 << 0)
#define FREQ_DATA_TYPE_RANGE (1 << 1)

/* size of sub-file ranges */
#define RANGE_BITS 20
#define RANGE_SIZE (1 << RANGE_BITS)
#define FREQ_POWER 4

#endif /* __HOT_TRACKING__ */
