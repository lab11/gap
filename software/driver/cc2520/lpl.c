#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>

#include "lpl.h"
#include "packet.h"
#include "cc2520.h"
#include "debug.h"

static enum hrtimer_restart cc2520_lpl_timer_cb(struct hrtimer *timer);
static void cc2520_lpl_start_timer(int index);

// static int lpl_window[CC2520_NUM_DEVICES];
// static int lpl_interval[CC2520_NUM_DEVICES];
// static bool lpl_enabled[CC2520_NUM_DEVICES];

struct timer_struct{
	struct hrtimer timer;
	int index;
};

// static struct timer_struct lpl_timer[CC2520_NUM_DEVICES];

// static u8* cur_tx_buf[CC2520_NUM_DEVICES];
// static u8 cur_tx_len[CC2520_NUM_DEVICES];

// static spinlock_t state_sl[CC2520_NUM_DEVICES];

// static unsigned long flags[CC2520_NUM_DEVICES];

enum cc2520_lpl_state_enum {
	CC2520_LPL_IDLE,
	CC2520_LPL_TX,
	CC2520_LPL_TIMER_EXPIRED
};

// static int lpl_state[CC2520_NUM_DEVICES];

int cc2520_lpl_init()
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){

		lpl_window[i] = CC2520_DEF_LPL_LISTEN_WINDOW;
		lpl_interval[i] = CC2520_DEF_LPL_WAKEUP_INTERVAL;
		lpl_enabled[i] = CC2520_DEF_LPL_ENABLED;

		// cur_tx_buf[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		// if (!cur_tx_buf[i]) {
		// 	goto error;
		// }

		spin_lock_init(&state_sl[i]);
		lpl_state[i] = CC2520_LPL_IDLE;

		hrtimer_init(&lpl_timer[i].timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		lpl_timer[i].timer.function = &cc2520_lpl_timer_cb;
		lpl_timer[i].index = i;
	}

	return 0;

	error:
		for(i = 0; i < CC2520_NUM_DEVICES; ++i){
			if (cur_tx_buf[i]) {
				kfree(cur_tx_buf[i]);
				cur_tx_buf[i] = NULL;
			}
		}

		return -EFAULT;
}

void cc2520_lpl_free()
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		if (cur_tx_buf[i]) {
			kfree(cur_tx_buf[i]);
			cur_tx_buf[i] = NULL;
		}

		hrtimer_cancel(&lpl_timer[i].timer);
	}
}

static int cc2520_lpl_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;

	if (lpl_enabled[index]) {
		spin_lock_irqsave(&state_sl[index], flags[index]);
		if (lpl_state[index] == CC2520_LPL_IDLE) {
			lpl_state[index] = CC2520_LPL_TX;
			spin_unlock_irqrestore(&state_sl[index], flags[index]);

			memcpy(cur_tx_buf[index], buf, len);
			cur_tx_len[index] = len;

			cc2520_csma_tx(cur_tx_buf[index], cur_tx_len[index], dev);
			cc2520_lpl_start_timer(index);
		}
		else {
			spin_unlock_irqrestore(&state_sl[index], flags[index]);
			INFO(KERN_INFO, "lpl%d tx busy.\n", index);
			cc2520_unique_tx_done(-CC2520_TX_BUSY, dev);
		}

		return 0;
	}
	else {
		return cc2520_csma_tx(buf, len, dev);
	}
}

static void cc2520_lpl_tx_done(u8 status, struct cc2520_dev *dev)
{
	int index = dev->id;

	if (lpl_enabled[index]) {
		spin_lock_irqsave(&state_sl[index], flags[index]);
		if (cc2520_packet_requires_ack_wait(cur_tx_buf[index])) {
			if (status == CC2520_TX_SUCCESS) {
				lpl_state[index] = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&state_sl[index], flags[index]);

				hrtimer_cancel(&lpl_timer[index].timer);
				cc2520_unique_tx_done(status, dev);
			}
			else if (lpl_state[index] == CC2520_LPL_TIMER_EXPIRED) {
				lpl_state[index] = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&state_sl[index], flags[index]);
				cc2520_unique_tx_done(-CC2520_TX_FAILED, dev);
			}
			else {
				spin_unlock_irqrestore(&state_sl[index], flags[index]);
				DBG(KERN_INFO, "lpl%d retransmit.\n", index);
				cc2520_csma_tx(cur_tx_buf[index], cur_tx_len[index], dev);
			}
		}
		else {
			if (lpl_state[index] == CC2520_LPL_TIMER_EXPIRED) {
				lpl_state[index] = CC2520_LPL_IDLE;
				spin_unlock_irqrestore(&state_sl[index], flags[index]);
				cc2520_unique_tx_done(CC2520_TX_SUCCESS, dev);
			}
			else {
				spin_unlock_irqrestore(&state_sl[index], flags[index]);
				cc2520_csma_tx(cur_tx_buf[index], cur_tx_len[index], dev);
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

static void cc2520_lpl_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;
	cc2520_unique_rx_done(buf, len, dev);
}

static void cc2520_lpl_start_timer(int index)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * (lpl_interval[index] + 2 * lpl_window[index]));
	hrtimer_start(&lpl_timer[index].timer, kt, HRTIMER_MODE_REL);
}

static enum hrtimer_restart cc2520_lpl_timer_cb(struct hrtimer *timer)
{
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);
	int index = tmp->index;

	spin_lock_irqsave(&state_sl[index], flags[index]);
	if (lpl_state[index] == CC2520_LPL_TX) {
		lpl_state[index] = CC2520_LPL_TIMER_EXPIRED;
		spin_unlock_irqrestore(&state_sl[index], flags[index]);
	}
	else {
		spin_unlock_irqrestore(&state_sl[index], flags[index]);
		INFO(KERN_INFO, "lpl%d timer in improbable state.\n", index);
	}

	return HRTIMER_NORESTART;
}

void cc2520_lpl_set_enabled(bool enabled, int index)
{
	lpl_enabled[index] = enabled;
}

void cc2520_lpl_set_listen_length(int length, int index)
{
	lpl_window[index] = length;
}

void cc2520_lpl_set_wakeup_interval(int interval, int index)
{
	lpl_interval[index] = interval;
}
