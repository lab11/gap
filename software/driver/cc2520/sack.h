#ifndef SACK_H
#define SACK_H

#include "cc2520.h"

extern struct cc2520_interface *sack_top[CC2520_NUM_DEVICES];
extern struct cc2520_interface *sack_bottom[CC2520_NUM_DEVICES];

int cc2520_sack_init(void);
void cc2520_sack_free(void);
void cc2520_sack_set_timeout(int timeout, int index);

#endif