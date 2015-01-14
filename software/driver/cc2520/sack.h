#ifndef SACK_H
#define SACK_H

#include "cc2520.h"

int cc2520_sack_init(struct cc2520_dev *dev);
void cc2520_sack_free(struct cc2520_dev *dev);
void cc2520_sack_set_timeout(int timeout, struct cc2520_dev *dev);
void cc2520_sack_set_enabled(bool enabled, struct cc2520_dev *dev);

int cc2520_sack_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
void cc2520_sack_tx_done(u8 status, struct cc2520_dev *dev);
void cc2520_sack_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);

#endif