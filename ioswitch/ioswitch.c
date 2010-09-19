/**
 * ioswitch.c
 */

#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/module.h>	/* Needed by all modules */
#include <linux/init.h>		/* Needed for the macros */
#include <linux/blkdev.h>
#include <linux/genhd.h>	/* struct hd_struct */
#include <linux/elevator.h>
#include <linux/kthread.h>	  /* kthread_run */
#include <linux/delay.h>	/* msleep */


#define DEV_PATH	"/dev/sda"
#define BOUNDARY	30


static struct task_struct *monitor = NULL;


int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *p = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *q = bdev_get_queue(bdev);
#endif

	unsigned long ave, spr, spw;
	unsigned long ior, iow;
	unsigned long long sr, sw;

	while (!kthread_should_stop()) {
		ior = part_stat_read(p, ios[READ]);
		sr = (unsigned long long)part_stat_read(p, sectors[READ]);
		iow = part_stat_read(p, ios[WRITE]);
		sw = (unsigned long long)part_stat_read(p, sectors[WRITE]);
		spr = sr / ior;
		spw = sw / iow;
		ave = (spr + spw) / 2;
#ifdef ELV_SWITCH
		if (ave > BOUNDARY) {
			if (elv_switch(q, "anticipatory") > 0)
				printk(KERN_INFO "elevator: switch to anticipatory\n");
		} else {
			if (elv_switch(q, "cfq") > 0)
				printk(KERN_INFO "elevator: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "sectors/read = %lu, sectors/write = %lu, ave = %lu\n", spr, spw, ave);
		msleep_interruptible(10000);
	}

	return 0;
}


int init_module(void)
{
	monitor = kthread_run(threadfn, NULL, "monitor");
	/*
	 * A non 0 return means init_module failed; module can't be loaded.
	 */
	return 0;
}


void cleanup_module(void)
{
	kthread_stop(monitor);
	printk(KERN_INFO "Unload ioswitch\n");
}


/*
 * Get rid of taint message by declaring code as GPL.
 */
MODULE_LICENSE("GPL");
