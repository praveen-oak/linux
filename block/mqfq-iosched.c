/*
 * Fairness queueing for multi-queue block devices (MQFQ)
 *
 * Based on ideas from CFQ and Jens Axboe.
 *
 * Scheduler is based on characteristics of Solid State Drives. We assume the following:
 *   - Reads are faster than writes.
 *   - Write order is not important. The physical devices usually a log-based approach to writes.
 *   - High queue depth is better.
 *   - Write bursts is served faster, while devices that are written to for longer periods
 *     finds a steady state. Usually at lower write throughput. 
 *
 * 
 */
#include <linux/module.h>
#include <linux/llist.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/elevator-mq.h>


struct mqfq_queue {
	
	/* list of outstanding reads */
	struct llist_head	lreads;
	atomic_t		lreads_size;

	/* list of outstanding writes */
	struct llist_head	lwrites;
	atomic_t		lwrites_size;

	pid_t pid;
};

static int mqfq_elevator_add_request(struct request_queue *q, struct request *rq)
{
	return 0;
}

static int mqfq_elevator_set_request(struct request_queue *q, struct request *rq, struct bio *bio, gfp_t gfp)
{
	return 0;
}

static void mqfq_elevator_put_request(struct request_queue *q, struct request *rq)
{

}

static int mqfq_elevator_init(struct request_queue *q)
{
	return 0;
}

static void mqfq_elevator_exit(void)
{
}	

static struct elevator_mq_type iosched_mqfq = {
	.ops = {
		.elevator_mq_add_req_fn = mqfq_elevator_add_request,
    
		.elevator_mq_set_req_fn = mqfq_elevator_set_request,
		.elevator_mq_put_req_fn = mqfq_elevator_put_request,

		.elevator_mq_init_fn = mqfq_elevator_init,
		.elevator_mq_exit_fn = mqfq_elevator_exit,
	},
	.elevator_name = "MQFQ",
	.elevator_owner = THIS_MODULE,
};

static int __init mqfq_init(void)
{
	return elv_mq_register(&iosched_mqfq);
}

static void __exit mqfq_exit(void)
{
	elv_mq_unregister();
}

module_init(mqfq_init);
module_exit(mqfq_exit);

MODULE_AUTHOR("Matias Bjorling");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multi-queue Fairness Queueing IO Scheduler"); 
