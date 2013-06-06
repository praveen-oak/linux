#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/libata.h>
#include <linux/hdreg.h>
#include <linux/uaccess.h>
#include <linux/blkdev.h>

#include "libata.h"

inline struct request_queue *ata_get_qc_request_queue(struct ata_queued_cmd *qc)
{
	struct request_queue *q;

	if (qc->request_queue)
		q = qc->request_queue;
	//else
	//	q = qc->scsicmd->device->request_queue;

	return q;
}

inline struct request *ata_get_qc_request(struct ata_queued_cmd *qc)
{
	struct request *rq;

	if (qc->request)
		rq = qc->request;

	return rq;
}

static void ata_blk_gen_ata_sense(struct ata_queued_cmd *qc)
{
	struct ata_device *dev = qc->dev;
	struct ata_taskfile *tf = &qc->result_tf;
	unsigned char *sb = &qc->sense_buffer;
	unsigned char *desc = sb + 8;
	int verbose = (qc->ap->ops->error_handler == NULL);
	u64 block;

	memset(sb, 0, ATA_SENSE_BUFFERSIZE);

	qc->result = (DRIVER_SENSE << 24) | SAM_STAT_CHECK_CONDITION;

	/* sense data is current and format is descriptor */
	sb[0] = 0x72;

	/* Use ata_to_sense_error() to map status register bits
	 * onto sense key, asc & ascq.
	 */
	if (qc->err_mask ||
	    tf->command & (ATA_BUSY | ATA_DF | ATA_ERR | ATA_DRQ)) {
		ata_to_sense_error(qc->ap->print_id, tf->command, tf->feature,
				   &sb[1], &sb[2], &sb[3], verbose);
		sb[1] &= 0x0f;
	}

	block = ata_tf_read_block(&qc->result_tf, dev);

	/* information sense data descriptor */
	sb[7] = 12;
	desc[0] = 0x00;
	desc[1] = 10;

	desc[2] |= 0x80;	/* valid */
	desc[6] = block >> 40;
	desc[7] = block >> 32;
	desc[8] = block >> 24;
	desc[9] = block >> 16;
	desc[10] = block >> 8;
	desc[11] = block;
}

static void ata_blk_gen_passthru_sense(struct ata_queued_cmd *qc)
{
	BUG();
}

void ata_blk_qc_prepare(struct ata_queued_cmd *qc)
{
	BUG();
}

void ata_blk_qc_complete(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	u8 *cdb = qc->cdb;
	int need_sense = (qc->err_mask != 0);

	/* For ATA pass thru (SAT) commands, generate a sense block if
	 * user mandated it or if there's an error.  Note that if we
	 * generate because the user forced us to [CK_COND =1], a check
	 * condition is generated and the ATA register values are returned
	 * whether the command completed successfully or not. If there
	 * was no error, we use the following sense data:
	 * sk = RECOVERED ERROR
	 * asc,ascq = ATA PASS-THROUGH INFORMATION AVAILABLE
	 */
	if (((cdb[0] == ATA_16) || (cdb[0] == ATA_12)) &&
	    ((cdb[2] & 0x20) || need_sense)) {
		ata_blk_gen_passthru_sense(qc);
	} else if (need_sense) {
		/* TODO: decide which descriptor format to use
		 * for 48b LBA devices and call that here
		 * instead of the fixed desc, which is only
		 * good for smaller LBA (and maybe CHS?)
		 * devices.
		 */
		printk("ata%u ata_blk_qc_complete3\n", qc->ap->print_id);
		ata_blk_gen_ata_sense(qc);
	}
	
	
	if (need_sense && !ap->ops->error_handler)
		ata_dump_status(ap->print_id, &qc->result_tf);

	qc->done_fn(qc);

	ata_qc_free(qc);
}

inline int ata_is_blk(struct ata_port *ap)
{
	return (ap->ops->blk_port_register != 0);
}



void ata_blk_scan_host(struct ata_port *ap, int sync)
{
	int tries = 5;
	struct ata_device *last_failed_dev = NULL;
	struct ata_link *link;
	struct ata_device *dev;

 repeat:
	ata_for_each_link(link, ap, EDGE) {
		ata_for_each_dev(dev, link, ENABLED) {
			
			if (dev->initialized)
				continue;

		}
	}

	/* If we scanned while EH was in progress or allocation
	 * failure occurred, scan would have failed silently.  Check
	 * whether all devices are attached.
	 */
	ata_for_each_link(link, ap, EDGE) {
		ata_for_each_dev(dev, link, ENABLED) {
			if (!dev->sdev)
				goto exit_loop;
		}
	}
 exit_loop:
	if (!link)
		return;

	/* we're missing some SCSI devices */
	if (sync) {
		/* If caller requested synchrnous scan && we've made
		 * any progress, sleep briefly and repeat.
		 */
		if (dev != last_failed_dev) {
			msleep(100);
			last_failed_dev = dev;
			goto repeat;
		}

		/* We might be failing to detect boot device, give it
		 * a few more chances.
		 */
		if (--tries) {
			msleep(100);
			goto repeat;
		}

		ata_port_err(ap,
			     "WARNING: synchronous ATA scan failed without making any progress, switching to async\n");
	}

	queue_delayed_work(system_long_wq, &ap->hotplug_task,
			   round_jiffies_relative(HZ));

}
