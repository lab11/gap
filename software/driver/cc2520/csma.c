#include <linux/types.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/workqueue.h>

#include "csma.h"
#include "cc2520.h"
#include "radio.h"
#include "debug.h"
#include "interface.h"

struct cc2520_interface *csma_top[CC2520_NUM_DEVICES];
struct cc2520_interface *csma_bottom[CC2520_NUM_DEVICES];

static int backoff_min[CC2520_NUM_DEVICES];
static int backoff_max_init[CC2520_NUM_DEVICES];
static int backoff_max_cong[CC2520_NUM_DEVICES];
static bool csma_enabled[CC2520_NUM_DEVICES];

struct timer_struct{
	struct hrtimer timer;
	struct cc2520_dev *dev;
};

static struct timer_struct backoff_timer[CC2520_NUM_DEVICES];

static u8* cur_tx_buf[CC2520_NUM_DEVICES];
static u8 cur_tx_len[CC2520_NUM_DEVICES];

static spinlock_t state_sl[CC2520_NUM_DEVICES];

struct wq_struct{
	struct work_struct work;
	struct cc2520_dev *dev;
};

static struct workqueue_struct *wq[CC2520_NUM_DEVICES];
static struct wq_struct work_s[CC2520_NUM_DEVICES];

enum cc2520_csma_state_enum {
	CC2520_CSMA_IDLE,
	CC2520_CSMA_TX,
	CC2520_CSMA_CONG
};

static int csma_state[CC2520_NUM_DEVICES];

static unsigned long flags[CC2520_NUM_DEVICES];

static int cc2520_csma_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
static void cc2520_csma_tx_done(u8 status, struct cc2520_dev *dev);
static void cc2520_csma_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);
static enum hrtimer_restart cc2520_csma_timer_cb(struct hrtimer *timer);
static void cc2520_csma_start_timer(int us_period, int index);
static int cc2520_csma_get_backoff(int min, int max);
static void cc2520_csma_wq(struct work_struct *work);

int cc2520_csma_init()
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		csma_top[i]->tx = cc2520_csma_tx;
		csma_bottom[i]->tx_done = cc2520_csma_tx_done;
		csma_bottom[i]->rx_done = cc2520_csma_rx_done;

		backoff_min[i] = CC2520_DEF_MIN_BACKOFF;
		backoff_max_init[i] = CC2520_DEF_INIT_BACKOFF;
		backoff_max_cong[i] = CC2520_DEF_CONG_BACKOFF;
		csma_enabled[i] = CC2520_DEF_CSMA_ENABLED;

		spin_lock_init(&state_sl[i]);
		csma_state[i] = CC2520_CSMA_IDLE;

		cur_tx_buf[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!cur_tx_buf[i]) {
			goto error;
		}

		wq[i] = alloc_workqueue("csma_wq%d", WQ_HIGHPRI, 128, i);
		if (!wq[i]) {
			goto error;
		}

		hrtimer_init(&backoff_timer[i].timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		backoff_timer[i].timer.function = &cc2520_csma_timer_cb;
		// cc2520_devices is defined in interface.c
		// I saw no better alternative than using a global variable to
		// get access to the devices within the timer callback function
		// for cc2520_radio_is_clear() to have access to dev->cca.
		work_s[i].dev = backoff_timer[i].dev = &cc2520_devices[i];
	}

	return 0;

	error:
		for(i = 0; i < CC2520_NUM_DEVICES; ++i){
			if (cur_tx_buf[i]) {
				kfree(cur_tx_buf[i]);
				cur_tx_buf[i] = NULL;
			}

			if (wq[i]) {
				destroy_workqueue(wq[i]);
			}
		}

		return -EFAULT;
}

void cc2520_csma_free()
{
	int i;

	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		if (cur_tx_buf[i]) {
			kfree(cur_tx_buf[i]);
			cur_tx_buf[i] = NULL;
		}

		if (wq[i]) {
			destroy_workqueue(wq[i]);
		}

		hrtimer_cancel(&backoff_timer[i].timer);
	}
}

static int cc2520_csma_get_backoff(int min, int max)
{
	uint rand_num;
	int span;

	span = max - min;
	get_random_bytes(&rand_num, 4);
	return min + (rand_num % span);
}

