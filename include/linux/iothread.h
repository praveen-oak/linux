#ifndef _KIOTHREAD_H
#define _KIOTHREAD_H

#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/completion.h>

struct kiothread {
	struct workqueue_struct *kio;
	struct work_struct work;
	struct list_head iolist;
	int in_progress;
};

struct file_io {
	struct file *file;
	const char __user *buf;
	size_t count;
	loff_t *pos;
	struct list_head list;
	struct completion sync;
};

void init_kiothread(void);
ssize_t add_file_io(struct file *file, const char *buf, size_t count, loff_t *pos);
void speculate_away_and_wait(void);
#endif
