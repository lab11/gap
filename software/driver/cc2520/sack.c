#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>

#include "sack.h"
#include "cc2520.h"
#include "packet.h"
#include "radio.h"
#include "debug.h"
#include "interface.h"
#include "csma.h"

static enum hrtimer_restart cc2520_sack_timer_cb(struct hrtimer *timer);
static void cc2520_sack_start_timer(struct cc2520_dev *dev);

// Two pieces to software acknowledgements:
// 1 - Taking packets we're transmitting, setting an ACK flag
//     on them, and waiting for that ACK to be received before
//     calling tx_done.
//     Requires:
//     - A timeout equivalent to the ACK period
//     - Storing the DSN of the outgoing packet
//     - Interface to verify if an ACK is correct
// 2 - Examining packets we're receiving and sending an ACK if
//     needed.
//     Requires:
//     - Buffer to build ACK packet
//     - Concurrency mechanism to prevent transmission
//       during ACKing.

enum cc2520_sack_state_enum {
	CC2520_SACK_IDLE,
	CC2520_SACK_TX, // Waiting for a tx to complete
	CC2520_SACK_TX_WAIT, // Waiting for an ack to be received
	CC2520_SACK_TX_ACK, // Waiting for a sent ack to finish
};

int cc2520_sack_init(struct cc2520_dev *dev)
{
	hrtimer_init(&dev->timeout_timer.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->timeout_timer.timer.function = &cc2520_sack_timer_cb;
	dev->timeout_timer.dev = dev;

	spin_lock_init(&dev->sack_sl);
	dev->sack_state = CC2520_SACK_IDLE;

	dev->ack_timeout = CC2520_DEF_ACK_TIMEOUT;
	dev->sack_enabled = CC2520_DEF_SACK_ENABLED;

	return 0;
}

void cc2520_sack_free(struct cc2520_dev *dev)
{
	hrtimer_cancel(&dev->timeout_timer.timer);
}

void cc2520_sack_set_timeout(int timeout, struct cc2520_dev *dev)
{
	dev->ack_timeout = timeout;
}

void cc2520_sack_set_enabled(bool enabled, struct cc2520_dev *dev)
{
	dev->sack_enabled = enabled;
}

static void cc2520_sack_start_timer(struct cc2520_dev *dev)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * dev->ack_timeout);
	hrtimer_start(&dev->timeout_timer.timer, kt, HRTIMER_MODE_REL);
}

int cc2520_sack_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	spin_lock_irqsave(&dev->sack_sl, dev->sack_flags);

	if (dev->sack_state != CC2520_SACK_IDLE) {
		INFO(KERN_INFO, "Ut oh! Tx spinlocking.\n");
	}

	while (dev->sack_state != CC2520_SACK_IDLE) {
		spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
		spin_lock_irqsave(&dev->sack_sl, dev->sack_flags);
	}
	dev->sack_state = CC2520_SACK_TX;
	spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);

	memcpy(dev->cur_tx_buf, buf, len);
	return cc2520_radio_tx(dev->cur_tx_buf, len, dev);
}

void cc2520_sack_tx_done(u8 status, struct cc2520_dev *dev)
{
	if(dev->sack_enabled){
		spin_lock_irqsave(&dev->sack_sl, dev->sack_flags);

		if (dev->sack_state == CC2520_SACK_TX) {
			if (cc2520_packet_requires_ack_wait(dev->cur_tx_buf)) {
				DBG(KERN_INFO, "radio%d entering TX wait state.\n", dev->id);
				dev->sack_state = CC2520_SACK_TX_WAIT;
				cc2520_sack_start_timer(dev);
				spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
			}
			else {
				dev->sack_state = CC2520_SACK_IDLE;
				spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
				cc2520_csma_tx_done(status, dev);
			}
		}
		else if (dev->sack_state == CC2520_SACK_TX_ACK) {
			dev->sack_state = CC2520_SACK_IDLE;
			spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
		}
		else {
			ERR(KERN_ALERT, "ERROR: radio%d tx_done state engine in impossible state.\n", dev->id);
		}
	}
	else {
		cc2520_csma_tx_done(status, dev);
	}
}

void cc2520_sack_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	// if this packet we just received requires
	// an ACK, transmit it.
	memcpy(dev->cur_rx_buf, buf, len);
	dev->cur_rx_buf_len = len;

	// NOTE: this is a big hack right now,
	// and I'm not sure if it's even needed.
	// We introduce a strong coupling between
	// the sack layer and the radio layer here
	// by providing a mechanism to explicitly
	// release the buffer. When I was troubleshooting
	// a terrible concurrency bug I added this
	// as a possible solution, but I don't
	// think it's needed anymore.
	cc2520_radio_release_rx(dev);

	spin_lock_irqsave(&dev->sack_sl, dev->sack_flags);

	if (cc2520_packet_is_ack(dev->cur_rx_buf)) {
		if (dev->sack_state == CC2520_SACK_TX_WAIT &&
			cc2520_packet_is_ack_to(dev->cur_rx_buf, dev->cur_tx_buf)) {
			dev->sack_state = CC2520_SACK_IDLE;
			spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);

			hrtimer_cancel(&dev->timeout_timer.timer);
			cc2520_csma_tx_done(CC2520_TX_SUCCESS, dev);
		}
		else {
			spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
			INFO(KERN_INFO, "stray ack received.\n");
		}
	}
	else {
		if (cc2520_packet_requires_ack_reply(dev->cur_rx_buf) &&
			dev->sack_enabled) {
			if (dev->sack_state == CC2520_SACK_IDLE) {
				cc2520_packet_create_ack(dev->cur_rx_buf, dev->ack_buf);
				dev->sack_state = CC2520_SACK_TX_ACK;
				spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
				cc2520_radio_tx(dev->ack_buf, IEEE154_ACK_FRAME_LENGTH + 1, dev);
				cc2520_csma_rx_done(dev->cur_rx_buf, dev->cur_rx_buf_len, dev);
			}
			else {
				spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
				INFO(KERN_INFO, "ACK skipped, soft-ack layer busy. %d \n", dev->sack_state);
			}
		}
		else {
			spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
			cc2520_csma_rx_done(dev->cur_rx_buf, dev->cur_rx_buf_len, dev);
		}
	}
}

static enum hrtimer_restart cc2520_sack_timer_cb(struct hrtimer *timer)
{
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);
	struct cc2520_dev *dev = tmp->dev;

	spin_lock_irqsave(&dev->sack_sl, dev->sack_flags);

	if (dev->sack_state == CC2520_SACK_TX_WAIT) {
		INFO(KERN_INFO, "radio%d tx ack timeout exceeded.\n", dev->id);
		dev->sack_state = CC2520_SACK_IDLE;
		spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);

		cc2520_csma_tx_done(-CC2520_TX_ACK_TIMEOUT, dev);
	}
	else {
		spin_unlock_irqrestore(&dev->sack_sl, dev->sack_flags);
	}

	return HRTIMER_NORESTART;
}

