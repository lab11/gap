#ifndef INTERFACE_H
#define INTERFACE_H

// Interface
int cc2520_interface_init(void);
void cc2520_interface_free(void);

extern struct cc2520_interface *interface_bottom;
extern struct cc2520_dev *cc2520_devices;

#endif
