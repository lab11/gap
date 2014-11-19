#ifndef CSMA_H
#define CSMA_H

#include "cc2520.h"

int cc2520_csma_init(void);
void cc2520_csma_free(void);

void cc2520_csma_set_enabled(bool enabled, int index);
void cc2520_csma_set_min_backoff(int timeout, int index);
void cc2520_csma_set_init_backoff(int timeout, int index);
void cc2520_csma_set_cong_backoff(int timeout, int index);

int cc2520_csma_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
void cc2520_csma_tx_done(u8 status, struct cc2520_dev *dev);
void cc2520_csma_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);

#endif