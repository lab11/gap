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

struct cc2520_interface *sack_top[CC2520_NUM_DEVICES];
struct cc2520_interface *sack_bottom[CC2520_NUM_DEVICES];

static int cc2520_sack_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
static void cc2520_sack_tx_done(u8 status, struct cc2520_dev *dev);
static void cc2520_sack_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);
static enum hrtimer_restart cc2520_sack_timer_cb(struct hrtimer *timer);
static void cc2520_sack_start_timer(int index);

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

static u8 *ack_buf[CC2520_NUM_DEVICES];
static u8 *cur_tx_buf[CC2520_NUM_DEVICES];

static u8 *cur_rx_buf[CC2520_NUM_DEVICES];
static u8 cur_rx_buf_len[CC2520_NUM_DEVICES];

struct timer_struct{
	struct hrtimer timer;
	struct cc2520_dev *dev;
};

static struct timer_struct timeout_timer[CC2520_NUM_DEVICES];
static int ack_timeout[CC2520_NUM_DEVICES]; //in microseconds
static int sack_state[CC2520_NUM_DEVICES];
static spinlock_t sack_sl[CC2520_NUM_DEVICES];

static unsigned long flags[CC2520_NUM_DEVICES];

enum cc2520_sack_state_enum {
	CC2520_SACK_IDLE,
	CC2520_SACK_TX, // Waiting for a tx to complete
	CC2520_SACK_TX_WAIT, // Waiting for an ack to be received
	CC2520_SACK_TX_ACK, // Waiting for a sent ack to finish
};

int cc2520_sack_init()
{
	int i;
	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		sack_top[i]->tx = cc2520_sack_tx;
		sack_bottom[i]->tx_done = cc2520_sack_tx_done;
		sack_bottom[i]->rx_done = cc2520_sack_rx_done;

		ack_buf[i] = kmalloc(IEEE154_ACK_FRAME_LENGTH + 1, GFP_KERNEL);
		if (!ack_buf[i]) {
			goto error;
		}

		cur_tx_buf[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!cur_tx_buf[i]) {
			goto error;
		}

		cur_rx_buf[i] = kmalloc(PKT_BUFF_SIZE, GFP_KERNEL);
		if (!cur_rx_buf[i]) {
			goto error;
		}

		hrtimer_init(&timeout_timer[i].timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	    timeout_timer[i].timer.function = &cc2520_sack_timer_cb;
	    timeout_timer[i].dev = &cc2520_devices[i];

		spin_lock_init(&sack_sl[i]);
		sack_state[i] = CC2520_SACK_IDLE;

		ack_timeout[i] = CC2520_DEF_ACK_TIMEOUT;
	}

	return 0;

	error:
		for(i = 0; i < CC2520_NUM_DEVICES; ++i){
			if (ack_buf[i]) {
				kfree(ack_buf[i]);
				ack_buf[i] = NULL;
			}

			if (cur_tx_buf[i]) {
				kfree(cur_tx_buf[i]);
				cur_tx_buf[i] = NULL;
			}
		}

		return -EFAULT;
}

void cc2520_sack_free()
{
	int i;

	for(i = 0; i < CC2520_NUM_DEVICES; ++i){
		if (ack_buf[i]) {
			kfree(ack_buf[i]);
			ack_buf[i] = NULL;
		}

		if (cur_tx_buf[i]) {
			kfree(cur_tx_buf[i]);
			cur_tx_buf[i] = NULL;
		}

		hrtimer_cancel(&timeout_timer[i].timer);
	}
}

void cc2520_sack_set_timeout(int timeout, int index)
{
	ack_timeout[index] = timeout;
}

static void cc2520_sack_start_timer(int index)
{
    ktime_t kt;
    kt = ktime_set(0, 1000 * ack_timeout[index]);
	hrtimer_start(&timeout_timer[index].timer, kt, HRTIMER_MODE_REL);
}

static int cc2520_sack_tx(u8 * buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;

	spin_lock_irqsave(&sack_sl[index], flags[index]);

	if (sack_state[index] != CC2520_SACK_IDLE) {
		INFO((KERN_INFO "[cc2520] - Ut oh! Tx spinlocking.\n"));
	}

	while (sack_state[index] != CC2520_SACK_IDLE) {
		spin_unlock_irqrestore(&sack_sl[index], flags[index]);
		spin_lock_irqsave(&sack_sl[index], flags[index]);
	}
	sack_state[index] = CC2520_SACK_TX;
	spin_unlock_irqrestore(&sack_sl[index], flags[index]);

	memcpy(cur_tx_buf[index], buf, len);
	return sack_bottom[index]->tx(cur_tx_buf[index], len, dev);
}

