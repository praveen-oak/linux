/*
 * Multi-queue block device elevator/IO-scheduler.
 */

#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/elevator-mq.h>

#include "blk.h"

struct elevator_mq_type *current_mq_elevator;

void elv_mq_add_request(struct request_queue *q, struct request *rq)
{
	current_mq_elevator->ops.elevator_mq_add_req_fn(q, rq);
}
EXPORT_SYMBOL(elv_mq_add_request);

int elv_mq_set_request(struct request_queue *q, struct request *rq, struct bio *bio)
{
	current_mq_elevator->ops.elevator_mq_set_req_fn(q, rq, bio, GFP_KERNEL);
	return 0;
}
EXPORT_SYMBOL(elv_mq_set_request);

void elv_mq_put_request(struct request_queue *q, struct request *rq)
{
	current_mq_elevator->ops.elevator_mq_put_req_fn(q, rq);
}
EXPORT_SYMBOL(elv_mq_put_request);

int elv_mq_init(struct request_queue *rq)
{
	return current_mq_elevator->ops.elevator_mq_init_fn(rq);
}
EXPORT_SYMBOL(elv_mq_init);

void elv_mq_exit(void)
{
	current_mq_elevator->ops.elevator_mq_exit_fn();
}
EXPORT_SYMBOL(elv_mq_exit);

int elv_mq_register(struct elevator_mq_type *iosched)
{
	current_mq_elevator = iosched;
	return 0;
}
EXPORT_SYMBOL(elv_mq_register);

void elv_mq_unregister(void)
{
}
EXPORT_SYMBOL(elv_mq_unregister);
