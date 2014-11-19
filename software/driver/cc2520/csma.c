#include <linux/types.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/random.h>
#include <linux/workqueue.h>

#include "csma.h"
#include "cc2520.h"
#include "lpl.h"
#include "sack.h"
#include "radio.h"
#include "debug.h"
#include "interface.h"

enum cc2520_csma_state_enum {
	CC2520_CSMA_IDLE,
	CC2520_CSMA_TX,
	CC2520_CSMA_CONG
};


static enum hrtimer_restart cc2520_csma_timer_cb(struct hrtimer *timer);
static void cc2520_csma_start_timer(int us_period, struct cc2520_dev *dev);
static int cc2520_csma_get_backoff(int min, int max);
static void cc2520_csma_wq(struct work_struct *work);

int cc2520_csma_init(struct cc2520_dev *dev)
{
	dev->backoff_min      = CC2520_DEF_MIN_BACKOFF;
	dev->backoff_max_init = CC2520_DEF_INIT_BACKOFF;
	dev->backoff_max_cong = CC2520_DEF_CONG_BACKOFF;
	dev->csma_enabled     = CC2520_DEF_CSMA_ENABLED;

	spin_lock_init(&dev->state_sl);
	dev->csma_state = CC2520_CSMA_IDLE;

	dev->wq = alloc_workqueue("csma_wq%d", WQ_HIGHPRI, 128, dev->id);
	if (!dev->wq) {
		goto error;
	}

	hrtimer_init(&dev->backoff_timer.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->backoff_timer.timer.function = &cc2520_csma_timer_cb;
	dev->work_s.dev = dev->backoff_timer.dev = dev;

	return 0;

	error:
		return -EFAULT;
}

void cc2520_csma_free(struct cc2520_dev *dev)
{
	if (dev->wq) {
		destroy_workqueue(dev->wq);
	}

	hrtimer_cancel(&dev->backoff_timer.timer);
}

static int cc2520_csma_get_backoff(int min, int max)
{
	uint rand_num;
	int span;

	span = max - min;
	get_random_bytes(&rand_num, 4);
	return min + (rand_num % span);
}

static void cc2520_csma_start_timer(int us_period, struct cc2520_dev *dev)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * us_period);
	hrtimer_start(&dev->backoff_timer.timer, kt, HRTIMER_MODE_REL);
}

static enum hrtimer_restart cc2520_csma_timer_cb(struct hrtimer *timer)
{
	ktime_t kt;
	int new_backoff;
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);
	struct cc2520_dev *dev = tmp->dev;

	if (cc2520_radio_is_clear(dev)) {
		// NOTE: We can absolutely not send from
		// interrupt context, there's a few places
		// where we spin lock and assume we can be
		// preempted. If we're running in atomic mode
		// that promise is broken. We use a work queue.

		// The workqueue adds about 30uS of latency.
		INIT_WORK(&dev->work_s.work, cc2520_csma_wq);
		queue_work(dev->wq, &dev->work_s.work);
		return HRTIMER_NORESTART;
	}
	else {
		spin_lock_irqsave(&dev->state_sl, dev->csma_flags);
		if (dev->csma_state == CC2520_CSMA_TX) {
			dev->csma_state = CC2520_CSMA_CONG;
			spin_unlock_irqrestore(&dev->state_sl, dev->csma_flags);

			new_backoff =
				cc2520_csma_get_backoff(dev->backoff_min, dev->backoff_max_cong);

			INFO(KERN_INFO, "radio%d channel still busy, waiting %d uS\n", dev->id, new_backoff);
			kt = ktime_set(0,1000 * new_backoff);
			hrtimer_forward_now(&dev->backoff_timer.timer, kt);
			return HRTIMER_RESTART;
		}
		else {
			dev->csma_state = CC2520_CSMA_IDLE;
			spin_unlock_irqrestore(&dev->state_sl, dev->csma_flags);

			cc2520_lpl_tx_done(-CC2520_TX_BUSY, dev);
			return HRTIMER_NORESTART;
		}
	}
}

static void cc2520_csma_wq(struct work_struct *work)
{
	struct wq_struct *tmp = container_of(work, struct wq_struct, work);
	struct cc2520_dev *dev = tmp->dev;
	cc2520_sack_tx(dev->csma_cur_tx_buf, dev->csma_cur_tx_len, dev);
}

int cc2520_csma_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	int backoff;

	if (!dev->csma_enabled) {
		return cc2520_sack_tx(buf, len, dev);
	}

	spin_lock_irqsave(&dev->state_sl, dev->csma_flags);
	if (dev->csma_state == CC2520_CSMA_IDLE) {
		dev->csma_state = CC2520_CSMA_TX;
		spin_unlock_irqrestore(&dev->state_sl, dev->csma_flags);

		memcpy(dev->csma_cur_tx_buf, buf, len);
		dev->csma_cur_tx_len = len;

		backoff = cc2520_csma_get_backoff(dev->backoff_min, dev->backoff_max_init);

		DBG(KERN_INFO, "radio%d waiting %d uS to send.\n", dev->id, backoff);
		cc2520_csma_start_timer(backoff, dev);
	}
	else {
		spin_unlock_irqrestore(&dev->state_sl, dev->csma_flags);
		DBG(KERN_INFO, "csma%d layer busy.\n", dev->id);
		cc2520_lpl_tx_done(-CC2520_TX_BUSY, dev);
	}

	return 0;
}

void cc2520_csma_tx_done(u8 status, struct cc2520_dev *dev)
{
	if (dev->csma_enabled) {
		spin_lock_irqsave(&dev->state_sl, dev->csma_flags);
		dev->csma_state = CC2520_CSMA_IDLE;
		spin_unlock_irqrestore(&dev->state_sl, dev->csma_flags);
	}

	cc2520_lpl_tx_done(status, dev);
}

void cc2520_csma_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	cc2520_lpl_rx_done(buf, len, dev);
}

void cc2520_csma_set_enabled(bool enabled, struct cc2520_dev *dev)
{
	dev->csma_enabled= enabled;
}

void cc2520_csma_set_min_backoff(int backoff, struct cc2520_dev *dev)
{
	dev->backoff_min = backoff;
}

void cc2520_csma_set_init_backoff(int backoff, struct cc2520_dev *dev)
{
	dev->backoff_max_init = backoff;
}

void cc2520_csma_set_cong_backoff(int backoff, struct cc2520_dev *dev)
{
	dev->backoff_max_cong = backoff;
}
