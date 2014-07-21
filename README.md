beaglebone-cc2520
=================

Code, hardware, and instructions to use the TI CC2520 with the Beaglebone Black, based on the [Raspberry Pi CC2520 board](https://github.com/lab11/raspberrypi-cc2520).

Hardware
--------
### Cape
A cape for the Beaglebone Black utilizing the CC2520 is in development. Currently, users can use the one built for the Raspberry Pi and a few jumper wires.

### EEPROM
Eventually, the cape will feature EEPROM that will allow for the capemgr to auto load the driver and configure pins on boot. There exists a utility to create the hexdump (data.eeprom) to flash to the EEPROM in beaglebone-cc2520/software/utility. After creating the hexdump, use this command to flash the EEPROM:

```bash
cat data.eeprom > /sys/bus/i2c/devices/1-0057/eeprom
```
Where 1-0057 is the default address for the Zigbeag cape. This can be changed by using the solder jumpers on the board.

Software
--------
### Install

This guide assumes the default Angstrom distro running under root.<br/>
Clone the repo to wherever you feel fit:

```bash
git clone https://github.com/lab11/beaglebone-cc2520.git
```

Note: you may have to follow the directions [here](http://derekmolloy.ie/fixing-git-and-curl-certificates-problem-on-beaglebone-blac/) to get git working correctly.

An install script is located in /software. <br/>
The script will copy the Device Tree Overlay (DTO) to /lib/firmware, and the driver to /lib/modules/KERNEL_VERSION, where KERNEL_VERSION is your current running kernel version number. <br/>
After navigating to software/, just run the install script:

```bash
cd beaglebone-cc2520/software/
./install
```

If using the Zigbeag cape, just plug it in and reboot the beaglebone and your board will be fully functional.

If using the board developed for the Raspberry Pi, it is not possible to autonomously configure the pinmux and load the driver on boot. This is because the beaglebone requires onboard EEPROM to tell it how to mux its pins and what driver to load. All this must be done manually without EEPROM.<br/>
To load the Device Tree Overlay:

```bash
echo BB-BONE-CC2520 > /sys/devices/bone_capemgr.9/slots
```
This command should complete with no output.
To check that the DTO has loaded, run:

```bash
cat /sys/devices/bone_capemgr.9/slots
```
Note: bone_capemgr.9 may actually be bone_capemgr.8 for different beaglebones. <br/>
You should check see that there is now an override board in a slot after the virtual HDMI cape. <br/>
Here is my output:

```
 0: 54:PF---
 1: 55:PF---
 2: 56:PF---
 3: 57:PF---
 4: ff:P-O-L Bone-LT-eMMC-2G,00A0,Texas Instrument,BB-BONE-EMMC-2G
 5: ff:P-O-L Bone-Black-HDMI,00A0,Texas Instrument,BB-BONELT-HDMI
 7: ff:P-O-L Override Board Name,00A0,Override Manuf,BB-BONE-CC2520
```

Now that the DTO is loaded, the pins are now muxed correctly for the cc2520 board. We can now load the driver with:

```bash
modprobe cc2520
```

This command should complete with no output. <br/>

#### Testing
You should now have a functioning Zigbee radio. There are test programs located in software/driver/tests. Try running the write and read program on two beaglebones, and you should be able to see them talking to eachother.

#### Install Issues
If you run into issues with any of the above commands, check dmesg for any output from the kernel. <br/>
Run this to check for issues with loading the DTO:

```bash
dmesg | grep bone_capemgr
```

Run this to check for issues with loading the driver:

```bash
dmesg | grep cc2520
```

### Kernel Module

In order to support the CC2520, you need the kernel module located in software/driver. The install script will automatically copy this into /lib/modules/KERNEL_VERSION. <br/>
This driver has been adapted from https://github.com/ab500/linux-cc2520-driver. <br/>
The driver uses the following pin configuration:

Pin on RPI-CC2520 board | Pin on BBB
---------------- | ----------
SFD | P9_14
FIFP | P9_23
FIFO | P9_27
RST | P9_15
CCA | P9_12
LED0 | P8_17
LED1 | P8_18
LED2 | P8_16
MOSI | P9_21
MISO | P9_18
SCLK | P9_22
CS | P9_17

If you would like to compile the driver from source, there is some setup involved.
I've found it easiest to use a symlink and compile from the current kernel on the Beaglebone.

```bash
opkg update
opkg upgrade
```
It would be a good idea to reboot at this point

```bash
opkg install kernel-headers
opkg install kernel-dev
cd /usr/src/kernel
make scripts
ln -s /usr/src/kernel /lib/modules/$(uname -r)/build
```

You can then navigate to beaglebone-cc2520/software/driver and run make to compile the driver. Make sure you copy cc2520.ko to /lib/modules/KERNEL_VERSION and run depmod -a to register the driver with your system.


### TinyOS

TinyOS support is not currently available, but is in the works.