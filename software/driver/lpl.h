#ifndef LPL_H
#define LPL_H

#include "cc2520.h"

extern struct cc2520_interface *lpl_top[CC2520_NUM_DEVICES];
extern struct cc2520_interface *lpl_bottom[CC2520_NUM_DEVICES];

int cc2520_lpl_init(void);
void cc2520_lpl_free(void);

void cc2520_lpl_set_enabled(bool enabled, int index);
void cc2520_lpl_set_listen_length(int length, int index);
void cc2520_lpl_set_wakeup_interval(int interval, int index);

#endif