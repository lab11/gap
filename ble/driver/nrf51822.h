#ifndef _nrf51822_H_
#define _nrf51822_H_

int nrf51822_issue_simple_command(uint8_t command);
static void nrf51822_read_irq(void);

#endif