static void cc2520_csma_start_timer(int us_period, int index)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * us_period);
	hrtimer_start(&backoff_timer[index].timer, kt, HRTIMER_MODE_REL);
}

static enum hrtimer_restart cc2520_csma_timer_cb(struct hrtimer *timer)
{
	ktime_t kt;
	int new_backoff;
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);
	struct cc2520_dev *dev = tmp->dev;
	int index = dev->id;

	if (cc2520_radio_is_clear(dev)) {
		// NOTE: We can absolutely not send from
		// interrupt context, there's a few places
		// where we spin lock and assume we can be
		// preempted. If we're running in atomic mode
		// that promise is broken. We use a work queue.

		// The workqueue adds about 30uS of latency.
		INIT_WORK(&work_s[index].work, cc2520_csma_wq);
		queue_work(wq[index], &work_s[index].work);
		return HRTIMER_NORESTART;
	}
	else {
		spin_lock_irqsave(&state_sl[index], flags[index]);
		if (csma_state[index] == CC2520_CSMA_TX) {
			csma_state[index] = CC2520_CSMA_CONG;
			spin_unlock_irqrestore(&state_sl[index], flags[index]);

			new_backoff =
				cc2520_csma_get_backoff(backoff_min[index], backoff_max_cong[index]);

			INFO(KERN_INFO, "radio%d channel still busy, waiting %d uS\n", index, new_backoff);
			kt = ktime_set(0,1000 * new_backoff);
			hrtimer_forward_now(&backoff_timer[index].timer, kt);
			return HRTIMER_RESTART;
		}
		else {
			csma_state[index] = CC2520_CSMA_IDLE;
			spin_unlock_irqrestore(&state_sl[index], flags[index]);

			csma_top[index]->tx_done(-CC2520_TX_BUSY, dev);
			return HRTIMER_NORESTART;
		}
	}
}

static void cc2520_csma_wq(struct work_struct *work)
{
	struct wq_struct *tmp = container_of(work, struct wq_struct, work);
	struct cc2520_dev *dev = tmp->dev;
	int index = dev->id;
	csma_bottom[index]->tx(cur_tx_buf[index], cur_tx_len[index], dev);
}

static int cc2520_csma_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	int backoff;
	int index = dev->id;

	if (!csma_enabled[index]) {
		return csma_bottom[index]->tx(buf, len, dev);
	}

	spin_lock_irqsave(&state_sl[index], flags[index]);
	if (csma_state[index] == CC2520_CSMA_IDLE) {
		csma_state[index] = CC2520_CSMA_TX;
		spin_unlock_irqrestore(&state_sl[index], flags[index]);

		memcpy(cur_tx_buf[index], buf, len);
		cur_tx_len[index] = len;

		backoff = cc2520_csma_get_backoff(backoff_min[index], backoff_max_init[index]);

		DBG(KERN_INFO, "radio%d waiting %d uS to send.\n", index, backoff);
		cc2520_csma_start_timer(backoff, index);
	}
	else {
		spin_unlock_irqrestore(&state_sl[index], flags[index]);
		DBG(KERN_INFO, "csma%d layer busy.\n", index);
		csma_top[index]->tx_done(-CC2520_TX_BUSY, dev);
	}

	return 0;
}

static void cc2520_csma_tx_done(u8 status, struct cc2520_dev *dev)
{
	int index = dev->id;

	if (csma_enabled[index]) {
		spin_lock_irqsave(&state_sl[index], flags[index]);
		csma_state[index] = CC2520_CSMA_IDLE;
		spin_unlock_irqrestore(&state_sl[index], flags[index]);
	}

	csma_top[index]->tx_done(status, dev);
}

static void cc2520_csma_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;

	csma_top[index]->rx_done(buf, len, dev);
}

void cc2520_csma_set_enabled(bool enabled, int index)
{
	csma_enabled[index]= enabled;
}

void cc2520_csma_set_min_backoff(int backoff, int index)
{
	backoff_min[index] = backoff;
}

void cc2520_csma_set_init_backoff(int backoff, int index)
{
	backoff_max_init[index] = backoff;
}

void cc2520_csma_set_cong_backoff(int backoff, int index)
{
	backoff_max_cong[index] = backoff;
}
