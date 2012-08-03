#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/hrtimer.h>

struct nullb {
	struct list_head list;
	struct request_queue *q;
	struct gendisk *disk;
	struct hrtimer timer;
	spinlock_t lock;

	/* Request list for timer-based approach */
	struct llist_head timer_requests;
};

static LIST_HEAD(nullb_list);
static struct mutex lock;
static int null_major;

static DEFINE_PER_CPU(struct llist_head, ipi_lists);

#define IRQ_NONE 0
#define IRQ_SOFTIRQ 1
#define IRQ_TIMER 2

static int submit_queues = 1;
module_param(submit_queues, int, S_IRUGO);
MODULE_PARM_DESC(submit_queues, "Number of submission queues");

static int complete_queues = 1;
module_param(complete_queues, int, S_IRUGO);
MODULE_PARM_DESC(complete_queues, "Number of completion queues");

static int home_node = NUMA_NO_NODE;
module_param(home_node, int, S_IRUGO);
MODULE_PARM_DESC(home_node, "Home node for the device");

static int use_mq = 1;
module_param(use_mq, int, S_IRUGO);
MODULE_PARM_DESC(use_mq, "Use blk-mq interface");

static int gb = 250;
module_param(gb, int, S_IRUGO);
MODULE_PARM_DESC(gb, "Size in GB");

static int bs = 512;
module_param(bs, int, S_IRUGO);
MODULE_PARM_DESC(bs, "Block size (in bytes)");

static int irqmode = 1;
module_param(irqmode, int, S_IRUGO);
MODULE_PARM_DESC(irqmode, "IRQ completion handler. 0-none, 1-softirq, 2-timer");

static int completion_time = 50000;
module_param(completion_time, int, S_IRUGO);
MODULE_PARM_DESC(completion_time, "Time in ns to complete a request in hardware. Default: 50.000ns");


MODULE_LICENSE("GPL");

static enum hrtimer_restart null_request_timer_expired(struct hrtimer *timer)
{
	struct nullb *blk = list_entry((&nullb_list)->next, struct nullb, list);
	struct llist_node *entry;
	struct request *rq;

	while ((entry = llist_del_first(&blk->timer_requests)) != NULL) {
		rq = llist_entry(entry, struct request, ll_list);
		blk_mq_end_io(rq->q->queue_hw_ctx, rq, 0);
	}

	return HRTIMER_NORESTART;
}

static void null_request_mq_end_timer(struct request *rq)
{
	struct nullb *blk = list_entry((&nullb_list)->next, struct nullb, list);

	rq->ll_list.next = NULL;
	if (llist_add(&rq->ll_list, &blk->timer_requests)) {
		hrtimer_start(&blk->timer, ktime_set(0, 50000), HRTIMER_MODE_REL);
	}
}

static void null_ipi_mq_end_io(void *data)
{
	struct llist_head *list = &per_cpu(ipi_lists, smp_processor_id());
	struct llist_node *entry;
	struct request *rq;

	while ((entry = llist_del_first(list)) != NULL) {
		rq = llist_entry(entry, struct request, ll_list);
		blk_mq_end_io(rq->q->queue_hw_ctx, rq, 0);
	}
}

static void null_request_mq_end_ipi(struct request *rq)
{
	int cpu = smp_processor_id();

	rq->ll_list.next = NULL;

	if (llist_add(&rq->ll_list, &per_cpu(ipi_lists, cpu))) {
		smp_call_function_single(cpu, null_ipi_mq_end_io, NULL, 0);
	}
}

static void null_request_end_ipi(struct request *rq)
{
	blk_end_request_all(rq, 0);
}

static inline void null_handle_mq_rq(struct blk_mq_hw_ctx *hctx, struct request *rq)
{
	/* Complete IO by inline, softirq or timer */
	switch (irqmode) {
	case IRQ_NONE:
		blk_mq_end_io(hctx, rq, 0);
		break;
	case IRQ_SOFTIRQ:
		null_request_mq_end_ipi(rq);
		break;
	case IRQ_TIMER:
		null_request_mq_end_timer(rq);
		break;
	}
}

static void null_request_fn(struct request_queue *q)
{
	struct request *rq;

	while ((rq = blk_fetch_request(q)) != NULL) {
		blk_complete_request(rq);
	}
}

