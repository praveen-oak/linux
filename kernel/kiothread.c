#include <linux/iothread.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/fs.h>

static DEFINE_SPINLOCK(iothread_lock);
static struct kiothread _kiothread;

static void run_queue(struct work_struct *work)
{
	struct kiothread *kio = container_of(work, struct kiothread, work);
	struct file_io *fo = NULL;

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

	printk("count: %u\n", fo->count);
	_vfs_write(fo->file, fo->buf, fo->count, fo->pos);
	complete(&fo->sync);
//	kfree(fo->buf);
//	kfree(fo);

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

void speculate_away_and_wait(void)
{
	if (!_kiothread.kio)
		return;

	spin_lock(&iothread_lock);
	while (_kiothread.in_progress) {
		spin_unlock(&iothread_lock);
		spin_lock(&iothread_lock);
	}
	spin_unlock(&iothread_lock);
}

ssize_t add_file_io(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
//	return _vfs_write(file, buf, count, pos);
	struct file_io *fo = kmalloc(sizeof(struct file_io), GFP_KERNEL);
	BUG_ON(!fo);

	fo->buf = kmalloc(count, GFP_KERNEL);
	BUG_ON(!fo->buf);

	memcpy(fo->buf, buf, count);

	fo->file = file;
	fo->count = count;
	fo->pos = pos;

	BUG_ON(!_kiothread.kio);
	list_add_tail(&fo->list, &_kiothread.iolist);
	init_completion(&fo->sync);
	queue_work(_kiothread.kio, &_kiothread.work);
	wait_for_completion(&fo->sync);
	return count;
}
