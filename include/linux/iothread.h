#ifndef _KIOTHREAD_H
#define _KIOTHREAD_H

#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/aio.h>
#include <linux/file.h>

struct kiothread {
	struct workqueue_struct *kio;
	struct work_struct work;
	struct list_head iolist;
	int in_progress;
};

struct file_io {
	struct list_head list;
	struct task_struct *tsk;
	struct file *f;
	size_t count;
	loff_t pos;
	char buf[];
};

void init_kiothread(void);
ssize_t add_file_io(struct file *f, const char *buf, size_t count, loff_t pos);
void speculate_set_iowait(void);
void speculate_remove_iowait(void);
void add_kiocb(struct kiocb *);
int speculate_away_and_wait(void);
#endif
