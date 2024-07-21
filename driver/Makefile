# Kbuild
#
# Date: March 21, 2017
# Author: Brandon Perez
# Author: Jared Choi
#
# The build file for compiling the AXI DMA driver against the kernel. This is
# used by the kernel to determine how to compile the driver.

# Indicate that this module should be compiled to the kernel Makefile, and the
# files to use to compile it.
$(AXIDMA_MODULE_NAME)-objs = $(patsubst %.c,%.o,$(filter %.c,$(AXIDMA_FILES)))
obj-m += $(AXIDMA_MODULE_NAME).o
