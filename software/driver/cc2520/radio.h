#ifndef RADIO_H
#define RADIO_H

#include <linux/types.h>
#include <linux/semaphore.h>  /* Semaphore */
#include <linux/workqueue.h>
#include <linux/spinlock.h>

// Radio Initializers
int cc2520_radio_init(struct cc2520_dev *dev);
void cc2520_radio_free(void);

// Radio Commands
void cc2520_radio_start(struct cc2520_dev *dev);
void cc2520_radio_on(struct cc2520_dev *dev);
void cc2520_radio_off(struct cc2520_dev *dev);
void cc2520_radio_set_channel(int channel, struct cc2520_dev *dev);
void cc2520_radio_set_address(u16 short_addr, u64 extended_addr, u16 pan_id, struct cc2520_dev *dev);
void cc2520_radio_set_txpower(u8 power, struct cc2520_dev *dev);

void cc2520_radio_release_rx(int index);
bool cc2520_radio_is_clear(struct cc2520_dev *dev);

// Radio Interrupt Callbacks
void cc2520_radio_sfd_occurred(u64 nano_timestamp, u8 is_high, struct cc2520_dev *dev);
void cc2520_radio_fifop_occurred(struct cc2520_dev *dev);

int cc2520_radio_tx(u8 *buf, u8 len, struct cc2520_dev *dev);

#endif