static void cc2520_sack_tx_done(u8 status, struct cc2520_dev *dev)
{
	int index = dev->id;

	spin_lock_irqsave(&sack_sl[index], flags[index]);

	if (sack_state[index] == CC2520_SACK_TX) {
		if (cc2520_packet_requires_ack_wait(cur_tx_buf[index])) {
			DBG((KERN_INFO "[cc2520] - radio%d entering TX wait state.\n", index));
			sack_state[index] = CC2520_SACK_TX_WAIT;
			cc2520_sack_start_timer(index);
			spin_unlock_irqrestore(&sack_sl[index], flags[index]);
		}
		else {
			sack_state[index] = CC2520_SACK_IDLE;
			spin_unlock_irqrestore(&sack_sl[index], flags[index]);
			sack_top[index]->tx_done(status, dev);
		}
	}
	else if (sack_state[index] == CC2520_SACK_TX_ACK) {
		sack_state[index] = CC2520_SACK_IDLE;
		spin_unlock_irqrestore(&sack_sl[index], flags[index]);
	}
	else {
		ERR((KERN_ALERT "[cc2520] - ERROR: radio%d tx_done state engine in impossible state.\n", index));
	}
}

static void cc2520_sack_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev)
{
	int index = dev->id;

	// if this packet we just received requires
	// an ACK, trasmit it.
	memcpy(cur_rx_buf[index], buf, len);
	cur_rx_buf_len[index] = len;

	// NOTE: this is a big hack right now,
	// and I'm not sure if it's even needed.
	// We introduce a strong coupling between
	// the sack layer and the radio layer here
	// by providing a mechanism to explicitly
	// release the buffer. When I was troubleshooting
	// a terrible concurrency bug I added this
	// as a possible solution, but I don't
	// think it's needed anymore.
	cc2520_radio_release_rx(index);

	spin_lock_irqsave(&sack_sl[index], flags[index]);

	if (cc2520_packet_is_ack(cur_rx_buf[index])) {
		if (sack_state[index] == CC2520_SACK_TX_WAIT &&
			cc2520_packet_is_ack_to(cur_rx_buf[index], cur_tx_buf[index])) {
			sack_state[index] = CC2520_SACK_IDLE;
			spin_unlock_irqrestore(&sack_sl[index], flags[index]);

			hrtimer_cancel(&timeout_timer[index].timer);
			sack_top[index]->tx_done(CC2520_TX_SUCCESS, dev);
		}
		else {
			spin_unlock_irqrestore(&sack_sl[index], flags[index]);
			INFO((KERN_INFO "[cc2520] - stray ack received.\n"));
		}
	}
	else {
		if (cc2520_packet_requires_ack_reply(cur_rx_buf[index])) {
			if (sack_state[index] == CC2520_SACK_IDLE) {
				cc2520_packet_create_ack(cur_rx_buf[index], ack_buf[index]);
				sack_state[index] = CC2520_SACK_TX_ACK;
				spin_unlock_irqrestore(&sack_sl[index], flags[index]);
				sack_bottom[index]->tx(ack_buf[index], IEEE154_ACK_FRAME_LENGTH + 1, dev);
				sack_top[index]->rx_done(cur_rx_buf[index], cur_rx_buf_len[index], dev);
			}
			else {
				spin_unlock_irqrestore(&sack_sl[index], flags[index]);
				INFO((KERN_INFO "[cc2520] - ACK skipped, soft-ack layer busy. %d \n", sack_state[index]));
			}
		}
		else {
			spin_unlock_irqrestore(&sack_sl[index], flags[index]);
			sack_top[index]->rx_done(cur_rx_buf[index], cur_rx_buf_len[index], dev);
		}
	}
}

//TODO this function needs to have hrtimer in a seperate struct to access index
static enum hrtimer_restart cc2520_sack_timer_cb(struct hrtimer *timer)
{
	struct timer_struct *tmp = container_of(timer, struct timer_struct, timer);
	struct cc2520_dev *dev = tmp->dev;
	int index = dev->id;

	spin_lock_irqsave(&sack_sl[index], flags[index]);

	if (sack_state[index] == CC2520_SACK_TX_WAIT) {
		INFO((KERN_INFO "[cc2520] - radio%d tx ack timeout exceeded.\n", index));
		sack_state[index] = CC2520_SACK_IDLE;
		spin_unlock_irqrestore(&sack_sl[index], flags[index]);

		sack_top[index]->tx_done(-CC2520_TX_ACK_TIMEOUT, dev);
	}
	else {
		spin_unlock_irqrestore(&sack_sl[index], flags[index]);
	}

	return HRTIMER_NORESTART;
}