static int null_queue_rq(struct blk_mq_hw_ctx *hctx, struct request *rq)
{
	null_handle_mq_rq(hctx, rq);
	return BLK_MQ_RQ_QUEUE_OK;
}

static struct blk_mq_ops null_mq_ops = {
	.queue_rq       = null_queue_rq,
	.map_queue      = blk_mq_map_single_queue,
};

static struct blk_mq_reg null_mq_reg = {
	.ops		= &null_mq_ops,
	.nr_hw_queues	= 1,
	.queue_depth	= 64,
	.flags		= BLK_MQ_F_SHOULD_MERGE,
};

static void null_del_dev(struct nullb *nullb)
{
	list_del_init(&nullb->list);

	del_gendisk(nullb->disk);
	if (use_mq)
		blk_mq_free_queue(nullb->q);
	else
		blk_cleanup_queue(nullb->q);
	put_disk(nullb->disk);
	kfree(nullb);
}

static int null_open(struct block_device *bdev, fmode_t mode)
{
	return 0;
}

static int null_release(struct gendisk *disk, fmode_t mode)
{
	return 0;
}

static const struct block_device_operations null_fops = {
	.owner =	THIS_MODULE,
	.open =		null_open,
	.release =	null_release,
};

static int null_add_dev(void)
{
	struct gendisk *disk;
	struct nullb *nullb;
	unsigned int i;
	sector_t size;

	/* Initialize a separate list for each CPU for issuing softirqs */
	for_each_possible_cpu(i)
		init_llist_head(&per_cpu(ipi_lists, i));

	nullb = kmalloc_node(sizeof(*nullb), GFP_KERNEL, home_node);
	if (!nullb)
		return -ENOMEM;

	memset(nullb, 0, sizeof(*nullb));

	if (irqmode == IRQ_TIMER) {
		hrtimer_init(&nullb->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		nullb->timer.function = null_request_timer_expired;

		init_llist_head(&nullb->timer_requests);
	}

	if (use_mq) {
		null_mq_reg.numa_node = home_node;
		nullb->q = blk_mq_init_queue(&null_mq_reg, &nullb->lock);
	} else {
		nullb->q = blk_init_queue_node(null_request_fn, &nullb->lock, home_node);
		blk_queue_softirq_done(nullb->q, null_request_end_ipi);
	}


	if (!nullb) {
		kfree(nullb);
		return -ENOMEM;
	}

	disk = nullb->disk = alloc_disk_node(1, home_node);
	if (!disk) {
		if (use_mq)
			blk_mq_free_queue(nullb->q);
		else
			blk_cleanup_queue(nullb->q);
		kfree(nullb);
		return -ENOMEM;
	}

	mutex_lock(&lock);
	list_add_tail(&nullb->list, &nullb_list);
	mutex_unlock(&lock);

	blk_queue_logical_block_size(nullb->q, bs);
	blk_queue_physical_block_size(nullb->q, bs);

	size = gb * 1024 * 1024 * 1024ULL;
	size /= (sector_t) bs;
	set_capacity(disk, size);

	disk->flags |= GENHD_FL_NO_PART_SCAN | GENHD_FL_EXT_DEVT;
	spin_lock_init(&nullb->lock);
	disk->major		= null_major;
	disk->first_minor	= 0;
	disk->fops		= &null_fops;
	disk->private_data	= nullb;
	disk->queue		= nullb->q;
	sprintf(disk->disk_name, "nullb%d", 0);
	add_disk(disk);
	return 0;
}

static int __init null_init(void)
{
	mutex_init(&lock);

	null_major = register_blkdev(0, "nullb");
	if (null_major < 0)
		return null_major;

	if (null_add_dev()) {
		unregister_blkdev(null_major, "nullb");
		return -EINVAL;
	}

	printk(KERN_INFO "null: module loaded\n");

	return 0;
}

static void __exit null_exit(void)
{
	struct nullb *nullb;

	unregister_blkdev(null_major, "nullb");

	mutex_lock(&lock);
	while (!list_empty(&nullb_list)) {
		nullb = list_entry(nullb_list.next, struct nullb, list);
		null_del_dev(nullb);
	}
	mutex_unlock(&lock);
}

module_init(null_init);
module_exit(null_exit);
