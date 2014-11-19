#ifndef UNIQUE_H
#define UNIQUE_H

#include "cc2520.h"

int cc2520_unique_init(struct cc2520_dev *dev);
void cc2520_unique_free(struct cc2520_dev *dev);

int cc2520_unique_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
void cc2520_unique_tx_done(u8 status, struct cc2520_dev *dev);
void cc2520_unique_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);

#endif