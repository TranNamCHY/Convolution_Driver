################################################################################
# Cross Compilation Options
################################################################################
#This indicate the patth to the toolchain that compatible with your target hardware
CROSS_COMPILE = /home/nambcn/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

# This variable informs the kernel Makefile what architecture you're targeting.
# This is required when building the driver, if CROSS_COMPILE is defined.
ARCH = arm

################################################################################
# Build Options
################################################################################

#The path to the kernel source tree, remember you need to ensure the kernel source has been configure before
#use it to build a kernel module
KBUILD_DIR = /home/nambcn/petalinux/zedboard/build/tmp/work-shared/zynq-generic/kernel-build-artifacts
