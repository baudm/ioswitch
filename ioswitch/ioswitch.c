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


static struct task_struct *monitor = NULL;


int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *p = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *q = bdev_get_queue(bdev);
#endif

	unsigned long peak = 0;
	unsigned long ave, spr, spw, s_ave = 0, s_spr = 0, s_spw = 0;
	unsigned long c_ior, c_iow, p_ior = 0, p_iow = 0;
	unsigned long long c_sr, c_sw, p_sr = 0, p_sw = 0;

	while (!kthread_should_stop()) {
		c_ior = part_stat_read(p, ios[READ]);
		c_sr = (unsigned long long)part_stat_read(p, sectors[READ]);
		c_iow = part_stat_read(p, ios[WRITE]);
		c_sw = (unsigned long long)part_stat_read(p, sectors[WRITE]);
		spr = c_sr / c_ior;
		spw = c_sw / c_iow;
		ave = (spr + spw) / 2;
		if (c_ior != p_ior) {
			s_spr = (c_sr - p_sr) / (c_ior - p_ior);
			p_ior = c_ior;
			p_sr = c_sr;
		} else {
			s_spr = 0;
		}
		if (c_iow != p_iow) {
			s_spw = (c_sw - p_sw) / (c_iow - p_iow);
			p_iow = c_iow;
			p_sw = c_sw;
		} else {
			s_spw = 0;
		}
		s_ave = (s_spr + s_spw) / 2;
		if (s_ave > peak)
			peak = s_ave;
#ifdef ELV_SWITCH
		if (s_ave > peak / 2) {
			if (elv_switch(q, "anticipatory") > 0)
				printk(KERN_INFO "elevator: switch to anticipatory\n");
		} else {
			if (elv_switch(q, "cfq") > 0)
				printk(KERN_INFO "elevator: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "s/r = %lu, s/w = %lu, ave = %lu; "
			"60s: s/r = %lu, s/w = %lu, ave = %lu, peak = %lu\n",
			spr, spw, ave, s_spr, s_spw, s_ave, peak);
		msleep_interruptible(60000);
	}

	return 0;
}


int init_module(void)
{
	monitor = kthread_run(threadfn, NULL, "monitor");
	printk(KERN_INFO "ioswitch loaded\n");
	/*
	 * A non 0 return means init_module failed; module can't be loaded.
	 */
	return 0;
}


void cleanup_module(void)
{
	kthread_stop(monitor);
	printk(KERN_INFO "ioswitch unloaded\n");
}


/*
 * Get rid of taint message by declaring code as GPL.
 */
MODULE_LICENSE("GPL");
