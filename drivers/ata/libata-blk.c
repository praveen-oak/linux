#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/libata.h>
#include <linux/hdreg.h>
#include <linux/uaccess.h>

#include "libata.h"




int ata_blk_add_port(struct ata_port *ap)
{
	int err;

	return 0;
}

void ata_blk_remove_port(struct ata_port *ap)
{

}

void ata_blk_scan_host(struct ata_port *ap, int sync)
{

}

int ata_blk_offline_dev(struct ata_device *dev)
{
	return 0;
}

void ata_blk_media_change_notify(struct ata_device *dev)
{

}

void ata_blk_hotplug(struct work_struct *work)
{

}

void ata_schedule_blk_eh(struct Scsi_Host *shost)
{

}

void ata_blk_dev_rescan(struct work_struct *work)
{

}
