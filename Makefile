# Include the user defined variables, if they specified them
-include config.mk

################################################################################
# Configuration
################################################################################

# Set the shell to BASH
SHELL = /bin/bash

# Allow the user to specify cross-compilation from the command line
CC = $(CROSS_COMPILE)gcc

# Standard gcc flags for compilation
GLOBAL_CFLAGS = -Wall -Wextra -Werror -std=gnu99 -g -O0

# The location where the compiled executables and driver will be stored
OUTPUT_DIR ?= outputs

################################################################################
# Targets
################################################################################

# None of the targets correspond to actual files
.PHONY: all clean cross_compiler_check help

# Compile the driver, library, and examples in release mode as the default
all: driver app

# Include the specific targets for the examples, library, and driver (the
# includes must go here, so the default target is 'all')
# Make the specified output directory, if it doesn't exist
$(OUTPUT_DIR):
	@mkdir -p $(OUTPUT_DIR)

include driver/driver.mk
include app/app.mk

# Clean up all temporary files
clean: driver_clean app_clean
	rm -rf $(OUTPUT_DIR)

# Check that the specified cross-compiler exists
cross_compiler_check:
ifdef CROSS_COMPILE
ifeq (,$(shell which $(CC)))
	@printf "Error: '$(CROSS_COMPILE)' is not a valid cross-compiler prefix. "
	@printf "'$(CC)' was not found in your path.\n"
	@exit 1
endif
endif

