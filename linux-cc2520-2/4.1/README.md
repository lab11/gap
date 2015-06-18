Linux 4.1 BBB Device Tree
=========================

Copy the device tree file to the correct location in the linux kernel,
then compile the kernel to compile the device tree.

    cp am335x-boneblack.dts am335x-bone-common.dtsi KERNEL/arch/arm/boot/dts/
