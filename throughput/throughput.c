/**
 * throughput.c
 *
 * throughput measuring module
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define DEV_PATH	"/dev/sda"
#define SAMPLING_T	15000	/* 15-sec sampling period */

struct raw_stats {
	unsigned long long sec[2]; /* sectors */
};

static struct task_struct *throughput = NULL;

/**
 * Get statistics for the interval. Note that the very first call to this
 * function would yield the "total" average while succeeding calls would yield
 * an average based on the current and previous samples.
 */
static void get_stats(struct raw_stats *a, struct hd_struct *part)
{
	/* Storage for the samples */
	static struct raw_stats data[2] = {{{0, 0}}, {{0, 0}}};
	/* Pointers to the current and previous samples, respectively */
	static struct raw_stats *c = &data[0], *p = &data[1];

	/* Read current stats for disk */
	c->sec[READ] = (unsigned long long)part_stat_read(part, sectors[READ]);
	c->sec[WRITE] = (unsigned long long)part_stat_read(part, sectors[WRITE]);

	a->sec[READ] = c->sec[READ] - p->sec[READ];
	a->sec[WRITE] = c->sec[WRITE] - p->sec[WRITE];

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
	struct raw_stats cur; /* Current and average stats data */

	get_stats(&cur, part);

	/* Loop until kthread_stop() is called */
	while (!kthread_should_stop()) {
		msleep_interruptible(SAMPLING_T);
		/* Get the average stats for the current interval */
		get_stats(&cur, part);

		printk(KERN_INFO "TMM: BWr=%llu BWw=%llu",
				cur.sec[READ]/15, cur.sec[WRITE]/15);

	}

	return 0;
}

static int __init throughput_init(void)
{
	throughput = kthread_run(threadfn, NULL, "TMM");
	printk(KERN_INFO "TMM loaded\n");

	return 0;
}

static void __exit throughput_exit(void)
{
	/* Interrupt msleep_interruptible() */
	force_sig(SIGUSR1, throughput);
	kthread_stop(throughput);
	printk(KERN_INFO "TMM unloaded\n");
}

module_init(throughput_init);
module_exit(throughput_exit);
/*
 * Get rid of taint message by declaring code as GPL.
 */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Throughput Measuring Module");
