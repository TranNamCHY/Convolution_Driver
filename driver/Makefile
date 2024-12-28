# Indicate that this module should be compiled to the kernel Makefile, and the
# files to use to compile it.
$(AXIDMA_MODULE_NAME)-objs = $(patsubst %.c,%.o,$(filter %.c,$(AXIDMA_FILES)))
obj-m += $(AXIDMA_MODULE_NAME).o