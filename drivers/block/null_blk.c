#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/hrtimer.h>

struct nullblk {
	struct list_head list;
	unsigned int index;
	struct request_queue *q;
	struct gendisk *disk;
	struct hrtimer timer;
	spinlock_t lock;
};

static LIST_HEAD(nullblk_list);
static struct mutex lock;
static int null_major;
static int nullblk_indexes;

struct completion_queue {
	struct llist_head list;
	struct hrtimer timer;
};

/*
 * These are per-cpu for now, they will need to be configured by the
 * complete_queues parameter and appropriately mapped.
 */
static DEFINE_PER_CPU(struct completion_queue, completion_queues);

enum {
	NULL_IRQ_NONE		= 0,
	NULL_IRQ_SOFTIRQ	= 1,
	NULL_IRQ_TIMER		= 2,

	NULL_Q_BIO		= 0,
	NULL_Q_RQ		= 1,
	NULL_Q_MQ		= 2,

	NULL_A_SINGLE		= 0,
	NULL_A_PERNODE		= 1,
	NULL_A_PERCPU		= 2,
};

static int complete_queues = 1;
module_param(complete_queues, int, S_IRUGO);
MODULE_PARM_DESC(complete_queues, "Number of completion queues");

static int home_node = NUMA_NO_NODE;
module_param(home_node, int, S_IRUGO);
MODULE_PARM_DESC(home_node, "Home node for the device");

static int queue_mode = NULL_Q_MQ;
module_param(queue_mode, int, S_IRUGO);
MODULE_PARM_DESC(use_mq, "Use blk-mq interface (0=bio,1=rq,2=multiqueue)");

static int gb = 250;
module_param(gb, int, S_IRUGO);
MODULE_PARM_DESC(gb, "Size in GB");

static int bs = 512;
module_param(bs, int, S_IRUGO);
MODULE_PARM_DESC(bs, "Block size (in bytes)");

static int nr_devices = 2;
module_param(nr_devices, int, S_IRUGO);
MODULE_PARM_DESC(nr_devices, "Number of devices to register");

static int irqmode = NULL_IRQ_SOFTIRQ;
module_param(irqmode, int, S_IRUGO);
MODULE_PARM_DESC(irqmode, "IRQ completion handler. 0-none, 1-softirq, 2-timer. Default: softirq");

static int completion_nsec = 10000;
module_param(completion_nsec, int, S_IRUGO);
MODULE_PARM_DESC(completion_nsec, "Time in ns to complete a request in hardware. Default: 10,000ns");

static int hw_queue_depth = 64;
module_param(hw_queue_depth, int, S_IRUGO);
MODULE_PARM_DESC(hw_queue_depth, "Queue depth for each hardware queue. Default: 64");

static int hctx_mode = 0;
module_param(hctx_mode, int, S_IRUGO);
MODULE_PARM_DESC(hctx_mode, "Allocation scheme for hardware context queues. 0-single, 1-per-node, 2-per-cpu. Default: single");

static void null_complete_request(struct request *rq)
{
	if (queue_mode == NULL_Q_MQ)
		blk_mq_end_io(rq, 0);
	else {
		INIT_LIST_HEAD(&rq->queuelist);
		blk_end_request_all(rq, 0);
	}
}

static enum hrtimer_restart null_bio_timer_expired(struct hrtimer *timer)
{
	struct completion_queue *cq;
	struct llist_node *entry;
	struct bio *bio;

	cq = &per_cpu(completion_queues, smp_processor_id());

