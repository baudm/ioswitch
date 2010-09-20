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
#define DECISION_PT	0.5
#define MAX_SAMPLES	5


struct raw_stats {
	unsigned long rreq; /* read requests */
	unsigned long wreq; /* write requests */
	unsigned long long rsec; /* read sectors */
	unsigned long long wsec; /* write sectors */
};


struct avg_stats {
	unsigned long rreq_sz; /* read sectors/requests */
	unsigned long wreq_sz; /* write sectors/requests */
	unsigned long req_sz; /* avg sectors/requests */
};


static struct task_struct *monitor = NULL;
/* circular buffer for stat samples */
static struct raw_stats samples[MAX_SAMPLES];
static struct raw_stats *cur = NULL, *old = NULL;
static short index = 0;


void init_buffer(void)
{
	short i;

	for (i = 0; i < MAX_SAMPLES; i++) {
		samples[i].rreq = 0;
		samples[i].wreq = 0;
		samples[i].rsec = 0;
		samples[i].wsec = 0;
	}
}


void read_stats(struct hd_struct *p)
{
	cur = &samples[index];
	cur->rreq = part_stat_read(p, ios[READ]);
	cur->rsec = (unsigned long long)part_stat_read(p, sectors[READ]);
	cur->wreq = part_stat_read(p, ios[WRITE]);
	cur->wsec = (unsigned long long)part_stat_read(p, sectors[WRITE]);
	/* get next index... */
	if (index < MAX_SAMPLES - 1)
		index++;
	else
		index = 0;
	/* and reference to the oldest sample */
	old = &samples[index];
}


void get_avg_stats(struct avg_stats *s, struct raw_stats *ref)
{
	if (ref) {
		if (cur->rreq != ref->rreq)
			s->rreq_sz = (cur->rsec - ref->rsec) / (cur->rreq - ref->rreq);
		else
			s->rreq_sz = 0;

		if (cur->wreq != ref->wreq)
			s->wreq_sz = (cur->wsec - ref->wsec) / (cur->wreq - ref->wreq);
		else
			s->wreq_sz = 0;
	} else {
		s->rreq_sz = cur->rsec / cur->rreq;
		s->wreq_sz = cur->wsec / cur->wreq;
	}

	s->req_sz = (s->rreq_sz + s->wreq_sz) / 2;
}


int threadfn(void *data)
{
	struct block_device *bdev = lookup_bdev(DEV_PATH);
	struct hd_struct *p = disk_get_part(bdev->bd_disk, 0);
#ifdef ELV_SWITCH
	struct request_queue *q = bdev_get_queue(bdev);
#endif
	struct avg_stats all, window;
	unsigned long peak_req_sz = 0;

	while (!kthread_should_stop()) {
		read_stats(p);
		get_avg_stats(&all, NULL);
		get_avg_stats(&window, old);
		if (window.req_sz > peak_req_sz)
			peak_req_sz = window.req_sz;
#ifdef ELV_SWITCH
		if ((float)window.req_sz / peak_req_sz > DECISION_PT) {
			if (elv_switch(q, "anticipatory") > 0)
				printk(KERN_INFO "elevator: switch to anticipatory\n");
		} else {
			if (elv_switch(q, "cfq") > 0)
				printk(KERN_INFO "elevator: switch to cfq\n");
		}
#endif
		printk(KERN_INFO "s/r = %lu, s/w = %lu, ave = %lu; "
			"5m: s/r = %lu, s/w = %lu, ave = %lu, peak = %lu\n",
			all.rreq_sz, all.wreq_sz, all.req_sz,
			window.rreq_sz, window.wreq_sz, window.req_sz, peak_req_sz);
		msleep_interruptible(60000);
	}

	return 0;
}


int init_module(void)
{
	init_buffer();
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
