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
#include <linux/sched.h>	/* calc_load variables */

#define DEV_PATH	"/dev/sda"
#define DECISION_PT	0.5

struct raw_stats {
	unsigned long rreq; /* read requests */
	unsigned long wreq; /* write requests */
	unsigned long long rsec; /* read sectors */
	unsigned long long wsec; /* write sectors */
};

static struct task_struct *monitor = NULL;

/**
 * copied from linux-2.6.32/kernel/sched.c
 */
static unsigned long
calc_load(unsigned long load, unsigned long exp, unsigned long active)
{
	load *= exp;
	load += active * (FIXED_1 - exp);
	return load >> FSHIFT;
}

static unsigned long calc_req_sz(struct hd_struct *part)
{
	static struct raw_stats data[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
	static struct raw_stats *c = &data[0], *p = &data[1];
	unsigned long rreq_sz = 0, wreq_sz = 0;

	/* read stats from disk */
	c->rreq = part_stat_read(part, ios[READ]);
	c->rsec = (unsigned long long)part_stat_read(part, sectors[READ]);
	c->wreq = part_stat_read(part, ios[WRITE]);
	c->wsec = (unsigned long long)part_stat_read(part, sectors[WRITE]);

	/* compute ave read request size */
	if (c->rreq != p->rreq)
		rreq_sz = (c->rsec - p->rsec) / (c->rreq - p->rreq);

	/* compute ave write request size */
	if (c->wreq != p->wreq)
		wreq_sz = (c->wsec - p->wsec) / (c->wreq - p->wreq);

	/* swap pointers of current and previous samples */
	if (c == &data[0]) {
		c = &data[1];
		p = &data[0];
	} else {
		c = &data[0];
		p = &data[1];
	}

	return (rreq_sz + wreq_sz) / 2;
}

static int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *p = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *q = bdev_get_queue(bdev);
#endif
	unsigned long cur_req_sz, ave_req_sz, peak_req_sz = 0;

	/* get initial sample and average */
	ave_req_sz = calc_req_sz(p);

	while (!kthread_should_stop()) {
		/* get average req size for current interval */
		cur_req_sz = calc_req_sz(p);
		/* get the exponential moving average */
		ave_req_sz = calc_load(ave_req_sz, EXP_5, cur_req_sz);
		/* check if we have a new peak average req size */
		if (ave_req_sz > peak_req_sz)
			peak_req_sz = ave_req_sz;
#ifdef ELV_SWITCH
		if ((float)ave_req_sz / peak_req_sz > DECISION_PT) {
			if (elv_switch(q, "anticipatory") > 0)
				printk(KERN_INFO "elevator: switch to anticipatory\n");
		} else {
			if (elv_switch(q, "cfq") > 0)
				printk(KERN_INFO "elevator: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "cur = %lu, ave = %lu, peak = %lu\n",
				cur_req_sz, ave_req_sz, peak_req_sz);
		msleep_interruptible(30000);
	}

	return 0;
}

static int __init ioswitch_init(void)
{
	monitor = kthread_run(threadfn, NULL, "monitor");
	printk(KERN_INFO "ioswitch loaded\n");

	return 0;
}

static void __exit ioswitch_exit(void)
{
	kthread_stop(monitor);
	printk(KERN_INFO "ioswitch unloaded\n");
}

module_init(ioswitch_init);
module_exit(ioswitch_exit);
/*
 * Get rid of taint message by declaring code as GPL.
 */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dynamic IO scheduler switcher");
