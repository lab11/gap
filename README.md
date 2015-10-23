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

The GAP cape features a SPI interface to two CC2520
radios, one of which is amplified with a CC2591, and one nRF51822 radio.
It also includes four LEDs.


Software
--------

The CC2520 radios are
[supported natively](https://github.com/torvalds/linux/blob/master/drivers/net/ieee802154/cc2520.c)
in newer versions of the Linux kernel (>=4.1).

The nRF51822 BLE radio is a work-in-progress. It can be used as a standalone
BLE radio (see [this repo](https://github.com/lab11/nrf5x-base) for a starting
point), but does not have great integration with the BBB at this point.



Setting Up GAP
--------------

To setup a BBB to work with GAP, follow these instructions:

1. Start with a recent build of Debian for the BBB. We suggest starting
with pretty new version of Debian, like that can be found
[here](http://elinux.org/Beagleboard:BeagleBoneBlack_Debian#Jessie_Snapshot_console).

2. Once that is setup, ssh to the BBB and update the kernel to the newest
version. This will not only make sure you're running the latest code,
but also ensure all of the kernel modules will be present.

        sudo apt-get update
        sudo apt-get install vim git lsb-release
        sudo /opt/scripts/tools/update_kernel.sh --beta --bone-channel

    Luckily [Robert C Nelson](https://github.com/RobertCNelson/)
    has made this really easy with a convenient script.

3. Now we have to setup the device tree overlay to let Linux know that
the the radios exist. The GAP overlay and others are setup in a repository also
maintained by RCN.

        git clone https://github.com/lab11/bb.org-overlays
        cd bb.org-overlays
        ./dtc-overlay.sh
        ./install.sh

    That puts the compiled overlay in the correct place, now we need to tell
    the BBB to use it at boot.

        vim /boot/uEnv.txt
        # Edit that line that looks like this to include the reference to GAP
        cape_enable=bone_capemgr.enable_partno=BB-GAP

4. Reboot to apply this.

        sudo reboot



Sniffing 15.4 Packets
---------------------

To make sure everything is working, it is pretty easy to get Linux to
print out the packets the radio is receiving.

1. Install the `wpan-tools` to configure all of the 15.4 devices.

        sudo apt-get install pkg-config libnl-3-dev libnl-genl-3-dev
        wget http://wpan.cakelab.org/releases/wpan-tools-0.5.tar.gz
        tar xf wpan-tools-0.5.tar.gz
        cd wpan-tools-0.5
        ./configure
        make
        sudo make install

1. Install `tcpdump` to view the packets.

        sudo apt-get install tcpdump

2. Configure the network devices. Be sure to set the channel and PANID
to match what is transmitting the 15.4 packets.

        iwpan phy phy0 set channel 0 11
        iwpan dev wpan0 del
        iwpan phy phy0 interface add wpan0 type node c0:98:e5:00:00:00:00:01
        iwpan dev wpan0 set pan_id 0x0022
        /sbin/ifconfig wpan0 up

3. Use `tcpdump` to view them.

        sudo tcpdump -i wpan0 -vvv


<!--

### Setup EEPROM

Now we are getting close. We have told Linux about the kernel modules
and added the device tree overlay to a place where Linux can find it.
The last step is to get Linux to load the overlay which will then cause
it to load the kernel modules. This is where the EEPROM comes in.

When the BeagleBone Black boots it checks for EEPROMs present on any capes
and uses the configuration data in the EEPROM to load the correct device
tree overlay.

There exists a utility to create the hexdump (data.eeprom) to flash to the
EEPROM in `/software/utility`. After creating the hexdump, apply a jumper
to the write header near the EEPROM chip and use this command to flash the
EEPROM:

Choose 12 characters to serve as the serial number, in
the form `WWYY&&&&nnnn`. From the SRM:

    WW = 2 digit week of the year of production.
    YY = 2 digit year of production.
    &&&& = Assembly code, up to you to decide.
    nnnn = incrementing board number for week of production.

After creating `data.eeprom` with the eepromflasher utilty, write it to the
cape EEPROM as root.

    # On the BBB
    sudo su
    cat data.eeprom > /sys/bus/i2c/devices/1-0057/eeprom

Where `1-0057` is the default address for the GAP cape. This can be changed
by using the solder jumper pads near the EEPROM on the upper left corner of the
board.

Unfortunately, the BBB debian distribution fails to load custom firmware from
the EEPROM on boot, and requires an capemanager configuration file to be
edited.

    sudo vim /etc/default/capemgr

    # Add this line:
    CAPE=BB-BONE-GAP


CC2520 Border Router
--------------------

One way to use the CC2520s on GAP is with the TinyOS code in a related
repo: [RaspberryPi-CC2520](https://github.com/lab11/raspberrypi-cc2520).
The `BorderRouter` application can be compiled with `make gap`.
-->

<!--


### EEPROM

The capes feature EEPROM that will allow for the BeagleBone capemgr to auto load the driver
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

You can then navigate to beaglebone-cc2520/software/driver and run make to compile the driver. Make sure you re-run the install script, which will copy cc2520.ko to /lib/modules/KERNEL_VERSION and run depmod -a to register the driver with your system. -->
