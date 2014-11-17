#ARCH=arm
#CROSS_COMPILE=arm-linux-gnueabi-

obj-y := cc2520/ nrf51822/ gapspi/

# Set this is your linux kernel checkout if cross-compiling.
#KDIR := /lib/modules/$(shell uname -r)/build
KDIR := ~/git/bb-kernel/KERNEL
PWD := $(shell pwd)

default:
		  $(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
		  $(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

.PHONY: clean default