	while ((entry = llist_del_all(&cq->list)) != NULL) {
		do {
			bio = container_of(entry, struct bio, bi_next);
			bio_endio(bio, 0);
			entry = entry->next;
		} while (entry);
	}

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart null_request_timer_expired(struct hrtimer *timer)
{
	struct completion_queue *cq;
	struct llist_node *entry;
	struct request *rq;

	cq = &per_cpu(completion_queues, smp_processor_id());

	while ((entry = llist_del_all(&cq->list)) != NULL)
		llist_for_each_entry(rq, entry, ll_list)
			null_complete_request(rq);

	return HRTIMER_NORESTART;
}

static void null_request_end_timer(struct request *rq)
{
	struct completion_queue *cq = &per_cpu(completion_queues, get_cpu());

	rq->ll_list.next = NULL;
	if (llist_add(&rq->ll_list, &cq->list)) {
		ktime_t kt = ktime_set(0, completion_nsec);

		hrtimer_start(&cq->timer, kt, HRTIMER_MODE_REL);
	}

	put_cpu();
}

static void null_bio_end_timer(struct bio *bio)
{
	struct completion_queue *cq = &per_cpu(completion_queues, get_cpu());

	bio->bi_next = NULL;
	if (llist_add((struct llist_node *) bio->bi_next, &cq->list)) {
		ktime_t kt = ktime_set(0, completion_nsec);

		hrtimer_start(&cq->timer, kt, HRTIMER_MODE_REL);
	}

	put_cpu();
}

static void null_ipi_request_end_io(void *data)
{
	struct completion_queue *cq;
	struct llist_node *entry;
	struct request *rq;

	cq = &per_cpu(completion_queues, smp_processor_id());

	while ((entry = llist_del_first(&cq->list)) != NULL) {
		rq = llist_entry(entry, struct request, ll_list);
		null_complete_request(rq);
	}
}

static void null_softirq_done_fn(struct request *rq)
{
	blk_end_request_all(rq, 0);
}

static void null_request_end_ipi(struct request *rq)
{
	struct call_single_data *data = &rq->csd;
	int cpu = get_cpu();
	struct completion_queue *cq = &per_cpu(completion_queues, cpu);

	rq->ll_list.next = NULL;

	if (llist_add(&rq->ll_list, &cq->list)) {
		data->func = null_ipi_request_end_io;
		data->flags = 0;
		__smp_call_function_single(cpu, data, 0);
	}

	put_cpu();
}

static inline void null_handle_rq(struct blk_mq_hw_ctx *hctx,
				  struct request *rq)
{
	/* Complete IO by inline, softirq or timer */
	switch (irqmode) {
	case NULL_IRQ_NONE:
		null_complete_request(rq);
		break;
	case NULL_IRQ_SOFTIRQ:
		null_request_end_ipi(rq);
		break;
	case NULL_IRQ_TIMER:
		null_request_end_timer(rq);
		break;
	}
}

static void null_queue_bio(struct request_queue *q, struct bio *bio)
{
	switch (irqmode) {
	case NULL_IRQ_SOFTIRQ:
	case NULL_IRQ_NONE:
		bio_endio(bio, 0);
		break;
	case NULL_IRQ_TIMER:
		null_bio_end_timer(bio);
		break;
	}
}

static void null_request_fn(struct request_queue *q)
{
	struct request *rq;

	while ((rq = blk_fetch_request(q)) != NULL) {
		spin_unlock_irq(q->queue_lock);
		null_handle_rq(NULL, rq);
		spin_lock_irq(q->queue_lock);
	}
}

static int null_queue_rq(struct blk_mq_hw_ctx *hctx, struct request *rq)
{
	null_handle_rq(hctx, rq);
	return BLK_MQ_RQ_QUEUE_OK;
}

static struct blk_mq_hw_ctx *null_alloc_hctx(struct blk_mq_reg *reg, unsigned int hctx_index)
{
	return kmalloc_node(sizeof(struct blk_mq_hw_ctx),
				GFP_KERNEL | __GFP_ZERO, hctx_index % nr_online_nodes);
}

static void null_free_hctx(struct blk_mq_hw_ctx* hctx, unsigned int hctx_index)
{
	kfree(hctx);
}

/*
 * Map each per-cpu software queue to a per-node hardware queue
 */
struct blk_mq_hw_ctx *null_queue_map_per_node(struct request_queue *q,
					      const int ctx_index)
{
	return q->queue_hw_ctx[cpu_to_node(ctx_index)];
}

static struct blk_mq_hw_ctx *null_queue_map_per_cpu(struct request_queue *q,
					      const int ctx_index)
{
	return q->queue_hw_ctx[ctx_index];
}


static struct blk_mq_ops null_mq_ops = {
	.queue_rq       = null_queue_rq,
	.map_queue      = blk_mq_map_single_queue,
};

static struct blk_mq_reg null_mq_reg = {
	.ops		= &null_mq_ops,
	.queue_depth	= 64,
	.flags		= BLK_MQ_F_SHOULD_MERGE,
	.nr_hw_queues = 1,
};

static void null_del_dev(struct nullblk *nullblk)
{
	list_del_init(&nullblk->list);

	del_gendisk(nullblk->disk);
	if (queue_mode == NULL_Q_MQ)
		blk_mq_free_queue(nullblk->q);
	else
		blk_cleanup_queue(nullblk->q);
	put_disk(nullblk->disk);
	kfree(nullblk);
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
	struct nullblk *nullblk;
	sector_t size;

	nullblk = kmalloc_node(sizeof(*nullblk), GFP_KERNEL, home_node);
	if (!nullblk)
		return -ENOMEM;

	memset(nullblk, 0, sizeof(*nullblk));

	spin_lock_init(&nullblk->lock);

	if (queue_mode == NULL_Q_MQ) {
		null_mq_reg.numa_node = home_node;
		null_mq_reg.queue_depth = hw_queue_depth;

		if (hctx_mode == NULL_A_SINGLE) {
			null_mq_reg.ops->alloc_hctx = blk_mq_alloc_single_hw_queue;
			null_mq_reg.ops->free_hctx = blk_mq_free_single_hw_queue;

		} else {
			null_mq_reg.ops->alloc_hctx = null_alloc_hctx;
			null_mq_reg.ops->free_hctx = null_free_hctx;

			if (hctx_mode == NULL_A_PERNODE) {
				null_mq_reg.nr_hw_queues = nr_online_nodes;
				null_mq_reg.ops->map_queue = null_queue_map_per_node;
			} else if (hctx_mode == NULL_A_PERCPU) {
				null_mq_reg.nr_hw_queues = nr_cpu_ids;
				null_mq_reg.ops->map_queue = null_queue_map_per_cpu;
			}
		}

		nullblk->q = blk_mq_init_queue(&null_mq_reg);
	} else if (queue_mode == NULL_Q_BIO) {
		nullblk->q = blk_alloc_queue_node(GFP_KERNEL, home_node);
		blk_queue_make_request(nullblk->q, null_queue_bio);
	} else {
		nullblk->q = blk_init_queue_node(null_request_fn, &nullblk->lock, home_node);
		if (nullblk->q)
			blk_queue_softirq_done(nullblk->q, null_softirq_done_fn);
	}

	if (!nullblk->q) {
		kfree(nullblk);
		return -ENOMEM;
	}

	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, nullblk->q);

