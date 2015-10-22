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


```
