#include <linux/iothread.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/aio.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/llist.h>
#include <linux/sched.h>

static DEFINE_SPINLOCK(iothread_lock);
static struct kiothread _kiothread;


static void run_queue(struct work_struct *work)
{
	struct kiothread *kio = container_of(work, struct kiothread, work);
	struct file_io *fo = NULL;
	unsigned int ret;

	spin_lock(&iothread_lock);
	if (kio->in_progress)
		goto done;

next:
	if (list_empty(&kio->iolist)) {
		kio->in_progress = 0;
		goto done;
	}

	kio->in_progress = 1;

	fo = list_first_entry(&kio->iolist, struct file_io, list);

	list_del(&fo->list);
done:
	spin_unlock(&iothread_lock);

	if (!fo)
		return;

	//printk("%p, %u %u\n", fo->f.file, fo->count, fo->pos);
	ret = vfs_write(fo->f, &fo->buf, fo->count, &fo->pos);
	if (ret > 0) {
		_vfs_write(fo->f, &fo->buf, fo->count, &fo->pos, fo->tsk);
	}

	fput_write(fo->f, fo->tsk);

	//printk("g2 ret %u\n", ret);

//	kfree(fo);

	fo = NULL;
	spin_lock(&iothread_lock);
	goto next;
}

void init_kiothread(void)
{
	spin_lock(&iothread_lock);
	if (_kiothread.kio) {
		spin_unlock(&iothread_lock);
		return;
	}
	spin_unlock(&iothread_lock); //FIXME: Race condition....
	_kiothread.kio = alloc_workqueue("kiothread", WQ_MEM_RECLAIM, 1);
	INIT_LIST_HEAD(&_kiothread.iolist);
	INIT_WORK(&_kiothread.work, run_queue);
	_kiothread.in_progress = 0;
	printk("kiothread initialized.\n");
}

int speculate_away_and_wait(void)
{
	if (!_kiothread.kio)
		return 0;

	spin_lock(&iothread_lock);
	while (_kiothread.in_progress) {
		spin_unlock(&iothread_lock);
		spin_lock(&iothread_lock);
	}
	spin_unlock(&iothread_lock);
	return 0;
}

void speculate_set_iowait(void)
{
	spin_lock(&iothread_lock);
	while (_kiothread.in_progress) {
		spin_unlock(&iothread_lock);
		spin_lock(&iothread_lock);
	}
	_kiothread.in_progress = 1;
	spin_unlock(&iothread_lock);
}

void speculate_remove_iowait(void)
{
	spin_lock(&iothread_lock);
	while (_kiothread.in_progress) {
		spin_unlock(&iothread_lock);
		spin_lock(&iothread_lock);
	}
	_kiothread.in_progress = 0;
	spin_unlock(&iothread_lock);
}

void add_kiocb(struct kiocb *kiocb)
{
	spin_lock(&iothread_lock);
	list_add_tail(&kiocb->io_list, &_kiothread.iolist);
	spin_unlock(&iothread_lock);
	queue_work(_kiothread.kio, &_kiothread.work);
}

ssize_t add_file_io(struct file *f, const char __user *buf, size_t count, loff_t pos)
{
	struct file_io *fo = kmalloc(sizeof(struct file_io)+count, GFP_KERNEL);
	BUG_ON(!fo);

	fo->f = f;
	fo->count = count;
	fo->pos = pos;
	fo->tsk = current;
	copy_from_user(&fo->buf, buf, count);

	INIT_LIST_HEAD(&fo->list);

	BUG_ON(!_kiothread.kio);

	spin_lock(&iothread_lock);
	list_add_tail(&fo->list, &_kiothread.iolist);
	spin_unlock(&iothread_lock);

	queue_work(_kiothread.kio, &_kiothread.work);
	return count;
}
