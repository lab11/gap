import ctypes
import socket
import fcntl

# Constants for setting up sockets and ioctls in Linux
IFF_PROMISC = 0x100
SIOCGIFFLAGS = 0x8913
SIOCSIFFLAGS = 0x8914

class ifreq(ctypes.Structure):
    _fields_ = [("ifr_ifrn", ctypes.c_char * 16),
                ("ifr_flags", ctypes.c_short)]

ETH_P_IEEE802154 = 0x00F6

# Which interface to read from
INTERFACE = 'wpan0'


# Create the raw socket
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_IEEE802154))

# Make it promiscuous
ifr = ifreq()
ifr.ifr_ifrn = INTERFACE
fcntl.ioctl(s.fileno(), SIOCGIFFLAGS, ifr) # G for Get
ifr.ifr_flags |= IFF_PROMISC
fcntl.ioctl(s.fileno(), SIOCSIFFLAGS, ifr) # S for Set

# Bind to the correct interface
s.bind((INTERFACE, 0))

# Read data
while True:
	data = s.recvfrom(15000)
	print(data)
