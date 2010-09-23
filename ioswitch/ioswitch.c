/**
 * ioswitch.c
 *
 * Dynamic I/O scheduler switcher
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/elevator.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define DEV_PATH	"/dev/sda"
#define DECISION_PT	50		/* 50 percent */
#define SAMPLING_T	15000	/* 15-sec sampling period */
#define EXP_1_15	1595	/* 1/exp(15sec/1min) as fixed-point */

struct raw_stats {
	unsigned long rreq; /* read requests */
	unsigned long wreq; /* write requests */
	unsigned long long rsec; /* read sectors */
	unsigned long long wsec; /* write sectors */
};

static struct task_struct *monitor = NULL;

/**
 * Calculate the average request size. Note that the very first call to this
 * function would yield the "total" average while succeeding calls would yield
 * an average based on the current and previous samples.
 */
static unsigned long calc_req_sz(struct hd_struct *part)
{
	/* Storage for the samples */
	static struct raw_stats data[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
	/* Pointers to the current and previous samples, respectively */
	static struct raw_stats *c = &data[0], *p = &data[1];
	unsigned rreq_sz = 0, wreq_sz = 0;

	/* Read stats for disk */
	c->rreq = part_stat_read(part, ios[READ]);
	c->rsec = (unsigned long long)part_stat_read(part, sectors[READ]);
	c->wreq = part_stat_read(part, ios[WRITE]);
	c->wsec = (unsigned long long)part_stat_read(part, sectors[WRITE]);

	/* Compute the average read request size */
	if (c->rreq != p->rreq)
		rreq_sz = (c->rsec - p->rsec) / (c->rreq - p->rreq);

	/* Compute the average write request size */
	if (c->wreq != p->wreq)
		wreq_sz = (c->wsec - p->wsec) / (c->wreq - p->wreq);

	/* Swap pointers of current and previous samples */
	if (c == &data[0]) {
		c = &data[1];
		p = &data[0];
	} else {
		c = &data[0];
		p = &data[1];
	}

	/*
	 * Return the average request size as fixed-point. Note that the following
	 * is equivalent to multiplying by FIXED_1 and then dividing by 2.
	 */
	return (rreq_sz + wreq_sz) << (FSHIFT - 1);
}

static int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *p = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *q = bdev_get_queue(bdev);
#endif
	unsigned long cur_req_sz, ave_req_sz, peak_req_sz;

	/*
	 * Get the initial peak average request size. This value will be equal to
	 * the total sectors accessed / total requests.
	 */
	peak_req_sz = calc_req_sz(p);

	/*
	 * Set the initial average request size to be just below the decision point
	 * so that CFQ would be selected as the initial scheduler.
	 */
	ave_req_sz = (DECISION_PT * peak_req_sz) / 101;

	while (!kthread_should_stop()) {
		/* Get the average request size for the current interval */
		cur_req_sz = calc_req_sz(p);

		/* Get the exponential moving average for a 1-minute window */
		CALC_LOAD(ave_req_sz, EXP_1_15, cur_req_sz);

		/* Check if we have a new peak average request size */
		if (ave_req_sz > peak_req_sz)
			peak_req_sz = ave_req_sz;

#ifdef ELV_SWITCH
		if ((100 * ave_req_sz) / peak_req_sz > DECISION_PT) {
			if (elv_switch(q, "anticipatory") > 0)
				printk(KERN_INFO "ioswitch: switch to anticipatory\n");
		} else if (elv_switch(q, "cfq") > 0) {
			printk(KERN_INFO "ioswitch: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "ioswitch: cur = %lu, ave = %lu, peak = %lu\n",
				cur_req_sz >> FSHIFT, ave_req_sz >> FSHIFT, peak_req_sz >> FSHIFT);
		msleep_interruptible(SAMPLING_T);
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
	/* Interrupt msleep_interruptible() */
	force_sig(SIGUSR1, monitor);
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
