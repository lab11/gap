#ARCH=arm
#CROSS_COMPILE=arm-linux-gnueabi-
#DRIVER = spike

obj-m += cc2520.o
cc2520-y = radio.o interface.o module.o platform.o sack.o lpl.o packet.o csma.o unique.o

# Set this is your linux kernel checkout.
#KDIR := /lib/modules/$(shell uname -r)/build 
#KDIR := ~/git/bb-kernel/KERNEL
#PWD := $(shell pwd)

#default:
#		  $(MAKE) -C $(KDIR) M=$(PWD) modules

#clean:
#		  $(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

#.PHONY: clean default
