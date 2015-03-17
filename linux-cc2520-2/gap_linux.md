BBB + GAP + Linux 3.19
======================

This guide is a good starting point for getting a Beaglebone Black running
with the latest Linux kernel (3.19 right now) and the necessary kernel
drivers for using the GAP cape.


Setup
------------

1. Get RCN's `bb-kernel` repo.

        git clone https://github.com/RobertCNelson/bb-kernel.git

2. Build Linux.

        cd bb-kernel
        git checkout am33x-v3.19
        ./build.sh

    The default kernel config options are correct and you shouldn't need
    to change them.

3. Get a USB drive that has
[BBB Debian](http://beagleboard.org/latest-images) on
it. To create one, on Windows I use Win32 Disk Imager.

4. Flash the kernel to the USB drive to overwrite the older Linux kernel that
comes with the Debian distribution.

        ./tools/install_kernel.sh

    Make sure you get the correct partition to install the kernel to!

5. Now compile the new device tree that includes entries for GAP. Hopefully,
in the future, we can go back to device tree overlays, but for now that
feature has been removed from Linux.

        scp am335x-boneblack.dts <bbb>:~/
        # do this on the beaglebone black
        dtc -O dtb -o am335x-boneblack.dtb am335x-boneblack.dts
        sudo cp am335x-boneblack.dtb /boot/uboot/dtbs/

        # reboot to apply the overlay
        sudo reboot

6. Once rebooted, you can check that things were setup correctly.

        lsmod

    If you see `cc2520` in the list, then the device tree successfully
    loaded the device driver. Further, you can:

        ifconfig -a

    You should see `wpan0` and `wpan1`.

7. Get the wpan-tools. These are the newest version of a project that
has gone through many name changes.

        sudo apt-get install libnl-3-dev libnl-genl-3-dev

        wget http://wpan.cakelab.org/releases/wpan-tools-0.4.tar.gz
        tar -xf wpan-tools-0.4.tar.gz
        cd wpan-tools-0.4
        ./configure
        make
        sudo make install




Configuring CC2520
------------------

Once the hardware and software packages are setup, the next step is the art
of using the in-flux tools to get things configured.

1. Set the channel on the physical device.

        iwpan phy wpan-phy0 set channel 0 11

1. Create the wpan device. If it exists, delete it first. We need to set the
address.

        iwpan dev wpan0 del
        iwpan phy wpan-phy0 interface add wpan0 type node c0:98:e5:00:00:00:00:01

2. Configure the wpan device with a PAN id.

        iwpan dev wpan0 set pan_id 0x0022

3. Add a lowpan device.

        ip link add link wpan0 name lowpan0 type lowpan

4. Bring things up.

        ifconfig lowpan0 up
        ifocnfig wpan0 up

5. Ping!

        ping6 fe80::<something>%lowpan0


