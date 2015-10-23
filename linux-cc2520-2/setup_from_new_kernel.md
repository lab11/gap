Setup new BBB with GAP
======================



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


Sniff broadcast packets:

```
sudo apt-get install tcpdump

iwpan phy phy0 set channel 0 11
iwpan dev wpan0 del
iwpan phy phy0 interface add wpan0 type node c0:98:e5:00:00:00:00:01
iwpan dev wpan0 set pan_id 0x0022
/sbin/ifconfig wpan0 up

sudo tcpdump -i wpan0 -vvv
```
