beaglebone-cc2520
=================

Code, hardware, and instructions to use the TI CC2520 with the Beaglebone Black, based on the [Raspberry Pi CC2520 board](https://github.com/lab11/raspberrypi-cc2520).

Hardware
--------
A cape for the Beaglebone Black utilizing the CC2520 is in development. Currently, users can use the one built for the Raspberry Pi and a few jumper wires.

Software
--------

### Kernel Module

In order to support the CC2520, you need the kernel module located in software/driver. This driver has been adapted from https://github.com/ab500/linux-cc2520-driver.
The driver uses the following configuration:

Pin on RPI board | Pin on BBB
---------------- | ----------
ID | P9_11
SFD | P9_12
FIFP | P9_13
FIFO | P9_14
RST | P9_15
CCA | P9_16
LED0 | P9_23
LED1 | P9_24

MOSI | P9_21
MISO | P9_18
SCLK | P9_22
CS | P9_17

### TinyOS

TinyOS support is not currently available, but is in the works.