	disk = nullblk->disk = alloc_disk_node(1, home_node);
	if (!disk) {
		if (queue_mode == NULL_Q_MQ)
			blk_mq_free_queue(nullblk->q);
		else
			blk_cleanup_queue(nullblk->q);
		kfree(nullblk);
		return -ENOMEM;
	}

	mutex_lock(&lock);
	list_add_tail(&nullblk->list, &nullblk_list);
	nullblk->index = nullblk_indexes++;
	mutex_unlock(&lock);

	blk_queue_logical_block_size(nullblk->q, bs);
	blk_queue_physical_block_size(nullblk->q, bs);

	size = gb * 1024 * 1024 * 1024ULL;
	size /= (sector_t) bs;
	set_capacity(disk, size);

	disk->flags |= GENHD_FL_EXT_DEVT;
	spin_lock_init(&nullblk->lock);
	disk->major		= null_major;
	disk->first_minor	= nullblk->index;
	disk->fops		= &null_fops;
	disk->private_data	= nullblk;
	disk->queue		= nullblk->q;
	sprintf(disk->disk_name, "nullblk%d", nullblk->index);
	add_disk(disk);
	return 0;
}

static int __init null_init(void)
{
	unsigned int i;

	if (queue_mode == NULL_Q_BIO && irqmode == NULL_IRQ_SOFTIRQ) {
		pr_warn("null: bio and softirq completions do not work\n");
		pr_warn("null: defaulting to inline completions\n");
		irqmode = NULL_IRQ_NONE;
	}

	mutex_init(&lock);

	/* Initialize a separate list for each CPU for issuing softirqs */
	for_each_possible_cpu(i) {
		struct completion_queue *cq = &per_cpu(completion_queues, i);

		init_llist_head(&cq->list);

		if (irqmode != NULL_IRQ_TIMER)
			continue;

		hrtimer_init(&cq->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		if (queue_mode == NULL_Q_BIO)
			cq->timer.function = null_bio_timer_expired;
		else
			cq->timer.function = null_request_timer_expired;
	}

	null_major = register_blkdev(0, "nullblk");
	if (null_major < 0)
		return null_major;

	for (i = 0; i < nr_devices; i++) {
		if (null_add_dev()) {
			unregister_blkdev(null_major, "nullblk");
			return -EINVAL;
		}
	}

	pr_info("null_blk: module loaded\n");
	return 0;
}

static void __exit null_exit(void)
{
	struct nullblk *nullblk;

	unregister_blkdev(null_major, "nullblk");

	mutex_lock(&lock);
	while (!list_empty(&nullblk_list)) {
		nullblk = list_entry(nullblk_list.next, struct nullblk, list);
		null_del_dev(nullblk);
	}
	mutex_unlock(&lock);
}

module_init(null_init);
module_exit(null_exit);

MODULE_AUTHOR("Jens Axboe <jaxboe@fusionio.com>");
MODULE_LICENSE("GPL");
