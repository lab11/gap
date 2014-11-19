#ifndef INTERFACE_H
#define INTERFACE_H

// Interface
int cc2520_interface_init(void);
void cc2520_interface_free(void);

void cc2520_interface_tx_done(u8 status, struct cc2520_dev *dev);
void cc2520_interface_rx_done(u8 *buf, u8 len, struct cc2520_dev *dev);


#endif
