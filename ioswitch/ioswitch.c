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
static void calc_req_sz(unsigned long req_sz[], struct hd_struct *part)
{
	/* Storage for the samples */
	static struct raw_stats data[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
	/* Pointers to the current and previous samples, respectively */
	static struct raw_stats *c = &data[0], *p = &data[1];

	/* Read stats for disk */
	c->rreq = part_stat_read(part, ios[READ]);
	c->rsec = (unsigned long long)part_stat_read(part, sectors[READ]);
	c->wreq = part_stat_read(part, ios[WRITE]);
	c->wsec = (unsigned long long)part_stat_read(part, sectors[WRITE]);

	/* Compute the average read request size as fixed-point */
	if (c->rreq != p->rreq)
		req_sz[READ] = FIXED_1 * (c->rsec - p->rsec) / (c->rreq - p->rreq);
	else
		req_sz[READ] = 0;

	/* Compute the average write request size as fixed-point */
	if (c->wreq != p->wreq)
		req_sz[WRITE] = FIXED_1 * (c->wsec - p->wsec) / (c->wreq - p->wreq);
	else
		req_sz[WRITE] = 0;

	/* Swap pointers of current and previous samples */
	if (c == &data[0]) {
		c = &data[1];
		p = &data[0];
	} else {
		c = &data[0];
		p = &data[1];
	}
}

static int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *part = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *queue = bdev_get_queue(bdev);
#endif
	/* Current, average, and peak average request sizes for reads and writes */
	unsigned long cur_req_sz[2], ave_req_sz[2], peak_req_sz[2];

	/*
	 * Get the initial peak average request size. This value will be equal to
	 * the total sectors accessed / total requests.
	 */
	calc_req_sz(peak_req_sz, part);

	/*
	 * Set the initial average request size to be just below the decision point
	 * so that CFQ would be selected as the initial scheduler.
	 */
	ave_req_sz[READ] = (DECISION_PT * peak_req_sz[READ]) / 101;
	ave_req_sz[WRITE] = (DECISION_PT * peak_req_sz[WRITE]) / 101;

	while (!kthread_should_stop()) {
		/* Get the average request size for the current interval */
		calc_req_sz(cur_req_sz, part);

		/* Get the exponential moving average for a 1-minute window */
		CALC_LOAD(ave_req_sz[READ], EXP_1_15, cur_req_sz[READ]);
		CALC_LOAD(ave_req_sz[WRITE], EXP_1_15, cur_req_sz[WRITE]);

		/* Check if we have a new peak read request size */
		if (ave_req_sz[READ] > peak_req_sz[READ])
			peak_req_sz[READ] = ave_req_sz[READ];

		/* Check if we have a new peak write request size */
		if (ave_req_sz[WRITE] > peak_req_sz[WRITE])
			peak_req_sz[WRITE] = ave_req_sz[WRITE];

#ifdef ELV_SWITCH
		if ((100 * ave_req_sz[READ]) / peak_req_sz[READ] > DECISION_PT) {
			if (elv_switch(queue, "anticipatory") > 0)
				printk(KERN_INFO "ioswitch: switch to anticipatory\n");
		} else if (elv_switch(queue, "cfq") > 0) {
			printk(KERN_INFO "ioswitch: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "ioswitch: r_cur = %lu, r_ave = %lu, r_peak = %lu, w_cur = %lu, w_ave = %lu, w_peak = %lu\n",
				cur_req_sz[READ] >> FSHIFT, ave_req_sz[READ] >> FSHIFT, peak_req_sz[READ] >> FSHIFT,
				cur_req_sz[WRITE] >> FSHIFT, ave_req_sz[WRITE] >> FSHIFT, peak_req_sz[WRITE] >> FSHIFT);
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
