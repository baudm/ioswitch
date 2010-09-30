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
#undef EXP_1
#define EXP_1		1595	/* 1/exp(15sec/1min) as fixed-point */

#define READ	0x01
#define SEQ	0X10

struct raw_stats {
	unsigned long req[2]; /* I/O requests */
	unsigned long long sec[2]; /* sectors */
};

struct ave_stats {
	unsigned long req[2]; /* I/O requests */
	unsigned long req_sz[2]; /* sectors/requests */
};

static struct task_struct *monitor = NULL;

/**
 * Calculate the ave request size. Note that the very first call to this
 * function would yield the "total" ave while succeeding calls would yield
 * an ave based on the cur and previous samples.
 */
static void get_stats(struct ave_stats *a, struct hd_struct *part)
{
	/* Storage for the samples */
	static struct raw_stats data[2] = {{{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
	/* Pointers to the cur and previous samples, respectively */
	static struct raw_stats *c = &data[0], *p = &data[1];

	/* Read cur stats for disk */
	c->req[READ] = part_stat_read(part, ios[READ]);
	c->sec[READ] = (unsigned long long)part_stat_read(part, sectors[READ]);
	c->req[WRITE] = part_stat_read(part, ios[WRITE]);
	c->sec[WRITE] = (unsigned long long)part_stat_read(part, sectors[WRITE]);

	/* Get I/O requests for cur interval */
	a->req[READ] = c->req[READ] - p->req[READ];
	a->req[WRITE] = c->req[WRITE] - p->req[WRITE];

	/* Compute the ave read request size as fixed-point */
	if (a->req[READ])
		a->req_sz[READ] = FIXED_1 * (c->sec[READ] - p->sec[READ]) / a->req[READ];
	else
		a->req_sz[READ] = 0;

	/* Compute the ave write request size as fixed-point */
	if (a->req[WRITE])
		a->req_sz[WRITE] = FIXED_1 * (c->sec[WRITE] - p->sec[WRITE]) / a->req[WRITE];
	else
		a->req_sz[WRITE] = 0;

	/* Convert to fixed-point */
	a->req[READ] *= FIXED_1;
	a->req[WRITE] *= FIXED_1;

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
	/* Current, ave, and peak ave request sizes for reads and writes */
	struct ave_stats cur, ave, peak_ave;
	unsigned short workload = 0x00; /* bit 0: read/write, bit 1: seq/rand */

	/*
	 * Get the initial peak ave request size. This value will be equal to
	 * the total sectors accessed / total requests.
	 */
	get_stats(&peak_ave, part);

	/*
	 * Set the initial ave request size to be just below the decision point
	 * so that CFQ would be selected as the initial scheduler.
	 */
	ave.req_sz[READ] = (DECISION_PT * peak_ave.req_sz[READ]) / 101;
	ave.req_sz[WRITE] = (DECISION_PT * peak_ave.req_sz[WRITE]) / 101;

	/* Get the ave requests for the cur interval */
	get_stats(&cur, part);
	ave.req[READ] = cur.req[READ];
	ave.req[WRITE] = cur.req[WRITE];

	while (!kthread_should_stop()) {
		/* Get the ave request size for the cur interval */
		get_stats(&cur, part);

		/* Get the exponential moving ave for a 1-minute window */
		CALC_LOAD(ave.req[READ], EXP_1, cur.req[READ]);
		CALC_LOAD(ave.req_sz[READ], EXP_1, cur.req_sz[READ]);
		CALC_LOAD(ave.req[WRITE], EXP_1, cur.req[WRITE]);
		CALC_LOAD(ave.req_sz[WRITE], EXP_1, cur.req_sz[WRITE]);

		/* Check if we have a new peak read request size */
		if (ave.req_sz[READ] > peak_ave.req_sz[READ])
			peak_ave.req_sz[READ] = ave.req_sz[READ];

		/* Check if we have a new peak write request size */
		if (ave.req_sz[WRITE] > peak_ave.req_sz[WRITE])
			peak_ave.req_sz[WRITE] = ave.req_sz[WRITE];

		if (ave.req[READ] > ave.req[WRITE])
			workload |= READ; // read
		else
			workload &= ~READ; // write

		if ((100 * ave.req_sz[READ]) / peak_ave.req_sz[READ] > DECISION_PT)
			workload |= SEQ; // sequential
		else
			workload &= ~SEQ; // random

		printk(KERN_INFO "%x", workload);
		switch (workload) {
		case 0x00:
			printk(KERN_INFO "ioswitch: random write\n");
			break;
		case 0x01:
			printk(KERN_INFO "ioswitch: random read");
			break;
		case 0x10:
			printk(KERN_INFO "ioswitch: sequential write\n");
			break;
		case 0x11:
			printk(KERN_INFO "ioswitch: sequential read\n");
		}
/*
#ifdef ELV_SWITCH
		if ((100 * ave.req_sz[READ]) / peak_ave.req_sz[READ] > DECISION_PT) {
			if (elv_switch(queue, "anticipatory") > 0)
				printk(KERN_INFO "ioswitch: switch to anticipatory\n");
		} else if (elv_switch(queue, "cfq") > 0) {
			printk(KERN_INFO "ioswitch: switch to cfq\n");
		}
#endif
*/
		printk(KERN_INFO "ioswitch: read: cur = %lu, ave = %lu, peak = %lu; write: cur = %lu, ave = %lu, peak = %lu\n",
				cur.req_sz[READ] >> FSHIFT, ave.req_sz[READ] >> FSHIFT, peak_ave.req_sz[READ] >> FSHIFT,
				cur.req_sz[WRITE] >> FSHIFT, ave.req_sz[WRITE] >> FSHIFT, peak_ave.req_sz[WRITE] >> FSHIFT);
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
