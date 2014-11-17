GAP: Generic Access Point
=========================

GAP is the WiFi router for low-power and embedded Internet of Things devices.
While WiFi routers provide ubiquitous Internet access for laptops and
smartphones, GAP provides Internet access for low-power sensors and wearable
devices. It supports both 802.15.4 and Bluetooth Low Energy.

GAP is implemented as a cape for the
[BeagleBone Black](http://beagleboard.org/black). It uses two TI CC2520 radios
and a Nordic nRF51822 radio to provide connectivity. Each radio has a linux
kernel module to allow userspace access to the radios.

Hardware
--------

### Cape

The Zigbeag Cape is on Revision B. It features an SPI interface to two CC2520
radios, one of which is amplified with a CC2591, and one nRF51822 radio.

### EEPROM

The capes feature EEPROM that will allow for the capemgr to auto load the driver
and configure pins on boot, in accordance with the Beaglebone Black SRM. There
exists a utility to create the hexdump (data.eeprom) to flash to the EEPROM in
beaglebone-cc2520/software/utility. After creating the hexdump, apply a jumper
to the write header and use this command to flash the EEPROM:

```bash
cat data.eeprom > /sys/bus/i2c/devices/1-0057/eeprom
```

Where 1-0057 is the default address for the Zigbeag cape. This can be changed by
using the solder jumper pads on the upper left corner of the board.


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
Navigate to software/ and run the install script:

```bash
cd beaglebone-cc2520/software/
./install
```

If using the Zigbeag cape with configured EEPROM, just plug it in and reboot the beaglebone and your board will be fully functional.

If EEPROM is not configured, to load the Device Tree Overlay manually:

```bash
echo BB-BONE-CC2520 > /sys/devices/bone_capemgr.9/slots
```
This command should complete with no output.
To check that the DTO has loaded, run:

```bash
cat /sys/devices/bone_capemgr.9/slots
```
Note: bone_capemgr.9 may actually be bone_capemgr.8 for different beaglebones. <br/>
You should check to see that there is now an override board in a slot after the virtual HDMI cape. <br/>
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
You can check that the driver loaded successfully with:

```bash
lsmod
```


#### Testing
You should now have a functioning Zigbeag cape. There are test programs located in software/driver/tests. Try running the write and read program on two beaglebones, and you should be able to see them talking to each other.

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
This driver has been adapted from the [Linux CC2520 Driver](https:/github.com/ab500/linux-cc2520-driver). <br/>
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

You can then navigate to beaglebone-cc2520/software/driver and run make to compile the driver. Make sure you re-run the install script, which will copy cc2520.ko to /lib/modules/KERNEL_VERSION and run depmod -a to register the driver with your system.