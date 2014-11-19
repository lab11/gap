#ifndef LPL_H
#define LPL_H

#include "cc2520.h"

int cc2520_lpl_init(void);
void cc2520_lpl_free(void);

void cc2520_lpl_set_enabled(bool enabled, int index);
void cc2520_lpl_set_listen_length(int length, int index);
void cc2520_lpl_set_wakeup_interval(int interval, int index);

int cc2520_lpl_tx(u8 * buf, u8 len, struct cc2520_dev *dev);
void cc2520_lpl_tx_done(u8 status, struct cc2520_dev *dev);
void cc2520_lpl_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);

#endif