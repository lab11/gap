#ifndef _BCP_H_
#define _BCP_H_

// Commands that are issued to the nRF51822
#define BCP_COMMAND_READ_IRQ                  1  // Read whatever caused the interrupt we received.
#define BCP_COMMAND_SNIFF_ADVERTISEMENTS      2  // Tell the nRF51822 to send us all received advertisements.
#define BCP_COMMAND_SNIFF_ADVERTISEMENTS_STOP 3  // Stop sending advertisements packets.



#endif