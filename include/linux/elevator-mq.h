#ifndef _LINUX_MQ_ELEVATOR_H
#define _LINUX_MQ_ELEVATOR_H

#ifdef CONFIG_BLOCK

#include <linux/elevator.h>

typedef int (elevator_mq_add_req_fn) (struct request_queue *, struct request *);

typedef int (elevator_mq_set_req_fn) (struct request_queue *, struct request *, struct bio *, gfp_t);
typedef void (elevator_mq_put_req_fn) (struct request_queue *, struct request *);

typedef int (elevator_mq_init_fn) (struct request_queue *);
typedef void (elevator_mq_exit_fn) (void);

struct elevator_mq_ops {
	elevator_mq_add_req_fn *elevator_mq_add_req_fn;

	elevator_mq_set_req_fn *elevator_mq_set_req_fn;
	elevator_mq_put_req_fn *elevator_mq_put_req_fn;

	elevator_mq_init_fn *elevator_mq_init_fn;
	elevator_mq_exit_fn *elevator_mq_exit_fn;
};

struct elevator_mq_type {
	struct elevator_mq_ops ops;
	char elevator_name[ELV_NAME_MAX];
	struct module *elevator_owner;
};

/*
 * mq block device elevator interface
 */

extern void elv_mq_add_request(struct request_queue *, struct request *);
extern int elv_mq_set_request(struct request_queue *, struct request *, struct bio *);
extern void elv_mq_put_request(struct request_queue *, struct request *);
extern int elv_mq_init(struct request_queue *rq);
extern void elv_mq_exit(void);

/*
 * Elevator interface 
 */

extern int elv_mq_register(struct elevator_mq_type *iosched);
extern void elv_mq_unregister(void);
#endif
#endif
