#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>

#include "lpl.h"
#include "packet.h"
#include "cc2520.h"
#include "csma.h"
#include "unique.h"
#include "debug.h"

static enum hrtimer_restart cc2520_lpl_timer_cb(struct hrtimer *timer);
static void cc2520_lpl_start_timer(struct cc2520_dev *dev);

// static int lpl_window[CC2520_NUM_DEVICES];
// static int lpl_interval[CC2520_NUM_DEVICES];
// static bool lpl_enabled[CC2520_NUM_DEVICES];

// struct timer_struct {
// 	struct hrtimer timer;
// 	int index;
// };

// static struct timer_struct lpl_timer[CC2520_NUM_DEVICES];

// static u8* lpl_cur_tx_buf[CC2520_NUM_DEVICES];
// static u8 lpl_cur_tx_len[CC2520_NUM_DEVICES];

// static spinlock_t lpl_state_sl[CC2520_NUM_DEVICES];

// static unsigned long flags[CC2520_NUM_DEVICES];

enum cc2520_lpl_state_enum {
	CC2520_LPL_IDLE,
	CC2520_LPL_TX,
	CC2520_LPL_TIMER_EXPIRED
};

// static int lpl_state[CC2520_NUM_DEVICES];

int cc2520_lpl_init(struct cc2520_dev *dev)
{
	// int i;
	// for(i = 0; i < CC2520_NUM_DEVICES; ++i){

		dev->lpl_window   = CC2520_DEF_LPL_LISTEN_WINDOW;
		dev->lpl_interval = CC2520_DEF_LPL_WAKEUP_INTERVAL;
		dev->lpl_enabled  = CC2520_DEF_LPL_ENABLED;

		// lpl_cur_tx_buf[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		// if (!lpl_cur_tx_buf[i]) {
		// 	goto error;
		// }

		spin_lock_init(&dev->lpl_state_sl);
		dev->lpl_state = CC2520_LPL_IDLE;

		hrtimer_init(&dev->lpl_timer.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		dev->lpl_timer.timer.function = &cc2520_lpl_timer_cb;
		dev->lpl_timer.dev = dev;
	//}
//
	return 0;

	// error:
	// 	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
	// 		if (lpl_cur_tx_buf[i]) {
	// 			kfree(lpl_cur_tx_buf[i]);
	// 			lpl_cur_tx_buf[i] = NULL;
	// 		}
	// 	}

	// 	return -EFAULT;
}

void cc2520_lpl_free(struct cc2520_dev *dev)
{
	// int i;
	// for(i = 0; i < CC2520_NUM_DEVICES; ++i){
	// 	if (lpl_cur_tx_buf[i]) {
	// 		kfree(lpl_cur_tx_buf[i]);
	// 		lpl_cur_tx_buf[i] = NULL;
	// 	}

		hrtimer_cancel(&dev->lpl_timer.timer);
	// }
}

int cc2520_lpl_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	if (dev->lpl_enabled) {
		spin_lock_irqsave(&dev->lpl_state_sl, dev->lpl_flags);
		if (dev->lpl_state == CC2520_LPL_IDLE) {
			dev->lpl_state = CC2520_LPL_TX;
			spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);

			memcpy(dev->lpl_cur_tx_buf, buf, len);
			dev->lpl_cur_tx_len = len;

			cc2520_csma_tx(dev->lpl_cur_tx_buf, dev->lpl_cur_tx_len, dev);
			cc2520_lpl_start_timer(dev);
		}
		else {
			spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);
			INFO(KERN_INFO, "lpl%d tx busy.\n", dev->id);
			cc2520_unique_tx_done(-CC2520_TX_BUSY, dev);
		}

		return 0;
	}
	else {
		return cc2520_csma_tx(buf, len, dev);
	}
}

void cc2520_lpl_tx_done(u8 status, struct cc2520_dev *dev)
{
	if (dev->lpl_enabled) {
		spin_lock_irqsave(&dev->lpl_state_sl, dev->lpl_flags);
		if (cc2520_packet_requires_ack_wait(dev->lpl_cur_tx_buf)) {
			if (status == CC2520_TX_SUCCESS) {
				dev->lpl_state = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);

				hrtimer_cancel(&dev->lpl_timer.timer);
				cc2520_unique_tx_done(status, dev);
			}
			else if (dev->lpl_state == CC2520_LPL_TIMER_EXPIRED) {
				dev->lpl_state = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);
				cc2520_unique_tx_done(-CC2520_TX_FAILED, dev);
			}
			else {
				spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);
				DBG(KERN_INFO, "lpl%d retransmit.\n", dev->id);
				cc2520_csma_tx(dev->lpl_cur_tx_buf, dev->lpl_cur_tx_len, dev);
			}
		}
		else {
			if (dev->lpl_state == CC2520_LPL_TIMER_EXPIRED) {
				dev->lpl_state = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);
				cc2520_unique_tx_done(CC2520_TX_SUCCESS, dev);
			}
			else {
				spin_unlock_irqrestore(&dev->lpl_state_sl, dev->lpl_flags);
				cc2520_csma_tx(dev->lpl_cur_tx_buf, dev->lpl_cur_tx_len, dev);
			}
		}
	}
	else {
		cc2520_unique_tx_done(status, dev);
	}
	// if packet requires ack, examine status.
	//    if success terminate LPL window
	//    else if status != TIMER_EXPIRED resend
	// else resend
}

void cc2520_lpl_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	cc2520_unique_rx_done(buf, len, dev);
}

static void cc2520_lpl_start_timer(struct cc2520_dev *dev)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * (dev->lpl_interval + 2 * dev->lpl_window));
	hrtimer_start(&dev->lpl_timer.timer, kt, HRTIMER_MODE_REL);
}

static enum hrtimer_restart cc2520_lpl_timer_cb(struct hrtimer *timer)
{
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);

	spin_lock_irqsave(&tmp->dev->lpl_state_sl, tmp->dev->lpl_flags);
	if (tmp->dev->lpl_state == CC2520_LPL_TX) {
		tmp->dev->lpl_state = CC2520_LPL_TIMER_EXPIRED;
		spin_unlock_irqrestore(&tmp->dev->lpl_state_sl, tmp->dev->lpl_flags);
	}
	else {
		spin_unlock_irqrestore(&tmp->dev->lpl_state_sl, tmp->dev->lpl_flags);
		INFO(KERN_INFO, "lpl%d timer in improbable state.\n", tmp->dev->id);
	}

	return HRTIMER_NORESTART;
}

void cc2520_lpl_set_enabled(bool enabled, struct cc2520_dev *dev)
{
	dev->lpl_enabled = enabled;
}

void cc2520_lpl_set_listen_length(int length, struct cc2520_dev *dev)
{
	dev->lpl_window = length;
}

void cc2520_lpl_set_wakeup_interval(int interval, struct cc2520_dev *dev)
{
	dev->lpl_interval = interval;
}
