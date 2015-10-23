BBB + GAP + Linux
======================

This provides some instructions for using GAP with Linux.

Setup from new BBB Install
--------------------------

After flashing the BBB, ssh to it and:
```
# lockout root
edit /etc/ssh/sshd_config

# change password
passwd

# packages
sudo apt-get update
sudo apt-get install vim git lsb-release

# update kernel
sudo /opt/scripts/tools/update_kernel.sh --beta --bone-channel

# Add overlays
git clone https://github.com/lab11/bb.org-overlays
cd bb.org-overlays
./dtc-overlay.sh
./install.sh

# Make GAP load on boot
edit /boot/uEnv.txt
  cape_enable=bone_capemgr.enable_partno=BB-GAP

# Reboot
sudo reboot

# Verify
/sbin/ifconfig -a

# Get WPAN tools
sudo apt-get install pkg-config libnl-3-dev libnl-genl-3-dev
wget http://wpan.cakelab.org/releases/wpan-tools-0.5.tar.gz
tar xf wpan-tools-0.5.tar.gz
cd wpan-tools-0.5
./configure
make
sudo make install

```


Sniff broadcast packets
-----------------------

```
sudo apt-get install tcpdump

iwpan phy phy0 set channel 0 11
iwpan dev wpan0 del
iwpan phy phy0 interface add wpan0 type node c0:98:e5:00:00:00:00:01
iwpan dev wpan0 set pan_id 0x0022
/sbin/ifconfig wpan0 up

sudo tcpdump -i wpan0 -vvv
```




Configuring CC2520 with 6LoWPAN
------------------

Once the hardware and software packages are setup, the next step is the art
of using the in-flux tools to get things configured.

1. Set the channel on the physical device.

        iwpan phy phy0 set channel 0 11

1. Create the wpan device. If it exists, delete it first. We need to set the
address.

        iwpan dev wpan0 del
        iwpan phy phy0 interface add wpan0 type node c0:98:e5:00:00:00:00:01

2. Configure the wpan device with a PAN id.

        iwpan dev wpan0 set pan_id 0x0022

3. Add a lowpan device.

        ip link add link wpan0 name lowpan0 type lowpan

4. Bring things up.

        ifconfig lowpan0 up
        ifconfig wpan0 up

5. Ping!

        ping6 fe80::<something>%lowpan0


