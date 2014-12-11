Setting Up The nRF51822
=======================

This guide explains how to get going with running code on nRF51822 based
platforms.

Setup
=====

1. Get the makefiles. This is an external set of makefiles for the nRF51822.
Apparently they are better than the ones that ship with Nordic's SDK. They
are also on github and can be modified as needed.

        git clone https://github.com/hlnd/nrf51-pure-gcc-setup.git

1. Make sure you have the [arm-none-eabi-gcc](https://launchpad.net/gcc-arm-embedded)
toolchain. You just need the binaries for your platform.

1. Get the nRF51822 SDK and S120 soft device from the
[downloads page](https://www.nordicsemi.com/eng/Products/Bluetooth-Smart-Bluetooth-low-energy/nRF51822?resource=20339).
You want the "nRF51 SDK Zip File" and the "S120 nRF51822 SoftDevice (Production ready)".
You do need to buy a nRF51822 evm kit to get access to these, because companies
are the worst.

1. Get the [Segger flashing tools](http://www.segger.com/jlink-software.html)
for your platform.



Install an Application
======================

1. Make sure the path to the SDK is set correctly in the application
makefile (or override it in your environment).

1. Make sure the path to the gcc Makefiles is set correctly in the application
makefile (or override it in your environment).

1. Just once you need to load the soft device onto the nRF51822. In the application
directory:

        sudo make flash-softdevice SOFTDEVICE=/path/to/softdevice/s120_nrf51822_X.X.X_softdevice.hex

1. Now compile and load the application code.

        sudo make flash


